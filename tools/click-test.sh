#!/bin/sh
#
# Drive the running program with real input and screenshot the result.
#
# This exists because reasoning about immediate-mode input is not reliable
# enough. Three separate bugs shipped that were invisible in the source and
# obvious the moment a real click was sent: a window handled on press instead
# of release, a window dismissed by the same release that opened it, and a menu
# chosen from by the very right-click that opened it.
#
#   tools/click-test.sh out.png ACTION [ACTION ...]
#
# Actions, in window coordinates:
#
#   x,y                 left click
#   R:x,y               right click
#   2:x,y               double click, inside the recognition window
#   D:x1,y1:x2,y2       left drag
#   M:x,y               move the pointer there and nothing else
#   K:name              a key, held long enough to be seen
#   T:some text         type it
#   W:x,y:n             scroll n notches at a point (negative for down)
#   P:seconds           pause
#
# Needs Xvfb and xdotool, and works on a machine with neither a display nor a
# sound card. Extra arguments to bencsynth go in BS_ARGS.
#
# Four things about this environment cost real time to find, so they are
# written down rather than rediscovered:
#
#   - A plain `xdotool click` or `key` presses and releases about 12 ms apart.
#     raylib polls once a frame, so at 60 Hz both edges can land between two
#     polls and neither is ever seen. Every press and release here is a
#     separate event with a gap. Typed *characters* survive it, because raylib
#     queues those; keys and buttons do not.
#   - There is no window manager, so nothing places the window, and the
#     geometry xdotool reports is relative to its parent rather than the root.
#   - The window ends up physically narrower than the framebuffer raylib
#     renders into, so the pointer clamps short of the right-hand edge.
#     Resizing it smaller brings everything back into reach; raylib reflows.
#   - The window lands at y=62 with no window manager, which is why every
#     coordinate here has that added to it.

set -eu

OUT="${1:?usage: click-test.sh <out.png> <action> [action ...]}"
shift

DISP="${BS_DISPLAY:-:88}"
WIN_W="${BS_WIN_W:-1300}"
WIN_H="${BS_WIN_H:-800}"
WIN_Y=62
FRAMES="${BS_FRAMES:-900}"

Xvfb "$DISP" -screen 0 1920x1200x24 >/dev/null 2>&1 &
XPID=$!
trap 'kill $XPID 2>/dev/null || true' EXIT
sleep 2

# shellcheck disable=SC2086
DISPLAY="$DISP" ./bencsynth --shot "$OUT" --frames "$FRAMES" ${BS_ARGS:-} \
    >/dev/null 2>&1 &
APP=$!
sleep 3

W=$(DISPLAY="$DISP" xdotool search --name BENCsynth | head -1)
DISPLAY="$DISP" xdotool windowsize "$W" "$WIN_W" "$WIN_H"
sleep 1

x() { DISPLAY="$DISP" xdotool "$@"; }

for act in "$@"; do
    case "$act" in
    2:*)
        p=${act#2:}
        x mousemove "${p%%,*}" $(( ${p##*,} + WIN_Y )); sleep 0.35
        # Both clicks well inside the 350 ms the program allows, but each edge
        # still spanning a frame so neither is missed.
        x mousedown 1; sleep 0.05; x mouseup 1; sleep 0.05
        x mousedown 1; sleep 0.05; x mouseup 1; sleep 0.5
        ;;
    R:*)
        p=${act#R:}
        x mousemove "${p%%,*}" $(( ${p##*,} + WIN_Y )); sleep 0.35
        x mousedown 3; sleep 0.3
        x mouseup 3;   sleep 0.5
        ;;
    D:*)
        p=${act#D:}
        a=${p%%:*}; b=${p##*:}
        x mousemove "${a%%,*}" $(( ${a##*,} + WIN_Y )); sleep 0.35
        x mousedown 1; sleep 0.3
        # Several steps, so the program sees a drag rather than a teleport.
        x mousemove_relative -- 10 10; sleep 0.1
        x mousemove "${b%%,*}" $(( ${b##*,} + WIN_Y )); sleep 0.4
        x mouseup 1; sleep 0.5
        ;;
    W:*)
        p=${act#W:}
        pt=${p%:*}; n=${p##*:}
        x mousemove "${pt%%,*}" $(( ${pt##*,} + WIN_Y )); sleep 0.3
        i=0
        while [ "$i" -lt "${n#-}" ]; do
            if [ "${n#-}" = "$n" ]; then x click 4; else x click 5; fi
            sleep 0.12
            i=$((i + 1))
        done
        sleep 0.3
        ;;
    M:*)
        p=${act#M:}
        x mousemove "${p%%,*}" $(( ${p##*,} + WIN_Y )); sleep 0.4
        ;;
    K:*)
        x keydown "${act#K:}"; sleep 0.28
        x keyup   "${act#K:}"; sleep 0.28
        ;;
    T:*)
        x type --delay 60 "${act#T:}"; sleep 0.4
        ;;
    P:*)
        sleep "${act#P:}"
        ;;
    *)
        x mousemove "${act%%,*}" $(( ${act##*,} + WIN_Y )); sleep 0.35
        x mousedown 1; sleep 0.3
        x mouseup 1;   sleep 0.5
        ;;
    esac
done

# Away from everything, so a screenshot proves a state rather than a hover.
x mousemove 640 $((760 + WIN_Y))
sleep 0.6
wait $APP || true
echo "wrote $OUT"
