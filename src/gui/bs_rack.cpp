#include "bs_rack.h"
#include "bs_modules.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using bs::Module;
using bs::Patch;
using bs::Cable;

enum { MENU_ADD = 1, MENU_MODULE = 2, MENU_PRESET = 3 };

static const float PANEL_INSET = 5.0f;
static const float GRID        = (float)bs::BS_HP;

/* ------------------------------------------------------------------ *
 * Panel layout, in rack coordinates
 *
 * Read straight off the module's own declarations. Nothing here knows what a
 * VCO is, which is the property that makes the module set extensible.
 * ------------------------------------------------------------------ */

static Rectangle panelRect(const Module *m)
{
    Rectangle r = { m->x, m->y, (float)bs::panelWidth(*m), (float)bs::panelHeight(*m) };
    return r;
}

static int knobRows(const Module *m)
{
    const int c = m->cols < 1 ? 1 : m->cols;
    return (m->paramCount() + c - 1) / c;
}

static Rectangle paramCell(const Module *m, int i)
{
    const int   c  = m->cols < 1 ? 1 : m->cols;
    const float w  = (float)bs::panelWidth(*m) - PANEL_INSET * 2.0f;
    const float cw = w / (float)c;
    const float y0 = m->y + bs::BS_PANEL_TOP + bs::BS_PANEL_PAD;
    Rectangle r = { m->x + PANEL_INSET + cw * (float)(i % c),
                    y0 + (float)bs::BS_KNOB_ROW * (float)(i / c),
                    cw, (float)bs::BS_KNOB_ROW };
    return r;
}

static float extraY(const Module *m)
{
    return m->y + bs::BS_PANEL_TOP + bs::BS_PANEL_PAD
         + (float)(knobRows(m) * bs::BS_KNOB_ROW);
}

static Rectangle extraRect(const Module *m)
{
    Rectangle r = { m->x + 6.0f, extraY(m) + 3.0f,
                    (float)bs::panelWidth(*m) - 12.0f,
                    (float)m->extraPanelHeight() - 8.0f };
    return r;
}

static float jacksY(const Module *m)
{
    return extraY(m) + (float)m->extraPanelHeight();
}

static Rectangle jackCell(const Module *m, int i, int isOutput)
{
    const int   jc = bs::panelJackCols(*m);
    const float w  = (float)bs::panelWidth(*m) - PANEL_INSET * 2.0f;
    const float cw = w / (float)jc;

    float y0 = jacksY(m);
    if (isOutput) {
        const int irow = (m->inputCount() + jc - 1) / jc;
        y0 += (float)(irow * bs::BS_JACK_ROW);
        if (irow) y0 += (float)bs::BS_JACK_GAP;
    }
    Rectangle r = { m->x + PANEL_INSET + cw * (float)(i % jc),
                    y0 + (float)bs::BS_JACK_ROW * (float)(i / jc),
                    cw, (float)bs::BS_JACK_ROW };
    return r;
}

Vector2 bs_rack_jack_pos(const Module *m, int port, int isOutput)
{
    return bs_jack_center(jackCell(m, port, isOutput));
}

/* ------------------------------------------------------------------ *
 * Cable lookups
 * ------------------------------------------------------------------ */

static const Cable *cableIntoInput(const Patch &p, int mod, int port)
{
    const std::vector<Cable> &cs = p.cableList();
    for (size_t i = 0; i < cs.size(); i++)
        if (cs[i].alive && cs[i].dst == mod && cs[i].dstPort == port) return &cs[i];
    return 0;
}

static const Cable *cableFromOutput(const Patch &p, int mod, int port)
{
    const std::vector<Cable> &cs = p.cableList();
    for (size_t i = 0; i < cs.size(); i++)
        if (cs[i].alive && cs[i].src == mod && cs[i].srcPort == port) return &cs[i];
    return 0;
}

