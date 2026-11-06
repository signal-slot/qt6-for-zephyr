#!/bin/bash
# Cross-build Qt6 (Core + Gui + Widgets + Qml + Quick + Svg) for the
# Raspberry Pi 4 (BCM2711, Cortex-A72) as Stage 1 static libraries.
#
# Derived from build-qt-zephyr-am62p.sh (the AArch64 Stage 1; board
# independent).  Differences:
#   - -mcpu=cortex-a72 (what Zephyr uses for rpi_4b);
#   - by default no OpenGL / EGL: Qt draws with the CPU (QPainter, Qt Quick
#     software scene graph), as in build-qt-zephyr-rt1170.sh.
#   - WITH_GL=1: OpenGL ES 2.0 + EGL for the board's V3D GPU through YakoGL
#     (github.com/signal-slot/yakogl; YAKOGL_DIR, default ~/org/yakogl).  As in
#     build-qt-zephyr-am62p.sh, Qt is built against YakoGL's headers and two
#     empty stand-in archives; the real GL/EGL symbols come from YakoGL's
#     Zephyr module in Stage 2 (zephyr-module/qt-app/boards/
#     rpi_4b_bcm2711_gl.conf).  The default PREFIX and BUILD_DIR get a "-gl"
#     suffix, so the two builds do not mix.  The GL build also needs the
#     qtbase change that gives QZephyrScreen a 32-bit depth for
#     Format_RGBX8888 (without it the screen reports 16 bit and the EGL
#     window surface is refused).
# Stage 2 must select CONFIG_PICOLIBC=y (the SDK ships no newlib for AArch64).
# The caveats at the top of build-qt-zephyr-am62p.sh (architecture_test, the
# Zephyr event dispatcher) apply here too.
#
# PREFIX is not cleaned: `cmake --install` leaves files from an earlier run
# in place (a stale libqtiff.a and its CMake files made the Stage 2 link fail
# once).  Remove the prefix, or use a new one, when options change.

set -e

WITH_GL="${WITH_GL:-0}"
case "$WITH_GL" in
    0|1) ;;
    *) echo "Error: WITH_GL must be 0 or 1 (got '$WITH_GL')" >&2; exit 1 ;;
esac
GL_SUFFIX=""
[ "$WITH_GL" = "1" ] && GL_SUFFIX="-gl"
PREFIX="${PREFIX:-${HOME}/qt-zephyr-rpi4${GL_SUFFIX}}"
QT_SOURCE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/build-qt-zephyr-rpi4${GL_SUFFIX}}"
YAKOGL_DIR="${YAKOGL_DIR:-${HOME}/org/yakogl}"
ZEPHYR_SDK_INSTALL_DIR="${ZEPHYR_SDK_INSTALL_DIR:-${HOME}/org/zephyrproject/zephyr-sdk-1.0.1}"
ZEPHYR_BASE="${ZEPHYR_BASE:-${HOME}/org/zephyrproject/zephyrproject/zephyr}"
QT_HOST_PATH="${QT_HOST_PATH:-${HOME}/qt-x11-sim}"
GREEN='\033[0;32m'; RED='\033[0;31m'; NC='\033[0m'
info()  { echo -e "${GREEN}Info:${NC} $*"; }
error() { echo -e "${RED}Error:${NC} $*" >&2; exit 1; }

[ -d "$QT_SOURCE_DIR" ] || error "Qt source not found at $QT_SOURCE_DIR"
# SDK 1.0.x moved toolchains under <SDK>/gnu/<arch>/.  Fall back to the
# pre-1.0 layout if someone points at an older SDK.
if [ -d "$ZEPHYR_SDK_INSTALL_DIR/gnu/aarch64-zephyr-elf" ]; then
    TOOLCHAIN_ROOT="$ZEPHYR_SDK_INSTALL_DIR/gnu/aarch64-zephyr-elf"
