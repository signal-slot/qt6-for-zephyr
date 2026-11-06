// SPDX-License-Identifier: BSD-3-Clause
// Display glue for the Raspberry Pi 4 (rpi_4b): the firmware framebuffer,
// flipped by pan, with the V3D GPU through YakoGL.
//
//   - The framebuffer is asked for through the property mailbox code of
//     YakoGL's Pi 4 board layer (rpi_mbox.c, compiled into the app by
//     zephyr/CMakeLists.txt): FB_W x FB_H on screen, FB_COUNT * FB_H rows in
//     memory.  Buffer i is the rows [i * FB_H, (i + 1) * FB_H).  Zephyr's own
//     rpi_fw / mailbox driver must be off (boards/rpi_4b_bcm2711_gl.overlay):
//     rpi_mbox.c polls the mailbox itself.
//   - A flip is SET_VIRTUAL_OFFSET 0,y (rpi_fb_pan): the screen shows buffer
//     y / FB_H from the next vsync on, nothing is copied.
//   - The vsync counter (qzephyr_gl_vsync_count) is derived from time, and
//     qzephyr_gl_wait_vsync sleeps until the predicted edge.  Why not one
//     SET_VSYNC per wait: on the official 7-inch DSI display, measured on the
//     board (issue #9), SET_VSYNC returned 17-23 ms after a call made right
//     behind a pan (never at the next edge 14.8 ms away), and with it the
//     frame intervals alternated short and long (a visible shake).  The edge
//     period is measured at start (SET_VSYNC called back to back: 14.8 ms,
//     now and then 29.6 ms when an edge is skipped, so the median is taken);
//     the prediction is checked against a real edge every RESYNC_MS.
//     The counter also decides when the renderer may reuse a buffer (the
//     QPA stamps a released buffer with counter + 1), so a predicted edge
//     that differs from the real one by d hands a buffer back up to d too
//     early in some frames.  d was not measured on the board against the
//     real edge: only the resyncs say that the prediction and SET_VSYNC
//     agree (8 readings within 5-20 us of a whole period, taken 1 s apart
//     with an earlier build; a later build read -35 and -44 us over
//     137 periods, 2 s after the start-up measurement); the panel's own
//     edge is a separate question (#10).
//   - When the period cannot be measured (SET_VSYNC fails, or answers
//     outside 5-50 ms, as with no display behind it) the glue paces to a
//     nominal 60 Hz and never calls SET_VSYNC again.
//   - qzephyr_display_write() (QPainter / QtWidgets frames) copies the frame
//     into the buffer that is not on screen, then flips.
//   - The mailbox code has no lock ("one request in flight; the sample is
//     single-threaded").  Its callers here: this file (init, flips, vsync
//     waits), all from the thread that renders (the main thread), and the
//     V3D clock switch (v3d_clock_set) that YakoGL calls from eglInitialize
//     (power on) and, during a frame, when it recovers a stuck GPU (reset:
//     power off and on).  The QPA does not use a render thread
//     (ThreadedOpenGL is false), so all of these are one thread; the mutex
//     below covers every mailbox call made from this file and from
//     v3d_clock_set, as a guard, not because a second thread was seen.
//
// Measured on a Raspberry Pi 4 with the official 7-inch DSI display, 800x480,
// quick-hello, before the window format got GLES_NATIVE_WINDOW_PIPELINED (a
// measurement build with counters): about 66 fps with 3 buffers, 99% of the
// frame intervals within 14-18 ms.  After that fix the motion looked very clean
// by eye; fps and intervals were not read.  Not measured: HDMI output, other panels, runs longer than a few
// minutes, tearing (none seen on 2 video frames; the video cannot exclude it),
// the cost of a resync on the frame it happens in, the panel's real refresh
// rate (if it is not the 14.8 ms SET_VSYNC reports, a buffer is given back up
// to the difference too early: issue #10), the QPainter path, the V3D clock
// (the board layer of YakoGL raises it to the maximum, this glue does not).

#include <zephyr/kernel.h>
#include <EGL/eglplatform.h>   // struct gles_native_window, GLES_NATIVE_FORMAT_* (YakoGL)
#include <errno.h>
#include <stdint.h>
#include <string.h>

extern "C" {
#include "rpi_mbox.h"          // YakoGL driver/zephyr/lib/board/rpi4
#include "osal_internal.h"     // YakoGL src/osal: struct osal_zephyr_v3d_init
}

// printk, not LOG_*: the board's settings do not turn CONFIG_LOG on, and these
// lines (one at start, one on an error) are what tells a dark screen apart.
#define QZ_PRINT(fmt, ...) printk("qzephyr_rpi4: " fmt "\n", ##__VA_ARGS__)

