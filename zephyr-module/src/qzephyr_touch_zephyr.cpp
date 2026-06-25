// Tier 4: GT911 capacitive touch -> QWindowSystemInterface mouse events.
//
// Zephyr's input subsystem delivers raw input_event structs from the
// GT911 driver via INPUT_CALLBACK_DEFINE.  We accumulate X/Y absolute
// coordinates and the BTN_TOUCH press/release flag across events, then
// when `sync` is set we post a synthesized QMouseEvent through
// QWindowSystemInterface so QGuiApplication can dispatch it.
//
// Pattern mirrors Slint's printerdemo touch handling.

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/logging/log.h>

#include <QtCore/QPoint>
#include <QtCore/Qt>
#include <QtCore/QEvent>
#include <QtCore/QObject>
#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>
#include <QtGui/QScreen>
#include <qpa/qwindowsysteminterface.h>

LOG_MODULE_REGISTER(qzephyr_touch, LOG_LEVEL_INF);

namespace {

struct TouchState {
    int x = 0;
    int y = 0;
    bool pressed = false;
    bool sent_press = false;
};
TouchState g_state;

// Liveness counters: input thread increments, main thread reads via
// the getters at the bottom of this file.  Naturally-aligned 32-bit
// stores on Cortex-M are atomic; we only look at trends so a torn
// read on main is harmless.
volatile uint32_t g_evtCount = 0;       // every raw input_event seen
volatile uint32_t g_pressCount = 0;     // QPA MouseButtonPress posted
volatile uint32_t g_releaseCount = 0;   // QPA MouseButtonRelease posted
volatile int g_lastPx = -1, g_lastPy = -1;  // DIAG: last raw GT911 coords
volatile int g_lastLx = -1, g_lastLy = -1;  // DIAG: last transformed (Qt) coords

// DIAG: count mouse press/release that actually reach Qt's delivery, via a
// global application event filter installed lazily from the main thread inside
// qzephyr_drain_qpa_events().  Compared with g_pressCount/g_releaseCount (which
// are incremented on the input thread when the event is POSTED), this splits:
//   filt flat while posted rises  -> posted but never drained/delivered to a QWindow
//   filt rises with posted        -> delivered to the QWindow, but the QtQuick
//                                    item/click never reacts (item-level drop)
// g_filtToWindow further confirms the receiver was a QWindow (not some other obj).
volatile uint32_t g_filtPress = 0;
volatile uint32_t g_filtRelease = 0;
volatile uint32_t g_filtToWindow = 0;

class QzephyrTouchProbe : public QObject
{
public:
    bool eventFilter(QObject *recv, QEvent *e) override
    {
        const QEvent::Type t = e->type();
        if (t == QEvent::MouseButtonPress || t == QEvent::MouseButtonRelease) {
            if (t == QEvent::MouseButtonPress)
                ++g_filtPress;
            else
                ++g_filtRelease;
            if (qobject_cast<QWindow *>(recv))
                ++g_filtToWindow;
        }
        return false;   // never consume
    }
};

// Raw touch frames, input thread -> main thread.
//
// Qt is built with FEATURE_thread=OFF on this port, which turns QMutex into a
// NO-OP: every "mutex-protected" Qt structure (including the QPA window-system
// event queue that AsynchronousDelivery appends to) is actually unsynchronized.
// Calling ANY Qt API from the Zephyr input/syswq thread therefore races the
// main thread — a concurrent QList append/takeFirst on the QPA queue
// reallocates under the reader and corrupts the heap (observed on the m4safety
// coffee soak as double frees / realloc-after-free at QML teardown).
//
// The input callback below must consequently touch NOTHING of Qt.  It only
// records the raw frame in this irq-locked ring; qzephyr_drain_qpa_events()
// (called every event-loop iteration on the main thread, latency <= the
// dispatcher's 50 ms wait cap) turns the frames into QPA mouse events.
struct RawTouchFrame {
    int16_t x;
    int16_t y;
    uint8_t pressed;
};
constexpr unsigned kRawRingSize = 64;     // power of two
RawTouchFrame g_rawRing[kRawRingSize];
volatile unsigned g_rawHead = 0;          // written by producer (input thread)
volatile unsigned g_rawTail = 0;          // written by consumer (main thread)
volatile uint32_t g_rawDropped = 0;

void process_event(input_event *evt, void *)
{
    // NO logging and NO Qt calls from this thread -- see the comment above
    // (no-op QMutex) and the UART note: this console is polling-write at
    // 115200 baud (~9 ms per line) and blocking here loses GT911 IRQs.
    ++g_evtCount;
    switch (evt->code) {
    case INPUT_ABS_X:
        g_state.x = evt->value;
        break;
    case INPUT_ABS_Y:
        g_state.y = evt->value;
        break;
    case INPUT_BTN_TOUCH:
        g_state.pressed = (evt->value != 0);
        break;
    default:
        return;
    }
    if (!evt->sync)
        return;

    // Complete frame: hand it to the main thread.  irq_lock makes the ring
    // safe regardless of which context the input subsystem dispatches from
    // (dedicated input thread, syswq, or a reporter's own thread).
    unsigned key = irq_lock();
    const unsigned head = g_rawHead;
    const unsigned next = (head + 1) & (kRawRingSize - 1);
    if (next == g_rawTail) {
        ++g_rawDropped;               // ring full: drop newest frame
    } else {
        g_rawRing[head].x = (int16_t)g_state.x;
        g_rawRing[head].y = (int16_t)g_state.y;
        g_rawRing[head].pressed = g_state.pressed ? 1 : 0;
        g_rawHead = next;
    }
    irq_unlock(key);
}

// Everything below runs on the MAIN thread only (called from
// qzephyr_drain_qpa_events): the coordinate transform, window targeting and
// the QPA post are all Qt API and must stay off the input thread.
struct TouchState2 {
    int x = 0;
    int y = 0;
    bool pressed = false;
    bool sent_press = false;
};
TouchState2 g_mainState;

void deliver_touch_frame(const RawTouchFrame &f)
{
    g_mainState.x = f.x;
    g_mainState.y = f.y;
    g_mainState.pressed = (f.pressed != 0);

    // GT911 reports coordinates in the panel's native portrait frame
    // (720 wide x 1280 tall on the RK055HDMIPI4MA0).  PXP rotates the
    // *image* 90 degrees CW so Qt sees a 1280x720 landscape canvas, but
    // PXP does not touch the input pipeline.  Apply the inverse rotation
    // (CCW) so the QMouseEvent lands at the position the user sees.
    //
    // Inverse of 90deg CW image rotation:
    //   landscape (lx, ly) <- panel-native (px, py)
    //   lx = py
    //   ly = (panel_max_x - px)
    constexpr int kPanelMaxX = 719;
    const int lx = g_mainState.y;
    const int ly = kPanelMaxX - g_mainState.x;
    g_lastPx = g_mainState.x; g_lastPy = g_mainState.y;   // DIAG: raw GT911 coords
    g_lastLx = lx;            g_lastLy = ly;              // DIAG: transformed (Qt) coords
    const QPoint p(lx, ly);
    const bool pressed = g_mainState.pressed;
    const bool was_pressed = g_mainState.sent_press;

    Qt::MouseButtons buttons = pressed ? Qt::LeftButton : Qt::NoButton;
    QEvent::Type evType =
        (pressed && !was_pressed) ? QEvent::MouseButtonPress :
        (!pressed && was_pressed) ? QEvent::MouseButtonRelease :
        QEvent::MouseMove;
    Qt::MouseButton button =
        (evType == QEvent::MouseMove) ? Qt::NoButton : Qt::LeftButton;

    g_mainState.sent_press = pressed;
    if (evType == QEvent::MouseButtonPress)   ++g_pressCount;
    if (evType == QEvent::MouseButtonRelease) ++g_releaseCount;

    // Route to the correct top-level window.  During a press-drag keep the
    // grabbed window; otherwise hit-test by position, preferring always-on-top
    // (overlay) windows and skipping input-transparent ones.  This keeps touch
    // consistent with the PXP PS/AS layer order -- the same WindowStaysOnTopHint
    // flag drives both the composite layer and the input z-order.
    static QWindow *s_grab = nullptr;
    QWindow *target = nullptr;
    const QPoint global = p;
    if (s_grab && was_pressed) {
        target = s_grab;                          // hold grab until release
    } else {
        QWindow *fallback = nullptr;
        const auto windows = QGuiApplication::topLevelWindows();
        for (QWindow *w : windows) {
            if (!w->isVisible() || (w->flags() & Qt::WindowTransparentForInput))
                continue;
            if (!fallback)
                fallback = w;
            if (!w->geometry().contains(global))
                continue;
            const bool wTop = bool(w->flags() & Qt::WindowStaysOnTopHint);
            const bool tTop = target && bool(target->flags() & Qt::WindowStaysOnTopHint);
            if (!target || (wTop && !tTop) || (wTop == tTop))   // on-top / later wins
                target = w;
        }
        if (!target)
            target = fallback;
    }
    if (evType == QEvent::MouseButtonPress)
        s_grab = target;
    else if (evType == QEvent::MouseButtonRelease)
        s_grab = nullptr;

    const QPoint local = target ? (global - target->geometry().topLeft()) : global;

    // We are on the main thread (called from the dispatcher's drain hook),
    // so queueing into the QPA window-system event queue is race-free; the
    // sendWindowSystemEvents() in qzephyr_drain_qpa_events() right after the
    // drain loop dispatches it within the same loop iteration.
    QWindowSystemInterface::handleMouseEvent<
        QWindowSystemInterface::AsynchronousDelivery>(
            target, local, global, buttons, button, evType, Qt::NoModifier);
}

} // namespace

INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_CHOSEN(zephyr_touch)),
                      process_event, NULL);

// Strong def of the weak hook qeventdispatcher_zephyr.cpp calls each
// iteration of processEvents().  Drains the QPA window-system event
// queue so events posted by handleMouseEvent() actually reach QWindow.
extern "C" void qzephyr_drain_qpa_events()
{
    // Lazily install the diagnostic mouse-event probe on the application object
    // the first time we run on the main thread (qApp exists once QGuiApplication
    // is constructed).  A global app event filter sees every event delivered via
    // QCoreApplication::notify, so it counts the mouse press/release that the
    // sendWindowSystemEvents() below actually dispatches to a QWindow.
    static QObject *s_probe = nullptr;
    if (!s_probe && qApp) {
        s_probe = new QzephyrTouchProbe;
        qApp->installEventFilter(s_probe);
    }

    // Convert raw touch frames (queued by the input thread) into QPA mouse
    // events HERE, on the main thread: with FEATURE_thread=OFF Qt's QMutex is
    // a no-op, so all Qt API use must be single-threaded (see process_event).
    for (;;) {
        RawTouchFrame f;
        unsigned key = irq_lock();
        if (g_rawTail == g_rawHead) {
            irq_unlock(key);
            break;
        }
        f = g_rawRing[g_rawTail];
        g_rawTail = (g_rawTail + 1) & (kRawRingSize - 1);
        irq_unlock(key);
        deliver_touch_frame(f);
    }

    QWindowSystemInterface::sendWindowSystemEvents(QEventLoop::AllEvents);
}

