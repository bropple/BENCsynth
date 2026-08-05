#!/bin/sh
#
# Regenerate assets/icon from the star geometry in the program.
#
# The PNGs are written by the synthesizer itself - `bencsynth --icons` calls
# the same bs_star_image() that produces the window icon, which is the same
# geometry bs_star() draws in the header. There is no separate icon artwork to
# keep in step, and no way for the icon in the taskbar to disagree with the
# icon in the .ico.
#
# The .ico is assembled here because packing several sizes into one container
# is the one step raylib cannot do. macOS builds its .icns from the same PNGs
# in tools/macos-app.sh.
#
# Committed rather than built: a release job should not need ImageMagick, and
# these change roughly never.
#
#   make icons          the usual way in
#   tools/make-icons.sh

set -eu

BIN="${BIN:-./bencsynth}"
OUT=assets/icon

if [ ! -x "$BIN" ]; then
    echo "no $BIN - run make first" >&2
    exit 1
fi

mkdir -p "$OUT"
"$BIN" --icons "$OUT" >/dev/null

# ImageMagick 7 calls it `magick`; 6 calls it `convert`. Both are still out
# there, and on 7 `convert` prints a deprecation warning to stderr that looks
# like an error in a build log.
if command -v magick >/dev/null 2>&1; then
    IM=magick
elif command -v convert >/dev/null 2>&1; then
    IM=convert
else
    echo "warning: no ImageMagick - the PNGs are updated, $OUT/bencsynth.ico is not" >&2
    exit 0
fi

# 16, 24, 32 and 48 are the sizes Windows asks for: the first two for the
# titlebar, the others for the taskbar. 128 and 256 are for Explorer's larger
# views. Anything absent gets scaled from a neighbour, badly.
$IM "$OUT/star-16.png" "$OUT/star-24.png" "$OUT/star-32.png" \
    "$OUT/star-48.png" "$OUT/star-64.png" "$OUT/star-128.png" \
    "$OUT/star-256.png" "$OUT/bencsynth.ico"

echo "wrote $OUT/star-*.png and $OUT/bencsynth.ico"
