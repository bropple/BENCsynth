/*
 * Where input comes from, which is not always raylib.
 *
 * The interface asks raylib whether a button is down. That works when raylib
 * owns the window - but when the editor draws offscreen for a plugin, the
 * window belongs to the host in another process and raylib's hidden one never
 * sees an event. The pointer is real, the clicks are real, and every one of
 * them arrives over shared memory instead.
 *
 * Rather than edit ninety-odd call sites - and miss one, and spend an evening
 * wondering why the right mouse button does nothing in a menu - the names are
 * shadowed here. Include this after raylib.h and every IsMouseButtonDown in
 * the file becomes bs_IsMouseButtonDown, which either asks raylib or answers
 * from the forwarded state depending on how this process was started.
 *
 * Shadowing standard names is worth being uneasy about. It is contained to one
 * header, applies to one library's input calls, and the alternative is
 * ninety-two chances to introduce a bug that only shows up under a DAW.
 */
#ifndef BS_INPUT_H
#define BS_INPUT_H

#include "raylib.h"

namespace bs { struct ShmBlock; }

/* Offscreen: take input from the block rather than from the window. */
void bs_input_attach(bs::ShmBlock *block, int w, int h);
/* Once per frame, before anything reads it. Computes the edges raylib would. */
void bs_input_frame(void);

bool    bs_IsMouseButtonPressed(int button);
bool    bs_IsMouseButtonDown(int button);
bool    bs_IsMouseButtonReleased(int button);
Vector2 bs_GetMousePosition(void);
float   bs_GetMouseWheelMove(void);
bool    bs_IsKeyPressed(int key);
bool    bs_IsKeyPressedRepeat(int key);
bool    bs_IsKeyDown(int key);
bool    bs_IsKeyReleased(int key);
int     bs_GetCharPressed(void);

#define IsMouseButtonPressed   bs_IsMouseButtonPressed
#define IsMouseButtonDown      bs_IsMouseButtonDown
#define IsMouseButtonReleased  bs_IsMouseButtonReleased
#define GetMousePosition       bs_GetMousePosition
#define GetMouseWheelMove      bs_GetMouseWheelMove
#define IsKeyPressed           bs_IsKeyPressed
#define IsKeyPressedRepeat     bs_IsKeyPressedRepeat
#define IsKeyDown              bs_IsKeyDown
#define IsKeyReleased          bs_IsKeyReleased
#define GetCharPressed         bs_GetCharPressed

#endif
