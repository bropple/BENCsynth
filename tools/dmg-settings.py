# dmgbuild settings for the BENCsynth disk image.
#
# The window layout of a .dmg lives in a .DS_Store file, which Finder normally
# writes when you tell it - through AppleScript - where to put things. That is
# how nearly every disk image is styled, and it does not work on a build runner:
# there is no interactive Finder session for osascript to talk to, so the image
# comes out unstyled and nothing says so until someone opens it.
#
# dmgbuild writes the .DS_Store itself, so the layout is produced rather than
# requested. No Finder, no window server, no session.
#
#   dmgbuild -s tools/dmg-settings.py -D app=BENCsynth.app BENCsynth out.dmg

import os.path

application = defines.get("app", "BENCsynth.app")     # noqa: F821 - dmgbuild
appname = os.path.basename(application)

format = "UDZO"
compression_level = 9
size = None

files = [application]
symlinks = {"Applications": "/Applications"}

# S. Tarr on the mounted volume: on the desktop, and in the Finder sidebar.
_icns = os.path.join(application, "Contents", "Resources", "BENCsynth.icns")
if os.path.exists(_icns):
    icon = _icns

# The window is exactly the background image, so the arrow drawn between the
# two icon positions in that image lands between the two icons here. Move one
# and the other has to move with it - see tools/make-dmg-background.sh.
background = defines.get("background", "assets/brand/dmg-background.png")
window_rect = ((200, 120), (640, 400))
icon_locations = {
    appname: (160, 250),
    "Applications": (480, 250),
}

default_view = "icon-view"
show_icon_preview = False
show_status_bar = False
show_tab_view = False
show_toolbar = False
show_pathbar = False
show_sidebar = False

arrange_by = None
grid_offset = (0, 0)
grid_spacing = 100
scroll_position = (0, 0)
label_pos = "bottom"
text_size = 12
icon_size = 96