elif [ -d "$ZEPHYR_SDK_INSTALL_DIR/aarch64-zephyr-elf" ]; then
    TOOLCHAIN_ROOT="$ZEPHYR_SDK_INSTALL_DIR/aarch64-zephyr-elf"
else
    error "Zephyr SDK AArch64 toolchain not found under $ZEPHYR_SDK_INSTALL_DIR (checked gnu/aarch64-zephyr-elf and aarch64-zephyr-elf)"
fi

[ -d "$ZEPHYR_BASE/include/zephyr" ] \
    || error "Zephyr source headers not found under $ZEPHYR_BASE/include/zephyr"
[ -d "$QT_HOST_PATH" ] || error "QT_HOST_PATH (host Qt6 tools) not found at $QT_HOST_PATH"

mkdir -p "$BUILD_DIR"

GL_TOOLCHAIN_INC=""
GL_OPTS=(
    # No GPU: raster path only (same settings as build-qt-zephyr-rt1170.sh).
    -DFEATURE_opengl=OFF
    -DFEATURE_opengles2=OFF
    -DFEATURE_opengles3=OFF
    -DFEATURE_egl=OFF
    # INPUT_opengl=no disables OpenGL auto-detection (no desktop/dynamic GL
    # probing).
    -DINPUT_opengl=no
)
if [ "$WITH_GL" = "1" ]; then
    [ -f "$YAKOGL_DIR/include/GLES2/gl2.h" ] && [ -f "$YAKOGL_DIR/include/EGL/egl.h" ] \
        || error "YAKOGL_DIR ($YAKOGL_DIR) has no include/GLES2/gl2.h + include/EGL/egl.h"
    # shellcheck source=zephyr-module/tools/rpi4-pins.sh
    . "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")/zephyr-module/tools/rpi4-pins.sh"
    rpi4_check_pin YakoGL "$YAKOGL_DIR" "$RPI4_YAKOGL_COMMIT" || exit 1
    # The GL "SDK" Qt's FindGLESv2 / FindEGL modules see: YakoGL's Khronos
    # headers and two empty archives named libGLESv2.a / libEGL.a (Qt only
    # needs the files to exist; every gl*/egl* symbol is provided in Stage 2
    # by YakoGL's Zephyr module).  Installed under the prefix so Qt6's
    # exported EGL::EGL / GLESv2::GLESv2 targets keep resolving from any
    # Stage 2 build tree.
    GL_SDK="$PREFIX/yakogl-sdk"
    # Qt 6.12: the precompiled header of the QuickLayouts plugin includes
    # <QtGui/QtGui> and with it qopengl.h (GLES2/gl2.h), but the plugin target
    # does not inherit the GL include directory (the compile failed with
    # "GLES2/gl2.h: No such file or directory"), so the directory goes to
    # every target through the toolchain file.
    GL_TOOLCHAIN_INC="include_directories(SYSTEM \"$GL_SDK/include\")"
    mkdir -p "$GL_SDK/lib" "$GL_SDK/include"
    cp -r "$YAKOGL_DIR/include/." "$GL_SDK/include/"
    : > "$BUILD_DIR/yakogl_stub.c"
    "$TOOLCHAIN_ROOT/bin/aarch64-zephyr-elf-gcc" -c "$BUILD_DIR/yakogl_stub.c" -o "$BUILD_DIR/yakogl_stub.o"
    for lib in GLESv2 EGL; do
        rm -f "$GL_SDK/lib/lib$lib.a"
        "$TOOLCHAIN_ROOT/bin/aarch64-zephyr-elf-ar" rcs "$GL_SDK/lib/lib$lib.a" "$BUILD_DIR/yakogl_stub.o"
    done
    GL_OPTS=(
        # OpenGL ES 2.0 + EGL: Qt's GLES2 RHI backend (Qt Quick's default
        # scene graph renderer) and the qzephyr QPA's EGL platform context.
        -DFEATURE_opengl=ON
        -DFEATURE_opengles2=ON
        -DFEATURE_opengles3=OFF
        -DFEATURE_egl=ON
        -DGLESv2_INCLUDE_DIR="$GL_SDK/include"
        -DGLESv2_LIBRARY="$GL_SDK/lib/libGLESv2.a"
        -DEGL_INCLUDE_DIR="$GL_SDK/include"
        -DEGL_LIBRARY="$GL_SDK/lib/libEGL.a"
        # INPUT_opengl=es2 pins the OpenGL flavour (no desktop/dynamic GL
        # probing against the stand-in library).
        -DINPUT_opengl=es2
    )
