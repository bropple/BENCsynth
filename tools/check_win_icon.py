#!/usr/bin/env python3
"""Check that a Windows binary carries the icon GLFW will actually use.

This exists because the obvious check is not the useful one. An icon can be
compiled into the binary, show up correctly in Explorer, pass a "does the .rsrc
section exist" test - and still leave the titlebar and the taskbar showing the
generic Windows application icon.

GLFW registers its window class with

    LoadImageW(instance, L"GLFW_ICON", IMAGE_ICON, ...)

and falls back to IDI_APPLICATION when that lookup fails. The lookup is by
*name*. An icon declared with a numeric id - `1 ICON "..."`, the usual
incantation - is invisible to it. Nothing warns you: the build succeeds, the
resource is there, and the window is wrong.

So this walks the PE resource directory of a built executable and asserts that
the icon group is named GLFW_ICON, and that the group carries enough images for
the sizes Windows asks for (16 and 24 for the titlebar, 32 and 48 for the
taskbar). Source-level greps cannot tell you whether windres ran.

Usage: check_win_icon.py bencsynth.exe
"""

import struct
import sys

RT_ICON = 3
RT_GROUP_ICON = 14


def resource_dir_offset(data):
    """Return (file offset of .rsrc, its RVA), or raise."""
    if data[:2] != b"MZ":
        raise SystemExit("not a PE file: no MZ header")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise SystemExit("not a PE file: no PE signature")
    nsections = struct.unpack_from("<H", data, pe + 6)[0]
    optsize = struct.unpack_from("<H", data, pe + 20)[0]
    sections = pe + 24 + optsize
    for i in range(nsections):
        s = sections + 40 * i
        name = data[s:s + 8].rstrip(b"\0").decode("latin-1")
        if name == ".rsrc":
            rva = struct.unpack_from("<I", data, s + 12)[0]
            raw = struct.unpack_from("<I", data, s + 20)[0]
            return raw, rva
    raise SystemExit("no .rsrc section - resources were never linked")


def entries(data, base, off):
    """Yield (name_or_id, is_directory, child_offset) for one directory node."""
    nnamed, nid = struct.unpack_from("<HH", data, base + off + 12)
    for i in range(nnamed + nid):
        e = base + off + 16 + 8 * i
        name_field, child = struct.unpack_from("<II", data, e)
        if name_field & 0x80000000:
            n = base + (name_field & 0x7FFFFFFF)
            length = struct.unpack_from("<H", data, n)[0]
            key = data[n + 2:n + 2 + 2 * length].decode("utf-16-le")
        else:
            key = name_field
        yield key, bool(child & 0x80000000), child & 0x7FFFFFFF


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: check_win_icon.py <exe>")
    path = sys.argv[1]
    data = open(path, "rb").read()
    base, _ = resource_dir_offset(data)

    groups, icons = [], 0
    for key, is_dir, off in entries(data, base, 0):
        if not is_dir:
            continue
        if key == RT_GROUP_ICON:
            groups = [k for k, _, _ in entries(data, base, off)]
        elif key == RT_ICON:
            icons = sum(1 for _ in entries(data, base, off))

    if not groups:
        raise SystemExit("FAIL: no icon group in %s" % path)
    if "GLFW_ICON" not in groups:
        raise SystemExit(
            "FAIL: the icon group is named %r, not 'GLFW_ICON'.\n"
            "      Explorer will look right and the titlebar and taskbar will "
            "not.\n"
            "      Declare it as `GLFW_ICON ICON \"...\"` in the .rc."
            % (groups[0],))
    if icons < 4:
        raise SystemExit(
            "FAIL: the icon group holds %d image(s). Windows picks a different "
            "size for\n      the titlebar (16, 24) than the taskbar (32, 48); "
            "with too few it\n      rescales one and it looks soft." % icons)

    print("ok: icon group 'GLFW_ICON' present, %d images - "
          "GLFW will use it for the window class" % icons)


if __name__ == "__main__":
    main()
