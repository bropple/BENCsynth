#include "bs_keyboard.h"

#include <cmath>
#include <cstdio>
#include <cstring>

/* Four octaves and the C on top. Wider than that and the keys get too narrow
 * to hit; narrower and the typing layout runs off the end of what is drawn,
 * which makes the highlight lie about what is sounding. */
enum { WHITE_KEYS = 29 };

static const int WHITE_SEMI[7] = { 0, 2, 4, 5, 7, 9, 11 };
/* The degrees that have a black key to their right: C, D, F, G, A. */
static const int HAS_SHARP[7]  = { 1, 1, 0, 1, 1, 1, 0 };

/* The layout every tracker and DAW has settled on: the lower two rows of the
 * keyboard are one octave, the upper two are the next. Anyone who has used
 * one already knows this one. */
typedef struct { int key; int semi; } bs_keymap;

static const bs_keymap MAP[] = {
    { KEY_Z, 0 }, { KEY_S, 1 }, { KEY_X, 2 }, { KEY_D, 3 }, { KEY_C, 4 },
    { KEY_V, 5 }, { KEY_G, 6 }, { KEY_B, 7 }, { KEY_H, 8 }, { KEY_N, 9 },
    { KEY_J, 10 }, { KEY_M, 11 }, { KEY_COMMA, 12 }, { KEY_L, 13 },
    { KEY_PERIOD, 14 }, { KEY_SEMICOLON, 15 }, { KEY_SLASH, 16 },

    { KEY_Q, 12 }, { KEY_TWO, 13 }, { KEY_W, 14 }, { KEY_THREE, 15 },
    { KEY_E, 16 }, { KEY_R, 17 }, { KEY_FIVE, 18 }, { KEY_T, 19 },
    { KEY_SIX, 20 }, { KEY_Y, 21 }, { KEY_SEVEN, 22 }, { KEY_U, 23 },
    { KEY_I, 24 }, { KEY_NINE, 25 }, { KEY_O, 26 }, { KEY_ZERO, 27 },
    { KEY_P, 28 }
};
static const int MAP_N = (int)(sizeof MAP / sizeof MAP[0]);

static int baseNote(const bs_keyboard *k) { return 12 * (k->octave + 1); }

/* Which keys draw lit.
 *
 * Answered from what this keyboard is holding rather than by asking the
 * engine, because the voice allocator belongs to the audio thread and reading
 * its array to light up a key would be exactly the cross-thread peek the event
 * queue exists to remove. This is also the more accurate answer: it lights the
 * keys that are down, not the voices that are sounding, so a note still
 * ringing out its release does not leave a key stuck on. */
static int holding(const bs_keyboard *k, int note)
{
    if (k->mouseNote == note) return 1;
    for (int i = 0; i < MAP_N; i++) if (k->typed[i] == note) return 1;
    return 0;
}

void bs_keyboard_init(bs_keyboard *k)
{
    memset(k, 0, sizeof *k);
    k->octave    = 3;          /* the lower row starts at C3, the upper at C4 */
    k->velocity  = 0.85f;
    k->mouseNote = -1;
    for (int i = 0; i < 128; i++) k->typed[i] = -1;
}

void bs_keyboard_release_all(bs_keyboard *k, bs::Engine *eng)
{
    for (int i = 0; i < MAP_N; i++) {
        if (k->typed[i] < 0) continue;
        eng->noteOff(k->typed[i]);
        k->typed[i] = -1;
    }
    if (k->mouseNote >= 0) { eng->noteOff(k->mouseNote); k->mouseNote = -1; }
}

/* ------------------------------------------------------------------ *
 * Typing
 * ------------------------------------------------------------------ */

void bs_keyboard_typing(bs_keyboard *k, bs::Engine *eng, int enabled)
{
    if (!enabled) return;

    if (IsKeyPressed(KEY_LEFT_BRACKET)  && k->octave > 0) k->octave--;
    if (IsKeyPressed(KEY_RIGHT_BRACKET) && k->octave < 7) k->octave++;

    if (IsKeyPressed(KEY_SPACE)) { k->sustain = 1; eng->setSustain(true); }
    if (IsKeyReleased(KEY_SPACE)) { k->sustain = 0; eng->setSustain(false); }

    const int base = baseNote(k);
    for (int i = 0; i < MAP_N; i++) {
        if (IsKeyPressed(MAP[i].key) && k->typed[i] < 0) {
            /* The note actually played is remembered rather than recomputed
             * on release, so shifting octave with a key held does not strand
             * a note that nothing will ever turn off. */
            k->typed[i] = base + MAP[i].semi;
            eng->noteOn(k->typed[i], k->velocity);
        }
        if (IsKeyReleased(MAP[i].key) && k->typed[i] >= 0) {
            eng->noteOff(k->typed[i]);
            k->typed[i] = -1;
        }
    }
}

