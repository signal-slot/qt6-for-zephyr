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
#include <zephyr/sys/sys_heap.h>   // sys_memory_stats -- heap-leak diagnostic
#ifdef CONFIG_CORTEX_M_DEBUG_MONITOR_HOOK
#include <cmsis_core.h>            // CoreDebug / DWT -- heap-corruption watchpoint
#endif

// Zephyr common-libc malloc heap stats accessor (lib/libc/common malloc.c,
// gated by CONFIG_SYS_HEAP_RUNTIME_STATS).  Used to watch heap usage climb
// toward the freeze.  Avoids newlib mallinfo() (which multiply-defines
// malloc/free against Zephyr's allocator).
extern "C" int malloc_runtime_stats_get(struct sys_memory_stats *stats);

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
#if defined(CONFIG_MCUX_ELCDIF_PXP_ROTATE_90) || defined(CONFIG_MCUX_ELCDIF_PXP_ROTATE_270) \
        || defined(CONFIG_QT_VGLITE_DISPLAY_ROTATE) || defined(CONFIG_QT_PXP_COMPOSITE) \
        || defined(CONFIG_QT_EXTERNAL_ROTATION)
    // PXP rotate-90/270 OR our GC355 rotation offload: the panel is portrait
    // (720x1280) but Qt should render landscape (1280x720); we rotate on
    // present, so report the swapped (landscape) geometry to QZephyrScreen.
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

// Runtime liveness hooks called from the bs.flush heartbeat in
// qzephyrbackingstore.cpp.  Heap stats via mallinfo() pulled in
// nano-malloc.c.o which collided with Zephyr's libc__common malloc
// (multiple definition of `malloc'/`free`).  Until we wire up
// `sys_heap_runtime_stats_get` against `_system_heap`, just return -1
// for heap stats so the heartbeat falls back to "-1K".
extern "C" int qzephyr_heap_free_bytes()    { return -1; }
extern "C" int qzephyr_heap_alloc_bytes()   { return -1; }
extern "C" int qzephyr_input_queue_used()   { return -1; }

// Stack high-water mark hook -- DISABLED.  k_thread_stack_space_get()
// faults with a precise data bus error (BFAR=0x53100000) when the
// thread's stack_info isn't fully populated, which is the case in
// this build because we don't enable CONFIG_INIT_STACKS=y (that fills
// the stack with a sentinel pattern at thread creation time, which
// stack_space_get walks looking for the high-water mark).  Until
// CONFIG_INIT_STACKS is added, return -1 so the heartbeat just shows
// stk_free=-1K and we know the metric is unavailable.
//
// CONFIRMED ON HARDWARE 2026-05-15: the previous body of this
// function reliably crashed cm5_v9_runtime on RT1170-EVKB about
// 6 taps in (lr=0x302986cf, PC=0x30000406 inside k_thread accessor).
extern "C" int qzephyr_main_stack_free_bytes()
{
    return -1;
}

