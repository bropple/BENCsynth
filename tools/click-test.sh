#!/bin/sh
#
# Click things in the running program and screenshot the result.
#
# This exists because reasoning about immediate-mode input is not reliable
# enough. The information window shipped broken twice: once because a click was
# handled on press, and once because the release that opened it was still
# "released" when the dismiss test ran later in the same frame, so the window
# opened and closed before anything was drawn. Neither was visible in the
# source; both were obvious the moment a real click was sent.
#
#   tools/click-test.sh out.png 1078,23 [more,points ...]
#
# Points are in window coordinates. Needs Xvfb and xdotool, and works on any
# machine with neither a display nor a sound card.
#
# Three things about this environment cost an hour to find, so they are written
# down rather than rediscovered:
#
#   - A plain `xdotool click` presses and releases about 12 ms apart. raylib
#     polls once a frame, so at 60 Hz both edges can land between two polls and
#     neither is ever seen. Press and release have to be separate, with a gap.
#
#   - There is no window manager, so nothing places the window, and the
#     geometry xdotool reports for it is relative to its parent rather than to
#     the root. It is not where it says it is.
#
#   - The window ends up physically narrower than the framebuffer raylib
#     renders into, so the pointer clamps short of the right-hand edge and the
#     top-right corner - where the information button is - cannot be clicked at
#     all. Resizing the window smaller brings everything back into reach;
#     raylib reflows on resize, so the layout is still the real layout.

set -eu

OUT="${1:?usage: click-test.sh <out.png> <x,y> [x,y ...]}"
shift

DISP="${BS_DISPLAY:-:84}"
WIN_W=1100
WIN_H=700
WIN_Y=62          # where the window lands with no window manager

Xvfb "$DISP" -screen 0 1920x1200x24 >/dev/null 2>&1 &
XPID=$!
trap 'kill $XPID 2>/dev/null || true' EXIT
sleep 2

DISPLAY="$DISP" ./bencsynth --shot "$OUT" --frames 600 >/dev/null 2>&1 &
APP=$!
sleep 3

W=$(DISPLAY="$DISP" xdotool search --name BENCsynth | head -1)
DISPLAY="$DISP" xdotool windowsize "$W" "$WIN_W" "$WIN_H"
sleep 1

for pt in "$@"; do
    X=${pt%%,*}
    Y=${pt##*,}
    DISPLAY="$DISP" xdotool mousemove "$X" $((Y + WIN_Y)); sleep 0.4
    DISPLAY="$DISP" xdotool mousedown 1; sleep 0.35
    DISPLAY="$DISP" xdotool mouseup 1;   sleep 0.6
done

# Away from everything, so what the screenshot shows is not merely a hover.
DISPLAY="$DISP" xdotool mousemove 500 $((400 + WIN_Y))
sleep 0.5

wait $APP || true
echo "wrote $OUT"