fi

info "Configuring Qt6 cross-build for Raspberry Pi 4 (Cortex-A72), WITH_GL=$WITH_GL..."
info "  source:      $QT_SOURCE_DIR"
info "  build:       $BUILD_DIR"
info "  prefix:      $PREFIX"
info "  Zephyr SDK:  $ZEPHYR_SDK_INSTALL_DIR"
info "  ZEPHYR_BASE: $ZEPHYR_BASE"
info "  QT_HOST_PATH:$QT_HOST_PATH"
if [ "$WITH_GL" = "1" ]; then
    info "  YAKOGL_DIR:  $YAKOGL_DIR"
elif [ -e "$PREFIX/yakogl-sdk/lib/libEGL.a" ]; then
    # A GL install left here: Stage 2's OpenGL check looks for that file, and
    # would pass against a prefix that has no GL-enabled Qt libraries.
    info "WARNING: $PREFIX has yakogl-sdk/ from a WITH_GL=1 run; use another PREFIX or remove it"
fi

cat > "$BUILD_DIR/zephyr-rpi4-toolchain.cmake" <<EOF
# CMake toolchain file for Qt6 cross-build targeting the
# Raspberry Pi 4 (BCM2711, Cortex-A72, AArch64).  Generated by
# build-qt-zephyr-rpi4.sh.

set(CMAKE_SYSTEM_NAME Zephyr)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_SYSTEM_VERSION 1)

set(TOOLCHAIN_BIN "$TOOLCHAIN_ROOT/bin")
set(CMAKE_C_COMPILER   \${TOOLCHAIN_BIN}/aarch64-zephyr-elf-gcc)
set(CMAKE_CXX_COMPILER \${TOOLCHAIN_BIN}/aarch64-zephyr-elf-g++)
set(CMAKE_ASM_COMPILER \${TOOLCHAIN_BIN}/aarch64-zephyr-elf-gcc)
set(CMAKE_AR           \${TOOLCHAIN_BIN}/aarch64-zephyr-elf-ar)
set(CMAKE_RANLIB       \${TOOLCHAIN_BIN}/aarch64-zephyr-elf-ranlib)
set(CMAKE_NM           \${TOOLCHAIN_BIN}/aarch64-zephyr-elf-nm)
set(CMAKE_OBJDUMP      \${TOOLCHAIN_BIN}/aarch64-zephyr-elf-objdump)
set(CMAKE_OBJCOPY      \${TOOLCHAIN_BIN}/aarch64-zephyr-elf-objcopy)

# Cortex-A72, the same CPU/ABI flags Zephyr's arch/arm64 build uses
# (rpi_4b: -mcpu=cortex-a72).  No --specs=picolibc.specs: the SDK's AArch64
# gcc already defaults to its picolibc (headers, archives and linker
# script), and naming the specs again makes the architecture_test link
# fail with "picolibc.ld appears multiple times".
set(CPU_FLAGS "-mcpu=cortex-a72 -mabi=lp64")

# Static libraries for a bare-metal MCU target must NOT be PIC.  CMake's
# default Qt build emits -fPIC for libraries; Zephyr links the final ELF
# with -fno-pic -fno-pie, which leaves any PIC relocations from Stage 1
# unresolvable against the linker's discarded crtbegin.o .got.plt
# section.  Force position-DEPENDENT code globally.
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)
add_compile_options(-fno-pic -fno-pie)
add_link_options(-fno-pic -fno-pie)
# Note: dropped -ffreestanding.  Zephyr apps have a real libc + heap so
# they are hosted, not freestanding.  gcc 14's libstdc++ guards every
# hosted-only header (qsimd.cpp pulls in <bits/requires_hosted.h>) with
# `#error "This header is not available in freestanding mode."`

