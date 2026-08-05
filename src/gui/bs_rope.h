/*
 * BENCsynth - patch cable physics
 *
 * A cable is a chain of point masses under gravity, integrated with Verlet
 * and relaxed against a maximum-length constraint per segment. Maximum
 * rather than fixed, because a cable is inextensible but perfectly happy to
 * bunch up: segments are only ever pulled together, never pushed apart, and
 * gravity does the rest. That one asymmetry is the whole difference between
 * something that hangs and something that behaves like a spring.
 *
 * Verlet rather than storing velocities because the ends are pinned to
 * moving jacks. With Verlet, moving an endpoint *is* giving it velocity - the
 * implied v is wherever it was last frame - so dragging a module makes its
 * cables swing without a line of code that says so.
 */

#ifndef BS_ROPE_H
#define BS_ROPE_H

#include "raylib.h"

enum { BS_ROPE_N = 16 };

typedef struct bs_rope {
    Vector2 p[BS_ROPE_N];
    Vector2 prev[BS_ROPE_N];
    float   rest;     /* maximum length of one segment */
    int     seeded;
} bs_rope;

/* Hangs a fresh cable between two points. The slack is chosen once, from the
 * distance at the time, and then kept - so a cable that was patched across a
 * short gap and later stretched goes taut, which is what a cable does. */
void bs_rope_seed(bs_rope *r, Vector2 a, Vector2 b);

/* One frame. Re-pins both ends, so the caller only has to say where the jacks
 * are now. Safe to call on an unseeded rope. */
void bs_rope_step(bs_rope *r, Vector2 a, Vector2 b, float dt);

/* Body over a darker edge, which is the fill/edge pairing every roster colour
 * already comes with and what gives the cable its roundness without a
 * gradient. */
void bs_rope_draw(const bs_rope *r, Color body, Color edge, float thick);

/* Nearest distance from a point to the cable, for hit-testing a click on it. */
float bs_rope_distance(const bs_rope *r, Vector2 m);

#endif /* BS_ROPE_H */
