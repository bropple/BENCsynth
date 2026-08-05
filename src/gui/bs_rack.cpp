#include "bs_rack.h"
#include "bs_modules.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using bs::Module;
using bs::Patch;
using bs::Cable;

enum { MENU_ADD = 1, MENU_MODULE = 2 };

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

static Rectangle offset(Rectangle a, float ox, float oy)
{
    a.x += ox; a.y += oy;
    return a;
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

/* ------------------------------------------------------------------ */

void bs_rack_init(bs_rack *r)
{
    r->scrollX = r->scrollY = 0.0f;
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
    buildAddMenu();
}

void bs_rack_home(bs_rack *r) { r->scrollX = 0.0f; r->scrollY = 0.0f; }

void bs_rack_add_menu(bs_rack *r, bs_ui *ui, Vector2 screenAt, Rectangle view)
{
    buildAddMenu();
    r->menuModule = -1;
    r->menuRackPos = (Vector2){ r->scrollX + 30.0f, r->scrollY + 30.0f };
    (void)view;
    bs_menu_open(ui, screenAt, addItems, addCount, MENU_ADD);
}

/* ------------------------------------------------------------------ *
 * The frame
 * ------------------------------------------------------------------ */

void bs_rack_frame(bs_rack *r, bs_ui *ui, bs::Engine *eng, Rectangle view, float dt)
{
    Patch &patch = eng->patch;
    const Vector2 m = GetMousePosition();
    const int overView = CheckCollisionPointRec(m, view) && !bs_ui_blocked(ui, m);

    r->edited = 0;

    /* ---- scroll extent ---- */
    float maxX = view.width, maxY = view.height;
    for (int id = 0; id < patch.slotCount(); id++) {
        const Module *mod = patch.module(id);
        if (!mod) continue;
        const Rectangle pr = panelRect(mod);
        if (pr.x + pr.width  + 60.0f > maxX) maxX = pr.x + pr.width  + 60.0f;
        if (pr.y + pr.height + 60.0f > maxY) maxY = pr.y + pr.height + 60.0f;
    }
    const float maxScrollX = maxX - view.width  > 0.0f ? maxX - view.width  : 0.0f;
    const float maxScrollY = maxY - view.height > 0.0f ? maxY - view.height : 0.0f;

    const float ox = view.x - r->scrollX;
    const float oy = view.y - r->scrollY;

    /* ---- backdrop ---- */
    DrawRectangleRec(view, BS_BG);

    /* A grid on the rack unit the panels are measured in. Dim enough to read
     * as texture rather than as a control, but present enough that dragging a
     * panel has something to line up against. */
    const Color GRIDC = { 0x0e, 0x17, 0x0a, 255 };
    for (float gx = -std::fmod(r->scrollX, GRID); gx < view.width; gx += GRID)
        DrawRectangle((int)(view.x + gx), (int)view.y, 1, (int)view.height, GRIDC);
    for (float gy = -std::fmod(r->scrollY, GRID); gy < view.height; gy += GRID)
        DrawRectangle((int)view.x, (int)(view.y + gy), (int)view.width, 1, GRIDC);

    BeginScissorMode((int)view.x, (int)view.y, (int)view.width, (int)view.height);

    /* ---- which panel is on top under the pointer ---- */
    int top = -1;
    if (overView) {
        for (int id = 0; id < patch.slotCount(); id++) {
            const Module *mod = patch.module(id);
            if (!mod) continue;
            if (CheckCollisionPointRec(m, offset(panelRect(mod), ox, oy))) top = id;
        }
    }

    /* Where the pointer would land a cable, decided during the pass. */
    int hoverMod = -1, hoverPort = -1, hoverOut = 0;
    int wheelTaken = 0;
    const float wheel = GetMouseWheelMove();

    /* ---- panels ---- */
    for (int id = 0; id < patch.slotCount(); id++) {
        Module *mod = patch.module(id);
        if (!mod) continue;

        const Rectangle pr = offset(panelRect(mod), ox, oy);
        if (!CheckCollisionRecs(pr, view)) continue;

        bs_panel(pr, BS_PANEL, BS_BORDER);

        /* Title bar. Dragging it moves the panel; right-clicking it is the
         * only way to delete one, which is deliberate - a close button on a
         * module you have spent ten minutes patching into is a trap. */
        Rectangle tb = { pr.x, pr.y, pr.width, (float)bs::BS_PANEL_TOP };
        const int tbHot = overView && top == id && CheckCollisionPointRec(m, tb)
                        && !r->patching;
        DrawRectangleRounded(tb, (float)BS_RADIUS / tb.height, 4,
                             tbHot ? BS_EDGE : BS_PANEL_HI);
        DrawRectangle((int)tb.x, (int)(tb.y + tb.height - 1), (int)tb.width, 1, BS_BORDER);
        bs_text_fit(ui, BS_F_SMALL, mod->title.c_str(), pr.x + pr.width * 0.5f,
                    pr.y + (tb.height - BS_F_SMALL) * 0.5f, pr.width - 8.0f,
                    tbHot ? BS_TEXT : BS_DIM);

        if (tbHot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            r->dragModule = id;
            r->dragGrab = (Vector2){ m.x - pr.x, m.y - pr.y };
        }
        if (tbHot && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
            r->menuModule = id;
            bs_menu_open(ui, m, MODULE_ITEMS, 3, MENU_MODULE);
        }

        /* ---- knobs ---- */
        ui->suppress = (top != id) || r->patching;
        for (int i = 0; i < mod->paramCount(); i++) {
            Rectangle cell = offset(paramCell(mod, i), ox, oy);
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
            const Rectangle er = offset(extraRect(mod), ox, oy);
            if (mod->typeId == "SCOPE") {
                bs::ModuleScope *s = static_cast<bs::ModuleScope *>(mod);
                Rectangle a = { er.x, er.y, er.width, er.height * 0.5f - 2.0f };
                Rectangle b = { er.x, er.y + er.height * 0.5f + 2.0f,
                                er.width, er.height * 0.5f - 2.0f };
                bs_scope(a, s->traceA, bs::ModuleScope::TRACE, s->writePos, BS_ACCENT);
                bs_scope(b, s->traceB, bs::ModuleScope::TRACE, s->writePos, BS_AMBER);
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
            const float dy = oy + jacksY(mod) + (float)(irow * bs::BS_JACK_ROW)
                           + (float)bs::BS_JACK_GAP * 0.5f;
            bs_divider(pr.x + 6.0f, dy, pr.width - 12.0f);
        }

        for (int side = 0; side < 2; side++) {
            const int n = side ? mod->outputCount() : mod->inputCount();
            for (int i = 0; i < n; i++) {
                const Rectangle cell = offset(jackCell(mod, i, side), ox, oy);
                const Cable *c = side ? cableFromOutput(patch, id, i)
                                      : cableIntoInput(patch, id, i);
                const Vector2 jc2 = bs_jack_center(cell);
                const int hot = overView && top == id
                              && CheckCollisionPointCircle(m, jc2, (float)BS_JACK_R + 4.0f);
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

    /* ---- cables ---- */

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
        const Module *s = patch.module(c.src);
        const Module *d = patch.module(c.dst);
        if (!s || !d) continue;

        const Vector2 ja = bs_rack_jack_pos(s, c.srcPort, 1);
        const Vector2 jb = bs_rack_jack_pos(d, c.dstPort, 0);
        const Vector2 a = { ja.x + ox, ja.y + oy };
        const Vector2 b = { jb.x + ox, jb.y + oy };

        const long key = cableKey(c);
        if (r->ropeKey[i] != key) {
            bs_rope_seed(&r->ropes[i], a, b);
            r->ropeKey[i] = key;
        }
        bs_rope_step(&r->ropes[i], a, b, dt);

        /* A polyphonic cable is drawn thicker. It is the only way to see, at
         * a glance, where the eight voices stop and the mono tail begins. */
        const int poly = s->outs[(size_t)c.srcPort].channels > 1;
        const float thick = poly ? 6.5f : 4.5f;
        const int ci = c.color % BS_CABLE_COLORS;
        bs_rope_draw(&r->ropes[i], BS_CABLE[ci], BS_CABLE_EDGE[ci], thick);

        if (overView && !r->patching && bs_rope_distance(&r->ropes[i], m) < thick + 3.0f) {
            r->hoverCable = c.id;
            bs_rope_draw(&r->ropes[i], BS_TEXT, BS_CABLE[ci], 1.5f);
        }
    }

    /* The cable in hand. Its free end is the pointer, so it swings from the
     * jack exactly as one would. */
    if (r->patching) {
        const Module *s = patch.module(r->fromModule);
        if (!s) {
            r->patching = 0;
        } else {
            const Vector2 fixedEnd = { bs_rack_jack_pos(s, r->fromPort, r->fromOutput).x + ox,
                                       bs_rack_jack_pos(s, r->fromPort, r->fromOutput).y + oy };
            if (!r->dragRope.seeded) bs_rope_seed(&r->dragRope, fixedEnd, m);
            bs_rope_step(&r->dragRope, fixedEnd, m, dt);
            const int ci = r->fromColor % BS_CABLE_COLORS;
            bs_rope_draw(&r->dragRope, BS_CABLE[ci], BS_CABLE_EDGE[ci], 5.0f);
            DrawCircleV(m, 6.0f, BS_CABLE_EDGE[ci]);
            DrawCircleV(m, 3.0f, BS_CABLE[ci]);
        }
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
            mod->x = m.x - r->dragGrab.x - ox;
            mod->y = m.y - r->dragGrab.y - oy;
            if (mod->x < 0.0f) mod->x = 0.0f;
            if (mod->y < 0.0f) mod->y = 0.0f;
            r->edited = 1;
        }
    }

    /* ---- panning and scrolling ---- */

    if (overView && r->dragModule < 0 && !r->patching) {
        if (IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE) ||
            (top == -1 && hoverMod == -1 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))) {
            r->panning = 1;
            r->panGrab = m;
            r->panScrollX = r->scrollX;
            r->panScrollY = r->scrollY;
        }
    }
    if (r->panning) {
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) || IsMouseButtonDown(MOUSE_BUTTON_MIDDLE)) {
            r->scrollX = r->panScrollX - (m.x - r->panGrab.x);
            r->scrollY = r->panScrollY - (m.y - r->panGrab.y);
        } else {
            r->panning = 0;
        }
    }

    if (overView && wheel != 0.0f && !wheelTaken) {
        if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT))
            r->scrollX -= wheel * 60.0f;
        else
            r->scrollY -= wheel * 60.0f;
    }

    if (r->scrollX < 0.0f) r->scrollX = 0.0f;
    if (r->scrollY < 0.0f) r->scrollY = 0.0f;
    if (r->scrollX > maxScrollX) r->scrollX = maxScrollX;
    if (r->scrollY > maxScrollY) r->scrollY = maxScrollY;

    /* ---- the add menu ---- */

    if (overView && top == -1 && hoverMod == -1 && !r->patching &&
        IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
        r->menuModule = -1;
        r->menuRackPos = (Vector2){ m.x - ox, m.y - oy };
        bs_menu_open(ui, m, addItems, addCount, MENU_ADD);
    }
}

/* ------------------------------------------------------------------ */

int bs_rack_menu(bs_rack *r, bs_ui *ui, bs::Engine *eng)
{
    int tag = 0;
    const int hit = bs_menu_take(ui, &tag);
    if (hit < 0) return 0;

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
