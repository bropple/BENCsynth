#!/bin/sh
# Stop GLFW looking for game controllers on Windows.
#
# This is a synthesiser. It has never read a joystick and it never will, and
# on Windows the only thing GLFW's joystick support does for it is cost ten
# and a half seconds of startup.
#
# Where that goes, measured on a machine with a great many peripherals
# attached: raylib forces GLFW's joystick subsystem awake immediately before
# it creates the window - deliberately, so the delay lands before the window
# rather than on the first frame, with a comment in rcore_desktop_glfw.c
# saying as much and referring to raylib issue 1554. On Win32 that reaches
# DirectInput8Create and then enumerates every game controller the machine has
# ever seen. Not the attached ones: DIEDFL_ALLDEVICES, so unplugged ones too,
# each answered by its own driver at its own pace. Ten and a half seconds
# before the window appears, every run, within a tenth of a per cent - which
# is what a pile of device drivers being asked politely looks like, and is why
# it was mistaken for a timeout for so long.
#
# The same wait shows up in Windows' own Game Controllers panel (joy.cpl) on
# that machine, which is the quickest way to tell this apart from anything
# this program is doing.
#
# So: report success, initialise nothing, and every joystick stays absent -
# which is the truth as far as this program is concerned. Everything
# downstream is already guarded on the DirectInput interface being present, so
# nothing else has to change:
#
#   _glfwDetectJoystickConnectionWin32   guards its DirectInput half
#   _glfwTerminateJoysticksWin32         guards the release
#   _glfwPollJoystickWin32               is only reached for connected sticks
#
# Applied to a raylib source tree before it is built. Windows only, because
# that is where the cost is: reading /dev/input on Linux and asking IOHID on
# macOS are both quick, and neither has ever been worth a patch.
#
#   tools/no-gamepads.sh /tmp/raylib
#
set -eu

RL="${1:?usage: no-gamepads.sh <raylib source dir>}"
J="$RL/src/external/glfw/src/win32_joystick.c"

test -f "$J" || { echo "no-gamepads: not a raylib source tree: $RL" >&2; exit 1; }

ANCHOR='GLFWbool _glfwInitJoysticksWin32(void)'
grep -qF -- "$ANCHOR" "$J" || {
    echo "no-gamepads: $ANCHOR is not in win32_joystick.c any more." >&2
    echo "no-gamepads: raylib's GLFW has moved; check before assuming this" >&2
    echo "no-gamepads: patch is still needed, then fix the anchor." >&2
    exit 1
}

# Already done - the workflows call this once, but a rerun in the same tree
# should not stack two returns on top of each other.
if grep -q 'BENCsynth: no gamepads' "$J"; then
    echo "no-gamepads: already patched"
    exit 0
fi

awk -v a="$ANCHOR" '
    index($0, a) { found = 1 }
    { print }
    found && !done && $0 == "{" {
        print "    /* BENCsynth: no gamepads. See tools/no-gamepads.sh."
        print "     *"
        print "     * Enumerating game controllers costs this program ten seconds of"
        print "     * startup on a machine with a lot of them and buys it nothing,"
        print "     * because it does not read them. Success with nothing"
        print "     * initialised leaves every joystick absent, which is true."
        print "     */"
        print "    return GLFW_TRUE;"
        done = 1
    }
' "$J" > "$J.tmp"

mv "$J.tmp" "$J"

grep -q 'BENCsynth: no gamepads' "$J" || {
    echo "no-gamepads: the edit did not take. Not shipping a build that still" >&2
    echo "no-gamepads: waits ten seconds while claiming not to." >&2
    exit 1
}

echo "no-gamepads: patched $J"
