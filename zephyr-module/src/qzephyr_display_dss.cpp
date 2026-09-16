// Stage 2 display glue for the Toradex Verdin AM62P: the SafeUI AM62P
// DSS driver's application plane (slint-safe-ui-zephyr-verdin-am62p,
// <safeui/am62p_dss.h>), which the DSI panel scans out.
//
// Qt owns CONFIG_QT_SCANOUT_BUFFERS full-panel XRGB8888 buffers in the
// image (.noinit) and flips the application plane to whichever one holds
// the latest complete frame.  Two producers use them:
//
//   - QPainter (QtWidgets, the raster backing store): flush() copies the
//     frame into the next buffer and flips (qzephyr_display_write).
//   - OpenGL ES (CONFIG_QT_OPENGL, QtQuick through YakoGL): the qzephyr
//     QPA creates one EGL window surface per buffer straight over its
//     physical address, the GPU renders into it, and the QPA flips after
//     the frame completes (qzephyr_gl_*).  The buffer rotation and the
//     "do not render into a buffer the display is still reading" rule
//     are the QPA's (qtbase/src/plugins/platforms/zephyr/qzephyrwindow.cpp,
//     mirroring the GPU driver repository's samples/zephyr/gl_slint).
//
// Zephyr on the A53 maps RAM identity (VA == PA), so a buffer's address
// is its physical address for the GPU MMU and the DSS DMA, the same way
// the gl_slint sample does it.

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/cache.h>
#include <zephyr/logging/log.h>
extern "C" {
#include <safeui/am62p_dss.h>   // a C header without extern "C" guards
}
#include <EGL/eglplatform.h>   // struct gles_native_window, GLES_NATIVE_FORMAT_* (YakoGL)
#ifdef CONFIG_QT_DEBUG_LOG
#include "pvr_backend.h"       // YakoGL's PowerVR backend options (private header, see zephyr/CMakeLists.txt)
#endif
#include <errno.h>
#include <stdint.h>
#include <string.h>

LOG_MODULE_REGISTER(qzephyr_dss, LOG_LEVEL_INF);

namespace {

// QImage::Format values (qimage.h, Qt 6.11); the QPA takes an int.
constexpr int QtFmt_RGB32 = 4;   // 0xffRRGGBB, what the DSS XRGB8888 plane scans out

constexpr int FB_W = CONFIG_QT_DSS_WIDTH;
constexpr int FB_H = CONFIG_QT_DSS_HEIGHT;
constexpr int FB_COUNT = CONFIG_QT_SCANOUT_BUFFERS;
constexpr uint32_t FB_STRIDE = FB_W * 4;
constexpr size_t FB_BYTES = size_t(FB_STRIDE) * FB_H;

uint8_t s_fb[FB_COUNT][FB_BYTES] __noinit __aligned(4096);
bool s_plane_on;
int s_raster_next;   // raster path: the buffer the next flush is copied into

const struct device *dss()
{
    return DEVICE_DT_GET(DT_NODELABEL(dss1));
}

// Put buffer `index` on the application plane; the flip takes effect at
// the next vertical sync.  The first call enables the plane and output.
int show(int index)
{
    const struct device *dev = dss();
    struct am62p_dss_plane_cfg plane = {};
    plane.pa = reinterpret_cast<uintptr_t>(s_fb[index]);
    plane.width = FB_W;
    plane.height = FB_H;
    plane.stride_bytes = FB_STRIDE;
    plane.format = AM62P_DSS_FMT_XRGB8888;

    if (!device_is_ready(dev)) {
        LOG_ERR("dss1 not ready");
        return -ENODEV;
    }
    int rc = am62p_dss_set_app_buffer(dev, &plane);
    if (rc == 0 && !s_plane_on) {
        rc = am62p_dss_enable_app_plane(dev, true);
        if (rc == 0)
            rc = am62p_dss_output_enable(dev, true);
        s_plane_on = (rc == 0);
        if (rc == 0)
            LOG_INF("application plane on: %dx%d XRGB8888, %d scan-out buffer(s)", FB_W, FB_H, FB_COUNT);
    }
    if (rc)
        LOG_WRN("DSS flip to buffer %d failed: %d", index, rc);
    return rc;
}

} // namespace