/* ------------------------------------------------------------------ *
 * Wheels
 * ------------------------------------------------------------------ */

static void wheel(bs_ui *ui, Rectangle r, const char *label, float *value,
                  int bipolar, int *dragging)
{
    const Vector2 m = GetMousePosition();
    const float thumbH = 18.0f;
    Rectangle track = { r.x, r.y, r.width, r.height - 14.0f };

    if (!bs_ui_blocked(ui, m) && CheckCollisionPointRec(m, track) &&
        IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) *dragging = 1;
    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        if (*dragging && bipolar) *value = 0.0f;   /* pitch springs back */
        *dragging = 0;
    }
    if (*dragging) {
        float t = 1.0f - (m.y - track.y - thumbH * 0.5f) / (track.height - thumbH);
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        *value = bipolar ? (t * 2.0f - 1.0f) : t;
    }

    const float t = bipolar ? (*value * 0.5f + 0.5f) : *value;
    DrawRectangleRec(track, (Color){ 0x04, 0x07, 0x03, 255 });
    DrawRectangleLinesEx(track, 1.0f, BS_BORDER);
    if (bipolar)
        DrawRectangle((int)track.x + 2, (int)(track.y + track.height * 0.5f),
                      (int)track.width - 4, 1, BS_BORDER);

    Rectangle th = { track.x + 2.0f,
                     track.y + (1.0f - t) * (track.height - thumbH),
                     track.width - 4.0f, thumbH };
    DrawRectangleRec(th, *dragging ? BS_ACCENT : BS_EDGE);
    DrawRectangle((int)th.x, (int)(th.y + th.height * 0.5f), (int)th.width, 1,
                  *dragging ? BS_RACK : BS_DIM);

    bs_text_center(ui, BS_F_TINY, label, r.x + r.width * 0.5f,
                   r.y + r.height - 12.0f, BS_DIM);
}

/* ------------------------------------------------------------------ *
 * The strip
 * ------------------------------------------------------------------ */

