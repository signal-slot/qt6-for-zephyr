#!/bin/bash
# west-build-am62p.sh -- build a Qt application into a Zephyr firmware for the
# Toradex Verdin AM62P (Cortex-A53) with OpenGL ES 2.0 through YakoGL.
#
# Usage:
#   QT_APP_DIR=/path/to/qt-app [BUILD_DIR=build/qt] tools/west-build-am62p.sh [extra west/cmake args]
#
# Environment (all default to the lab layout; override as needed):
#   ZEPHYR_BASE        Zephyr tree the SafeUI app repo pins (af17c0c8, 4.4.99)
#   SAFEUI_ROOT        slint-safe-ui-zephyr-verdin-am62p (board, SoC, DSS/DSI drivers)
#   PVR_ROOT           powervr-bxs-am62p (GPU driver; YakoGL is its yakogl/ submodule)
#   FATFS_ROOT         Zephyr's fatfs module (the SafeUI module needs it present)
#   QT_ZEPHYR_PREFIX   the build-qt-zephyr-am62p.sh install
#   QT_HOST_PATH       host Qt (moc, rcc, qmlimportscanner; the one Stage 1 used)
#   QT_APP_DIR         the (unmodified) Qt application directory
#   BUILD_DIR          west build directory (default build/qt-am62p)
#
# Then boot it from Torizon:
#   BOARD_HOST=torizon@<board> $PVR_ROOT/tools/kexec-zephyr/kexec_run.sh $BUILD_DIR/zephyr/zephyr.bin
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QT_MODULE="$(cd "$HERE/.." && pwd)"

SAFEUI_ROOT="${SAFEUI_ROOT:-$HOME/com/github/signal-slot/slint-safe-ui-zephyr-verdin-am62p}"
PVR_ROOT="${PVR_ROOT:-$HOME/com/github/signal-slot/powervr-bxs-am62p}"
ZEPHYR_BASE="${ZEPHYR_BASE:?set ZEPHYR_BASE to the pinned Zephyr tree}"
FATFS_ROOT="${FATFS_ROOT:-$(dirname "$ZEPHYR_BASE")/modules/fs/fatfs}"
export QT_ZEPHYR_PREFIX="${QT_ZEPHYR_PREFIX:-$HOME/qt-zephyr-am62p}"
QT_HOST_PATH="${QT_HOST_PATH:-$HOME/qt-x11-sim}"
QT_APP_DIR="${QT_APP_DIR:?set QT_APP_DIR to the Qt application directory}"
BUILD_DIR="${BUILD_DIR:-build/qt-am62p}"
export ZEPHYR_BASE ZEPHYR_TOOLCHAIN_VARIANT="${ZEPHYR_TOOLCHAIN_VARIANT:-zephyr}"

for d in "$SAFEUI_ROOT/boards" "$PVR_ROOT/yakogl/include" "$ZEPHYR_BASE/include/zephyr" \
         "$QT_ZEPHYR_PREFIX/lib/cmake/Qt6" "$QT_ZEPHYR_PREFIX/yakogl-sdk" "$QT_APP_DIR/CMakeLists.txt"; do
    [ -e "$d" ] || { echo "west-build-am62p: missing $d" >&2; exit 1; }
done

MODULES="$FATFS_ROOT;$SAFEUI_ROOT;$PVR_ROOT;$PVR_ROOT/yakogl;$QT_MODULE"

# QT_TOUCH=1: the DSI display's ILI2132A touch controller (opt-in while its
# shared reset line with the panel bridge is being validated).
TOUCH_ARGS=()
if [ "${QT_TOUCH:-0}" = "1" ]; then
    TOUCH_ARGS=(-DEXTRA_DTC_OVERLAY_FILE="$QT_MODULE/qt-app/boards/verdin_am62p_am62p54_a53_touch.overlay"
                -DEXTRA_CONF_FILE="$QT_MODULE/qt-app/boards/verdin_am62p_am62p54_a53_touch.conf")
fi

set -x
west build -p auto -b verdin_am62p/am62p54/a53 -d "$BUILD_DIR" "$QT_MODULE/qt-app" -- \
    -DZEPHYR_EXTRA_MODULES="$MODULES" \
    -DQT_APP_DIR="$QT_APP_DIR" \
    -DQT_HOST_PATH="$QT_HOST_PATH" \
    "${TOUCH_ARGS[@]}" \
    "$@"
