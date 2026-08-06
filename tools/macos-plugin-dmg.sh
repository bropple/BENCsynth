#!/bin/sh
#
# A disk image for the plugins.
#
# The program gets one already (tools/macos-dmg.sh) and the plugins need one
# for a different reason. A .app has an obvious destination and Finder draws it
# as a single icon; a .clap has neither. Finder does not know the extension, so
# it shows the bundle as a plain folder, and the place it belongs is four
# levels inside ~/Library, which is hidden by default. Unpacked from a tarball
# that is three unexplained folders and no clue.
#
# So: everything in one window, with a script that puts each piece where it
# goes and strips the quarantine flag on the way - which is the part a tarball
# genuinely cannot do, and the reason a downloaded plugin gets reported as
# damaged even when it is signed.
#
#   tools/macos-plugin-dmg.sh <out.dmg> [volume name]
#
# Takes whatever is built: the .app, the .clap, the .vst3. Missing pieces are
# left out rather than faked.

set -eu

OUT="$1"
VOL="${2:-BENCsynth Plug-Ins}"
DMGBUILD="${DMGBUILD:-dmgbuild}"

APP=BENCsynth.app
CLAP=build/bencsynth.clap
VST3=build/bencsynth.vst3

STAGE="$(mktemp -d)/stage"
mkdir -p "$STAGE"

HAVE=""
[ -d "$APP" ]  && { cp -R "$APP"  "$STAGE/"; HAVE="$HAVE app"; }
[ -d "$CLAP" ] && { cp -R "$CLAP" "$STAGE/"; HAVE="$HAVE clap"; }
[ -d "$VST3" ] && { cp -R "$VST3" "$STAGE/"; HAVE="$HAVE vst3"; }
[ -n "$HAVE" ] || { echo "nothing to package - build something first" >&2; exit 1; }
echo "packaging:$HAVE"

# The editor is the .app, and the plugin finds it either beside itself or in
# /Applications. Installing it there is what makes the rack open.
ln -s /Applications "$STAGE/Applications"

cat > "$STAGE/Install Plug-Ins.command" <<'INSTALL'
#!/bin/sh
#
# Copies the plugins into your user plug-in folders and clears the quarantine
# flag macOS puts on anything downloaded. No administrator password: everything
# goes in your own Library.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"

CLAPDIR="$HOME/Library/Audio/Plug-Ins/CLAP"
VST3DIR="$HOME/Library/Audio/Plug-Ins/VST3"
mkdir -p "$CLAPDIR" "$VST3DIR"

installed=""

if [ -d "$HERE/bencsynth.clap" ]; then
    rm -rf "$CLAPDIR/bencsynth.clap"
    cp -R "$HERE/bencsynth.clap" "$CLAPDIR/"
    xattr -dr com.apple.quarantine "$CLAPDIR/bencsynth.clap" 2>/dev/null || true
    installed="$installed\n  CLAP  -> $CLAPDIR"
fi

if [ -d "$HERE/bencsynth.vst3" ]; then
    rm -rf "$VST3DIR/bencsynth.vst3"
    cp -R "$HERE/bencsynth.vst3" "$VST3DIR/"
    xattr -dr com.apple.quarantine "$VST3DIR/bencsynth.vst3" 2>/dev/null || true
    installed="$installed\n  VST3  -> $VST3DIR"
fi

# The editor. The plugin opens the rack by starting this, so a plugin without
# it shows a row of sliders and nothing else.
if [ -d "$HERE/BENCsynth.app" ] && [ ! -d /Applications/BENCsynth.app ]; then
    cp -R "$HERE/BENCsynth.app" /Applications/ 2>/dev/null || true
fi
[ -d /Applications/BENCsynth.app ] && {
    xattr -dr com.apple.quarantine /Applications/BENCsynth.app 2>/dev/null || true
    installed="$installed\n  editor-> /Applications/BENCsynth.app"
}

printf 'installed:%b\n\nRestart your DAW - it scans for plugins at startup.\n' "$installed"
INSTALL
chmod +x "$STAGE/Install Plug-Ins.command"

cat > "$STAGE/README.txt" <<'README'
BENCsynth plug-ins for macOS
============================

The quick way
-------------
Right-click "Install Plug-Ins.command" and choose Open, then Open again when
macOS asks. It copies everything where it belongs and clears the quarantine
flag. Right-click rather than double-click: an unsigned script downloaded from
the internet is blocked on a double-click, with no button to continue.

By hand
-------
  bencsynth.clap  ->  ~/Library/Audio/Plug-Ins/CLAP
  bencsynth.vst3  ->  ~/Library/Audio/Plug-Ins/VST3
  BENCsynth.app   ->  /Applications   (drag onto the Applications shortcut)

Then, in Terminal, for each one you copied:

  xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/CLAP/bencsynth.clap

Without that last step macOS reports the plugin as damaged. The bundles are
signed, but signing is not notarisation and only notarisation clears
quarantine - that needs a paid Apple Developer ID, which this does not have.

Why the .app matters
--------------------
It is not just the standalone program. The plugin opens the rack - cables,
knobs, keyboard - by starting that binary as a separate process. Without it
installed you get your DAW's generic parameter list and no rack.

The VST3 needs the CLAP
-----------------------
The VST3 is a shim that loads bencsynth.clap at runtime. Install both, or it
appears in your DAW with nothing to play.
README

rm -f "$OUT"

if command -v "$DMGBUILD" >/dev/null 2>&1; then
    "$DMGBUILD" -s tools/dmg-settings-plugins.py -D stage="$STAGE" "$VOL" "$OUT"
    echo "built $OUT (styled)"
else
    echo "warning: dmgbuild not found - building an unstyled image" >&2
    hdiutil create -srcfolder "$STAGE" -volname "$VOL" -fs HFS+ \
        -format UDZO -ov "$OUT" >/dev/null
    echo "built $OUT (unstyled)"
fi
