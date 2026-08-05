/*
 * BENCsynth - the rack
 *
 * Draws the modules, runs the cables, and turns mouse gestures into edits on
 * the patch. Immediate mode throughout: there is no retained widget tree, so
 * a module added this frame is patchable this frame, and a module deleted
 * this frame leaves nothing behind to dangle.
 *
 * Panel geometry comes from bs_module.h rather than from here. The rack draws
 * what the module declared it has, in the order it declared it, which is why
 * adding a knob to a module is a one-line change and not a layout exercise.
 */

#ifndef BS_RACK_H
#define BS_RACK_H

#include "bs_gui.h"
#include "bs_rope.h"
#include "bs_engine.h"

#include <vector>

typedef struct bs_rack {
    /* The rack-space point at the top left of the viewport, and how many
     * screen pixels a rack unit is worth. Everything inside the viewport is
     * laid out, hit-tested and simulated in rack units; the camera turns them
     * into pixels once, at the point of drawing. */
    float scrollX, scrollY;
    float zoom;

    /* Moving a panel. */
    int     dragModule;
    Vector2 dragGrab;      /* where in the panel it was taken hold of */

    /* Dragging the rack itself. `panGrab` is in screen pixels, because the
     * gesture is "this many pixels of pointer movement" whatever the zoom. */
    int     panning;
    Vector2 panGrab;
    float   panScrollX, panScrollY;

    /* A cable with one end in the hand. `fromOutput` says which kind of jack
     * the fixed end is, because patching backwards - from an input, looking
     * for a source - is a thing people do and there is no reason to refuse
     * it. */
    int     patching;
    int     fromModule, fromPort, fromOutput;
    int     fromColor;
    bs_rope dragRope;

    /* One rope per cable slot, kept in step with the patch by the key below:
     * a cable slot is reused as soon as it is freed, so "same index" is not
     * enough to know it is the same cable. */
    std::vector<bs_rope> ropes;
    std::vector<long>    ropeKey;

    /* Caret and scroll for each scratchpad panel, by module id. Kept here
     * because a text area is a caret in a buffer that has to survive between
     * frames, and immediate mode leaves it nowhere else to live. */
    std::vector<bs_edit> edits;

    int hoverCable;        /* the cable under the pointer, for tracing a patch */
    int menuModule;        /* whose context menu is open */
    Vector2 menuRackPos;   /* where a module added from the menu should land */

    /* Set for one frame when the patch changed, so the window can say so. */
    int edited;

    /* The factory rack just loaded from the menu, or -1. The rack owns the
     * menus, so it is the rack that acts on the choice; the window only wants
     * to know so it can say which one arrived. */
    int presetLoaded;
} bs_rack;

void bs_rack_init(bs_rack *r);

/* Everything: layout, input and drawing, in one pass. `view` is the screen
 * rectangle the rack lives in. */
void bs_rack_frame(bs_rack *r, bs_ui *ui, bs::Engine *eng, Rectangle view, float dt);

/* Puts the scroll back where a fresh rack starts. */
void bs_rack_home(bs_rack *r);

/* Call when the whole rack has been swapped out - loaded, cleared, reset to
 * the default. The cable ropes are keyed on which jacks they join, and after a
 * wholesale replacement an identical-looking connection between different
 * modules can hash to the same key; the rope would then keep the geometry, and
 * the length, of a cable that no longer exists. Homes the scroll too, since
 * the rack under it is not the one that was being looked at. */
void bs_rack_patch_replaced(bs_rack *r);

/* Menu results, handled after the frame so a module can be deleted without
 * the draw loop noticing it has gone. Returns nonzero if it did something. */
int  bs_rack_menu(bs_rack *r, bs_ui *ui, bs::Engine *eng);

/* The same menu the rack's own right-click opens, for a toolbar button. The
 * module lands near the top left of what is currently on screen, because a
 * menu opened from the toolbar has no position of its own to speak of. */
void bs_rack_add_menu(bs_rack *r, bs_ui *ui, Vector2 screenAt, Rectangle view);

/* The factory racks, for a toolbar button. Choosing one replaces everything. */
void bs_rack_preset_menu(bs_rack *r, bs_ui *ui, Vector2 screenAt);

/* The rack coordinates of a module's jack, in case anything outside needs to
 * point at one. */
Vector2 bs_rack_jack_pos(const bs::Module *m, int port, int isOutput);

#endif /* BS_RACK_H */
