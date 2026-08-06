/*
 * The macOS half of embedding, which is a different problem from the others.
 *
 * On Windows a window is an HWND - a kernel object - and SetParent puts one
 * process's window inside another's with a single call. X11 has
 * XReparentWindow. macOS has neither: an NSView is an Objective-C object in
 * one process's address space, and Apple does not intend view hierarchies to
 * span processes. Pointers do not cross that boundary.
 *
 * So the pixels have to. The editor renders into an IOSurface - a buffer the
 * kernel can hand to another process - and the plugin displays that surface in
 * a CALayer inside the view the host gave it. Input makes the return trip as
 * events. That is the whole design, and it is why this file exists at all.
 *
 * IOSurface rather than CALayerHost: the latter is what everyone reaches for
 * and it is private API. IOSurface is public, documented, and does the same
 * job here.
 *
 * Plain C entry points so the CLAP wrapper stays C++ and never sees an
 * Objective-C type.
 */
#ifndef BS_COCOA_H
#define BS_COCOA_H

namespace bs {

/* Creates our view inside the host's and returns it, or null. Main thread
 * only, which CLAP guarantees for every gui.* call. */
void *bs_cocoa_attach(void *parentNSView, int w, int h);

/* Removes and releases it. */
void  bs_cocoa_detach(void *view);

/* The host resized its window; match it. */
void  bs_cocoa_resize(void *view, int w, int h);

/* What the panel says while there is no rack in it yet. Stage 1 has no
 * pixels from the editor, so it says so rather than showing a blank. */
void  bs_cocoa_set_status(void *view, const char *text);

} /* namespace bs */

#endif
