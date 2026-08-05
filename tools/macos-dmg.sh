#!/bin/sh
#
# Build a drag-to-Applications disk image around an existing .app.
#
# A .app is a directory that Finder draws as a single icon. That works, but it
# only survives a download if whatever unpacked the archive kept it intact and
# the person on the other end knows not to open it. A tarball gives neither
# guarantee: unpack one and you get a folder called Contents, with no clue
# which part is the program.
#
# A .dmg removes the question. It mounts as a window with the application on
# one side and a shortcut to /Applications on the other, so the obvious gesture
# is the correct one and there is nothing to explain.
#
#   tools/macos-dmg.sh <app> <out.dmg> [volume name]
#
# Styling is done with dmgbuild rather than by driving Finder over AppleScript.
# The window layout lives in a .DS_Store, and the usual way to get one written
# is to mount the image and ask Finder to arrange it - which needs an
# interactive session and therefore cannot work on a build runner. dmgbuild
# writes the .DS_Store directly. Set DMGBUILD to point at it if it is not on
# PATH; without it this still produces a working image, just an unstyled one,
# and says so rather than pretending.

set -eu

APP="$1"
OUT="$2"
VOL="${3:-BENCsynth}"

DMGBUILD="${DMGBUILD:-dmgbuild}"

BG=assets/brand/dmg-background.png

# Retina: the window carries the wordmark and 15 px type, and a 1x image on a
# 2x display is visibly soft - a strange thing to ship on the one screen this
# whole aesthetic is built around. tiffutil packs both resolutions into one
# file, which is how a background gets to be crisp on both.
if command -v tiffutil >/dev/null 2>&1 && \
   [ -f assets/brand/dmg-background@2x.png ]; then
    if tiffutil -cathidpicheck assets/brand/dmg-background.png \
                assets/brand/dmg-background@2x.png \
                -out /tmp/bs-dmg-background.tiff >/dev/null 2>&1; then
        BG=/tmp/bs-dmg-background.tiff
    fi
fi

rm -f "$OUT"

if command -v "$DMGBUILD" >/dev/null 2>&1; then
    "$DMGBUILD" -s tools/dmg-settings.py \
        -D app="$APP" -D background="$BG" \
        "$VOL" "$OUT"
    echo "built $OUT (styled)"
else
    echo "warning: dmgbuild not found - building an unstyled image" >&2
    STAGE="$(mktemp -d)/stage"
    mkdir -p "$STAGE"
    cp -R "$APP" "$STAGE/"
    ln -s /Applications "$STAGE/Applications"
    hdiutil create -srcfolder "$STAGE" -volname "$VOL" -fs HFS+ \
        -format UDZO -ov "$OUT" >/dev/null
    echo "built $OUT (unstyled)"
fi
