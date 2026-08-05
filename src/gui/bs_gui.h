/*
 * BENCsynth GUI - theme and widget set
 *
 * Drawn here rather than taken from a toolkit, for the same reason BENCmouth
 * draws its own: the BENCO look is flat fills, thin dim borders and small
 * radii, which is what an immediate-mode renderer produces by default, and a
 * native widget set would have to be argued out of its scrollbars, focus
 * rings and animations at every step. A knob and a patch jack are not in any
 * toolkit anyway.
 *
 * Every colour here comes from the BENCO style guide and nowhere else.
 */

#ifndef BS_GUI_H
#define BS_GUI_H

#include "raylib.h"
#include "bs_module.h"

#include <string>

/* ------------------------------------------------------------------ *
 * Palette
 * ------------------------------------------------------------------ */

extern Color BS_BG;        /* window background, a green-tinted near-black */
extern Color BS_RACK;      /* the cabinet behind the panels, one step down */
extern Color BS_PANEL;     /* a module's face                              */
extern Color BS_PANEL_HI;  /* its title bar                                */
extern Color BS_BORDER;    /* 1px, dim, never decorative                   */
extern Color BS_TEXT;      /* phosphor - the screen-glow green, not white  */
extern Color BS_DIM;       /* labels, captions, anything secondary         */
extern Color BS_ACCENT;    /* the brand green: fills, active states        */
extern Color BS_EDGE;      /* pressed states and outlines                  */
extern Color BS_ALERT;     /* errors, clipping                             */
extern Color BS_AMBER;     /* warnings                                     */
extern Color BS_STAR;      /* S. Tarr                                      */
extern Color BS_STAR_EDGE;

/* The roster colours, used for patch cables. Seven characters, seven cable
 * colours, each with the 30-40% darker edge the style guide pairs it with -
 * which is exactly what a cable needs anyway, a body and a shadow. */
#define BS_CABLE_COLORS 7
extern Color BS_CABLE[BS_CABLE_COLORS];
extern Color BS_CABLE_EDGE[BS_CABLE_COLORS];

#define BS_RADIUS  3
#define BS_PAD     10

/* Terminus is a bitmap design: crisp at its native sizes, mush between them.
 * These are native sizes and the font is loaded with point filtering to keep
 * it that way. Unantialiased text at fixed sizes is the terminal look arrived
 * at honestly rather than simulated. */
enum {
    BS_F_TINY  = 12,   /* jack and knob labels, readouts */
    BS_F_SMALL = 16,   /* module titles, buttons         */
    BS_F_BODY  = 20,   /* status line                    */
    BS_F_TITLE = 28    /* the wordmark                   */
};

/* ------------------------------------------------------------------ *
 * State
 * ------------------------------------------------------------------ */

typedef struct bs_ui {
    Font tiny, small, body, title;
    int  loaded;
    const char *font_name;

    /* Which continuous control has the mouse. One at a time, by caller-chosen
     * id, so a drag that wanders off its own rectangle keeps going - which is
     * what dragging a knob is, most of the time. Zero is nobody. */
    int   active;
    float grabY;        /* mouse y when the drag started                  */
    float grabValue;    /* the control's position then, 0..1              */

    /* Double-click detection, for the reset-to-default gesture. */
    int    lastId;
    double lastClick;

    /* A menu popped up over the layout has to draw after everything it covers
     * and take the mouse away from what is underneath. Immediate mode gives
     * neither for free, so the menu publishes its rectangle here as it is
     * declared, widgets called later ignore a mouse inside it, and the drawing
     * is deferred to bs_ui_overlay. */
    int       menuOpen;
    Rectangle menuRect;
    const char **menuItems;
    int       menuCount;
    int       menuHover;
    Vector2   menuAnchor;
    int       menuTag;      /* what the menu belongs to, for the caller  */

    /* Set for one frame when a control changed, so the caller can mark the
     * patch dirty without every widget returning a tuple. */
    int  changed;

    /* Which text area has the keyboard, by caller-chosen id. Zero is nobody,
     * and nobody is the usual state. While it is set, the window stops feeding
     * keystrokes to the musical typing - otherwise typing a note into a
     * scratchpad also plays it. */
    int  focus;

    /* Set by the caller around a group of controls that must not take the
     * mouse this frame - a panel with another one drawn over it, or anything
     * at all while a cable is being dragged across the rack. A control
     * already being dragged keeps the mouse regardless, which is what stops a
     * knob from letting go when the pointer wanders over a neighbour. */
    int  suppress;
} bs_ui;

void bs_ui_init(bs_ui *ui);
void bs_ui_free(bs_ui *ui);

/* Finds a file shipped beside the program - a font, the wordmark. Tries the
 * executable's own directory before the working directory, and knows where a
 * macOS bundle keeps its Resources. Returns a path that exists, written into
 * `probe`, or null. */
const char *bs_find_asset(const char *relative, char *probe, size_t cap);

/* A small round information button, the same one BENCmouth puts in its corner:
 * a lowercase i in a circle. */
int bs_info_button(bs_ui *ui, Rectangle r, int lit);

/* True when something owns the mouse this frame and ordinary controls must
 * keep their hands off it - an open menu, or a caller that has set `suppress`.
 * Takes no position: it tests the real pointer against the menu, which is
 * drawn in screen space even while the rack underneath is not. */
int  bs_ui_blocked(const bs_ui *ui);

/* The pointer, in whatever space the caller is currently drawing in.
 *
 * The rack draws under a zoom transform, so a knob's rectangle is in rack
 * units while GetMousePosition() answers in screen pixels, and hit-testing one
 * against the other is wrong by exactly the zoom factor. Widgets ask through
 * this instead, and the rack answers in the space it is drawing in. Everywhere
 * else it is GetMousePosition() unchanged. */
