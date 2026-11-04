#!/bin/bash
# west-build-rpi4.sh -- build a Qt application into a Zephyr firmware for the
# Raspberry Pi 4 (rpi_4b/bcm2711) with OpenGL ES 2.0 on the V3D through YakoGL.
#
# Usage:
#   QT_APP_DIR=/path/to/qt-app [BUILD_DIR=build/qt-rpi4-gl] tools/west-build-rpi4.sh [extra cmake args]
#
# Environment (`west` must be on PATH; the Zephyr SDK as usual):
#   ZEPHYR_BASE        Zephyr tree at the pin in rpi4-pins.sh
#                      (default ~/org/zephyrproject/zephyrproject/zephyr)
#   YAKOGL_DIR         YakoGL checkout at the pin in rpi4-pins.sh (default ~/org/yakogl)
#   QT_ZEPHYR_PREFIX   the build-qt-zephyr-rpi4.sh WITH_GL=1 install (default ~/qt-zephyr-rpi4-gl)
#   QT_HOST_PATH       host Qt (moc, rcc, qmlimportscanner; the one Stage 1 used)
#                      (default ~/qt-x11-sim)
#   QT_APP_DIR         the (unmodified) Qt application directory
#   BUILD_DIR          west build directory (default build/qt-rpi4-gl)
#   RUSTUP_TOOLCHAIN   for YakoGL's Rust part (default nightly, for -Z build-std=core;
#                      built here with rustc 1.98.0-nightly d595fce01; not pinned)
#
# Then copy $BUILD_DIR/zephyr/zephyr.bin to the SD card's boot partition.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QT_MODULE="$(cd "$HERE/.." && pwd)"
# shellcheck source=rpi4-pins.sh
. "$HERE/rpi4-pins.sh"

ZEPHYR_BASE="${ZEPHYR_BASE:-$HOME/org/zephyrproject/zephyrproject/zephyr}"
YAKOGL_DIR="${YAKOGL_DIR:-$HOME/org/yakogl}"
export QT_ZEPHYR_PREFIX="${QT_ZEPHYR_PREFIX:-$HOME/qt-zephyr-rpi4-gl}"
QT_HOST_PATH="${QT_HOST_PATH:-$HOME/qt-x11-sim}"
QT_APP_DIR="${QT_APP_DIR:?set QT_APP_DIR to the Qt application directory}"
BUILD_DIR="${BUILD_DIR:-build/qt-rpi4-gl}"
export ZEPHYR_BASE ZEPHYR_TOOLCHAIN_VARIANT="${ZEPHYR_TOOLCHAIN_VARIANT:-zephyr}"
export RUSTUP_TOOLCHAIN="${RUSTUP_TOOLCHAIN:-nightly}"

for d in "$ZEPHYR_BASE/include/zephyr" "$YAKOGL_DIR/include/EGL/egl.h" \
         "$QT_ZEPHYR_PREFIX/lib/cmake/Qt6" "$QT_ZEPHYR_PREFIX/yakogl-sdk" \
         "$QT_HOST_PATH/lib/cmake" "$QT_APP_DIR/CMakeLists.txt"; do
    [ -e "$d" ] || { echo "west-build-rpi4: missing $d" >&2; exit 1; }
done
# Zephyr's CMake resolves a relative module path against ZEPHYR_BASE, not against
# the directory checked here.
ZEPHYR_BASE="$(cd "$ZEPHYR_BASE" && pwd)"
YAKOGL_DIR="$(cd "$YAKOGL_DIR" && pwd)"

command -v west >/dev/null || { echo "west-build-rpi4: west is not on PATH" >&2; exit 1; }

rpi4_check_pin Zephyr "$ZEPHYR_BASE" "$RPI4_ZEPHYR_COMMIT"
rpi4_check_pin YakoGL "$YAKOGL_DIR" "$RPI4_YAKOGL_COMMIT"
# Stage 1 copied YakoGL's headers into the install: it must be the same YakoGL.
if ! diff -rq "$YAKOGL_DIR/include" "$QT_ZEPHYR_PREFIX/yakogl-sdk/include" >/dev/null; then
    rpi4_pin_report YakoGL "the headers in $QT_ZEPHYR_PREFIX/yakogl-sdk differ from $YAKOGL_DIR/include"
fi

BOARDS="$QT_MODULE/qt-app/boards"
set -x
west build -p auto -b rpi_4b/bcm2711 -d "$BUILD_DIR" "$QT_MODULE/qt-app" -- \
    -DZEPHYR_EXTRA_MODULES="$QT_MODULE;$YAKOGL_DIR" \
    -DQT_APP_DIR="$QT_APP_DIR" \
    -DQT_HOST_PATH="$QT_HOST_PATH" \
    -DEXTRA_CONF_FILE="$BOARDS/rpi_4b_bcm2711_gl.conf" \
    -DEXTRA_DTC_OVERLAY_FILE="$BOARDS/rpi_4b_bcm2711_gl.overlay" \
    "$@"
