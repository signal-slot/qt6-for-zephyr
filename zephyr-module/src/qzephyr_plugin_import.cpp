// Import the qzephyr QPA platform plugin and force QT_QPA_PLATFORM=zephyr
// before QGuiApplication is constructed.
//
// QML plugin imports are NOT listed here -- they are auto-discovered
// per sample by Qt's own qt_import_qml_plugins() machinery, invoked
// from QtZephyrApp.cmake on the sample's library target.  This means
// adding a new Qt sample that imports new QML modules requires zero
// changes to the qt-zephyr-port module.
//
// Q_IMPORT_PLUGIN(QZephyrIntegrationPlugin) stays here because the
// QPA plugin is not a QML plugin and is not part of any Qt-shipped
// default plugin set, so Qt's auto-import will not pick it up.

#include <QtCore/QtPlugin>
#include <cstdlib>

namespace {
__attribute__((constructor)) void zephyr_qpa_default()
{
    if (!std::getenv("QT_QPA_PLATFORM"))
        setenv("QT_QPA_PLATFORM", "zephyr", 0);
}
} // namespace

Q_IMPORT_PLUGIN(QZephyrIntegrationPlugin)