namespace {

// QImage::Format_RGBX8888 (qimage.h, Qt 6.11): R, G, B, X in memory, which
// is what the firmware's RGB pixel order gives (GLES_NATIVE_FORMAT_RGBA8888).
constexpr int QtFmt_RGBX8888 = 16;

constexpr int FB_W = CONFIG_QT_RPI4_WIDTH;
constexpr int FB_H = CONFIG_QT_RPI4_HEIGHT;
constexpr int FB_COUNT = CONFIG_QT_SCANOUT_BUFFERS;

// A resync is one SET_VSYNC, which can take a period or two: the frame it
// happens in is late.  The phase drifted by about 17 us per second when
// measured at 1 s intervals, and the period is corrected at each resync.
constexpr int64_t RESYNC_MS = 30000;
// The first resync comes early: it is accepted only when the phase error is
// within P/10, and the error grows with the time since the start-up
// calibration, whose period is one interval's reading.  It happens at the
// first frame wait 2 s or more after the calibration (a slow start shrinks the
// tolerance: at 10 s about 148 ppm).  After 2 s a period error of up to about
// 740 ppm is inside the window; after 30 s only about 49 ppm would be (host model, not measured on the board; the board read
// about 17 ppm).  A rejected resync changes nothing and prints a line, so a
// start that is out of the window is visible, and it would not recover.
constexpr int64_t FIRST_RESYNC_MS = 2000;

// The period measured at start must lie in [PERIOD_MIN_US, PERIOD_MAX_US] to
// be used.
constexpr uint64_t PERIOD_MIN_US = 5000;
constexpr uint64_t PERIOD_MAX_US = 50000;
constexpr uint64_t NOMINAL_PERIOD_US = 16667;

K_MUTEX_DEFINE(s_lock);

struct rpi_fb s_fb;
bool s_tried;
bool s_ready;
int s_shown;             // the buffer on screen (or about to be)

// The vsync counter: s_edge is the cycle count at which the last SET_VSYNC
// returned (a real edge), s_edge_count the counter then, s_period the cycles
// per edge.
uint64_t s_period;
uint64_t s_edge;
unsigned int s_edge_count;
int64_t s_last_sync_ms;
bool s_resync_off;       // SET_VSYNC is not used any more (it failed, or no period)
bool s_resync_reported;  // the first resync is printed, later ones only when odd
int s_pan_failures;

inline uint64_t us(uint64_t cycles)
{
    return cycles * 1000000ull / sys_clock_hw_cycles_per_sec();
}

inline uint64_t cycles_from_us(uint64_t usec)
{
    return usec * sys_clock_hw_cycles_per_sec() / 1000000ull;
}

unsigned int vsync_now()
{
    if (!s_period)   // before init() has measured it
        return s_edge_count;
    return s_edge_count + unsigned((k_cycle_get_64() - s_edge) / s_period);
}

// Measure the edge period: 17 SET_VSYNC calls back to back, the median of the
// 16 intervals.  Leaves s_edge at the last return.  Falls
// back to the nominal period, without any further SET_VSYNC, when the calls
// fail or the median is not a plausible period.
void calibrate()
{
    uint64_t iv[16], prev = 0;
    int n = 0;
    for (int i = 0; i < 17; ++i) {
        if (rpi_fb_wait_vsync(nullptr))
            break;
        const uint64_t now = k_cycle_get_64();
        if (prev)
            iv[n++] = now - prev;
        prev = now;
        s_edge = now;
    }
    uint64_t median = 0;
    if (n >= 8) {
        for (int i = 1; i < n; ++i) {   // insertion sort
            const uint64_t v = iv[i];
            int j = i - 1;
            while (j >= 0 && iv[j] > v) {
                iv[j + 1] = iv[j];
                --j;
            }
            iv[j + 1] = v;
        }
        median = iv[n / 2];
    }
    if (n < 8 || us(median) < PERIOD_MIN_US || us(median) > PERIOD_MAX_US) {
        QZ_PRINT("vsync period not usable (%d intervals, median %u us): pacing to %u us, no SET_VSYNC",
                 n, (unsigned)us(median), (unsigned)NOMINAL_PERIOD_US);
        s_period = cycles_from_us(NOMINAL_PERIOD_US);
        s_edge = k_cycle_get_64();
        s_resync_off = true;
    } else {
        s_period = median;
        QZ_PRINT("vsync period %u us (median of %d)", (unsigned)us(s_period), n);
    }
    s_last_sync_ms = k_uptime_get() - (RESYNC_MS - FIRST_RESYNC_MS);
}

bool init()
{
    if (s_tried)
        return s_ready;
    s_tried = true;

    k_mutex_lock(&s_lock, K_FOREVER);
    int rc = rpi_mbox_init();
    if (rc) {
        k_mutex_unlock(&s_lock);
        QZ_PRINT("mailbox init failed: %d", rc);
        return false;
    }
    uint32_t got[4] = {};
    rc = rpi_fb_alloc_virtual(FB_W, FB_H, uint32_t(FB_H) * uint32_t(FB_COUNT), &s_fb, got);
    if (rc) {
        k_mutex_unlock(&s_lock);
        QZ_PRINT("framebuffer %dx%d (%d rows in memory) refused: %d (granted %ux%u, virtual %ux%u)",
                 FB_W, FB_H, FB_H * FB_COUNT, rc, got[0], got[1], got[2], got[3]);
        return false;
    }
    if (s_fb.pitch != uint32_t(FB_W) * 4u) {
        k_mutex_unlock(&s_lock);
        // YakoGL takes only tightly packed rows for a window.
        QZ_PRINT("framebuffer pitch %u is not %d * 4: no GL window", s_fb.pitch, FB_W);
        return false;
    }
    // Bit 31 of the tag's code word: the firmware answered it; without it
    // `order` is just the 1 that was asked for.
    if (!(s_fb.order_code & 0x80000000u) || s_fb.order != 1u) {
        k_mutex_unlock(&s_lock);
        QZ_PRINT("pixel order: code 0x%08x, order %u: not an answered RGB", s_fb.order_code,
                 s_fb.order);
        return false;
    }
    memset(s_fb.cpu, 0, s_fb.size);
    s_ready = true;
    calibrate();
    k_mutex_unlock(&s_lock);
    QZ_PRINT("framebuffer %ux%u, %d buffer(s), pitch %u, phys 0x%08x, RGB order (glue built %s %s)",
             s_fb.width, s_fb.height, FB_COUNT, s_fb.pitch, s_fb.phys, __DATE__, __TIME__);
    return true;
}

// Show buffer `index` from the next vsync on.
int show(int index)
{
    k_mutex_lock(&s_lock, K_FOREVER);
    uint32_t echo = 0;
    const int rc = rpi_fb_pan(uint32_t(index) * uint32_t(FB_H), &echo, nullptr);
    k_mutex_unlock(&s_lock);
    if (rc) {
        if (s_pan_failures++ == 0)   // once: a dead mailbox would print every frame
            QZ_PRINT("pan to buffer %d failed: %d (further failures not printed)", index, rc);
    } else {
        s_shown = index;
    }
    return rc;
}

// The V3D OSAL asks for its clock through this (eglGetDisplay with no
// argument): YakoGL's board layer defines it; the board layer is not in this
// image.
int v3d_clock_set(void *ctx, bool on)
{
    ARG_UNUSED(ctx);
    k_mutex_lock(&s_lock, K_FOREVER);
    const int rc = rpi_clock_state(RPI_FW_CLK_V3D, on);
    k_mutex_unlock(&s_lock);
    return rc;
}

const struct osal_zephyr_v3d_init s_v3d_platform = { v3d_clock_set, nullptr };

// One SET_VSYNC, to check the prediction against a real edge.  SET_VSYNC
// returns k whole periods after the predicted edge (k = 1 in the 8 resyncs
// read with an earlier build, 1 s apart, and in the first resync of the later builds
// on the board); what is left, the
// phase error `ep`, corrects the period: the edges since the last anchor were
// N periods of the true length.  The anchor moves to the return of SET_VSYNC,
// which is edge number before + 1 + k of the old numbering (so the counter
// does not go back: the call starts at or after predicted edge `before`, which
// makes k >= -1), and only when `ep` is small; a large error means the
// prediction or the answer is not what this model assumes, and neither the
// anchor nor the period changes.  A failed SET_VSYNC ends resyncing for good.
void resync()
{
    k_mutex_lock(&s_lock, K_FOREVER);
    const unsigned int before = vsync_now();
    const uint64_t predicted = s_edge + uint64_t(before + 1 - s_edge_count) * s_period;
    const int rc = rpi_fb_wait_vsync(nullptr);
    const uint64_t t1 = k_cycle_get_64();
    k_mutex_unlock(&s_lock);
    s_last_sync_ms = k_uptime_get();
    if (rc) {
        s_resync_off = true;
        QZ_PRINT("vsync resync failed: %d; no more SET_VSYNC, the prediction stays", rc);
        return;
    }

    const int64_t P = int64_t(s_period);
    const int64_t e = int64_t(t1) - int64_t(predicted);
    const int64_t k = (e + (e >= 0 ? P / 2 : -P / 2)) / P;
    const int64_t ep = e - k * P;
    const int64_t N = int64_t(before + 1 - s_edge_count) + k;
    const int64_t ep_us = ep >= 0 ? int64_t(us(uint64_t(ep))) : -int64_t(us(uint64_t(-ep)));
    const bool ok = ep > -P / 10 && ep < P / 10;
    if (ok) {
        if (N >= 30)
            s_period = uint64_t(P + ep / N);
        s_edge_count = before + 1 + unsigned(k);
        s_edge = t1;
    }
    if (!ok || !s_resync_reported) {
        s_resync_reported = true;
        QZ_PRINT("vsync resync: k=%d, phase error %d us over %d periods, period %u us%s", int(k),
                 int(ep_us), int(N), (unsigned)us(s_period), ok ? "" : " (anchor and period kept)");
    }
}

} // namespace

