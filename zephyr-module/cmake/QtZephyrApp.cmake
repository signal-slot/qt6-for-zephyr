# qt_zephyr_app() -- turn an existing Qt 6 application source
# directory into a Zephyr `app` target without modifying that app's
# own CMakeLists.txt, and without the qt-zephyr-port module having
# to maintain a per-app list of Q_IMPORT_PLUGIN macros.
#
# Strategy:
#
#   1. Override `qt_add_executable` BEFORE add_subdirectory() so the
#      Qt app's `qt_add_executable(name main.cpp)` becomes a STATIC
#      library named `name`.  The library inherits Zephyr's
#      `zephyr_interface` (Cortex-M7 / __ZEPHYR__ / include paths /
#      defines) so all the app's TUs compile correctly the first
#      time, and it has no link step of its own so the duplicate
#      `picolibc.ld` / missing-_start problems Zephyr's executable
#      LINK_OPTIONS would otherwise cause never arise.
#
#   2. Override `qt_add_qml_module` to inject NO_PLUGIN.  Qt's
#      default for non-executable backing targets auto-creates a
#      separate <target>plugin target that ends up redundant for a
#      static lib linked into a single firmware AND that wouldn't
#      inherit Zephyr's compile flags anyway.  With NO_PLUGIN the
#      QML types stay registered through the backing library itself.
#
#   3. Let the app's CMakeLists.txt run verbatim via add_subdirectory().
#      Qt's own qt_add_qml_module() machinery fires inside the app's
#      scope, producing AUTOMOC output, qmlcache loaders, qmltype
#      registrations etc.
#
#   4. After add_subdirectory(), propagate `zephyr_interface` to every
#      target the app's CMakeLists.txt chain created -- not only the
#      qt_add_executable target.  Apps that build helper static /
#      OBJECT libraries (Qt's widgets/painting/shared is one example,
#      most in-house Qt apps have similar helper libraries) need
#      Cortex-M7 / __ZEPHYR__ flags propagated to those TUs too.
#
#   5. Call qt_import_qml_plugins() on the app target so Qt's own
#      QML import scanner reads the app's qt_add_qml_module(URI ...)
#      chain, generates <target>_qml_plugin_imports.cpp with the
#      right Q_IMPORT_PLUGIN() macros, and links Qt6::<plugin> +
#      Qt6::<plugin>_init + Qt6::<plugin>_resources_N IMPORTED
#      targets onto the app's library.  qt_add_executable's normal
#      DEFER-finalization would do this but our STATIC override
#      skips that path.
#
#   6. target_link_libraries(app PRIVATE <app_library>) with
#      WHOLE_ARCHIVE.  That brings into Zephyr's `app` target:
#        - main.cpp.o (overrides Zephyr's weak main())
#        - AUTOMOC mocs_compilation.cpp.o
#        - qmltyperegistrations.cpp.o
#        - per-file QML caches + qmlcache loader
#        - <target>_qml_plugin_imports.cpp.o (auto-discovered, NOT
#          hand-maintained)
#        - Transitive Qt6:: framework + per-plugin link deps
#
# Adding a Qt app that imports new QML modules (Imagine, Material,
# Particles, anything else) requires ZERO changes to qt-zephyr-port
# -- Qt's own scanner picks it up.

include_guard(GLOBAL)

# Recursively walk a directory's BUILDSYSTEM_TARGETS (and the
# SUBDIRECTORIES list it propagates) and attach zephyr_interface to
# every buildable target.  Lets a Qt app's add_subdirectory chain
# (helper static / OBJECT libraries that share .cpp / qrc with the
# main executable) inherit Cortex-M7 / __ZEPHYR__ flags without
# editing the app's CMakeLists.txt.
function(_qt_zephyr_app_apply_zephyr_to_subdir _qzas_dir)
    get_directory_property(_qzas_targets DIRECTORY "${_qzas_dir}" BUILDSYSTEM_TARGETS)
    foreach(_qzas_t IN LISTS _qzas_targets)
        if(NOT TARGET ${_qzas_t})
            continue()
        endif()
        get_target_property(_qzas_type ${_qzas_t} TYPE)
        if(_qzas_type STREQUAL "EXECUTABLE"
                OR _qzas_type STREQUAL "STATIC_LIBRARY"
                OR _qzas_type STREQUAL "OBJECT_LIBRARY"
                OR _qzas_type STREQUAL "SHARED_LIBRARY"
                OR _qzas_type STREQUAL "MODULE_LIBRARY")
            target_link_libraries(${_qzas_t} PRIVATE zephyr_interface)
        endif()
    endforeach()
    get_directory_property(_qzas_subdirs DIRECTORY "${_qzas_dir}" SUBDIRECTORIES)
    foreach(_qzas_sub IN LISTS _qzas_subdirs)
        _qt_zephyr_app_apply_zephyr_to_subdir("${_qzas_sub}")
    endforeach()
endfunction()

