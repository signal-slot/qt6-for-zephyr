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

# qeventdispatcher_zephyr.cpp ends with #include "moc_qeventdispatcher_zephyr_p.cpp"
# (the upstream idiom -- its Q_OBJECT lives in the _p.h private header, and that
# moc carries the metaobject AND the dispatcher's vtable).  qtbase's own AUTOMOC
# would produce that moc, but this TU is deferred to the Stage-2 Zephyr app build
# (QT_DEFER_ZEPHYR_RUNTIME=ON), and the `app` target's AUTOMOC is disabled there
# ("no valid Qt version").  Without the moc the vtable is undefined and resolves
# to garbage under -Wl,--unresolved-symbols=ignore-all, faulting on the first
# virtual call (wakeUp() from QCoreApplication::postEvent).  So generate the moc
# explicitly and put it on the include path for the #include to pick up.
find_program(QZ_MOC NAMES moc
    HINTS "${QT_ZEPHYR_PREFIX}/libexec" "${QT_ZEPHYR_PREFIX}/bin"
          /usr/lib64/qt6/libexec /usr/lib/qt6/libexec /usr/lib/qt6/bin)
if(NOT QZ_MOC)
    message(FATAL_ERROR "qt-zephyr-port: a host 'moc' is required to build "
        "qeventdispatcher_zephyr_p.h but none was found.")
endif()
set(_qz_disp_hdr "${QT_QTBASE_SOURCE}/src/corelib/kernel/qeventdispatcher_zephyr_p.h")
set(_qz_moc_dir "${CMAKE_CURRENT_BINARY_DIR}/qz_moc")
file(MAKE_DIRECTORY "${_qz_moc_dir}")
# Generate at configure time (re-runs on reconfigure, e.g. header mtime change).
execute_process(
    COMMAND "${QZ_MOC}" "${_qz_disp_hdr}"
            -o "${_qz_moc_dir}/moc_qeventdispatcher_zephyr_p.cpp"
    RESULT_VARIABLE _qz_moc_rc)
if(NOT _qz_moc_rc EQUAL 0)
    message(FATAL_ERROR "qt-zephyr-port: moc failed (${_qz_moc_rc}) for "
        "${_qz_disp_hdr}")
endif()
target_include_directories(app PRIVATE "${_qz_moc_dir}")

# Idle keep-alive thread (src/qzephyr_keepalive.c): prevents the silent
# idle/SysTick clock-gating hang seen on long GUI soaks (collidingmice).
# Force-linked with -u because the TU's only symbols are a K_THREAD_DEFINE
# registration the linker would otherwise drop as unreferenced.
if(CONFIG_QT_GUI)
    target_sources(app PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../src/qzephyr_keepalive.c"
    )
    target_link_options(app PRIVATE "-Wl,-u,qz_keepalive_link_anchor")
endif()

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

# Temporary heap diagnostics (2026-06-12): fill-on-free via --wrap=free so
# use-after-free reads surface as 0xDBDBDBDB in crash registers.  Rides the
# CONFIG_SYS_HEAP_VALIDATE diagnostic gate; absent from normal builds.
if(CONFIG_SYS_HEAP_VALIDATE)
    target_sources(app PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../src/qzephyr_heap_diag.c"
    )
    target_link_options(app PRIVATE "-Wl,--wrap=free" "-Wl,--wrap=realloc")
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
    _qza_module_private_dir(QtQml _qza_qml_private)
    _qza_module_private_dir(QtQuick _qza_quick_private)
    target_include_directories(app PRIVATE
        "${QT_ZEPHYR_PREFIX}/include/QtQml"
        "${QT_ZEPHYR_PREFIX}/include/QtQuick"
        "${_qza_qml_private}"
        "${_qza_qml_private}/QtQml"
        "${_qza_quick_private}"
        "${_qza_quick_private}/QtQuick"
    )
endif()