// ===== Freeze probe (diagnostic) =====
// A separate thread that, when display_write stops completing for >~4s
// (the observed freeze), dumps the ELCDIF frame-done interrupt status,
// CUR_BUF, CTRL and the NVIC enable/pending/active state for the ELCDIF
// IRQ.  This pinpoints *why* the frame-done k_sem is never given:
//   - NVIC pend=1 (IRQ fired but not serviced) => masked / priority starvation
//   - status CurFrameDone set but pend=0/en=0  => IRQ disabled by a prior ISR
//   - CUR_BUF not advancing / mismatch          => scan-out wedged
#define QZEPHYR_FREEZE_PROBE 0
#if QZEPHYR_FREEZE_PROBE
#include <fsl_elcdif.h>
#include <fsl_pxp.h>
#include <zephyr/devicetree.h>
#define QZ_DISP_NODE DT_CHOSEN(zephyr_display)
static volatile uint32_t s_dw_completions;
// Defined in qsgvgliterenderloop.cpp: continuous-animation update counters.
extern volatile uint32_t qz_mu_count;
extern volatile uint32_t qz_hu_count;
static void qzephyr_freeze_probe(void *, void *, void *)
{
    uint32_t last = 0;
    int stall = 0;
    for (;;) {
        k_msleep(2000);
        const uint32_t now = s_dw_completions;
        if (now == last && now > 0) {
            ++stall;
            // Dump ~5 times (2 s apart) across the stall so STAT/CTRL/NEXT
            // evolution tells static-idle (completed-no-IRQ / never-started)
            // apart from progressing (stalled mid-transfer).
            if (stall >= 2 && stall <= 6) {
                LCDIF_Type *base = reinterpret_cast<LCDIF_Type *>(DT_REG_ADDR(QZ_DISP_NODE));
                const unsigned irqn = DT_IRQN(QZ_DISP_NODE);
                printk("[freeze] dw_done=%u STALLED >4s mu=%u hu=%u: ELCDIF stat=0x%08x "
                       "CUR_BUF=0x%08x CTRL=0x%08x | NVIC irq=%u en=%d pend=%d act=%d\n",
                       now, (unsigned)qz_mu_count, (unsigned)qz_hu_count,
                       ELCDIF_GetInterruptStatus(base),
                       (unsigned)base->CUR_BUF, (unsigned)base->CTRL, irqn,
                       NVIC_GetEnableIRQ((IRQn_Type)irqn),
                       NVIC_GetPendingIRQ((IRQn_Type)irqn),
                       (int)NVIC_GetActive((IRQn_Type)irqn));
#if DT_NODE_HAS_PROP(QZ_DISP_NODE, nxp_pxp)
                // If main is wedged in the PXP rotate-DMA completion wait
                // (display_mcux_elcdif.c L207), the PXP IRQ state reveals it:
                // pend=1 => fired but not serviced (masked/starved); en=1 pend=0
                // => DMA never raised completion (errored / overrun / stuck).
                const unsigned pxp_irq = DT_IRQN(DT_PHANDLE(QZ_DISP_NODE, nxp_pxp));
                PXP_Type *pxp = reinterpret_cast<PXP_Type *>(
                        DT_REG_ADDR(DT_PHANDLE(QZ_DISP_NODE, nxp_pxp)));
                // STAT: bit0 IRQ (done), AXI write/read error bits (0x..),
                // CTRL bit0 ENABLE.  Busy => stuck mid-rotate; AXI err =>
                // bad source/dest address; IRQ set + NVIC pend=0 => done but
                // not delivered.
                printk("[freeze] #%d PXP irq=%u en=%d pend=%d act=%d | STAT=0x%08x CTRL=0x%08x "
                       "NEXT=0x%08x dw=%u\n",
                       stall, pxp_irq, NVIC_GetEnableIRQ((IRQn_Type)pxp_irq),
                       NVIC_GetPendingIRQ((IRQn_Type)pxp_irq),
                       (int)NVIC_GetActive((IRQn_Type)pxp_irq),
                       (unsigned)pxp->STAT, (unsigned)pxp->CTRL, (unsigned)pxp->NEXT, now);
#endif
            }
        } else {
            stall = 0;
        }
        last = now;
    }
}
K_THREAD_DEFINE(qz_freeze_probe, 2048, qzephyr_freeze_probe, NULL, NULL, NULL, 7, 0, 0);
#endif

#ifdef CONFIG_QT_VGLITE_DISPLAY_ROTATE
// ===== GC355 rotation offload (break the PXP 29fps ceiling) =====
// Instead of the ELCDIF PXP rotating the whole landscape frame on every present
// (~20 ms DMA, the 29 fps ceiling), the GC355 2D GPU rotate-blits Qt's rendered
// landscape buffer into a panel-native portrait buffer, which is then scanned
// out directly (PXP rotation OFF -> ~2.5 ms DMA).  Two __nocache portrait
// buffers are alternated so the GPU writes the back buffer while the LCDIF
// scans the front; display_write's frame-done wait gives the vsync sync, so the
// flip is tear-free.  Works for ANY Qt backend (software / widgets / vglite).
#include "vg_lite.h"
#include "qzephyr_vglite.h"
#include <zephyr/cache.h>

// Panel-native portrait double-buffer (max 720x1280 RGB565).
__nocache __aligned(64) static uint16_t s_rot_buf[2][1280 * 720];
static int s_rot_idx = 0;
static bool s_gpu_ready = false;