extern "C" const struct osal_zephyr_v3d_init *osal_zephyr_v3d_default_platform(void)
{
    return &s_v3d_platform;
}

// ---- QZephyrScreen / raster backing store hooks --------------------------

extern "C" bool qzephyr_display_query_caps(int *out_w, int *out_h, int *out_qfmt)
{
    if (!init())
        return false;
    *out_w = FB_W;
    *out_h = FB_H;
    *out_qfmt = QtFmt_RGBX8888;
    return true;
}

// QPainter frame (QZephyrBackingStore::flush): the whole frame into the buffer
// that is not on screen, then flip to it.
extern "C" void qzephyr_display_write(int x, int y, int w, int h, int pitch_bytes,
                                      int bytes_per_pixel, const void *buf)
{
    if (!s_ready || bytes_per_pixel != 4 || w <= 0 || h <= 0 || x < 0 || y < 0
        || x + w > FB_W || y + h > FB_H)
        return;
    const int target = FB_COUNT > 1 ? (s_shown + 1) % FB_COUNT : 0;
    uint8_t *dst = s_fb.cpu + size_t(target) * size_t(FB_H) * s_fb.pitch;
    for (int row = 0; row < h; ++row) {
        memcpy(dst + size_t(y + row) * s_fb.pitch + size_t(x) * 4u,
               static_cast<const uint8_t *>(buf) + size_t(row) * size_t(pitch_bytes),
               size_t(w) * 4u);
    }
    show(target);
}

