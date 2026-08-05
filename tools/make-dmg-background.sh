#!/bin/sh
#
# Draw the disk image's background.
#
# The window is exactly this image, and the arrow is drawn between the two
# positions tools/dmg-settings.py puts the icons at. Move one and the other has
# to move with it - there is nothing that checks, because the only way to see
# it is to look at a mounted image.
#
# Two resolutions: the window carries type, and a 1x image on a Retina display
# is visibly soft, which is a strange thing to ship on the one screen this
# whole aesthetic is built around. tools/macos-dmg.sh packs both into a single
# TIFF with tiffutil at build time.
#
# Committed rather than built, so the release job needs no ImageMagick.
#
#   tools/make-dmg-background.sh

set -eu

if command -v magick >/dev/null 2>&1; then
    IM=magick
elif command -v convert >/dev/null 2>&1; then
    IM=convert
else
    echo "needs ImageMagick" >&2
    exit 1
fi

FONT=assets/fonts/TerminusTTF.ttf
WORD=assets/brand/BENCO_Logo_Terminal.png
OUT=assets/brand

# Icon centres, matching tools/dmg-settings.py.
APP_X=160
DST_X=480
ICON_Y=250

draw() {
    scale="$1"
    out="$2"

    w=$((640 * scale))
    h=$((400 * scale))
    ax=$((APP_X * scale))
    dx=$((DST_X * scale))
    iy=$((ICON_Y * scale))

    # The arrow runs between the two icons, clear of both.
    a0=$((ax + 78 * scale))
    a1=$((dx - 78 * scale))
    head=$((a1 - 16 * scale))

    # The wordmark is a stencil: the letters are cuts in solid blocks, and the
    # blocks are what carries the colour. Painting a flat colour and taking the
    # shape from the source's alpha channel keeps the cuts transparent.
    # Colorizing the source instead also tints its transparent pixels, which
    # bleeds dark fringes into the cuts when it is resized.
    tmp="$(mktemp -d)"
    "$IM" "$WORD" -resize $((190 * scale))x -alpha extract "$tmp/mask.png"
    "$IM" -size "$("$IM" identify -format '%wx%h' "$tmp/mask.png")" "xc:#3f5c28" \
        "$tmp/mask.png" -alpha off -compose CopyOpacity -composite "$tmp/word.png"

    "$IM" -size "${w}x${h}" "xc:#0c1408" \
        "$tmp/word.png" \
        -gravity north -geometry "+0+$((34 * scale))" -composite \
        -gravity northwest \
        -font "$FONT" \
        -fill "#cdeab0" -pointsize $((30 * scale)) \
        -annotate "+$((210 * scale))+$((104 * scale))" "BENCsynth" \
        -fill "#6f8a5c" -pointsize $((15 * scale)) \
        -annotate "+$((212 * scale))+$((136 * scale))" "a virtual modular synthesizer" \
        -fill "#3f5c28" -stroke "#3f5c28" -strokewidth $((3 * scale)) \
        -draw "line $a0,$iy $head,$iy" \
        -draw "polygon $a1,$iy $head,$((iy - 9 * scale)) $head,$((iy + 9 * scale))" \
        -stroke none -fill "#6f8a5c" -pointsize $((15 * scale)) \
        -gravity north \
        -annotate "+0+$((330 * scale))" "drag BENCsynth to Applications" \
        "$out"

    rm -rf "$tmp"
}

draw 1 "$OUT/dmg-background.png"
draw 2 "$OUT/dmg-background@2x.png"
echo "wrote $OUT/dmg-background.png and $OUT/dmg-background@2x.png"