// Rotate-blit `buf` (landscape WxH) into portrait back buffer (HxW) and scan it
// out.  Returns true if it handled the present; false -> caller falls back to
// the plain (PXP) path (e.g. GPU not up yet).
static bool qzephyr_gpu_rotate_present(const void *buf, int w, int h, int pitch_bytes)
{
    if (!s_gpu_ready) {
        s_gpu_ready = (qzephyr_vglite_gpu_bringup(64, 64) == 0);
        if (!s_gpu_ready)
            return false;
    }
    if ((size_t)w * (size_t)h > 1280u * 720u)
        return false;

    // GC355 reads the source via AXI -> flush Qt's cached frame to SDRAM first.
    sys_cache_data_flush_range(const_cast<void *>(buf), (size_t)h * pitch_bytes);

    uint16_t *dst = s_rot_buf[s_rot_idx];
    vg_lite_buffer_t src = {};
    src.width = w;
    src.height = h;
    src.stride = pitch_bytes;
    src.format = VG_LITE_BGR565;
    src.memory = const_cast<void *>(buf);
    src.address = (vg_lite_uint32_t)(uintptr_t)buf;

    vg_lite_buffer_t tgt = {};
    tgt.width = h;                 // portrait width  = landscape height
    tgt.height = w;                // portrait height = landscape width
    tgt.stride = h * 2;
    tgt.format = VG_LITE_BGR565;
    tgt.memory = dst;
    tgt.address = (vg_lite_uint32_t)(uintptr_t)dst;

    // 90-degree rotation into the positive quadrant: rotate(90) then
    // translate(srcHeight, 0).  Matches the PXP rotate-90 orientation; flip to
    // 270 if the panel comes out upside-down.
    vg_lite_matrix_t m;
    vg_lite_identity(&m);
    vg_lite_translate((vg_lite_float_t)h, 0.0f, &m);
    vg_lite_rotate(90.0f, &m);

    if (vg_lite_blit(&tgt, &src, &m, VG_LITE_BLEND_NONE, 0, VG_LITE_FILTER_POINT) != VG_LITE_SUCCESS)
        return false;
    vg_lite_finish();

    struct display_buffer_descriptor d = {};
    d.width = h;                   // portrait dims
    d.height = w;
    d.pitch = h;
    d.buf_size = (uint32_t)w * (uint32_t)h * 2u;
    d.frame_incomplete = false;
    display_write(s_display, 0, 0, &d, dst);

    s_rot_idx ^= 1;
    return true;
}
#endif // CONFIG_QT_VGLITE_DISPLAY_ROTATE

#ifdef CONFIG_QT_PXP_COMPOSITE
// ===== PXP hardware compositor (Tier 9, Part B) =====
// Drive the ELCDIF PXP directly (fsl_pxp) to alpha-blend a background "process
// surface" (PS) with an "alpha surface" (AS = a Qt::WindowStaysOnTopHint overlay
// window) and rotate the result to the panel-native portrait orientation, in a
// single PXP pass.  Build with CONFIG_MCUX_ELCDIF_PXP=n so the ELCDIF driver
// does NOT also touch the PXP -- we own it; display_write then just DMAs our
// composited portrait buffer to the panel.  Sequence mirrors the proven
// dma_mcux_pxp.c config (rotate the *process surface*, output dims swapped).
#include <fsl_pxp.h>
#include <zephyr/cache.h>

__nocache __aligned(64) static uint16_t s_pxp_out[2][1280 * 720];
// Scratch for PASS-1 overlay rotation (portrait); caps the overlay at 512x512.
__nocache __aligned(64) static uint16_t s_as_rot[512 * 512];
static int s_pxp_idx = 0;
static bool s_pxp_ready = false;

// Last-known background (PS) and overlay (AS) layers, set by the layer-aware
// flush hook below.  The overlay rect is in landscape (Qt) coordinates.
static const void *s_ps_buf = nullptr;
static int s_ps_w = 0, s_ps_h = 0, s_ps_pitch = 0;
static const void *s_as_buf = nullptr;
static int s_as_w = 0, s_as_h = 0, s_as_pitch = 0, s_as_x = 0, s_as_y = 0;
static bool s_as_valid = false;

