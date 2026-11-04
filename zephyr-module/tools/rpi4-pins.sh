# rpi4-pins.sh -- the Zephyr and YakoGL commits the Raspberry Pi 4 GL build is
# known to run with (and with qtbase-for-zephyr's depth change).  Sourced by build-qt-zephyr-rpi4.sh (WITH_GL=1) and
# tools/west-build-rpi4.sh; this is the only place that names them.
#
# YakoGL is not a submodule: it is put on ZEPHYR_EXTRA_MODULES by hand, so
# nothing else ties a build to a commit.  Set RPI4_ALLOW_UNPINNED=1 to try
# another commit on purpose: the check then warns instead of stopping.

RPI4_YAKOGL_COMMIT=6e2723ac129828555ad8306de296c0c00650434c
RPI4_ZEPHYR_COMMIT=41d10b59730406bbab4ca9f1fbbe9e96aafb4966 # v4.5.0

# rpi4_check_pin <name> <directory> <commit>
rpi4_check_pin() {
    local name="$1" dir="$2" want="$3" have dirty problem=""
    have="$(git -C "$dir" rev-parse HEAD 2>/dev/null)" || have=""
    if [ -z "$have" ]; then
        problem="$dir is not a git checkout, so its commit cannot be confirmed"
    elif [ "$have" != "$want" ]; then
        problem="$dir is at $have, the pin is $want"
    else
        # Tracked files only.  Untracked files are not checked, although a
        # glob in a build file or Stage 1's copy of include/ can pick them up.
        if ! dirty="$(git -C "$dir" status --porcelain --untracked-files=no)"; then
            problem="the state of $dir cannot be read"
        elif [ -n "$dirty" ]; then
            problem="$dir has changes to tracked files:
$dirty"
        fi
    fi
    rpi4_pin_report "$name" "$problem"
}

# rpi4_pin_report <name> <problem, or empty>
rpi4_pin_report() {
    local name="$1" problem="$2"
    [ -n "$problem" ] || return 0
    if [ "${RPI4_ALLOW_UNPINNED:-0}" = "1" ]; then
        echo "WARNING: $name: $problem (RPI4_ALLOW_UNPINNED=1)" >&2
        return 0
    fi
    echo "Error: $name: $problem" >&2
    echo "Check out the pin (and repeat Stage 1 into an empty prefix if the install differs)," >&2
    echo "or set RPI4_ALLOW_UNPINNED=1 to try this tree on purpose." >&2
    return 1
}
