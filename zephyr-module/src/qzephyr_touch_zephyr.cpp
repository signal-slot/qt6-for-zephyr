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

void process_event(input_event *evt, void *)
{
    static uint32_t evt_count = 0;
    ++evt_count;
    if (evt_count <= 10 || (evt_count % 50) == 0) {
        printk("[touch_evt #%u] type=%u code=%u value=%d sync=%d\n",
               evt_count, evt->type, evt->code, evt->value, evt->sync);
    }
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
    const int lx = g_state.y;
    const int ly = kPanelMaxX - g_state.x;
    const QPoint p(lx, ly);
    const bool pressed = g_state.pressed;
    const bool was_pressed = g_state.sent_press;

    Qt::MouseButtons buttons = pressed ? Qt::LeftButton : Qt::NoButton;
    QEvent::Type evType =
        (pressed && !was_pressed) ? QEvent::MouseButtonPress :
        (!pressed && was_pressed) ? QEvent::MouseButtonRelease :
        QEvent::MouseMove;
    Qt::MouseButton button =
        (evType == QEvent::MouseMove) ? Qt::NoButton : Qt::LeftButton;

    g_state.sent_press = pressed;

    // Find the single top-level QWindow this embedded app has.
    QWindow *target = nullptr;
    const auto windows = QGuiApplication::topLevelWindows();
    if (!windows.isEmpty())
        target = windows.constFirst();

    static uint32_t dispatched = 0;
    ++dispatched;
    if (dispatched <= 6) {
        printk("[touch] dispatch #%u type=%d target=%p pos=(%d,%d) btn=%d\n",
               dispatched, (int)evType, target, p.x(), p.y(),
               (int)button);
    }

    // Asynchronous delivery: queue the event for the main thread to
    // drain via qzephyr_drain_qpa_events().  Synchronous delivery (the
    // default) would inline QApplication::notify() right here on
    // Zephyr's input thread, whose stack defaults to
    // CONFIG_INPUT_THREAD_STACK_SIZE = 1024 bytes.
    // QApplication::notify() allocates ~4.5 KB of stack frame on entry,
    // which on a 1 KB input thread is a guaranteed overflow -> MPU
    // fault at the very first store after the function prologue.
    QWindowSystemInterface::handleMouseEvent<
        QWindowSystemInterface::AsynchronousDelivery>(
            target, p, p, buttons, button, evType, Qt::NoModifier);
}

} // namespace

INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_CHOSEN(zephyr_touch)),
                      process_event, NULL);

// Strong def of the weak hook qeventdispatcher_zephyr.cpp calls each
// iteration of processEvents().  Drains the QPA window-system event
// queue so events posted by handleMouseEvent() actually reach QWindow.
extern "C" void qzephyr_drain_qpa_events()
{
    QWindowSystemInterface::sendWindowSystemEvents(QEventLoop::AllEvents);
}

