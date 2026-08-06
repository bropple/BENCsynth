# dmgbuild settings for the plug-in disk image.
#
# No custom background here, unlike the program's image. That one has an arrow
# painted between two icon positions, and an arrow is only honest when there is
# exactly one gesture to make. This window has four things in it and two
# different destinations, so the instructions are written down instead - in
# README.txt and in the installer's own name.

import os

stage = defines.get("stage", "")            # noqa: F821 - dmgbuild defines it

files = []
symlinks = {}
for name in sorted(os.listdir(stage)):
    path = os.path.join(stage, name)
    if os.path.islink(path):
        symlinks[name] = os.readlink(path)
    else:
        files.append(path)

format = "UDZO"
compression_level = 9
size = None

_icns = os.path.join(stage, "BENCsynth.app", "Contents", "Resources", "BENCsynth.icns")
if os.path.exists(_icns):
    icon = _icns

window_rect = ((200, 120), (660, 440))
icon_locations = {
    "Install Plug-Ins.command": (150, 110),
    "README.txt":               (330, 110),
    "BENCsynth.app":            (510, 110),
    "bencsynth.clap":           (150, 280),
    "bencsynth.vst3":           (330, 280),
    "Applications":             (510, 280),
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
icon_size = 80