/* A cable slot is handed straight back out when it is freed, so the index
 * alone does not identify a cable. Keying the rope on both endpoints means a
 * reused slot is recognised as a different cable and gets hung afresh rather
 * than snapping across the rack from wherever the last one ended. */
static long cableKey(const Cable &c)
{
    return ((long)c.src << 24) ^ ((long)c.srcPort << 18)
         ^ ((long)c.dst << 6)  ^ (long)c.dstPort;
}

/* ------------------------------------------------------------------ *
 * Menus
 * ------------------------------------------------------------------ */

static const char *addItems[64];
static int         addCount = 0;
static char        addLabels[64][32];

static void buildAddMenu()
{
    if (addCount) return;
    for (int i = 0; i < bs::moduleTypeCount() && i < 64; i++) {
        const bs::ModuleType *t = bs::moduleTypeAt(i);
        snprintf(addLabels[addCount], sizeof addLabels[0], "%s", t->name);
        addItems[addCount] = addLabels[addCount];
        addCount++;
    }
}

static const char *MODULE_ITEMS[] = { "UNPATCH", "RESET KNOBS", "REMOVE" };

static const char *presetItems[32];
static int         presetCount = 0;

static void buildPresetMenu()
{
    if (presetCount) return;
    for (int i = 0; i < bs::rackPresetCount() && i < 32; i++)
        presetItems[presetCount++] = bs::rackPresetAt(i)->name;
}

/* ------------------------------------------------------------------ */

void bs_rack_init(bs_rack *r)
{
    r->scrollX = r->scrollY = 0.0f;
    r->zoom = 1.0f;
    r->dragModule = -1;
    r->dragGrab = (Vector2){ 0, 0 };
    r->panning = 0;
    r->panGrab = (Vector2){ 0, 0 };
    r->panScrollX = r->panScrollY = 0.0f;
    r->patching = 0;
    r->fromModule = r->fromPort = r->fromOutput = -1;
    r->fromColor = 0;
    memset(&r->dragRope, 0, sizeof r->dragRope);
    r->ropes.clear();
    r->ropeKey.clear();
    r->hoverCable = -1;
    r->menuModule = -1;
    r->menuRackPos = (Vector2){ 0, 0 };
    r->edited = 0;
    r->presetLoaded = -1;
    buildAddMenu();
    buildPresetMenu();
}

void bs_rack_home(bs_rack *r)
{
    r->scrollX = 0.0f;
    r->scrollY = 0.0f;
    r->zoom    = 1.0f;
}

void bs_rack_patch_replaced(bs_rack *r)
{
    r->ropes.clear();
    r->ropeKey.clear();
    r->patching = 0;
    r->dragModule = -1;
    r->hoverCable = -1;
    r->menuModule = -1;
    bs_rack_home(r);
}

void bs_rack_add_menu(bs_rack *r, bs_ui *ui, Vector2 screenAt, Rectangle view)
{
    buildAddMenu();
    r->menuModule = -1;
    r->menuRackPos = (Vector2){ r->scrollX + 30.0f, r->scrollY + 30.0f };
    (void)view;
    bs_menu_open(ui, screenAt, addItems, addCount, MENU_ADD);
}

void bs_rack_preset_menu(bs_rack *r, bs_ui *ui, Vector2 screenAt)
{
    buildPresetMenu();
    r->menuModule = -1;
    bs_menu_open(ui, screenAt, presetItems, presetCount, MENU_PRESET);
}

/* ------------------------------------------------------------------ *
 * The frame
 * ------------------------------------------------------------------ */