void    bs_ui_push_mouse(Vector2 where);
void    bs_ui_pop_mouse(void);
Vector2 bs_mouse(void);

/* ------------------------------------------------------------------ *
 * Text
 * ------------------------------------------------------------------ */

/* Text is drawn by size and the size picks the font. Passing a Font and
 * inferring the size from it breaks the moment two sizes fall back to the
 * same built-in face, which is what happens when no Terminus is installed. */
void  bs_text(const bs_ui *ui, int size, const char *s, float x, float y, Color c);
void  bs_text_spaced(const bs_ui *ui, int size, const char *s, float x, float y, Color c);
void  bs_text_center(const bs_ui *ui, int size, const char *s, float cx, float y, Color c);
float bs_measure(const bs_ui *ui, int size, const char *s, float spacing);

/* Draws as much of `s` as fits in `w`, ellipsised. Panels are narrow and a
 * readout that runs into its neighbour is worse than one that is cut. */
void  bs_text_fit(const bs_ui *ui, int size, const char *s, float cx, float y,
                  float w, Color c);

/* ------------------------------------------------------------------ *
 * Chrome
 * ------------------------------------------------------------------ */

void bs_panel(Rectangle r, Color fill, Color border);
void bs_divider(float x, float y, float w);

int  bs_button(bs_ui *ui, int id, Rectangle r, const char *label, int enabled);
int  bs_button_lit(bs_ui *ui, int id, Rectangle r, const char *label, int lit);

/* ------------------------------------------------------------------ *
 * Panel controls
 * ------------------------------------------------------------------ */

/* A knob fills `cell` - name above, dial in the middle, readout below.
 * Vertical drag is coarse, shift is fine, the wheel steps, and a double click
 * puts it back to the value the module was built with. Returns nonzero on any
 * frame the value moved. */
int bs_knob(bs_ui *ui, int id, Rectangle cell, bs::Param *p);

/* A named switch, drawn as a small flat button cycling through its positions.
 * Left click advances, right click goes back - with three positions, always
 * going forwards means two clicks to step back one. */
int bs_switch(bs_ui *ui, int id, Rectangle cell, bs::Param *p);

/* Off/on, filled when on. The state is the fill, so it reads at a glance. */
int bs_toggle(bs_ui *ui, int id, Rectangle cell, bs::Param *p);

/* A patch point. Returns 1 while hovered. `plugged` draws the plug sitting in
 * it, in the cable's colour. */
enum { BS_JACK_R = 9 };
void bs_jack(const bs_ui *ui, Rectangle cell, const char *label,
             int plugged, Color plugColor, int hot, int isOutput);
Vector2 bs_jack_center(Rectangle cell);

/* Level, two ways: `rms` is the fill and `peak` a marker over it. They answer
 * different questions and a resonant filter makes them disagree constantly. */
void bs_meter(Rectangle r, float peakL, float rmsL, float peakR, float rmsR, int clip);

/* An oscilloscope trace. `ring` is a wrapping buffer, `head` the next write. */
void bs_scope(Rectangle r, const float *ring, int len, int head, Color c);

/* ------------------------------------------------------------------ *
 * S. Tarr
 *
 * The mascot, drawn rather than loaded: a five-pointed star with a visor, from
 * the roster SVG. Drawn because a vector shape this simple is fewer lines than
 * the code to find, load and scale a file, and it stays crisp at any size the
 * window happens to be.
 * ------------------------------------------------------------------ */

void bs_star(Vector2 center, float radius, float rotation);

/* The same mark, rasterised into a square RGBA image with a transparent
 * background - the window icon, and the source every packaged icon file is
 * generated from.
 *
 * Rasterised from the geometry rather than loaded from a file so that the icon
 * in the taskbar and the icon in the .ico cannot drift apart, and so that a
 * build which has lost its assets folder still has an icon. The caller owns
 * the image and must UnloadImage it. */
Image bs_star_image(int size);

/* ------------------------------------------------------------------ *
 * Menu
 * ------------------------------------------------------------------ */

void bs_menu_open(bs_ui *ui, Vector2 at, const char **items, int count, int tag);
void bs_menu_close(bs_ui *ui);
/* Returns the index chosen this frame, or -1. Call before bs_ui_overlay. */
int  bs_menu_take(bs_ui *ui, int *tag);

/* ------------------------------------------------------------------ *
 * Text area
 *
 * A scrolling, editable, soft-wrapped text box: caret, selection, clipboard
 * and a scrollbar when the text outgrows it.
 *
 * Wrapping and caret placement lean on the interface font being monospace,
 * which Terminus is and raylib's fallback is too. That turns "which character
 * is under this pixel" from a measuring problem into a division, and it is the
 * difference between this being a few hundred lines and being a project.
 *
 * `id` is any nonzero number unique among the areas on screen; it is what
 * focus is tracked by. Returns nonzero on any frame the text changed.
 * ------------------------------------------------------------------ */

typedef struct bs_edit {
    int   caret;      /* byte index of the insertion point                */
    int   sel;        /* the other end of the selection; == caret if none */
    float scroll;     /* pixels scrolled down                             */
    float blink;
    int   dragText;   /* sweeping out a selection with the mouse          */
    int   dragBar;    /* dragging the scrollbar thumb                     */
    float dragGrab;   /* where in the thumb it was taken hold of          */
} bs_edit;

int bs_textarea(bs_ui *ui, int id, Rectangle r, std::string &text, bs_edit *st,
                int editable);

/* Draws whatever popped up, above everything. Call once, last. */
void bs_ui_overlay(bs_ui *ui);

#endif /* BS_GUI_H */
