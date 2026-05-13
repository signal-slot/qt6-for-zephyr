# Qt 6 on Zephyr -- module CMakeLists.
#
# Zephyr invokes this when CONFIG_QT=y.  Pulls in every per-app shim
# that used to be copy-pasted into each examples/zephyr-rt1170-*/
# directory: the qzephyr QPA plugin's static archive, the Stage 2
# Zephyr-runtime TUs (display + touch bridges, event-dispatcher
# factory), the libc stubs that need <zephyr/kernel.h>, the embedded
# Roboto-Regular TTF, and the static QML plugins demos.coffee /
# Widgets Gallery / qmlsample all need.
#
# User-side overhead reduces to: drop the example into a workspace,
# point west at it, run `west build`.

if(NOT CONFIG_QT)
    return()
endif()

# Make `include(QtZephyrApp)` resolvable from the user wrapper.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../cmake")
set(CMAKE_MODULE_PATH "${CMAKE_MODULE_PATH}" CACHE STRING "" FORCE)

if(NOT DEFINED ENV{QT_ZEPHYR_PREFIX})
    message(FATAL_ERROR
        "CONFIG_QT=y but QT_ZEPHYR_PREFIX is not set; point it at the "
        "Stage 1 cross-build install (e.g. $HOME/qt-zephyr-rt1170).")
endif()
set(QT_ZEPHYR_PREFIX "$ENV{QT_ZEPHYR_PREFIX}" CACHE PATH "Qt 6 install root")

# Stage 1's Cortex-M7 build of Qt6 was compiled with -fno-short-enums
# so QCborValue::Type and QJsonValue::Type stay 4-byte; without this
# the Zephyr app TUs (which inherit -fshort-enums from the ARM EABI
# default) hit a static_assert in qjsonvalue.h.
zephyr_compile_options(-fno-short-enums)

# Both Stage 1 deferred TUs and QML cache loaders carry Q_OBJECT.
set_target_properties(app PROPERTIES AUTOMOC ON)

# Repo root (two levels up from zephyr-module/zephyr/).
set(QT_QTBASE_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../../qtbase")

# Core-only glue: the event dispatcher is required whenever Qt is in
# use (CONFIG_QT=y), regardless of GUI / input choices.  Stage 2
# builds qeventdispatcher_zephyr.cpp inside the Zephyr app so it can
# include <zephyr/kernel.h>.
target_sources(app PRIVATE
    "${QT_QTBASE_SOURCE}/src/corelib/kernel/qeventdispatcher_zephyr.cpp"
)

# QPA plugin import + display-side glue (display_write /
# display_get_capabilities hooks, QZephyrScreen format detection).
# Only relevant when QtGui is in use; a Core-only firmware does not
# need a platform integration nor a framebuffer driver.
if(CONFIG_QT_GUI)
    target_sources(app PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../src/qzephyr_plugin_import.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../src/qzephyr_display_zephyr.cpp"
    )
endif()

# Touch-side glue.  Requires CONFIG_INPUT (Zephyr's input subsystem)
# AND CONFIG_QT_GUI (QWindowSystemInterface routes pointer events to
# a QWindow).  A headless or input-less board skips this entirely.
if(CONFIG_QT_GUI AND CONFIG_INPUT)
    target_sources(app PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../src/qzephyr_touch_zephyr.cpp"
    )
endif()

# Embedded Roboto font -- needs QtGui (QFontDatabase) and the explicit
# user opt-in.
if(CONFIG_QT_GUI AND CONFIG_QT_EMBEDDED_ROBOTO_FONT)
    target_sources(app PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../src/embedded_font.c"
        "${CMAKE_CURRENT_LIST_DIR}/../src/qzephyr_font_loader.cpp"
    )
    target_compile_definitions(app PRIVATE QT_HAS_EMBEDDED_FONT)
endif()

# Locate Qt6's private-header version dirs.  Qt installs them under
# <module>/<X.Y.Z>/<Module>/private/...; the X.Y.Z component is the
# Qt patch version and floats over releases.  Glob it per module so a
# Qt 6.11.1 / 6.12.0 / etc. patch/minor bump does not require an edit
# here.  FATAL_ERROR with a clear diagnostic when the glob comes back
# empty -- the silent list(GET ... 0) on an empty list yields
# "<var>-NOTFOUND" which gets stuffed into target_include_directories
# and produces an opaque later failure.
function(_qza_module_private_dir _qza_mod _qza_out)
    file(GLOB _qza_dirs LIST_DIRECTORIES TRUE
        "${QT_ZEPHYR_PREFIX}/include/${_qza_mod}/[0-9]*.[0-9]*.[0-9]*")
    if(NOT _qza_dirs)
        message(FATAL_ERROR
            "qt-zephyr-port: cannot find ${_qza_mod} private-header version "
            "directory under ${QT_ZEPHYR_PREFIX}/include/${_qza_mod}/.  "
            "Check that QT_ZEPHYR_PREFIX points at a Cortex-M7 Stage 1 "
            "install that included ${_qza_mod}.")
    endif()
    list(GET _qza_dirs 0 _qza_first)
    set(${_qza_out} "${_qza_first}" PARENT_SCOPE)
endfunction()

_qza_module_private_dir(QtCore _qza_core_private)

target_include_directories(app PRIVATE
    "${QT_ZEPHYR_PREFIX}/include"
    "${QT_ZEPHYR_PREFIX}/include/QtCore"
    "${_qza_core_private}"
    "${_qza_core_private}/QtCore"
)

if(CONFIG_QT_GUI)
    _qza_module_private_dir(QtGui _qza_gui_private)
    target_include_directories(app PRIVATE
        "${QT_ZEPHYR_PREFIX}/include/QtGui"
        "${_qza_gui_private}"
        "${_qza_gui_private}/QtGui"
    )
endif()

if(CONFIG_QT_QML)
    target_include_directories(app PRIVATE
        "${QT_ZEPHYR_PREFIX}/include/QtQml"
        "${QT_ZEPHYR_PREFIX}/include/QtQuick"
    )
endif()

# Link the qzephyr QPA plugin's static archive when QtGui is in use.
# A Core-only firmware has no QGuiApplication and does not need a
# platform integration plugin.
if(CONFIG_QT_GUI)
    target_link_libraries(app PRIVATE
        "${QT_ZEPHYR_PREFIX}/plugins/platforms/libqzephyr.a"
    )
endif()

# QML / Quick / Controls / Svg plugin archives + their _init + _resources
# OBJECT siblings are linked from cmake/QtZephyrApp.cmake::qt_zephyr_app()
# using Qt's official Qt6::<plugin> imported targets.  That requires
# find_package(Qt6) to have run first (which it has in qt_zephyr_app()'s
# scope), so we cannot link them from here -- the Qt6:: targets do not
# exist yet at module-CMakeLists evaluation time.

# Stage 1's libQt6Core.a was linked with -Wl,--unresolved-symbols=ignore-all
# so QtCore TUs that reference UNIX-only filesystem entry points can resolve
# at Stage 2.  Propagate the same flag here.
zephyr_link_libraries(
    "-Wl,--unresolved-symbols=ignore-all"
    "-Wl,--warn-unresolved-symbols"
)
