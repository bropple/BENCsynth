/* The two sources of input, behind one set of names. See bs_input.h. */

#include "raylib.h"
#include "bs_shm.h"

#include <cstring>

/* Undo the shadowing for this file alone - it is the one place that has to
 * reach the real functions. */
#include "bs_input.h"
#undef IsMouseButtonPressed
#undef IsMouseButtonDown
#undef IsMouseButtonReleased
#undef GetMousePosition
#undef GetMouseWheelMove
#undef IsKeyPressed
#undef IsKeyPressedRepeat
#undef IsKeyDown
#undef IsKeyReleased
#undef GetCharPressed

namespace {

bs::ShmBlock *g_block = 0;      /* null: raylib owns the window */

const int NBUTTON = 3;
const int NKEY    = 512;

struct State {
    bool  down[NBUTTON];
    bool  pressed[NBUTTON];
    bool  released[NBUTTON];
    float x, y;
    float wheel;

    bool  keyDown[NKEY];
    bool  keyPressed[NKEY];
    bool  keyReleased[NKEY];

    /* Characters arrive as a queue because that is how raylib serves them and
     * how a text field expects to read them. */
    int   chars[64];
    int   charHead, charTail;
} g;

} /* namespace */

void bs_input_attach(bs::ShmBlock *block, int, int)
{
    g_block = block;
    std::memset(&g, 0, sizeof g);
}

void bs_input_frame(void)
{
    if (!g_block) return;

    /* Edges last one frame, so they are cleared before the new ones arrive.
     * Held state is not: a button stays down between frames. */
    std::memset(g.pressed,     0, sizeof g.pressed);
    std::memset(g.released,    0, sizeof g.released);
    std::memset(g.keyPressed,  0, sizeof g.keyPressed);
    std::memset(g.keyReleased, 0, sizeof g.keyReleased);
    g.wheel = 0.0f;

    bs::ShmInput e;
    while (bs::bs_shm_pop_input(g_block, &e)) {
        switch (e.kind) {
        case bs::SI_MOUSE_DOWN:
            if (e.button < NBUTTON) {
                /* Pressed is set even if the release arrives in the same
                 * frame. A click faster than a frame is still a click, and
                 * losing it means a knob that occasionally ignores you. */
                g.down[e.button] = true;
                g.pressed[e.button] = true;
            }
            g.x = e.x; g.y = e.y;
            break;
        case bs::SI_MOUSE_UP:
            if (e.button < NBUTTON) {
                g.down[e.button] = false;
                g.released[e.button] = true;
            }
            g.x = e.x; g.y = e.y;
            break;
        case bs::SI_MOUSE_MOVE:
            g.x = e.x; g.y = e.y;
            break;
        case bs::SI_WHEEL:
            g.wheel += e.value;
            break;
        case bs::SI_KEY_DOWN: {
            const int k = (int)e.value;
            if (k > 0 && k < NKEY) { g.keyDown[k] = true; g.keyPressed[k] = true; }
            break;
        }
        case bs::SI_KEY_UP: {
            const int k = (int)e.value;
            if (k > 0 && k < NKEY) { g.keyDown[k] = false; g.keyReleased[k] = true; }
            break;
        }
        case bs::SI_TEXT: {
            const int next = (g.charHead + 1) % (int)(sizeof g.chars / sizeof g.chars[0]);
            if (next != g.charTail) {
                g.chars[g.charHead] = (int)e.value;
                g.charHead = next;
            }
            break;
        }
        default: break;
        }
    }
}

bool bs_IsMouseButtonPressed(int b)
{
    if (!g_block) return IsMouseButtonPressed(b);
    return (b >= 0 && b < NBUTTON) ? g.pressed[b] : false;
}

bool bs_IsMouseButtonDown(int b)
{
    if (!g_block) return IsMouseButtonDown(b);
    return (b >= 0 && b < NBUTTON) ? g.down[b] : false;
}

bool bs_IsMouseButtonReleased(int b)
{
    if (!g_block) return IsMouseButtonReleased(b);
    return (b >= 0 && b < NBUTTON) ? g.released[b] : false;
}

Vector2 bs_GetMousePosition(void)
{
    if (!g_block) return GetMousePosition();
    Vector2 v = { g.x, g.y };
    return v;
}

float bs_GetMouseWheelMove(void)
{
    if (!g_block) return GetMouseWheelMove();
    return g.wheel;
}

bool bs_IsKeyPressed(int k)
{
    if (!g_block) return IsKeyPressed(k);
    return (k > 0 && k < NKEY) ? g.keyPressed[k] : false;
}

/* Repeat is a keyboard-driven convenience - holding a key to step a value -
 * and the host sends a real key-down for each repeat, so the two coincide. */
bool bs_IsKeyPressedRepeat(int k)
{
    if (!g_block) return IsKeyPressedRepeat(k);
    return (k > 0 && k < NKEY) ? g.keyPressed[k] : false;
}

bool bs_IsKeyDown(int k)
{
    if (!g_block) return IsKeyDown(k);
    return (k > 0 && k < NKEY) ? g.keyDown[k] : false;
}

bool bs_IsKeyReleased(int k)
{
    if (!g_block) return IsKeyReleased(k);
    return (k > 0 && k < NKEY) ? g.keyReleased[k] : false;
}

int bs_GetCharPressed(void)
{
    if (!g_block) return GetCharPressed();
    if (g.charHead == g.charTail) return 0;
    const int c = g.chars[g.charTail];
    g.charTail = (g.charTail + 1) % (int)(sizeof g.chars / sizeof g.chars[0]);
    return c;
}