# QSG vglite adaptation -- registers itself with qtdeclarative via the
# weak `qt_zephyr_qsg_external_adaptation` hook (patched into
# qtdeclarative/src/quick/scenegraph/qsgcontextplugin.cpp).  Compiled
# only when CONFIG_QT_VGLITE=y, so a Qt firmware without the opt-in
# leaves the weak symbol unresolved and the patch is a no-op.
if(CONFIG_QT_VGLITE)
    # The vglite QtQuick scenegraph backend: adaptation entry point, the
    # node + render-context factory, the renderer, the basic render loop,
    # and one source per QSGVGLite* node type.  Fully software-independent
    # (no QSGSoftware* symbol) -- see src/qsg_vglite/qsgvglitecontext_p.h.
    target_include_directories(app PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../src")

    # RT1170 GC355 GPU2D bring-up (clock + __nocache pools + IRQ + vg_lite_init).
    # Opt-in: the __nocache region alone perturbs display-DMA timing, so the
    # default vglite firmware (CPU-composite) does not pay for it.
    if(CONFIG_QT_VGLITE_GPU)
        target_sources(app PRIVATE
            "${CMAKE_CURRENT_LIST_DIR}/../src/qzephyr_vglite_init.c"
        )
    endif()

    # vg_lite QPaintEngine for QtWidgets / QPainter content: offloads the CPU
    # QRasterPaintEngine fills (collidingmice ~88 ms paint) to the GC355.  The
    # Stage-1 plugin's backing store picks it up via the weak hook
    # qzephyr_make_vglite_paint_device().  Opt-in (needs the GPU bring-up).
    if(CONFIG_QT_VGLITE_QPAINTENGINE)
        target_sources(app PRIVATE
            "${CMAKE_CURRENT_LIST_DIR}/../src/qzephyr_vglite_paintengine.cpp"
        )
    endif()

    set(_qsgv_dir "${CMAKE_CURRENT_LIST_DIR}/../src/qsg_vglite")
    target_sources(app PRIVATE
        "${_qsgv_dir}/qsg_vglite_adaptation.cpp"
        "${_qsgv_dir}/qsgvglitecontext.cpp"
        "${_qsgv_dir}/qsgvgliterenderloop.cpp"
        "${_qsgv_dir}/qsgvgliterenderer.cpp"
        "${_qsgv_dir}/qsgvglitetexture.cpp"
        "${_qsgv_dir}/qsgvglitelayer.cpp"
        "${_qsgv_dir}/qsgvgliterectanglenode.cpp"
        "${_qsgv_dir}/qsgvgliteimagenode.cpp"
        "${_qsgv_dir}/qsgvgliteglyphnode.cpp"
        "${_qsgv_dir}/qsgvglitepainternode.cpp"
        "${_qsgv_dir}/qsgvglitepublicnodes.cpp"
        "${_qsgv_dir}/qsgvglitespritenode.cpp"
    )

    # Vivante vg_lite library (vendored from LVGL's bundle, which itself
    # mirrors NXP MCUXpresso's `middleware/vglite`).  The LV_USE_VG_LITE_DRIVER
    # / LV_VG_LITE_HAL_GPU_* defines come from us, not LVGL -- we sed'd the
    # `#include "../../../lv_conf_internal.h"` line out of every source file
    # so the library compiles standalone without LVGL.
    set(_vg_lite_dir "${CMAKE_CURRENT_LIST_DIR}/../vg_lite")
    target_sources(app PRIVATE
        "${_vg_lite_dir}/VGLite/vg_lite.c"
        "${_vg_lite_dir}/VGLite/vg_lite_image.c"
        "${_vg_lite_dir}/VGLite/vg_lite_matrix.c"
        "${_vg_lite_dir}/VGLite/vg_lite_path.c"
        "${_vg_lite_dir}/VGLite/vg_lite_stroke.c"
        "${_vg_lite_dir}/VGLiteKernel/vg_lite_kernel.c"
        "${_vg_lite_dir}/hal/vg_lite_hal_zephyr.c"
    )
    target_include_directories(app PRIVATE
        "${_vg_lite_dir}"
        "${_vg_lite_dir}/inc"
        "${_vg_lite_dir}/VGLite"
        "${_vg_lite_dir}/VGLiteKernel"
        "${_vg_lite_dir}/hal"
    )
    target_compile_definitions(app PRIVATE
        LV_USE_VG_LITE_DRIVER=1
        # Variant selection.  GC355 with revision 0x0_1216 is what the
        # RT1176 ships per Slint memory + LVGL convention.  Confirm
        # against the actual GPU CHIPID/REVISION readback at probe time.
        LV_VG_LITE_HAL_GPU_SERIES=gc355
        LV_VG_LITE_HAL_GPU_REVISION=0x0_1216
        # GC355 register window base on iMX RT1176.  Placeholder until we
        # wire this from the device tree -- the NXP reference manual
        # places GPU2D at 0x41800000 on RT1170/RT1176.
        LV_VG_LITE_HAL_GPU_BASE_ADDRESS=0x41800000U
    )
endif()

if(CONFIG_QT_WIDGETS)
    _qza_module_private_dir(QtWidgets _qza_widgets_private)
    target_include_directories(app PRIVATE
        "${QT_ZEPHYR_PREFIX}/include/QtWidgets"
        "${_qza_widgets_private}"
        "${_qza_widgets_private}/QtWidgets"
    )
endif()

if(CONFIG_QT_QUICK_WIDGETS)
    target_include_directories(app PRIVATE
        "${QT_ZEPHYR_PREFIX}/include/QtQuickWidgets"
    )
endif()

if(CONFIG_QT_XML)
    target_include_directories(app PRIVATE
        "${QT_ZEPHYR_PREFIX}/include/QtXml"
    )
endif()

if(CONFIG_QT_MULTIMEDIA)
    _qza_module_private_dir(QtMultimedia _qza_multimedia_private)
    target_include_directories(app PRIVATE
        "${QT_ZEPHYR_PREFIX}/include/QtMultimedia"
        "${_qza_multimedia_private}"
        "${_qza_multimedia_private}/QtMultimedia"
    )
    target_link_libraries(app PRIVATE
        "${QT_ZEPHYR_PREFIX}/lib/libQt6Multimedia.a"
    )
    target_sources(app PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../src/qzephyr_audio_zephyr.cpp"
    )
endif()

if(CONFIG_QT_SQL)
    _qza_module_private_dir(QtSql _qza_sql_private)
    target_include_directories(app PRIVATE
        "${QT_ZEPHYR_PREFIX}/include/QtSql"
        "${_qza_sql_private}"
        "${_qza_sql_private}/QtSql"
    )
    target_link_libraries(app PRIVATE
        "${QT_ZEPHYR_PREFIX}/lib/libQt6Sql.a"
        "${QT_ZEPHYR_PREFIX}/plugins/sqldrivers/libqsqlite.a"
        "${QT_ZEPHYR_PREFIX}/plugins/sqldrivers/objects-Release/QSQLiteDriverPlugin_init/QSQLiteDriverPlugin_init.cpp.obj"
    )
    target_sources(app PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../src/qzephyr_sqlite_plugin_import.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../src/qzephyr_sqlite_vfs_patch.c"
        "${CMAKE_CURRENT_LIST_DIR}/../src/qzephyr_sqlite_posix_stubs.c"
    )
    target_include_directories(app PRIVATE
        "${QT_QTBASE_SOURCE}/src/3rdparty/sqlite"
    )
    zephyr_link_libraries("-Wl,--wrap=lseek" "-Wl,--wrap=read")
endif()

if(CONFIG_QT_NETWORK)
    _qza_module_private_dir(QtNetwork _qza_network_private)
    target_include_directories(app PRIVATE
        "${QT_ZEPHYR_PREFIX}/include/QtNetwork"
        "${_qza_network_private}"
        "${_qza_network_private}/QtNetwork"
    )
    target_link_libraries(app PRIVATE
        "${QT_ZEPHYR_PREFIX}/lib/libQt6Network.a"
    )
    target_sources(app PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../src/qzephyr_network_stubs.c"
        "${CMAKE_CURRENT_LIST_DIR}/../src/qzephyr_poll.cpp"
    )
    if(CONFIG_QT_TLS)
        target_sources(app PRIVATE
            "${CMAKE_CURRENT_LIST_DIR}/../src/qzephyr_tls_socket.cpp"
        )
        target_include_directories(app PRIVATE
            "${CMAKE_CURRENT_LIST_DIR}/../src"
        )
    endif()
    if(CONFIG_QT_WOLFSSL)
        target_link_libraries(app PRIVATE
            "${QT_ZEPHYR_PREFIX}/plugins/tls/libqwolfsslbackend.a"
            "${QT_ZEPHYR_PREFIX}/plugins/tls/objects-Release/QTlsBackendWolfSSLPlugin_init/QTlsBackendWolfSSLPlugin_init.cpp.obj"
            "$ENV{HOME}/com/wolfssl/wolfssl/.install-arm/lib/libwolfssl.a"
        )
        target_sources(app PRIVATE
            "${CMAKE_CURRENT_LIST_DIR}/../src/qzephyr_wolfssl_plugin_import.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/../src/qzephyr_wolfssl_seed.c"
        )
        target_include_directories(app PRIVATE
            "$ENV{HOME}/com/wolfssl/wolfssl/.install-arm/include"
        )
        target_compile_definitions(app PRIVATE WOLFSSL_USER_SETTINGS)
    endif()
    if(CONFIG_QT_MQTT)
        set(_mqtt_src "$ENV{HOME}/io/qt/release/6.11.1/Src/qtmqtt/src/mqtt")
        set(_mqtt_inc "${CMAKE_BINARY_DIR}/qtmqtt_inc")
        file(MAKE_DIRECTORY "${_mqtt_inc}/QtMqtt")
        file(GLOB _mqtt_hdrs "${_mqtt_src}/*.h")
        foreach(_h ${_mqtt_hdrs})
            get_filename_component(_name "${_h}" NAME)
            if(NOT EXISTS "${_mqtt_inc}/QtMqtt/${_name}")
                file(CREATE_LINK "${_h}" "${_mqtt_inc}/QtMqtt/${_name}" SYMBOLIC)
            endif()
        endforeach()
        file(WRITE "${_mqtt_inc}/QtMqtt/qtmqttexports.h"
            "#pragma once\n#define Q_MQTT_EXPORT\n#define Q_MQTT_PRIVATE_EXPORT\n")
        target_sources(app PRIVATE
            "${_mqtt_src}/qmqttauthenticationproperties.cpp"
            "${_mqtt_src}/qmqttclient.cpp"
            "${_mqtt_src}/qmqttconnection.cpp"
            "${_mqtt_src}/qmqttconnectionproperties.cpp"
            "${_mqtt_src}/qmqttcontrolpacket.cpp"
            "${_mqtt_src}/qmqttmessage.cpp"
            "${_mqtt_src}/qmqttpublishproperties.cpp"
            "${_mqtt_src}/qmqttsubscription.cpp"
            "${_mqtt_src}/qmqttsubscriptionproperties.cpp"
            "${_mqtt_src}/qmqtttopicfilter.cpp"
            "${_mqtt_src}/qmqtttopicname.cpp"
            "${_mqtt_src}/qmqtttype.cpp"
        )
        target_include_directories(app PRIVATE "${_mqtt_src}" "${_mqtt_inc}")
    endif()
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