// ---- OpenGL: scan-out buffers the V3D renders into -----------------------

extern "C" int qzephyr_gl_buffer_count(void)
{
    return init() ? FB_COUNT : 0;
}

extern "C" bool qzephyr_gl_native_window(int index, struct gles_native_window *out)
{
    if (!init() || index < 0 || index >= FB_COUNT || !out)
        return false;
    out->phys = uint64_t(s_fb.phys) + uint64_t(index) * uint64_t(FB_H) * s_fb.pitch;
    out->width = FB_W;
    out->height = FB_H;
    out->stride_bytes = s_fb.pitch;
    // PIPELINED: this glue (through the QPA) double-buffers the window itself
    // and shows buffers with qzephyr_gl_present.  Without the flag YakoGL
    // double-buffers each window in a ring of its own, because the V3D OSAL
    // always registers a present hook (which here returns ENOTSUP: the
    // "display refused the window's buffer: -134" line in the log), and then
    // only some of the frames reach the buffer that is on screen
    // (read in YakoGL's egl.rs and eglplatform.h; not measured on the board).
    out->format = GLES_NATIVE_FORMAT_RGBA8888 | GLES_NATIVE_WINDOW_PIPELINED;
    return true;
}

extern "C" void qzephyr_gl_present(int index)
{
    if (s_ready && index >= 0 && index < FB_COUNT)
        show(index);
}

extern "C" unsigned int qzephyr_gl_vsync_count(void)
{
    return vsync_now();
}

// Block until the vsync counter reaches `count`: sleep to the predicted edge
// (see the header for why not one SET_VSYNC per wait), after a resync if one
// is due.  The QPA asks for the next edge (the swap throttle) or for the one
// after a buffer's release stamp, so the wait is one period or two; a count
// far in the future would simply be waited for, in pieces of 100 ms.
extern "C" void qzephyr_gl_wait_vsync(unsigned int count)
{
    if (!s_ready || int(vsync_now() - count) >= 0)
        return;
    if (!s_resync_off && k_uptime_get() - s_last_sync_ms >= RESYNC_MS)
        resync();
    // The last 100 us or so are spun (the kernel tick is 100 us).
    while (int(vsync_now() - count) < 0) {
        const uint64_t at = s_edge + uint64_t(count - s_edge_count) * s_period;
        const uint64_t now = k_cycle_get_64();
        if (now >= at)
            break;
        const uint64_t wait_us = us(at - now);
        if (wait_us > 200)
            k_usleep(int32_t(wait_us > 100000 ? 100000 : wait_us - 100));
    }
}