static void qzephyr_pxp_init_once()
{
    if (s_pxp_ready)
        return;
    PXP_Init(PXP);
    PXP_SetProcessSurfaceBackGroundColor(PXP, 0U);
    PXP_SetAlphaSurfacePosition(PXP, 0xFFFFU, 0xFFFFU, 0U, 0U);   // AS off by default
    PXP_EnableCsc1(PXP, false);
    s_pxp_ready = true;
}

// Composite s_ps (+ s_as if valid) and rotate 90 into a portrait back buffer,
// then scan it out.  Returns true if it presented.
static bool qzephyr_pxp_composite_present()
{
    if (!s_ps_buf)
        return false;
    const int w = s_ps_w, h = s_ps_h;            // landscape source dims
    if ((size_t)w * (size_t)h > 1280u * 720u)
        return false;

    qzephyr_pxp_init_once();

    // PXP reads PS/AS from SDRAM via its own master -> flush Qt's cached frames.
    sys_cache_data_flush_range(const_cast<void *>(s_ps_buf), (size_t)h * s_ps_pitch);
    if (s_as_valid && s_as_buf)
        sys_cache_data_flush_range(const_cast<void *>(s_as_buf), (size_t)s_as_h * s_as_pitch);

    uint16_t *dst = s_pxp_out[s_pxp_idx];

    // PXP rotates only ONE surface per pass.  We rotate the PS (background) to
    // portrait, which means the AS (overlay) would otherwise be placed
    // un-rotated.  So PASS 1 first rotates the overlay landscape->portrait into a
    // scratch buffer; PASS 2 then rotates the background PS and composites the
    // (now portrait) overlay over it, placed un-rotated at the rotated position.
    const void *as_src = nullptr;
    int as_pw = 0, as_ph = 0;                    // portrait (rotated) overlay dims
    if (s_as_valid && s_as_buf
        && (size_t)s_as_w * (size_t)s_as_h <= sizeof(s_as_rot) / 2) {
        as_pw = s_as_h;                          // portrait width  = landscape height
        as_ph = s_as_w;                          // portrait height = landscape width
        pxp_ps_buffer_config_t aps = {};
        aps.pixelFormat = kPXP_PsPixelFormatRGB565;
        aps.bufferAddr = (uint32_t)(uintptr_t)s_as_buf;
        aps.pitchBytes = (uint16_t)s_as_pitch;
        PXP_SetProcessSurfaceBufferConfig(PXP, &aps);
        pxp_output_buffer_config_t aout = {};
        aout.pixelFormat = kPXP_OutputPixelFormatRGB565;
        aout.interlacedMode = kPXP_OutputProgressive;
        aout.buffer0Addr = (uint32_t)(uintptr_t)s_as_rot;
        aout.pitchBytes = (uint16_t)(as_pw * 2);
        aout.width = (uint16_t)as_pw;
        aout.height = (uint16_t)as_ph;
        PXP_SetOutputBufferConfig(PXP, &aout);
        PXP_SetProcessSurfacePosition(PXP, 0U, 0U, (uint16_t)as_pw, (uint16_t)as_ph);
        PXP_SetAlphaSurfacePosition(PXP, 0xFFFFU, 0xFFFFU, 0U, 0U);
        PXP_SetRotateConfig(PXP, kPXP_RotateProcessSurface, kPXP_Rotate90, kPXP_FlipDisable);
        PXP_Start(PXP);
        while (!(PXP_GetStatusFlags(PXP) & kPXP_CompleteFlag)) { }
        PXP_ClearStatusFlags(PXP, kPXP_CompleteFlag);
        as_src = s_as_rot;
    }

    pxp_ps_buffer_config_t ps = {};
    ps.pixelFormat = kPXP_PsPixelFormatRGB565;
    ps.swapByte = false;
    ps.bufferAddr = (uint32_t)(uintptr_t)s_ps_buf;
    ps.pitchBytes = (uint16_t)s_ps_pitch;
    PXP_SetProcessSurfaceBufferConfig(PXP, &ps);

    pxp_output_buffer_config_t out = {};
    out.pixelFormat = kPXP_OutputPixelFormatRGB565;
    out.interlacedMode = kPXP_OutputProgressive;
    out.buffer0Addr = (uint32_t)(uintptr_t)dst;
    out.buffer1Addr = 0;
    out.pitchBytes = (uint16_t)(h * 2);          // portrait row bytes
    out.width = (uint16_t)h;                     // portrait width  = landscape height
    out.height = (uint16_t)w;                    // portrait height = landscape width
    PXP_SetOutputBufferConfig(PXP, &out);

    // Rotate the process surface 90 deg; PS active region is the rotated (output)
    // portrait extent, matching dma_mcux_pxp.c.
    PXP_SetProcessSurfacePosition(PXP, 0U, 0U, out.width, out.height);

    if (as_src) {
        pxp_as_buffer_config_t as = {};
        as.pixelFormat = kPXP_AsPixelFormatRGB565;
        as.bufferAddr = (uint32_t)(uintptr_t)as_src;
        as.pitchBytes = (uint16_t)(as_pw * 2);
        PXP_SetAlphaSurfaceBufferConfig(PXP, &as);

        // AS is already portrait; place it at the overlay's rotated position.
        // Landscape (x,y,w,h) -> portrait top-left (y, W - x - w) for a 90deg CW
        // background rotation (W = landscape width).
        const int px = s_as_y;
        const int py = s_ps_w - s_as_x - s_as_w;
        PXP_SetAlphaSurfacePosition(PXP, (uint16_t)px, (uint16_t)py,
                                    (uint16_t)(px + as_pw - 1),
                                    (uint16_t)(py + as_ph - 1));
        pxp_as_blend_config_t blend = {};
        blend.alpha = 0xFFU;
        blend.invertAlpha = false;
        blend.alphaMode = kPXP_AlphaOverride;    // opaque overlay (RGB565 has no alpha)
        blend.ropMode = kPXP_RopMergeAs;
        PXP_SetAlphaSurfaceBlendConfig(PXP, &blend);
    } else {
        PXP_SetAlphaSurfacePosition(PXP, 0xFFFFU, 0xFFFFU, 0U, 0U);  // AS off
    }

    PXP_SetRotateConfig(PXP, kPXP_RotateProcessSurface, kPXP_Rotate90, kPXP_FlipDisable);

    PXP_Start(PXP);
    while (!(PXP_GetStatusFlags(PXP) & kPXP_CompleteFlag)) { /* fast (<~20ms) */ }
    PXP_ClearStatusFlags(PXP, kPXP_CompleteFlag);

    struct display_buffer_descriptor d = {};
    d.width = (uint16_t)h;
    d.height = (uint16_t)w;
    d.pitch = (uint16_t)h;
    d.buf_size = (uint32_t)w * (uint32_t)h * 2u;
    d.frame_incomplete = false;
    display_write(s_display, 0, 0, &d, dst);

    s_pxp_idx ^= 1;
    return true;
}

