/*
 * BENCsynth - the keyboard
 *
 * Four octaves of keys along the bottom, playable with the mouse or with the
 * computer keyboard. Both routes end at the same engine calls, so a note held
 * with a finger and a note held with a mouse button behave identically -
 * including releasing correctly when the other one takes the same key.
 *
 * The wheels sit to the left where they sit on the hardware, and they spring
 * back when released, because a pitch wheel that stays where you left it is a
 * synthesizer that is permanently out of tune.
 */

#ifndef BS_KEYBOARD_H
#define BS_KEYBOARD_H

#include "bs_gui.h"
#include "bs_engine.h"

typedef struct bs_keyboard {
    int   octave;        /* the MIDI octave the typing layout starts on */
    float velocity;      /* what typed notes play at                    */

    int   mouseNote;     /* the key the mouse is currently holding, -1 none */
    int   typed[128];    /* notes the computer keyboard is holding          */

    /* Wheels. Held while dragged, sprung back when let go - bend snaps to
     * centre, mod stays where it is put, which is what each of them does on
     * every instrument that has both. */
    float bend, mod;
    int   dragBend, dragMod;

    int   sustain;
} bs_keyboard;

void bs_keyboard_init(bs_keyboard *k);

/* Drawing and mouse. Call once a frame with the strip it occupies. */
void bs_keyboard_frame(bs_keyboard *k, bs_ui *ui, bs::Engine *eng, Rectangle area);

/* Musical typing. Kept apart from the frame so the window can turn it off
 * while something else wants the keys, and so it runs whether or not the
 * pointer is anywhere near the keyboard. */
void bs_keyboard_typing(bs_keyboard *k, bs::Engine *eng, int enabled);

/* Everything up, everywhere. */
void bs_keyboard_release_all(bs_keyboard *k, bs::Engine *eng);

#endif /* BS_KEYBOARD_H */