void bs_keyboard_frame(bs_keyboard *k, bs_ui *ui, bs::Engine *eng, Rectangle area)
{
    const Vector2 m = GetMousePosition();

    DrawRectangleRec(area, BS_RACK);
    DrawRectangle((int)area.x, (int)area.y, (int)area.width, 1, BS_BORDER);

    /* ---- left-hand controls ---- */

    Rectangle box = { area.x + 8.0f, area.y + 8.0f, 150.0f, area.height - 16.0f };

    Rectangle octRow = { box.x, box.y, box.width, 22.0f };
    bs_text_spaced(ui, BS_F_TINY, "OCTAVE", octRow.x, octRow.y + 5.0f, BS_DIM);
    Rectangle bDown = { octRow.x + 62.0f, octRow.y, 22.0f, 22.0f };
    Rectangle bUp   = { octRow.x + 88.0f, octRow.y, 22.0f, 22.0f };
    if (bs_button(ui, 9001, bDown, "-", k->octave > 0)) k->octave--;
    if (bs_button(ui, 9002, bUp,   "+", k->octave < 7)) k->octave++;
    char oct[16];
    snprintf(oct, sizeof oct, "C%d", k->octave);
    bs_text(ui, BS_F_SMALL, oct, octRow.x + 116.0f, octRow.y + 3.0f, BS_TEXT);

    Rectangle wBend = { box.x, box.y + 28.0f, 44.0f, box.height - 28.0f };
    Rectangle wMod  = { box.x + 52.0f, box.y + 28.0f, 44.0f, box.height - 28.0f };
    wheel(ui, wBend, "BEND", &k->bend, 1, &k->dragBend);
    wheel(ui, wMod,  "MOD",  &k->mod,  0, &k->dragMod);

    /* Only when they move. These post events now, and a wheel sitting still
     * has no business filling the queue sixty times a second with the value it
     * already sent. */
    if (k->bend != k->sentBend) { eng->setBend(k->bend); k->sentBend = k->bend; }
    if (k->mod  != k->sentMod)  { eng->setMod(k->mod);   k->sentMod  = k->mod; }

    Rectangle sus = { box.x + 104.0f, box.y + 28.0f, 46.0f, 26.0f };
    if (bs_button_lit(ui, 9003, sus, "SUS", k->sustain)) {
        k->sustain = !k->sustain;
        eng->setSustain(k->sustain != 0);
    }
    bs_text(ui, BS_F_TINY, "SPACE", box.x + 104.0f, box.y + 58.0f, BS_EDGE);
    bs_text(ui, BS_F_TINY, "[ ] OCT", box.x + 104.0f, box.y + 72.0f, BS_EDGE);
    bs_text(ui, BS_F_TINY, "ESC ALL", box.x + 104.0f, box.y + 86.0f, BS_EDGE);

    /* ---- keys ---- */

    Rectangle keys = { box.x + box.width + 12.0f, area.y + 8.0f,
                       area.x + area.width - (box.x + box.width + 12.0f) - 8.0f,
                       area.height - 16.0f };
    if (keys.width < 100.0f) { keys.width = 100.0f; }

    const float ww = keys.width / (float)WHITE_KEYS;
    const float bw = ww * 0.62f;
    const float bh = keys.height * 0.62f;
    const int   base = baseNote(k);

    /* What the pointer is over. Black keys are tested first because they are
     * drawn on top of the whites and overlap them - testing in draw order
     * would make the upper half of every black key play the white beneath. */
    int over = -1;
    float overVel = k->velocity;
    if (CheckCollisionPointRec(m, keys) && !bs_ui_blocked(ui, m)) {
        for (int i = 0; i < WHITE_KEYS - 1 && over < 0; i++) {
            const int deg = i % 7;
            if (!HAS_SHARP[deg]) continue;
            Rectangle r = { keys.x + (float)(i + 1) * ww - bw * 0.5f, keys.y, bw, bh };
            if (CheckCollisionPointRec(m, r)) {
                over = base + (i / 7) * 12 + WHITE_SEMI[deg] + 1;
                overVel = 0.35f + 0.65f * (m.y - r.y) / r.height;
            }
        }
        for (int i = 0; i < WHITE_KEYS && over < 0; i++) {
            Rectangle r = { keys.x + (float)i * ww, keys.y, ww, keys.height };
            if (CheckCollisionPointRec(m, r)) {
                over = base + (i / 7) * 12 + WHITE_SEMI[i % 7];
                overVel = 0.35f + 0.65f * (m.y - r.y) / r.height;
            }
        }
    }

    /* Press, then glide: holding the button and sliding across the keys
     * releases each note as the next one is taken, which is a glissando and
     * costs nothing to support. */
    if (over >= 0 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (k->mouseNote >= 0) eng->noteOff(k->mouseNote);
        k->mouseNote = over;
        eng->noteOn(over, overVel);
    } else if (k->mouseNote >= 0 && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        if (over != k->mouseNote) {
            eng->noteOff(k->mouseNote);
            k->mouseNote = -1;
            if (over >= 0) { k->mouseNote = over; eng->noteOn(over, overVel); }
        }
    }
    if (k->mouseNote >= 0 && !IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        eng->noteOff(k->mouseNote);
        k->mouseNote = -1;
    }

    for (int i = 0; i < WHITE_KEYS; i++) {
        const int note = base + (i / 7) * 12 + WHITE_SEMI[i % 7];
        Rectangle r = { keys.x + (float)i * ww, keys.y, ww - 1.0f, keys.height };
        const int held = holding(k, note);
        DrawRectangleRec(r, held ? BS_ACCENT : (note == over ? BS_TEXT : BS_DIM));
        DrawRectangleLinesEx(r, 1.0f, BS_EDGE);

        /* C is labelled, and nothing else is. It is the only landmark anyone
         * looks for, and a label on every key turns the keyboard into a wall
         * of text. */
        if (i % 7 == 0) {
            char lab[8];
            snprintf(lab, sizeof lab, "C%d", k->octave + i / 7);
            bs_text_center(ui, BS_F_TINY, lab, r.x + r.width * 0.5f,
                           r.y + r.height - 15.0f, BS_RACK);
        }
    }

    for (int i = 0; i < WHITE_KEYS - 1; i++) {
        const int deg = i % 7;
        if (!HAS_SHARP[deg]) continue;
        const int note = base + (i / 7) * 12 + WHITE_SEMI[deg] + 1;
        Rectangle r = { keys.x + (float)(i + 1) * ww - bw * 0.5f, keys.y, bw, bh };
        const int held = holding(k, note);
        DrawRectangleRec(r, held ? BS_ACCENT : (note == over ? BS_EDGE : BS_RACK));
        DrawRectangleLinesEx(r, 1.0f, BS_EDGE);
    }
}