// Layer-aware present hook the backing store calls (weak default is the plain
// qzephyr_display_write).  isOverlay routes the window to AS vs PS; (wx,wy) is
// the window's top-left in landscape Qt coordinates.
extern "C" void qzephyr_display_present_layer(int wx, int wy, int w, int h,
                                              int pitch_bytes, int /*bpp*/,
                                              const void *buf, int isOverlay)
{
    if (!s_display_ready || w <= 0 || h <= 0)
        return;
    if (isOverlay) {
        s_as_buf = buf; s_as_w = w; s_as_h = h; s_as_pitch = pitch_bytes;
        s_as_x = wx; s_as_y = wy; s_as_valid = true;
    } else {
        s_ps_buf = buf; s_ps_w = w; s_ps_h = h; s_ps_pitch = pitch_bytes;
    }
    qzephyr_pxp_composite_present();
}
#endif // CONFIG_QT_PXP_COMPOSITE

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

#ifdef CONFIG_CORTEX_M_DEBUG_MONITOR_HOOK
    // Heap-corruption hunt, stage 2 (temporary, 2026-06-12): every soak crash
    // traced back to ONE poisoned word -- the malloc arena's z_heap end_chunk
    // field (arena base + 8) overwritten with 0x004fff4f.  Arm a self-hosted
    // DWT write-watchpoint on that word at first frame (after heap init wrote
    // it legitimately).  The poisoning write then raises a DebugMonitor
    // exception whose fatal dump prints the WRITER's pc over UART.  Works
    // only without an external debugger attached (MON_EN requires halting
    // debug to be off) -- which matches the plain-soak repro conditions.
    static bool s_dwt_armed = false;
    if (!s_dwt_armed) {
        s_dwt_armed = true;
        extern struct sys_heap *z_malloc_heap_get(void);
        const uintptr_t end_chunk_addr =
                reinterpret_cast<uintptr_t>(z_malloc_heap_get()->heap) + 8;
        CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk | CoreDebug_DEMCR_MON_EN_Msk;
        DWT->COMP1 = end_chunk_addr;
        DWT->MASK1 = 2;          /* watch the whole 4-byte word */
        DWT->FUNCTION1 = 6;      /* break on write */
        printk("[dwt] write-watchpoint armed on heap end_chunk @%p\n",
               reinterpret_cast<void *>(end_chunk_addr));
    }
