// Display glue for the i.MX8M Plus boards (Toradex Verdin iMX8M Plus, Atmark
// Armadillo-X2): the GPU driver repository's imx8mp_display, which either owns
// the Verdin's LCDIF -> MIPI DSIM -> SN65DSI84 chain (bringing it up from cold
// when nothing is scanning) or adopts an LCDIF the previous kernel left
// running (the X2's LCDIF3 -> HDMI). The A53 image drives the panel directly;
// the SafeUI Cortex-M7 is not involved.
//
// The i.MX8MP sibling of qzephyr_display_dss.cpp, with the same QPA hooks, so
// the qzephyr platform plugin needs no change:
//
//   - QPainter (QtWidgets, the raster backing store): flush() copies the
//     frame into the next buffer and flips (qzephyr_display_write).
//   - OpenGL ES (CONFIG_QT_OPENGL, milestone 2): EGL window surfaces over the
//     same buffers (qzephyr_gl_*).
//
// Qt owns CONFIG_QT_SCANOUT_BUFFERS XRGB8888 buffers in the image (.noinit).
// Zephyr maps its RAM identity on the A53 (VA == PA), so a buffer's address is
// its physical address for the LCDIF's DMA. imx8mp_display_set_fb() waits
// for the LCDIF to latch a flip, which is the vsync the QPA needs.

#include <zephyr/kernel.h>
#include <zephyr/cache.h>
#include <zephyr/logging/log.h>
extern "C" {
#include <imx8mp_display.h>   // a C header without extern "C" guards
}
#include <EGL/eglplatform.h>   // struct gles_native_window, GLES_NATIVE_FORMAT_* (YakoGL)
#include <errno.h>
#include <stdint.h>
#include <string.h>

LOG_MODULE_REGISTER(qzephyr_imx8mp, LOG_LEVEL_INF);

namespace {

// QImage::Format values (qimage.h, Qt 6.11); the QPA takes an int.
constexpr int QtFmt_RGB32 = 4;   // 0xffRRGGBB, what the LCDIF's XRGB8888 layer scans out

constexpr int FB_W = CONFIG_QT_IMX8MP_WIDTH;
constexpr int FB_H = CONFIG_QT_IMX8MP_HEIGHT;
constexpr int FB_COUNT = CONFIG_QT_SCANOUT_BUFFERS;
constexpr uint32_t FB_STRIDE = FB_W * 4;
constexpr size_t FB_BYTES = size_t(FB_STRIDE) * FB_H;

uint8_t s_fb[FB_COUNT][FB_BYTES] __noinit __aligned(4096);
bool s_on;
int s_raster_next;          // raster path: the buffer the next flush is copied into
unsigned int s_latched;     // flips the LCDIF has latched: the QPA's vsync count

// The panel the devicetree describes must be the one the buffers were sized
// for, or every frame lands at the wrong pitch.
bool geometry_ok()
{
    uint32_t w = 0, h = 0;

    imx8mp_display_geometry(&w, &h);
    if (w != uint32_t(FB_W) || h != uint32_t(FB_H)) {
        LOG_ERR("the panel is %ux%u, the buffers were built for %dx%d "
                "(CONFIG_QT_IMX8MP_WIDTH/HEIGHT)", w, h, FB_W, FB_H);
        return false;
    }
    return true;
}

// Put buffer `index` on the panel. The first call brings the display up (or
// adopts it); later ones flip and wait for the latch.
int show(int index)
{
    const uintptr_t pa = reinterpret_cast<uintptr_t>(s_fb[index]);
    int rc;

    if (!s_on) {
        rc = imx8mp_display_on(pa, FB_STRIDE);
        if (rc) {
            LOG_ERR("imx8mp_display_on: %d", rc);
            return rc;
        }
        s_on = true;
        LOG_INF("display on: %dx%d, %s", FB_W, FB_H,
                imx8mp_display_info()->adopted ? "adopted the running chain"
                                               : "brought up from cold");
    } else {
        rc = imx8mp_display_set_fb(pa);
        if (rc) {
            LOG_WRN("flip to buffer %d: %d", index, rc);
            return rc;
        }
    }
    s_latched++;
    return 0;
}

} // namespace

// ---- QZephyrScreen / raster backing store hooks --------------------------

extern "C" bool qzephyr_display_query_caps(int *out_w, int *out_h, int *out_qfmt)
{
    if (!geometry_ok())
        return false;
    *out_w = FB_W;
    *out_h = FB_H;
    *out_qfmt = QtFmt_RGB32;
    return true;
}

// QPainter frame (QZephyrBackingStore::flush): copy into the next buffer,
// clean it to DDR (the LCDIF reads by DMA and the buffers are cached), flip.
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
            LOG_ERR("raster frame is %d bytes/pixel; the layer is XRGB8888 (4)",
                    bytes_per_pixel);
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

// ---- OpenGL ES swap-chain hooks (CONFIG_QT_OPENGL, milestone 2) ----------

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

// imx8mp_display_set_fb() returns once the LCDIF has latched the flip, so the
// count of latched flips is the vsync count the QPA asks for: a buffer
// flipped away from is no longer being scanned once the next flip latched.
extern "C" unsigned int qzephyr_gl_vsync_count(void)
{
    return s_latched;
}

extern "C" void qzephyr_gl_wait_vsync(unsigned int count)
{
    // Nothing to wait for that show() has not already waited for; a count
    // ahead of the flips made means the caller is waiting for a flip it has
    // not asked for yet, and blocking would only stall the UI.
    ARG_UNUSED(count);
}

extern "C" void qzephyr_gl_debug_set(int on)
{
    ARG_UNUSED(on);
}