// ---- QZephyrScreen / raster backing store hooks --------------------------

extern "C" bool qzephyr_display_query_caps(int *out_w, int *out_h, int *out_qfmt)
{
    if (!device_is_ready(dss())) {
        LOG_ERR("dss1 not ready; no display");
        return false;
    }
    *out_w = FB_W;
    *out_h = FB_H;
    *out_qfmt = QtFmt_RGB32;
    return true;
}

// QPainter frame (QZephyrBackingStore::flush): whole-frame copy into the
// next scan-out buffer, then flip.  The buffers are cached memory the
// DSS reads by DMA, so the copy is cleaned to DDR first.
extern "C" void qzephyr_display_write(int x, int y, int w, int h,
                                      int pitch_bytes, int bytes_per_pixel,
                                      const void *buf)
{
    if (w <= 0 || h <= 0 || !buf)
        return;
    if (bytes_per_pixel != 4) {
        static bool warned;
        if (!warned) {
            warned = true;
            LOG_ERR("raster frame is %d bytes/pixel; the DSS plane is XRGB8888 (4)", bytes_per_pixel);
        }
        return;
    }
    const int cols = MIN(w, FB_W - x);
    const int rows = MIN(h, FB_H - y);
    if (cols <= 0 || rows <= 0)
        return;

    uint8_t *dst = s_fb[s_raster_next];
    const uint8_t *src = static_cast<const uint8_t *>(buf);
    for (int r = 0; r < rows; ++r) {
        memcpy(dst + size_t(y + r) * FB_STRIDE + size_t(x) * 4,
               src + size_t(r) * pitch_bytes, size_t(cols) * 4);
    }
    sys_cache_data_flush_range(dst, FB_BYTES);
    if (show(s_raster_next) == 0)
        s_raster_next = (s_raster_next + 1) % FB_COUNT;
}

// ---- OpenGL ES swap-chain hooks (CONFIG_QT_OPENGL) -------------------------

extern "C" int qzephyr_gl_buffer_count(void)
{
    return FB_COUNT;
}

extern "C" bool qzephyr_gl_native_window(int index, struct gles_native_window *out)
{
    if (index < 0 || index >= FB_COUNT || !out)
        return false;
    out->phys = reinterpret_cast<uintptr_t>(s_fb[index]);
    out->width = FB_W;
    out->height = FB_H;
    out->stride_bytes = FB_STRIDE;
    out->format = GLES_NATIVE_FORMAT_XRGB8888;
    return true;
}

extern "C" void qzephyr_gl_present(int index)
{
    if (index >= 0 && index < FB_COUNT)
        show(index);
}

extern "C" unsigned int qzephyr_gl_vsync_count(void)
{
    struct am62p_dss_stats st;
    const struct device *dev = dss();
    if (!device_is_ready(dev))
        return 0;
    am62p_dss_get_stats(dev, &st);
    return st.vsyncs;
}

// Block until the display's vsync counter has reached `count` (a buffer
// flipped away is scanned out until the next sync latches the flip).
extern "C" void qzephyr_gl_wait_vsync(unsigned int count)
{
    if (!device_is_ready(dss()))
        return;
    int guard = 0;
    while (int(qzephyr_gl_vsync_count() - count) < 0) {
        k_busy_wait(200);
        if (++guard > 500) {   // > 100 ms: the display is not syncing; do not hang the UI
            LOG_WRN("vsync %u not reached (at %u); continuing", count, qzephyr_gl_vsync_count());
            return;
        }
    }
}

// Debug aid (CONFIG_QT_DEBUG_LOG): the QPA switches the backend's per-draw
// command-stream dump on for the first frames and off again, so the console
// sees what each draw carried without being flooded at frame rate.
extern "C" void qzephyr_gl_debug_set(int on)
{
#ifdef CONFIG_QT_DEBUG_LOG
    pvr_backend_set_option(PVR_BACKEND_OPT_DEBUG, on ? 1u : 0u);
#else
    ARG_UNUSED(on);
#endif
}