// Main-thread accessors for the liveness counters.  Read by the
// bs.flush heartbeat in qzephyrbackingstore.cpp.  Used to distinguish
// "touch chip stopped firing IRQs" (evt counter flat) from "touch
// driver alive but QPA path dropped events" (evt rises but press/
// release don't) from "QPA path fine but widget never reacted"
// (evt + press/release all rise but linkPoke doesn't).
extern "C" int qzephyr_touch_evt_count()     { return (int)g_evtCount; }
extern "C" int qzephyr_touch_press_count()   { return (int)g_pressCount; }
extern "C" int qzephyr_touch_release_count() { return (int)g_releaseCount; }
extern "C" int qzephyr_touch_last_px()       { return g_lastPx; }
extern "C" int qzephyr_touch_last_py()       { return g_lastPy; }
extern "C" int qzephyr_touch_last_lx()       { return g_lastLx; }
extern "C" int qzephyr_touch_last_ly()       { return g_lastLy; }

// DIAG: mouse press/release that reached Qt delivery (see QzephyrTouchProbe).
extern "C" int qzephyr_touch_filt_press()    { return (int)g_filtPress; }
extern "C" int qzephyr_touch_filt_release()  { return (int)g_filtRelease; }
extern "C" int qzephyr_touch_filt_window()   { return (int)g_filtToWindow; }

