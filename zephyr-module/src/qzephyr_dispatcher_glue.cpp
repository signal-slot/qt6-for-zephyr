// Stage 2 strong definition of the qzephyr QPA's event-dispatcher hook.
//
// QZephyrIntegration::createEventDispatcher() calls this weak hook
// because QEventDispatcherZephyr's header includes <zephyr/kernel.h>,
// which only compiles inside `west build`.  One TU, compiled for every
// CONFIG_QT firmware regardless of the display glue in use.

#include "../../qtbase/src/corelib/kernel/qeventdispatcher_zephyr_p.h"
#ifdef CONFIG_QT_GUI
#include <QtCore/qeventloop.h>
#include <qpa/qwindowsysteminterface.h>
#endif

extern "C" QAbstractEventDispatcher *qzephyr_make_event_dispatcher()
{
    return new QEventDispatcherZephyr();
}

// Strong definition of the weak hook the dispatcher calls on every
// processEvents() iteration: drains the QPA window-system event queue so
// the expose and geometry events the QPA window posts (and touch events,
// when an input device is present) reach QWindow.  Without it a
// QQuickWindow is never exposed and never renders.  This used to live in
// the touch glue, which a board without CONFIG_INPUT does not compile.
#ifdef CONFIG_QT_GUI
extern "C" void qzephyr_drain_qpa_events()
{
    QWindowSystemInterface::sendWindowSystemEvents(QEventLoop::AllEvents);
}
#endif