# gcc 14 (Zephyr SDK 1.0.1) promotes several K&R-era warnings to errors
# by default.  Bundled 3rd-party C (PCRE2, zlib) trips on these.  Demote
# back to warnings for the cross build.
add_compile_options(
    -Wno-error=incompatible-pointer-types
    -Wno-error=implicit-function-declaration
    -Wno-error=implicit-int
    -Wno-error=int-conversion
)
# qharfbuzzng.cpp:512 assigns hb_codepoint_t* (unsigned long*) to glyph_t*
# (unsigned int*).  Both are 32-bit on ARM-M but the C++ type checker
# doesn't know that.  -fpermissive demotes the strict conversion error to
# a warning -- the assignment is binary-safe at this CPU width.
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fpermissive>)

# Define __ZEPHYR__ so qtbase/src/corelib/global/qsystemdetection.h
# picks the Q_OS_ZEPHYR branch we patched in.  Stage 1 deliberately does
# NOT include <zephyr/*> headers (that comes in Stage 2 inside west build),
# so we don't need CONFIG_* macros or stub generated headers.
add_compile_definitions(
    __ZEPHYR__=1
    # picolibc gates clock_gettime & co. on the POSIX visibility macros
    # (sys/features.h); expose POSIX 2008 + the BSD-derived legacy APIs
    # (putenv, posix_memalign, ...) Qt corelib reaches for unconditionally.
    _POSIX_TIMERS=1
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE=1
    # HarfBuzz: the SDK libstdc++ has no std::mutex (single-threaded
    # build), and we have no pthread.  HB_NO_MT switches its mutex impl
    # to a no-op stub (typedef int).
    HB_NO_MT
)