void bs_rack_frame(bs_rack *r, bs_ui *ui, bs::Engine *eng, Rectangle view, float dt)
{
    Patch &patch = eng->patch;

    /* Two pointers, and keeping them straight is most of what this function
     * has to get right. `sm` is where the mouse is on the screen and is what
     * the viewport, the panning gesture and the menus are measured against.
     * `wm` is the same point in rack units and is what every panel, knob and
     * jack is measured against, because that is the space they are laid out
     * in. */
    const Vector2 sm = GetMousePosition();
    const int overView = CheckCollisionPointRec(sm, view) && !bs_ui_blocked(ui);

    r->edited = 0;
    r->presetLoaded = -1;

    if (r->zoom < 0.35f) r->zoom = 0.35f;
    if (r->zoom > 2.50f) r->zoom = 2.50f;

    Camera2D cam;
    cam.offset   = (Vector2){ view.x, view.y };
    cam.target   = (Vector2){ r->scrollX, r->scrollY };
    cam.rotation = 0.0f;
    cam.zoom     = r->zoom;

    const Vector2 wm = GetScreenToWorld2D(sm, cam);

    /* ---- how far the rack extends, in rack units ---- */
    float maxX = 0.0f, maxY = 0.0f;
    for (int id = 0; id < patch.slotCount(); id++) {
        const Module *mod = patch.module(id);
        if (!mod) continue;
        const Rectangle pr = panelRect(mod);
        if (pr.x + pr.width  + 60.0f > maxX) maxX = pr.x + pr.width  + 60.0f;
        if (pr.y + pr.height + 60.0f > maxY) maxY = pr.y + pr.height + 60.0f;
    }
    /* What one screenful is worth shrinks as you zoom in, so the amount of
     * rack you can scroll past grows. */
    const float pageW = view.width  / r->zoom;
    const float pageH = view.height / r->zoom;
    const float maxScrollX = maxX - pageW > 0.0f ? maxX - pageW : 0.0f;
    const float maxScrollY = maxY - pageH > 0.0f ? maxY - pageH : 0.0f;

    /* ---- backdrop ---- */
    DrawRectangleRec(view, BS_BG);

    BeginScissorMode((int)view.x, (int)view.y, (int)view.width, (int)view.height);
    BeginMode2D(cam);
    bs_ui_push_mouse(wm);

    /* A grid on the rack unit the panels are measured in. Dim enough to read
     * as texture rather than as a control, but present enough that dragging a
     * panel has something to line up against. Drawn in rack units, so it is
     * the grid the panels snap to at any zoom - and its spacing on screen is
     * the readout for how far in you are.
     *
     * Coarsened when zoomed out, because a one-pixel line every seven screen
     * pixels stops being texture and becomes a moire. */
    {
        const Color GRIDC = { 0x0e, 0x17, 0x0a, 255 };
        float step = GRID;
        while (step * r->zoom < 12.0f) step *= 4.0f;
        const float x0 = std::floor(r->scrollX / step) * step;
        const float y0 = std::floor(r->scrollY / step) * step;
        const float th = 1.0f / r->zoom;      /* one screen pixel, whatever the zoom */
        for (float gx = x0; gx < r->scrollX + pageW; gx += step)
            DrawRectangleRec((Rectangle){ gx, r->scrollY, th, pageH }, GRIDC);
        for (float gy = y0; gy < r->scrollY + pageH; gy += step)
            DrawRectangleRec((Rectangle){ r->scrollX, gy, pageW, th }, GRIDC);
    }

    /* ---- which panel is on top under the pointer ---- */
    int top = -1;
    if (overView) {
        for (int id = 0; id < patch.slotCount(); id++) {
            const Module *mod = patch.module(id);
            if (!mod) continue;
            if (CheckCollisionPointRec(wm, panelRect(mod))) top = id;
        }
    }

    /* Where the pointer would land a cable, decided during the pass. */
    int hoverMod = -1, hoverPort = -1, hoverOut = 0;
    int wheelTaken = 0;
    const float wheel = GetMouseWheelMove();

    /* What is actually on screen, in rack units, for culling. */
    const Rectangle worldView = { r->scrollX, r->scrollY, pageW, pageH };

    /* ---- panels ---- */
    for (int id = 0; id < patch.slotCount(); id++) {
        Module *mod = patch.module(id);
        if (!mod) continue;

        const Rectangle pr = panelRect(mod);
        if (!CheckCollisionRecs(pr, worldView)) continue;

        bs_panel(pr, BS_PANEL, BS_BORDER);

        /* Title bar. Dragging it moves the panel; right-clicking it is the
         * only way to delete one, which is deliberate - a close button on a
         * module you have spent ten minutes patching into is a trap. */
        Rectangle tb = { pr.x, pr.y, pr.width, (float)bs::BS_PANEL_TOP };
        const int tbHot = overView && top == id && CheckCollisionPointRec(wm, tb)
                        && !r->patching;
        DrawRectangleRounded(tb, (float)BS_RADIUS / tb.height, 4,
                             tbHot ? BS_EDGE : BS_PANEL_HI);
        DrawRectangleRec((Rectangle){ tb.x, tb.y + tb.height - 1.0f, tb.width, 1.0f },
                         BS_BORDER);
        bs_text_fit(ui, BS_F_SMALL, mod->title.c_str(), pr.x + pr.width * 0.5f,
                    pr.y + (tb.height - BS_F_SMALL) * 0.5f, pr.width - 8.0f,
                    tbHot ? BS_TEXT : BS_DIM);

        if (tbHot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            r->dragModule = id;
            r->dragGrab = (Vector2){ wm.x - pr.x, wm.y - pr.y };
        }
        if (tbHot && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
            r->menuModule = id;
            bs_menu_open(ui, sm, MODULE_ITEMS, 3, MENU_MODULE);
        }

        /* ---- knobs ---- */
        ui->suppress = (top != id) || r->patching;
        for (int i = 0; i < mod->paramCount(); i++) {
            Rectangle cell = paramCell(mod, i);
            bs::Param *p = &mod->params[(size_t)i];
            const int wid = (id + 1) * 256 + i + 1;
            int hit = 0;
            if (p->kind == bs::PK_SWITCH)      hit = bs_switch(ui, wid, cell, p);
            else if (p->kind == bs::PK_TOGGLE) hit = bs_toggle(ui, wid, cell, p);
            else                               hit = bs_knob(ui, wid, cell, p);
            if (hit && wheel != 0.0f) wheelTaken = 1;
            if (hit) r->edited = 1;
        }
        ui->suppress = 0;

        /* ---- the two panels that show something ---- */
        if (mod->extraPanelHeight() > 0) {
            const Rectangle er = extraRect(mod);
            if (mod->typeId == "SCOPE") {
                bs::ModuleScope *sc = static_cast<bs::ModuleScope *>(mod);
                Rectangle a = { er.x, er.y, er.width, er.height * 0.5f - 2.0f };
                Rectangle b = { er.x, er.y + er.height * 0.5f + 2.0f,
                                er.width, er.height * 0.5f - 2.0f };
                bs_scope(a, sc->traceA, bs::ModuleScope::TRACE, sc->writePos, BS_ACCENT);
                bs_scope(b, sc->traceB, bs::ModuleScope::TRACE, sc->writePos, BS_AMBER);
            } else if (mod->typeId == "OUT") {
                bs::ModuleOut *o = static_cast<bs::ModuleOut *>(mod);
                Rectangle mr = { er.x, er.y + 14.0f, er.width, er.height - 20.0f };
                bs_text(ui, BS_F_TINY, o->clipping ? "CLIP" : "LEVEL",
                        er.x + 1.0f, er.y, o->clipping ? BS_ALERT : BS_DIM);
                bs_meter(mr, o->peak[0], o->rms[0], o->peak[1], o->rms[1],
                         o->clipping);
            }
        }

        /* ---- jacks ---- */
        const int jc = bs::panelJackCols(*mod);
        const int irow = (mod->inputCount() + jc - 1) / jc;
        if (irow && mod->outputCount()) {
            const float dy = jacksY(mod) + (float)(irow * bs::BS_JACK_ROW)
                           + (float)bs::BS_JACK_GAP * 0.5f;
            bs_divider(pr.x + 6.0f, dy, pr.width - 12.0f);
        }

        for (int side = 0; side < 2; side++) {
            const int n = side ? mod->outputCount() : mod->inputCount();
            for (int i = 0; i < n; i++) {
                const Rectangle cell = jackCell(mod, i, side);
                const Cable *c = side ? cableFromOutput(patch, id, i)
                                      : cableIntoInput(patch, id, i);
                const Vector2 jp = bs_jack_center(cell);
                const int hot = overView && top == id
                              && CheckCollisionPointCircle(wm, jp, (float)BS_JACK_R + 4.0f);
                if (hot) { hoverMod = id; hoverPort = i; hoverOut = side; }

                Color plug = c ? BS_CABLE[c->color % BS_CABLE_COLORS] : BS_EDGE;
                bs_jack(ui, cell, side ? mod->outInfo[(size_t)i].name
                                       : mod->inInfo[(size_t)i].name,
                        c != 0, plug, hot, side);
            }
        }
    }

    /* ---- starting and finishing a patch ---- */

    if (!r->patching && hoverMod >= 0 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        const Cable *c = hoverOut ? cableFromOutput(patch, hoverMod, hoverPort)
                                  : cableIntoInput(patch, hoverMod, hoverPort);
        if (!hoverOut && c) {
            /* Pulling a plug out of an input picks the whole cable up by that
             * end, so moving a connection somewhere else is one gesture
             * rather than a delete followed by a patch. */
            const int src = c->src, sp = c->srcPort, col = c->color;
            eng->disconnect(c->id);
            r->patching   = 1;
            r->fromModule = src;
            r->fromPort   = sp;
            r->fromOutput = 1;
            r->fromColor  = col;
            r->dragRope.seeded = 0;
            r->edited = 1;
        } else {
            r->patching   = 1;
            r->fromModule = hoverMod;
            r->fromPort   = hoverPort;
            r->fromOutput = hoverOut;
            /* A new cable gets the next colour along, so a rack patched up
             * over a session ends up banded rather than monochrome. */
            r->fromColor  = (int)(patch.cableList().size() % BS_CABLE_COLORS);
            r->dragRope.seeded = 0;
        }
    }

    /* Right-clicking a jack empties it, which is the one-handed version of
     * pulling the plug out and dropping it on the floor. */
    if (!r->patching && hoverMod >= 0 && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
        const Cable *c = hoverOut ? cableFromOutput(patch, hoverMod, hoverPort)
                                  : cableIntoInput(patch, hoverMod, hoverPort);
        if (c) { eng->disconnect(c->id); r->edited = 1; }
    }

    if (r->patching && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        if (hoverMod >= 0 && hoverOut != r->fromOutput) {
            if (r->fromOutput) eng->connect(r->fromModule, r->fromPort, hoverMod, hoverPort);
            else               eng->connect(hoverMod, hoverPort, r->fromModule, r->fromPort);
            r->edited = 1;
        }
        r->patching = 0;
    }

    /* ---- cables ----
     *
     * Simulated in rack units, not screen pixels. A cable is a physical thing
     * hanging off a panel, so its slack belongs to the rack: zoom in and it
     * gets bigger along with everything else, rather than staying the same
     * number of pixels and appearing to shrink. It also means the geometry
     * does not have to be resettled every time the view moves. */
    const std::vector<Cable> &cs = patch.cableList();
    if (r->ropes.size() < cs.size()) {
        r->ropes.resize(cs.size());
        r->ropeKey.resize(cs.size(), 0);
        for (size_t i = 0; i < r->ropes.size(); i++)
            if (!r->ropeKey[i]) r->ropes[i].seeded = 0;
    }

    r->hoverCable = -1;
    for (size_t i = 0; i < cs.size(); i++) {
        const Cable &c = cs[i];
        if (!c.alive) { r->ropeKey[i] = 0; continue; }
        const Module *sMod = patch.module(c.src);
        const Module *dMod = patch.module(c.dst);
        if (!sMod || !dMod) continue;

        const Vector2 a = bs_rack_jack_pos(sMod, c.srcPort, 1);
        const Vector2 b = bs_rack_jack_pos(dMod, c.dstPort, 0);

        const long key = cableKey(c);
        if (r->ropeKey[i] != key) {
            bs_rope_seed(&r->ropes[i], a, b);
            r->ropeKey[i] = key;
        }
        bs_rope_step(&r->ropes[i], a, b, dt);

        /* A polyphonic cable is drawn thicker. It is the only way to see, at
         * a glance, where the eight voices stop and the mono tail begins. */
        const int poly = sMod->outs[(size_t)c.srcPort].channels > 1;
        const float thick = poly ? 6.5f : 4.5f;
        const int ci = c.color % BS_CABLE_COLORS;
        bs_rope_draw(&r->ropes[i], BS_CABLE[ci], BS_CABLE_EDGE[ci], thick);

        if (overView && !r->patching &&
            bs_rope_distance(&r->ropes[i], wm) < thick + 3.0f) {
            r->hoverCable = c.id;
            bs_rope_draw(&r->ropes[i], BS_TEXT, BS_CABLE[ci], 1.5f);
        }
    }

    /* The cable in hand. Its free end is the pointer, so it swings from the
     * jack exactly as one would. */
    if (r->patching) {
        const Module *sMod = patch.module(r->fromModule);
        if (!sMod) {
            r->patching = 0;
        } else {
            const Vector2 fixedEnd = bs_rack_jack_pos(sMod, r->fromPort, r->fromOutput);
            if (!r->dragRope.seeded) bs_rope_seed(&r->dragRope, fixedEnd, wm);
            bs_rope_step(&r->dragRope, fixedEnd, wm, dt);
            const int ci = r->fromColor % BS_CABLE_COLORS;
            bs_rope_draw(&r->dragRope, BS_CABLE[ci], BS_CABLE_EDGE[ci], 5.0f);
            DrawCircleV(wm, 6.0f, BS_CABLE_EDGE[ci]);
            DrawCircleV(wm, 3.0f, BS_CABLE[ci]);
        }
    }

    bs_ui_pop_mouse();
    EndMode2D();

    /* How far in, when it is not all the way out. Drawn in screen space,
     * after the camera, so it stays the same size and in the same corner. */
    if (std::fabs(r->zoom - 1.0f) > 0.01f) {
        char z[24];
        snprintf(z, sizeof z, "%.0f%%", (double)(r->zoom * 100.0f));
        bs_text_spaced(ui, BS_F_TINY, z, view.x + 8.0f,
                       view.y + view.height - 18.0f, BS_EDGE);
    }
    EndScissorMode();

    /* ---- moving a panel ---- */

    if (r->dragModule >= 0) {
        Module *mod = patch.module(r->dragModule);
        if (!mod || !IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            if (mod) {
                /* Snapped on release rather than while dragging: snapping
                 * live makes the panel stutter under the pointer. */
                mod->x = std::floor(mod->x / 5.0f + 0.5f) * 5.0f;
                mod->y = std::floor(mod->y / 5.0f + 0.5f) * 5.0f;
            }
            r->dragModule = -1;
        } else {
            mod->x = wm.x - r->dragGrab.x;
            mod->y = wm.y - r->dragGrab.y;
            if (mod->x < 0.0f) mod->x = 0.0f;
            if (mod->y < 0.0f) mod->y = 0.0f;
            r->edited = 1;
        }
    }

    /* ---- panning and zooming ---- */

    if (overView && r->dragModule < 0 && !r->patching) {
        if (IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE) ||
            (top == -1 && hoverMod == -1 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))) {
            r->panning = 1;
            r->panGrab = sm;
            r->panScrollX = r->scrollX;
            r->panScrollY = r->scrollY;
        }
    }
    if (r->panning) {
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) || IsMouseButtonDown(MOUSE_BUTTON_MIDDLE)) {
            /* The gesture is a number of screen pixels however far in the view
             * is, so the rack keeps up with the pointer rather than crawling
             * when zoomed in. */
            r->scrollX = r->panScrollX - (sm.x - r->panGrab.x) / r->zoom;
            r->scrollY = r->panScrollY - (sm.y - r->panGrab.y) / r->zoom;
        } else {
            r->panning = 0;
        }
    }

    if (overView && wheel != 0.0f && !wheelTaken) {
        if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) {
            r->scrollX -= wheel * 60.0f / r->zoom;
        } else {
            /* Toward the pointer, not the middle of the window. Zooming
             * around the centre means the thing you were looking at slides
             * away, and you chase it with the other hand.
             *
             * The rack point under the cursor is `wm`, and it has to still be
             * under the cursor afterwards - which pins the new scroll exactly,
             * since scroll is the rack point at the viewport's top left. */
            float z = r->zoom * std::pow(1.12f, wheel);
            if (z < 0.35f) z = 0.35f;
            if (z > 2.50f) z = 2.50f;
            r->scrollX = wm.x - (sm.x - view.x) / z;
            r->scrollY = wm.y - (sm.y - view.y) / z;
            r->zoom = z;
        }
    }

    if (r->scrollX < 0.0f) r->scrollX = 0.0f;
    if (r->scrollY < 0.0f) r->scrollY = 0.0f;
    if (r->scrollX > maxScrollX) r->scrollX = maxScrollX;
    if (r->scrollY > maxScrollY) r->scrollY = maxScrollY;

    /* ---- the add menu ---- */

    if (overView && top == -1 && hoverMod == -1 && !r->patching &&
        IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
        r->menuModule = -1;
        r->menuRackPos = wm;
        bs_menu_open(ui, sm, addItems, addCount, MENU_ADD);
    }
}

