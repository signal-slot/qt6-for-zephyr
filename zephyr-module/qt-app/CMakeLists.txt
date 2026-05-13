# Generic Qt-on-Zephyr application skeleton bundled inside the
# qt-zephyr-port module.  Lets `west build` consume an unmodified Qt 6
# application directory (any project that uses qt_add_executable() +
# qt_add_qml_module() etc.; Qt SDK Examples are one obvious case but
# the same path works for in-house Qt apps).  The user writes ZERO
# CMakeLists.txt / prj.conf files of their own, the same way the
# standard Qt-for-embedded-Linux workflow keeps stock Qt sources.
#
# Usage:
#
#   export ZEPHYR_BASE=...          # Zephyr source tree
#   export QT_ZEPHYR_PREFIX=...     # Cortex-M7 Qt install root
#   export ZEPHYR_EXTRA_MODULES=... # qt-zephyr-port module root
#
#   west build -b <board> $ZEPHYR_EXTRA_MODULES/qt-app             \
#       -- [-DSHIELD=<shield>]                                     \
#          -DQT_APP_DIR=/path/to/your/qt-app
#
# QT_APP_DIR points at any existing Qt application source directory --
# something that runs as-is on desktop / embedded Linux today.  No
# files inside it are modified.  Per-board prj.conf and devicetree
# overlay are picked up automatically from boards/<board>/ below.

cmake_minimum_required(VERSION 3.24.0)  # $<LINK_LIBRARY:WHOLE_ARCHIVE,...>

# Register the WHOLE_ARCHIVE link-library feature for the C/CXX
# linkers BEFORE Zephyr's CMake module runs.  CMake auto-registers it
# on Apple/Linux/Solaris but Zephyr forces CMAKE_SYSTEM_NAME=Generic,
# leaving WHOLE_ARCHIVE unsupported even though the underlying GNU
# arm-zephyr-eabi ld supports --whole-archive identically to Linux.
# qt_zephyr_app() needs it to wrap the Qt app's static library so
# main.cpp.o and the QML / moc / qmlcache / plugin-import .o files
# are actually pulled into the firmware (a normal static-archive
# search would skip them because Zephyr's __weak main(void) keeps
# the app's strong main from creating an unresolved-strong ref).
foreach(_lang C CXX)
    set(CMAKE_${_lang}_LINK_LIBRARY_USING_WHOLE_ARCHIVE
        "LINKER:--whole-archive"
        "<LINK_ITEM>"
        "LINKER:--no-whole-archive")
    set(CMAKE_${_lang}_LINK_LIBRARY_USING_WHOLE_ARCHIVE_SUPPORTED TRUE)
endforeach()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(qt_app LANGUAGES CXX C)

if(NOT DEFINED QT_APP_DIR)
    if(DEFINED ENV{QT_APP_DIR})
        set(QT_APP_DIR "$ENV{QT_APP_DIR}")
    else()
        message(FATAL_ERROR
            "QT_APP_DIR is not set.  Pass -DQT_APP_DIR=/path/to/your/qt-app "
            "(or export QT_APP_DIR=...) to point at the Qt application "
            "source directory you want to build into this firmware.")
    endif()
endif()

list(APPEND CMAKE_MODULE_PATH "${ZEPHYR_QT_ZEPHYR_PORT_MODULE_DIR}/cmake")
include(QtZephyrApp)
qt_zephyr_app(APP_DIR "${QT_APP_DIR}")