function(qt_zephyr_app)
    cmake_parse_arguments(QZA "" "APP_DIR;TARGET" "" ${ARGN})

    if(NOT QZA_APP_DIR)
        message(FATAL_ERROR "qt_zephyr_app: APP_DIR is required.")
    endif()

    # Auto-detect the Qt app's executable target name from its
    # qt_add_executable() / add_executable() call.
    if(NOT QZA_TARGET)
        if(NOT EXISTS "${QZA_APP_DIR}/CMakeLists.txt")
            message(FATAL_ERROR
                "qt_zephyr_app: ${QZA_APP_DIR}/CMakeLists.txt does not "
                "exist -- check QT_APP_DIR.")
        endif()
        file(READ "${QZA_APP_DIR}/CMakeLists.txt" _qza_app_cml)
        if(_qza_app_cml MATCHES "(qt_add_executable|add_executable)[ \t]*\\([ \t\r\n]*([A-Za-z0-9_-]+)")
            set(QZA_TARGET "${CMAKE_MATCH_2}")
            message(STATUS
                "qt_zephyr_app: auto-detected app target '${QZA_TARGET}' "
                "from ${QZA_APP_DIR}/CMakeLists.txt")
        else()
            message(FATAL_ERROR
                "qt_zephyr_app: could not auto-detect a target name in "
                "${QZA_APP_DIR}/CMakeLists.txt -- pass TARGET <name> "
                "explicitly.")
        endif()
    endif()

    if(NOT DEFINED ENV{QT_ZEPHYR_PREFIX})
        message(FATAL_ERROR "qt_zephyr_app: QT_ZEPHYR_PREFIX must be set.")
    endif()
    list(APPEND CMAKE_PREFIX_PATH "$ENV{QT_ZEPHYR_PREFIX}")
    set(CMAKE_PREFIX_PATH "${CMAKE_PREFIX_PATH}" PARENT_SCOPE)

    # find_package(Qt6) at wrapper scope so Qt6::* imported targets are
    # globally visible -- needed so the generator expressions Qt bakes
    # into the app's link list (e.g. $<TARGET_PROPERTY:Qt6::Qml,...>)
    # can resolve when finally linked into `app`.  Component set is
    # gated by CONFIG_QT_* so a Core-only firmware does not pull QtQuick
    # in just because qt_zephyr_app() runs.
    set(_qza_components Core)
    if(CONFIG_QT_GUI)
        list(APPEND _qza_components Gui)
    endif()
    if(CONFIG_QT_QML)
        list(APPEND _qza_components Qml Quick)
    endif()
    if(CONFIG_QT_QUICK_CONTROLS)
        list(APPEND _qza_components QuickControls2)
    endif()
    if(CONFIG_QT_SVG)
        list(APPEND _qza_components Svg)
    endif()
    if(CONFIG_QT_WIDGETS)
        list(APPEND _qza_components Widgets)
    endif()
    if(CONFIG_QT_QUICK_WIDGETS)
        list(APPEND _qza_components QuickWidgets)
    endif()
    if(CONFIG_QT_XML)
        list(APPEND _qza_components Xml)
    endif()
    if(CONFIG_QT_NETWORK)
        list(APPEND _qza_components Network)
    endif()
    if(CONFIG_QT_MULTIMEDIA)
        list(APPEND _qza_components Multimedia)
    endif()
    find_package(Qt6 REQUIRED COMPONENTS ${_qza_components})

    # ----- override qt_add_executable ----------------------------------
    function(qt_add_executable _qza_target)
        set(_qza_options
            WIN32 MACOSX_BUNDLE
            MANUAL_FINALIZATION NO_GENERATE_DEPLOY_SUPPORT NO_UNITY_BUILD)
        cmake_parse_arguments(_qza_arg "${_qza_options}" "" "" ${ARGN})
        qt_add_library(${_qza_target} STATIC ${_qza_arg_UNPARSED_ARGUMENTS})
        # zephyr_interface propagation happens after add_subdirectory,
        # over every target the Qt app (and its helper add_subdirectory
        # chains) created.
    endfunction()

    # ----- override qt_add_qml_module ----------------------------------
    # When the backing target is STATIC (the case we set up above) and
    # the app did not pass NO_PLUGIN, Qt auto-creates a separate
    # <target>plugin target for the QML module.  That target lives
    # outside `app`'s compile graph and so misses Zephyr's flags, and
    # ends up redundant anyway because the static backing library is
    # already linked into the single firmware.  Inject NO_PLUGIN so
    # the QML types stay registered through the backing library.
    function(qt_add_qml_module _qaqm_target)
        set(_qaqm_args "${ARGN}")
        if(NOT "NO_PLUGIN" IN_LIST _qaqm_args)
            list(APPEND _qaqm_args NO_PLUGIN)
        endif()
        qt6_add_qml_module(${_qaqm_target} ${_qaqm_args})
    endfunction()

    # Process the Qt app verbatim.
    add_subdirectory(${QZA_APP_DIR} qt_app_build)

    if(NOT TARGET ${QZA_TARGET})
        message(FATAL_ERROR
            "qt_zephyr_app: after processing ${QZA_APP_DIR} no target "
            "named '${QZA_TARGET}' exists.")
    endif()

    # Propagate Zephyr's INTERFACE compile flags / include paths /
    # defines (zephyr_interface) to *every* target the Qt app's
    # CMakeLists.txt chain created -- not only the qt_add_executable
    # target.  Apps that build their own helper static / OBJECT
    # libraries (Qt's widgets/painting/shared/painting_shared is one
    # example) would otherwise compile those TUs with the host's
    # flags and fail in qprocessordetection.h.  Walk the
    # BUILDSYSTEM_TARGETS of the qt_app_build subdirectory (and any
    # nested directories it add_subdirectory'd) and apply
    # zephyr_interface to each buildable target type.
    _qt_zephyr_app_apply_zephyr_to_subdir("${CMAKE_CURRENT_BINARY_DIR}/qt_app_build")

    # Run Qt's QML import scanner now (when QML is in use).
    # qt_add_executable normally cmake_language(DEFER)s a call to
    # _qt_internal_finalize_executable which would do this, but our
    # STATIC-library override doesn't set up that deferred call.
    # Calling qt_import_qml_plugins explicitly produces
    # <target>_qml_plugin_imports.cpp + links the right Qt6::<plugin>
    # + _init + _resources_N IMPORTED targets onto the app library.
    # For Core/Widgets-only firmware (no QtQml in find_package), skip:
    # qt_import_qml_plugins requires Qt6::Qml to exist.
    if(CONFIG_QT_QML)
        qt_import_qml_plugins(${QZA_TARGET})
    endif()

    # The SVG image-format plugin is a runtime *plugin* (loaded by
    # QImageReader via the static-plugin registry), not a QML import.
    # qt_import_qml_plugins() will not pick it up.  When CONFIG_QT_SVG
    # is on, link Qt6::QSvgPlugin + its _init OBJECT sibling onto the
    # app so QGuiApplication can decode .svg / .svgz assets at
    # runtime.
    #
    # Guard each plugin target individually with TARGET-existence
    # checks: the icon-engine flavour Qt6::QSvgIconPlugin is created
    # only when QtSvg is built with the iconengines plugin set, which
    # is not guaranteed across all Stage 1 configurations.
    if(CONFIG_QT_SVG)
        if(TARGET Qt6::QSvgPlugin)
            target_link_libraries(${QZA_TARGET} PRIVATE
                Qt6::QSvgPlugin Qt6::QSvgPlugin_init)
        endif()
        if(TARGET Qt6::QSvgIconPlugin)
            target_link_libraries(${QZA_TARGET} PRIVATE
                Qt6::QSvgIconPlugin Qt6::QSvgIconPlugin_init)
        endif()
    endif()

    # Pull the app static library into Zephyr's `app`.  WHOLE_ARCHIVE
    # is mandatory because:
    #   - main() (strong) in the app would otherwise be skipped by
    #     the linker -- Zephyr already provides a __weak main(void)
    #     stub, so the app's main.cpp.o does not resolve any
    #     unresolved-strong reference and a normal static-archive
    #     search leaves it out, and the firmware boots into Zephyr's
    #     do-nothing main rather than into QGuiApplication.
    #   - Q_OBJECT static initializers + qt_static_plugin_*() helpers
    #     are referenced only from .init_array constructor entries,
    #     which similarly fall outside the unresolved-strong set.
    # CMake's $<LINK_LIBRARY:WHOLE_ARCHIVE,...> generator expression
    # (requires CMake 3.24+) brackets the target with the right
    # --whole-archive / --no-whole-archive linker flags without
    # leaking into adjacent libraries the way a hand-rolled mix would
    # after target_link_libraries de-duplication.
    # Zero-touch official demos (e.g. demos/coffee) do QGuiApplication(argc,argv)
    # with main()'s argc/argv, which Zephyr leaves as stack garbage -> the ctor
    # faults.  Rename the app's main() to qt_zephyr_user_main() and supply a real
    # Zephyr main(void) (qt_zephyr_main_shim.cpp) that calls it with a synthetic
    # argv.  The shim is C++ so its qt_zephyr_user_main() reference name-mangles
    # the same way as the app's (now ordinary, no longer the special "main") C++
    # function.  Apps that build their own synthetic argv just ignore what we pass.
    target_compile_definitions(${QZA_TARGET} PRIVATE "main=qt_zephyr_user_main")
    target_sources(app PRIVATE
        "${ZEPHYR_QT_ZEPHYR_PORT_MODULE_DIR}/src/qt_zephyr_main_shim.cpp")

    target_link_libraries(app PRIVATE
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,${QZA_TARGET}>"
    )
endfunction()
