/*
 * Reparenting the editor's window into a host's.
 *
 * This is its own file for one reason: <windows.h> and raylib cannot be
 * included together. Both define Rectangle, CloseWindow, ShowCursor and
 * several more, and the usual workaround - defining NOGDI and NOUSER before
 * windows.h - removes exactly the USER functions this needs. So raylib stays
 * on one side of the wall, windows.h on the other, and a void* crosses it.
 */
#ifndef BS_EMBED_H
#define BS_EMBED_H

#include <cstdint>

namespace bs {

/* `child` is what raylib's GetWindowHandle() returned. That is an HWND on
 * Windows and - despite the name - a GLFWwindow* on Linux, where raylib's own
 * X11 branch is commented out with a TODO. `parent` is the host's handle from
 * gui.set_parent. Does nothing on macOS, where pixels cross the process
 * boundary instead; see bs_cocoa.h. */
void bs_embed_window(void *child, uint64_t parent, int w, int h);

/* The host resized its container. Windows and X11 only. */
void bs_embed_resize(void *child, int w, int h);

} /* namespace bs */

#endif