#endif

#ifdef CONFIG_SYS_HEAP_VALIDATE
    // Heap-corruption hunt (temporary, 2026-06-12): a soak bus-faulted inside
    // sys_heap_alloc walking a poisoned chunk header (collidingmice, ~20 min).
    // Validate the libc malloc heap every 16 frames (~0.8 s at 19.5 fps) so
    // the corruption is caught right after the offending write instead of
    // minutes later when the allocator trips over it.  z_malloc_heap_get()
    // is the diagnostic accessor left in lib/libc/common malloc.c.
    {
        extern struct sys_heap *z_malloc_heap_get(void);
        if ((call_count & 0xF) == 0 && !sys_heap_validate(z_malloc_heap_get())) {
            printk("[heapval] MALLOC HEAP CORRUPT at display frame %u\n", call_count);
            k_panic();
        }
    }
#endif

#ifdef CONFIG_QT_VGLITE_DISPLAY_ROTATE
    // Offload the landscape->portrait rotation to the GC355 (PXP rotate OFF).
    if (qzephyr_gpu_rotate_present(buf, w, h, pitch_bytes))
        return;
#endif

    s_desc.buf_size = static_cast<uint32_t>(h * pitch_bytes);
    s_desc.width = static_cast<uint16_t>(w);
    s_desc.height = static_cast<uint16_t>(h);
    // Zephyr's `pitch` field is in PIXELS, not bytes.
    s_desc.pitch = static_cast<uint16_t>(pitch_bytes / bytes_per_pixel);
    s_desc.frame_incomplete = false;

#if QZEPHYR_FREEZE_PROBE && DT_NODE_HAS_PROP(QZ_DISP_NODE, nxp_pxp)
    // Anomaly check: the PXP should be idle (ENABLE=0, complete-flag clear)
    // entering each frame.  If it enters "dirty" (a leftover complete flag or
    // ENABLE still set) right before the frame that then hangs, the previous
    // op's completion was mishandled (sem drift / lost callback).
    {
        PXP_Type *pxp = reinterpret_cast<PXP_Type *>(
                DT_REG_ADDR(DT_PHANDLE(QZ_DISP_NODE, nxp_pxp)));
        const uint32_t s = pxp->STAT, c = pxp->CTRL;
        if ((s & 0x1u) || (c & 0x1u))
            printk("[pxp-pre] call=%u STAT=0x%08x CTRL=0x%08x (dirty entry)\n",
                   call_count, (unsigned)s, (unsigned)c);
    }
#endif

    const int ret = display_write(s_display,
                                  static_cast<uint16_t>(x),
                                  static_cast<uint16_t>(y),
                                  &s_desc, buf);
#if QZEPHYR_FREEZE_PROBE
    ++s_dw_completions;   // advanced only when display_write actually returns
#endif
    if (ret != 0)
        LOG_WRN("display_write(%d,%d %dx%d) failed: %d", x, y, w, h, ret);
}

// Strong def for the weak hook in qzephyrbackingstore.cpp.  Enables the
// partial (2-frame-union sub-rect) flush path when CONFIG_QT_PXP_PARTIAL_FLUSH
// is set, so a single moving element repaints far less than the full 1.84 MB
// frame through PXP.  Default off -- the whole-frame flush is the safe path
// (partial under PXP rotate is double-buffer-sensitive; validate on the panel).
extern "C" bool qzephyr_pxp_partial_flush_enabled()
{
#ifdef CONFIG_QT_PXP_PARTIAL_FLUSH
    return true;
#else
    return false;
#endif
}