/* ------------------------------------------------------------------ */

int bs_rack_menu(bs_rack *r, bs_ui *ui, bs::Engine *eng)
{
    int tag = 0;
    const int hit = bs_menu_take(ui, &tag);
    if (hit < 0) return 0;

    if (tag == MENU_PRESET) {
        if (hit < 0 || hit >= bs::rackPresetCount()) return 0;
        eng->buildPreset(hit);
        /* Everything the rack knew about the old patch - cable geometry,
         * scroll, zoom, what was being dragged - refers to modules that no
         * longer exist. */
        bs_rack_patch_replaced(r);
        r->presetLoaded = hit;
        r->edited = 1;
        return 1;
    }

    if (tag == MENU_ADD) {
        const bs::ModuleType *t = bs::moduleTypeAt(hit);
        if (!t) return 0;
        eng->addModule(t->id, r->menuRackPos.x, r->menuRackPos.y);
        r->edited = 1;
        return 1;
    }

    if (tag == MENU_MODULE && r->menuModule >= 0) {
        bs::Module *mod = eng->patch.module(r->menuModule);
        if (!mod) return 0;
        switch (hit) {
        case 0: {   /* UNPATCH */
            const std::vector<Cable> &cs = eng->patch.cableList();
            /* Collected first, because disconnecting rewrites the list the
             * loop would otherwise be walking. */
            std::vector<int> doomed;
            for (size_t i = 0; i < cs.size(); i++)
                if (cs[i].alive && (cs[i].src == r->menuModule || cs[i].dst == r->menuModule))
                    doomed.push_back(cs[i].id);
            for (size_t i = 0; i < doomed.size(); i++) eng->disconnect(doomed[i]);
            break;
        }
        case 1:     /* RESET KNOBS */
            mod->resetParams();
            break;
        case 2:     /* REMOVE */
            eng->removeModule(r->menuModule);
            r->menuModule = -1;
            break;
        default: break;
        }
        r->edited = 1;
        return 1;
    }
    return 0;
}
