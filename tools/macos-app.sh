#!/bin/sh
#
# Wrap the binary in a .app bundle.
#
# macOS is the one platform where the window icon cannot come from the program
# itself. Windows reads it from a named resource compiled into the executable
# and X11 takes it from _NET_WM_ICON, which the binary sets at startup - but
# GLFW's Cocoa backend ignores glfwSetWindowIcon entirely, because a bare
# Mach-O executable has no Finder or Dock identity to attach an icon to. The
# icon is a property of a bundle, so there has to be a bundle.
#
#   tools/macos-app.sh <binary> <output.app>
#
# Run on macOS: iconutil ships with the developer tools and has no equivalent
# elsewhere.

set -eu

BIN="$1"
APP="$2"
NAME="$(basename "$APP" .app)"

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

cp "$BIN" "$APP/Contents/MacOS/$NAME"
chmod +x "$APP/Contents/MacOS/$NAME"

# The iconset names are fixed; iconutil rejects anything it does not recognise.
# The set does not have to be complete, only correctly named - there is no
# 1024 px source, so 512x512@2x is simply absent.
ICONSET="$(mktemp -d)/icon.iconset"
mkdir -p "$ICONSET"
cp assets/icon/star-16.png  "$ICONSET/icon_16x16.png"
cp assets/icon/star-32.png  "$ICONSET/icon_16x16@2x.png"
cp assets/icon/star-32.png  "$ICONSET/icon_32x32.png"
cp assets/icon/star-64.png  "$ICONSET/icon_32x32@2x.png"
cp assets/icon/star-128.png "$ICONSET/icon_128x128.png"
cp assets/icon/star-256.png "$ICONSET/icon_128x128@2x.png"
cp assets/icon/star-256.png "$ICONSET/icon_256x256.png"
cp assets/icon/star-512.png "$ICONSET/icon_256x256@2x.png"
cp assets/icon/star-512.png "$ICONSET/icon_512x512.png"

iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/$NAME.icns"

# The font and the wordmark travel inside the bundle. A .app is meant to be one
# self-contained thing a user drags to Applications, and a folder of assets
# that had to stay beside it would defeat that. bs_find_asset() looks in
# ../Resources relative to the executable, which is exactly here.
cp -R assets "$APP/Contents/Resources/assets"

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>              <string>$NAME</string>
    <key>CFBundleDisplayName</key>       <string>BENCsynth</string>
    <key>CFBundleExecutable</key>        <string>$NAME</string>
    <key>CFBundleIdentifier</key>        <string>net.ropple.bencsynth</string>
    <key>CFBundleIconFile</key>          <string>$NAME</string>
    <key>CFBundlePackageType</key>       <string>APPL</string>
    <key>CFBundleShortVersionString</key><string>${BS_VERSION:-0.0.0}</string>
    <key>CFBundleVersion</key>           <string>${BS_VERSION:-0.0.0}</string>
    <key>LSMinimumSystemVersion</key>    <string>11.0</string>
    <!-- Without this the window is drawn at 1x and scaled up, which on a
         Retina display makes a bitmap font look exactly as bad as it sounds. -->
    <key>NSHighResolutionCapable</key>   <true/>
    <key>NSSupportsAutomaticGraphicsSwitching</key> <true/>
</dict>
</plist>
PLIST

# Ad-hoc sign the assembled bundle.
#
# This is not about trust - an ad-hoc signature identifies nobody - it is about
# the signature being *valid*. The linker ad-hoc signs the executable on its
# own, and that signature covers the executable alone. Once the same file sits
# inside a bundle, macOS validates it against the bundle: Info.plist,
# Resources, the lot. The standalone signature does not cover any of that, so
# validation fails, and Gatekeeper reports a bundle that is perfectly intact as
# "damaged and can't be opened" - with no option to open it anyway, because
# that offer is for software it distrusts, not for software it thinks is
# corrupt.
#
# Signing here, after everything is in place, makes the signature cover what it
# is checked against. Gatekeeper then says what is actually true: unidentified
# developer, open at your own risk.
if command -v codesign >/dev/null 2>&1; then
    codesign --force --sign - --timestamp=none "$APP"
    codesign --verify --deep --strict --verbose=2 "$APP"
    echo "signed (ad-hoc) and verified"
else
    echo "warning: codesign not found - the bundle will be reported as damaged" >&2
fi

echo "built $APP"
