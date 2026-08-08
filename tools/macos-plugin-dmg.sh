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
AU=build/BENCsynth.component

STAGE="$(mktemp -d)/stage"
mkdir -p "$STAGE"

HAVE=""
[ -d "$APP" ]  && { cp -R "$APP"  "$STAGE/"; HAVE="$HAVE app"; }
[ -d "$CLAP" ] && { cp -R "$CLAP" "$STAGE/"; HAVE="$HAVE clap"; }
[ -d "$VST3" ] && { cp -R "$VST3" "$STAGE/"; HAVE="$HAVE vst3"; }
[ -d "$AU" ]   && { cp -R "$AU"   "$STAGE/"; HAVE="$HAVE au"; }
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
AUDIR="$HOME/Library/Audio/Plug-Ins/Components"
mkdir -p "$CLAPDIR" "$VST3DIR" "$AUDIR"

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

# AU, which is the only one of these Logic and GarageBand will load.
if [ -d "$HERE/BENCsynth.component" ]; then
    rm -rf "$AUDIR/BENCsynth.component"
    cp -R "$HERE/BENCsynth.component" "$AUDIR/"
    xattr -dr com.apple.quarantine "$AUDIR/BENCsynth.component" 2>/dev/null || true
    installed="$installed\n  AU    -> $AUDIR"
fi

# The editor. The plugin opens the rack by starting this, so a plugin without
# it shows a row of sliders and nothing else.
#
# Always replaced, never skipped. An earlier version of this script installed
# it only when /Applications/BENCsynth.app was absent, which meant every
# reinstall after the first silently kept the old editor - and an old editor
# given a flag it does not know opens a window of its own instead of drawing
# into the host's. The plugin and the editor are one program in two files and
# they have to be updated together.
if [ -d "$HERE/BENCsynth.app" ]; then
    rm -rf /Applications/BENCsynth.app
    cp -R "$HERE/BENCsynth.app" /Applications/ 2>/dev/null || true
    xattr -dr com.apple.quarantine /Applications/BENCsynth.app 2>/dev/null || true
    installed="$installed\n  editor-> /Applications/BENCsynth.app"

    # A copy sitting beside the plugin is searched before /Applications, so a
    # stale one there wins over whatever was just installed. Replace it rather
    # than delete it: somebody put it there on purpose.
    if [ -d "$CLAPDIR/BENCsynth.app" ]; then
        rm -rf "$CLAPDIR/BENCsynth.app"
        cp -R "$HERE/BENCsynth.app" "$CLAPDIR/"
        xattr -dr com.apple.quarantine "$CLAPDIR/BENCsynth.app" 2>/dev/null || true
        installed="$installed\n  editor-> $CLAPDIR/BENCsynth.app (replaced)"
    fi
fi

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

If something does not work
--------------------------
The plugin writes what it is doing to

  ~/Library/Logs/BENCsynth.log

Every time your DAW asks it for a window, and every place it looks for the
editor, is in there. If no rack opens, that file says which of the two went
wrong - the host never asking, or the editor not being found.

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
