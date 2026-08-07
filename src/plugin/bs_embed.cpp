#include "bs_embed.h"

#if defined(_WIN32)
#  include <windows.h>
#elif defined(__linux__)
#  include <X11/Xlib.h>
/* Declared rather than included: GLFW's native header wants the whole GLFW
 * configuration to agree, and these two symbols are already in the raylib we
 * link against. */
extern "C" unsigned long glfwGetX11Window(void *window);
extern "C" void         *glfwGetX11Display(void);
#endif

namespace bs {

void bs_embed_window(void *child, uint64_t parent, int w, int h)
{
#if defined(_WIN32)
    HWND c = (HWND)child;
    HWND p = (HWND)(uintptr_t)parent;
    if (!c || !p) return;

    /* A window created as top-level keeps its caption and border when it is
     * reparented, which inside somebody's FX rack reads as a bug. WS_CHILD
     * drops both; WS_CLIPSIBLINGS keeps it from drawing over the host's own
     * controls. */
    SetWindowLongPtrA(c, GWL_STYLE, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS);
    SetParent(c, p);
    SetWindowPos(c, 0, 0, 0, w, h, SWP_NOZORDER | SWP_SHOWWINDOW);
#elif defined(__linux__)
    /* X11 reparents, the same idea as SetParent, and works across processes
     * for the same reason: a Window is a server-side object with an id, not a
     * pointer into anybody's address space. That is the thing macOS does not
     * have.
     *
     * raylib will not hand over the Window. GetWindowHandle returns
     * platform.handle on Linux - the GLFWwindow* - with the X11 call sitting
     * right above it commented out and a TODO beside it. GLFW's own accessor
     * is in the library we already link, so it is declared here rather than
     * pulling in GLFW's native header, which would want the whole GLFW
     * configuration to agree. */
    if (!child || !parent) return;
    Display *dpy = (Display *)glfwGetX11Display();
    const Window win = glfwGetX11Window(child);
    if (!dpy || !win) return;

    XReparentWindow(dpy, win, (Window)parent, 0, 0);
    XResizeWindow(dpy, win, (unsigned)(w > 0 ? w : 1), (unsigned)(h > 0 ? h : 1));
    XMapWindow(dpy, win);
    /* Or the reparent sits in the queue until something else happens to flush
     * it, which looks like an embed that worked once every few tries. */
    XFlush(dpy);
#else
    (void)child; (void)parent; (void)w; (void)h;
#endif
}

void bs_embed_resize(void *child, int w, int h)
{
#if defined(_WIN32)
    HWND c = (HWND)child;
    if (c) SetWindowPos(c, 0, 0, 0, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
#elif defined(__linux__)
    if (!child) return;
    Display *dpy = (Display *)glfwGetX11Display();
    const Window win = glfwGetX11Window(child);
    if (!dpy || !win) return;
    XResizeWindow(dpy, win, (unsigned)(w > 0 ? w : 1), (unsigned)(h > 0 ? h : 1));
    XFlush(dpy);
#else
    (void)child; (void)w; (void)h;
#endif
}

} /* namespace bs */