# Stage 1 POSIX shim headers for the handful of headers that Qt corelib
# unconditionally pulls in but picolibc does not provide (pthread.h,
# sys/utsname.h, sys/mman.h).  Placed BEFORE the SDK's system include
# path so the shims take precedence for these specific headers without
# affecting any other resolution.
include_directories(BEFORE SYSTEM "$QT_SOURCE_DIR/qtbase/mkspecs/zephyr-aarch64-g++/stage1-shim")
$GL_TOOLCHAIN_INC
set(CMAKE_C_FLAGS_INIT   "\${CPU_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "\${CPU_FLAGS}")
set(CMAKE_ASM_FLAGS_INIT "\${CPU_FLAGS}")

# try_compile() default target: static lib avoids link-time unresolved
# symbol failures for simple per-feature checks.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# QtBaseConfigureTests.cmake builds the architecture_test as an executable
# via try_compile in project mode, which ignores CMAKE_TRY_COMPILE_TARGET_TYPE.
# Allow the link to "succeed" with newlib stubs unresolved; the binary is
# never run, only grepped for the embedded magic-string architecture tag.
# picolibc's default linker script sizes its "flash" region for a tiny
# MCU (64 KiB) and the architecture_test's .rodata does not fit; the
# script takes the region sizes from these symbols when they are defined.
# Only Stage 1's throw-away test executables are linked with this: the
# firmware is linked by Zephyr with its own script in Stage 2.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,--unresolved-symbols=ignore-all -Wl,--defsym=__flash_size=0x2000000 -Wl,--defsym=__ram_size=0x4000000")

# Tell Qt this is a Zephyr build so qt_set01(ZEPHYR ...) yields 1 and the
# qzephyr plugin subdirectory + corelib's qeventdispatcher_zephyr.cpp are
# added to the build.  ZEPHYR_BASE also gets propagated to the plugin's
# include path (see qtbase/src/plugins/platforms/zephyr/CMakeLists.txt:28).
set(ZEPHYR_BASE "$ZEPHYR_BASE")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Bypass architecture autodetect: try_compile cannot run the resulting
# binary on the host, so tell Qt what we are (AArch64 LP64 little-endian;
# NEON is baseline on ARMv8-A and needs no subarch flag).
set(TEST_architecture_arch "arm64" CACHE STRING "")
set(TEST_subarch_result "" CACHE STRING "")
set(TEST_buildAbi "arm64-little_endian-lp64" CACHE STRING "")
EOF

CMAKE_OPTS=(
    -DCMAKE_INSTALL_PREFIX="$PREFIX"
    -DCMAKE_TOOLCHAIN_FILE="${BUILD_DIR}/zephyr-rpi4-toolchain.cmake"
    -DCMAKE_BUILD_TYPE=Release
    -DBUILD_SHARED_LIBS=OFF
    -DFEATURE_static=ON
    -DQT_HOST_PATH="$QT_HOST_PATH"
    # Stage 1 cross-build uses an AArch64-specific mkspec whose
    # qplatformdefs.h deliberately does NOT include <zephyr/kernel.h>
    # or <zephyr/posix/*>.  Those headers require Zephyr's Kconfig
    # autoconf.h / generated syscall stubs, which only exist inside
    # west build (Stage 2).
    -DQT_QMAKE_TARGET_MKSPEC=zephyr-aarch64-g++
    # Defer Zephyr-runtime-aware Qt code (corelib's qeventdispatcher_zephyr
    # and the qzephyr QPA plugin sources) to Stage 2.  See
    # qtbase/src/corelib/CMakeLists.txt:1465 for the condition.
    -DQT_DEFER_ZEPHYR_RUNTIME=ON

    # Tier 6 scope: Core + Gui + Widgets (QApplication + QTabWidget +
    # QPushButton + ... for the Qt-official Widgets Gallery demo).
    -DFEATURE_gui=ON
    -DFEATURE_widgets=ON
    # PrintSupport ships moc-generated code that references a private
    # QPrintDialogPrivate but the corresponding cpp is not compiled in
    # the bare-build configuration -- we don't need printing on an MCU.
    -DFEATURE_printsupport=OFF

    # FreeType: needed for any text rendering on the qzephyr backing
    # store (no system font server on the MCU).  Use the bundled copy
    # that ships inside qtbase rather than the nonexistent system one.
    -DFEATURE_freetype=ON
    -DFEATURE_system_freetype=OFF
    # HarfBuzz: complex-script shaping the gallery / qmlsample examples
    # need on the host build too -- use the bundled fork.
    -DFEATURE_harfbuzz=ON
    -DFEATURE_system_harfbuzz=OFF
    # PNG: bundle for QImage::load() in the demos.  zlib is already
    # bundled by qtbase.
    -DFEATURE_png=ON
    -DFEATURE_system_png=OFF

    # Disable features that need OS/host services we don't have on Zephyr.
    -DFEATURE_concurrent=OFF
    -DFEATURE_dbus=OFF
    -DFEATURE_dlopen=OFF
    -DFEATURE_eventfd=OFF
    -DFEATURE_filesystemwatcher=OFF
    -DFEATURE_fontconfig=OFF
    -DFEATURE_future=OFF
    -DFEATURE_glib=OFF
    -DFEATURE_icu=OFF
    -DFEATURE_jpeg=ON
    # qtimageformats (an optional dependency of qtdeclarative, built when it is
    # checked out) builds the TIFF
    # plugin, but its libtiff OS layer (tif_unix.c) is only compiled when
    # CMake's UNIX is true, which it is not under CMAKE_SYSTEM_NAME=Zephyr;
    # linking then fails with undefined _TIFFmalloc / _TIFFmemcpy / ...
    -DFEATURE_tiff=OFF
    -DFEATURE_library=OFF
    -DFEATURE_network=OFF
    # OpenGL / EGL: GL_OPTS above (off, or YakoGL's headers with WITH_GL=1).
    "${GL_OPTS[@]}"
    # (Matters only if EGL is turned on.)  With EGL found Qt auto-enables its Unix EGL platform plugins (eglfs,
    # minimalegl), which need the generic Unix font database / event
    # dispatcher Qt does not build on Zephyr.  The qzephyr plugin is the
    # only platform here.
    -DFEATURE_eglfs=OFF
    -DFEATURE_vulkan=OFF
    # quick_vectorimage compiles SVG resources to QML at build-time via
    # the svgtoqml host tool.  Our host Qt install does not build that
    # tool, and the regular QImage / QSvgRenderer path is enough for
    # demos like coffee that just <Image source="*.svg"/> their assets.
    -DFEATURE_quick_vectorimage=OFF
    # The V4 JS engine's JIT (qtdeclarative/src/3rdparty/masm/) requires
    # mprotect + sysconf(_SC_PAGESIZE), neither of which is available
    # under Zephyr's POSIX subset.  Disable the JIT and run the QML
    # bytecode interpreter only -- plenty fast for embedded UIs.
    -DFEATURE_qml_jit=OFF
    # The QML network loader needs QNetworkAccessManager (qtbase
    # FEATURE_network=OFF for us), so cut it from QML too.  Apps that
    # ship their own qrc:/ resources do not need the network path.
    -DFEATURE_qml_network=OFF
    -DFEATURE_openssl=OFF
    -DFEATURE_pch=OFF
    -DFEATURE_pkg_config=OFF
    -DFEATURE_process=OFF
    -DFEATURE_processenvironment=OFF
    -DFEATURE_sharedmemory=OFF
    -DFEATURE_sql=OFF
    -DFEATURE_system_doubleconversion=OFF
    -DFEATURE_systemsemaphore=OFF
    -DFEATURE_testlib=OFF
    -DFEATURE_thread=OFF
    -DFEATURE_xml=OFF
    -DFEATURE_zstd=OFF

    # Qt's compile-test feature detection misjudges glibc/Linux-only
    # features because newlib's headers declare the function prototype
    # (gated on _GNU_SOURCE) without providing the implementation.  The
    # compile test passes, FEATURE_xxx is set ON, then the real build
    # references the undefined function.  Explicitly OFF for everything
    # that's GNU-extension / Linux-only and would never link against
    # newlib's bare-metal libc anyway.
    -DFEATURE_dup3=OFF
    -DFEATURE_accept4=OFF
    -DFEATURE_copy_file_range=OFF
    -DFEATURE_futimens=OFF
    -DFEATURE_getauxval=OFF
    -DFEATURE_getentropy=OFF
    -DFEATURE_inotify=OFF
    -DFEATURE_linkat=OFF
    -DFEATURE_memmem=OFF
    -DFEATURE_memrchr=OFF
    -DFEATURE_ppoll=OFF
    -DFEATURE_pollts=OFF
    -DFEATURE_pthread_clockjoin=OFF
    -DFEATURE_pthread_condattr_setclock=OFF
    -DFEATURE_pthread_timedjoin=OFF
    -DFEATURE_cxa_thread_atexit=OFF
    -DFEATURE_cxa_thread_atexit_impl=OFF
    -DFEATURE_dladdr=OFF

    # qtbase: Core + Gui + Widgets statics.
    # qtdeclarative: QtQml + QtQuick + Quick Controls 2.  Includes the
    # software scene-graph adaptation we need (no GPU on this board).
    # qtsvg: QSvgPlugin (image format) + the QtSvg module for vector
    # cup/icon resources used by demos like coffee.
    #
    # NOTE: qtshadertools is intentionally NOT included here -- not
    # because it can't compile for Zephyr (the qtshadertools submodule
    # carries a patch that makes glslang's OSdependent build on
    # Zephyr via its Unix backend), but because at runtime the
    # Cortex-M7 firmware never invokes the qsb / shader-pipeline code:
    # we run QtQuick's software scene-graph adaptation, which uses
    # QPainter rather than precompiled .qsb shaders.  qtdeclarative's
    # *build* step does need qsb to pre-compile shaders that get
    # embedded in libQt6Quick.a's QRC, but that runs on the host via
    # QT_HOST_PATH/bin/qsb -- no need to cross-build a Cortex-M7 qsb.
    # QT_SUBMODULES (env) extends the set: Qt Quick 3D needs
    # "qtbase;qtdeclarative;qtsvg;qtshadertools;qtquicktimeline;qtquick3d"
    # (Quick 3D links QtShaderTools for its runtime material shader
    # generation, so the target-side library is built then; see the
    # BUILD_qtshadertools note below).
    "-DQT_BUILD_SUBMODULES=${QT_SUBMODULES:-qtbase;qtdeclarative;qtsvg}"
    # Explicitly force these submodules ON.  An earlier Tier 2 build
    # configured this tree with -DBUILD_qtdeclarative=OFF and the cache
    # still carries it as UNINITIALIZED=OFF, which makes
    # qt5/CMakeLists.txt:71 skip the submodule dependency check.
    -DBUILD_qtdeclarative=ON
    -DBUILD_qtsvg=ON
    # qtshadertools gets auto-pulled by qtdeclarative as a dependency,
    # but its runtime libraries hit std::mutex / lvalue-conversion
    # errors when cross-built without full libstdc++ threads.  Skip the
    # target build -- the only thing qtdeclarative actually needs from
    # qtshadertools is the host `qsb` tool, which lives in QT_HOST_PATH.
    "-DBUILD_qtshadertools=$(case ";${QT_SUBMODULES:-}" in *";qtshadertools;"*|*";qtshadertools") echo ON;; *) echo OFF;; esac)"
    "-DBUILD_qtquick3d=$(case ";${QT_SUBMODULES:-}" in *";qtquick3d;"*|*";qtquick3d") echo ON;; *) echo OFF;; esac)"
    "-DBUILD_qtquicktimeline=$(case ";${QT_SUBMODULES:-}" in *";qtquicktimeline;"*|*";qtquicktimeline") echo ON;; *) echo OFF;; esac)"
    # Qt Quick 3D on the target: no assimp asset importer (a host tooling
    # step; balsam converts assets on the host) and no OpenXR. Both are
    # large and thread-based; the runtime renderer needs neither.
    -DFEATURE_quick3d_assimp=OFF
    -DFEATURE_quick3dxr_openxr=OFF
    # Do NOT build target-side host tools (qml runtime, qmlscene, qml-
    # easing, etc.) -- they are huge Qt apps in their own right and the
    # default Cortex-M7 linker script obviously cannot fit a 20+ MB
    # executable into flash.  Stage 2 only consumes the static
    # libQt6Qml.a / libQt6Quick.a archives, not the qml binary.
    -DQT_BUILD_TOOLS_BY_DEFAULT=OFF
    # Cross-build: the host has SDL2 installed (for the X11 sim plugin)
    # but we don't want QZEPHYR_WITH_SDL baked into libqzephyr.a for the
    # embedded target -- otherwise flush()'s non-SDL Zephyr branch
    # becomes dead code.
    -DZEPHYR_NATIVE_SIM=OFF
    -DQT_BUILD_EXAMPLES=OFF
    -DQT_BUILD_TESTS=OFF
)

info "Running CMake configure..."
cmake -G Ninja -S "$QT_SOURCE_DIR" -B "$BUILD_DIR" "${CMAKE_OPTS[@]}"

info "Building..."
cmake --build "$BUILD_DIR" --parallel "$(nproc)"

info "Installing to $PREFIX..."
cmake --install "$BUILD_DIR"

info "Done.  Qt6 (Raspberry Pi 4 Cortex-A72 static libs) installed under $PREFIX/lib."
