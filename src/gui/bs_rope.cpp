#include "bs_rope.h"

#include <cmath>

/* Low against the pixel scale - roughly lunar, if a pixel were a centimetre.
 * That is not an aesthetic choice but a numerical one. The relaxation below
 * never fully satisfies the length constraint in the iterations available, and
 * the steady-state error is proportional to how hard gravity is pushing
 * against it: at earth-like values a long cable settles a third longer than it
 * is supposed to be and hangs correspondingly further. Since an inextensible
 * cable's resting shape is set by its length and not by gravity, weakening
 * gravity costs nothing but settling time - and the slower, rubbery swing when
 * a panel is dragged is the better look anyway. */
static const float GRAVITY  = 900.0f;
static const float DAMPING  = 0.94f;

/* Relaxation is Gauss-Seidel, so a correction travels one point per sweep in
 * whichever direction the sweep runs. Sweeping one way only, sixteen points
 * need sixteen sweeps before the far end has heard about the near end at all,
 * and the visible result is a cable that sags further every frame. Alternating
 * direction fixes the reach; the count then only has to be enough to converge.
 *
 * Forty sweeps over sixteen points, across a few dozen cables, is a rounding
 * error against the cost of drawing them. */
static const int   ITERS    = 40;

static float dist2(Vector2 a, Vector2 b)
{
    const float dx = a.x - b.x, dy = a.y - b.y;
    return dx * dx + dy * dy;
}

/* How far a cable is meant to hang below the straight line between its ends,
 * in pixels. Deliberately a fixed distance rather than a fixed proportion of
 * slack: a catenary's sag goes as the square root of length times slack, so
 * "20% longer than the gap" droops gently across a narrow panel and falls off
 * the bottom of the window across a wide rack. Solving for the slack that
 * produces a given sag is what keeps a cable looking like the same cable
 * wherever it is patched. */
static const float SAG = 38.0f;

void bs_rope_seed(bs_rope *r, Vector2 a, Vector2 b)
{
    const float d = std::sqrt(dist2(a, b));
    const float span = d < 1.0f ? 1.0f : d;

    /* Inverting sag = sqrt(3 * span * extra / 8). */
    float extra = 8.0f * SAG * SAG / (3.0f * span);

    /* Two jacks almost on top of each other would otherwise be asked for more
     * rope than the gap, which loops the cable into a knot. The floor is only
     * there to keep the arithmetic away from zero - the formula above already
     * holds the sag constant as the span grows, and raising the floor to
     * something that sounds reasonable is what makes long cables hang wrong. */
    const float cap = 0.55f * span + 26.0f;
    if (extra > cap)   extra = cap;
    if (extra < 1.0f)  extra = 1.0f;

    const float total = span + extra;
    r->rest = total / (float)(BS_ROPE_N - 1);

    const float bow = std::sqrt(3.0f * span * extra / 8.0f);
    for (int i = 0; i < BS_ROPE_N; i++) {
        const float t = (float)i / (float)(BS_ROPE_N - 1);
        Vector2 q = { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t };
        /* Started already bowed, so the first frame is a cable settling rather
         * than a straight line snapping downward. */
        q.y += std::sin(t * 3.14159265f) * bow;
        r->p[i] = q;
        r->prev[i] = q;
    }
    r->seeded = 1;
}

void bs_rope_step(bs_rope *r, Vector2 a, Vector2 b, float dt)
{
    if (!r->seeded) { bs_rope_seed(r, a, b); return; }

    /* A frame that took too long - the window was dragged, the machine
     * stalled - must not be integrated as one huge step, or every cable in
     * the rack detonates at once. */
    if (dt > 1.0f / 30.0f) dt = 1.0f / 30.0f;
    if (dt <= 0.0f) return;

    const float g = GRAVITY * dt * dt;

    for (int i = 1; i < BS_ROPE_N - 1; i++) {
        const Vector2 cur = r->p[i];
        float vx = (cur.x - r->prev[i].x) * DAMPING;
        float vy = (cur.y - r->prev[i].y) * DAMPING;

        /* A point that has somehow acquired an enormous velocity is a bug
         * somewhere else; clamping keeps it from becoming a visible one. */
        const float v2 = vx * vx + vy * vy;
        if (v2 > 4000.0f) {
            const float s = std::sqrt(4000.0f / v2);
            vx *= s; vy *= s;
        }

        r->prev[i] = cur;
        r->p[i].x  = cur.x + vx;
        r->p[i].y  = cur.y + vy + g;
    }

    r->p[0] = a;
    r->p[BS_ROPE_N - 1] = b;

    for (int it = 0; it < ITERS; it++) {
        const int back = (it & 1);
        for (int s = 0; s < BS_ROPE_N - 1; s++) {
            const int i = back ? (BS_ROPE_N - 2 - s) : s;
            Vector2 &u = r->p[i];
            Vector2 &v = r->p[i + 1];
            const float dx = v.x - u.x, dy = v.y - u.y;
            const float d2 = dx * dx + dy * dy;
            if (d2 <= r->rest * r->rest) continue;   /* slack: leave it alone */

            const float d = std::sqrt(d2);
            const float excess = (d - r->rest) / d;

            /* The pinned ends do not move, so their share of the correction
             * goes to the neighbour instead - otherwise the constraint is
             * only half applied at each end and the cable creeps longer. */
            const int fixedU = (i == 0);
            const int fixedV = (i + 1 == BS_ROPE_N - 1);
            const float wu = fixedU ? 0.0f : (fixedV ? 1.0f : 0.5f);
            const float wv = fixedV ? 0.0f : (fixedU ? 1.0f : 0.5f);

            u.x += dx * excess * wu;  u.y += dy * excess * wu;
            v.x -= dx * excess * wv;  v.y -= dy * excess * wv;
        }
        r->p[0] = a;
        r->p[BS_ROPE_N - 1] = b;
    }
}

void bs_rope_draw(const bs_rope *r, Color body, Color edge, float thick)
{
    for (int i = 0; i < BS_ROPE_N - 1; i++)
        DrawLineEx(r->p[i], r->p[i + 1], thick + 2.0f, edge);
    for (int i = 1; i < BS_ROPE_N - 1; i++)
        DrawCircleV(r->p[i], (thick + 2.0f) * 0.5f, edge);

    for (int i = 0; i < BS_ROPE_N - 1; i++)
        DrawLineEx(r->p[i], r->p[i + 1], thick, body);
    for (int i = 1; i < BS_ROPE_N - 1; i++)
        DrawCircleV(r->p[i], thick * 0.5f, body);
}

float bs_rope_distance(const bs_rope *r, Vector2 m)
{
    float best = 1e30f;
    for (int i = 0; i < BS_ROPE_N - 1; i++) {
        const Vector2 a = r->p[i], b = r->p[i + 1];
        const float dx = b.x - a.x, dy = b.y - a.y;
        const float len2 = dx * dx + dy * dy;
        float t = 0.0f;
        if (len2 > 1e-6f) {
            t = ((m.x - a.x) * dx + (m.y - a.y) * dy) / len2;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
        }
        const Vector2 q = { a.x + dx * t, a.y + dy * t };
        const float d2 = dist2(q, m);
        if (d2 < best) best = d2;
    }
    return std::sqrt(best);
}
