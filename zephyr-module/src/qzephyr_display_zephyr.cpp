// Stage 2 strong definitions of the weak qzephyr_display_* hooks that
// qzephyrbackingstore.cpp / qzephyrscreen.cpp call.  Lives in the Zephyr
// app build so it can include <zephyr/drivers/display.h>.
//
// Tier 3-2 / 3-3 design notes:
//   - qzephyr_display_query_caps() runs once during QZephyrScreen ctor
//     (inside QGuiApplication construction).  Opens the chosen display
//     device, reads caps, optionally forces RGB565 if needed, and
//     turns the backlight on.
//   - qzephyr_display_write() runs every QBackingStore::flush().
//     Reuses one static display_buffer_descriptor sized to the dirty
//     rect; QImage's scanlines go to the LCDIF driver verbatim.
//
// RK055HDMIPI4MA0 quirk: the panel driver reports BGR_565 capability
// but expects little-endian RGB565 bytes (see Zephyr issue #53642).
// Treat both BGR_565 and RGB_565 as QImage::Format_RGB16 without
// byte-swap; Slint's printerdemo does the same.

#include <zephyr/kernel.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>

// Pull in the Zephyr-runtime event dispatcher class so we can `new` it
// in qzephyr_make_event_dispatcher() below.  The header lives in
// qtbase's source tree (it is not installed -- it's only needed by the
// per-board Zephyr glue that lives in this module).
#include "../../qtbase/src/corelib/kernel/qeventdispatcher_zephyr_p.h"

LOG_MODULE_REGISTER(qzephyr_display, LOG_LEVEL_INF);

// QImage::Format enum values we need to surface to Stage 1.  Hardcoded
// to match QtGui's qimage.h on Qt 6.11; sync if Qt renumbers them.
enum QtImageFormat {
    QtFmt_Invalid = 0,
    QtFmt_RGB32 = 4,                  // 0xff RR GG BB
    QtFmt_ARGB32 = 5,
    QtFmt_ARGB32_Premultiplied = 6,
    QtFmt_RGB16 = 7,                  // 565
    QtFmt_RGB888 = 13,
};

static const struct device *s_display = nullptr;
static struct display_capabilities s_caps;
static struct display_buffer_descriptor s_desc;
static bool s_display_ready = false;

extern "C" bool qzephyr_display_query_caps(int *out_w, int *out_h, int *out_qfmt)
{
    if (!s_display) {
        s_display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
    }
    if (!device_is_ready(s_display)) {
        LOG_ERR("zephyr,display chosen device is not ready");
        return false;
    }

    display_get_capabilities(s_display, &s_caps);
    LOG_INF("display %ux%u, format=%u supported=%#x",
            s_caps.x_resolution, s_caps.y_resolution,
            s_caps.current_pixel_format, s_caps.supported_pixel_formats);

    enum display_pixel_format active = s_caps.current_pixel_format;
    if (active != PIXEL_FORMAT_RGB_565 && active != PIXEL_FORMAT_RGB_565X) {
        if (s_caps.supported_pixel_formats & PIXEL_FORMAT_RGB_565) {
            if (display_set_pixel_format(s_display, PIXEL_FORMAT_RGB_565) == 0) {
                active = PIXEL_FORMAT_RGB_565;
                LOG_INF("switched display to PIXEL_FORMAT_RGB_565");
            }
        }
    }

    int qfmt;
    switch (active) {
    case PIXEL_FORMAT_RGB_565:
    case PIXEL_FORMAT_RGB_565X:   // RK055 / byte-swapped 565 -- treat as 565 LE
        qfmt = QtFmt_RGB16;
        break;
    case PIXEL_FORMAT_RGB_888:
        qfmt = QtFmt_RGB888;
        break;
    case PIXEL_FORMAT_ARGB_8888:
        qfmt = QtFmt_ARGB32_Premultiplied;
        break;
    default:
        LOG_WRN("unsupported pixel format %u, defaulting to RGB16", active);
        qfmt = QtFmt_RGB16;
        break;
    }

    // With CONFIG_MCUX_ELCDIF_PXP_ROTATE_90 / _270 the driver still
    // reports panelWidth x panelHeight (720 x 1280) in caps, but PXP
    // rotates the source 90/270 degrees during DMA -- so the source
    // buffer Qt should produce is 1280 x 720.  Same idea for 180 (no
    // swap needed, dimensions identical) and the no-rotation case.
#if defined(CONFIG_MCUX_ELCDIF_PXP_ROTATE_90) || defined(CONFIG_MCUX_ELCDIF_PXP_ROTATE_270)
    *out_w = s_caps.y_resolution;
    *out_h = s_caps.x_resolution;
#else
    *out_w = s_caps.x_resolution;
    *out_h = s_caps.y_resolution;
#endif
    *out_qfmt = qfmt;
    s_display_ready = true;

    display_blanking_off(s_display);
    return true;
}

// Strong def of the weak hook in qzephyrintegration.cpp.  Called once
// during QPlatformIntegration::createEventDispatcher() while
// QGuiApplication is being constructed.  We hand it a freshly-allocated
// QEventDispatcherZephyr; QPlatformIntegration takes ownership.
extern "C" QAbstractEventDispatcher *qzephyr_make_event_dispatcher()
{
    return new QEventDispatcherZephyr();
}

extern "C" void qzephyr_display_write(int x, int y, int w, int h,
                                       int pitch_bytes, int bytes_per_pixel,
                                       const void *buf)
{
    static uint32_t call_count = 0;
    ++call_count;

    if (!s_display_ready) {
        if (call_count <= 3)
            printk("[qzephyr_display_write] s_display_ready=false (call %u)\n", call_count);
        return;
    }
    if (w <= 0 || h <= 0) {
        if (call_count <= 3)
            printk("[qzephyr_display_write] empty rect %dx%d (call %u)\n", w, h, call_count);
        return;
    }

    s_desc.buf_size = static_cast<uint32_t>(h * pitch_bytes);
    s_desc.width = static_cast<uint16_t>(w);
    s_desc.height = static_cast<uint16_t>(h);
    // Zephyr's `pitch` field is in PIXELS, not bytes.
    s_desc.pitch = static_cast<uint16_t>(pitch_bytes / bytes_per_pixel);
    s_desc.frame_incomplete = false;

    const int ret = display_write(s_display,
                                  static_cast<uint16_t>(x),
                                  static_cast<uint16_t>(y),
                                  &s_desc, buf);
    if (ret != 0)
        LOG_WRN("display_write(%d,%d %dx%d) failed: %d", x, y, w, h, ret);
}
