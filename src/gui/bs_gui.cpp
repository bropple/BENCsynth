/*
 * BENCsynth GUI - theme and widget set.
 * See bs_gui.h for why these are hand-drawn.
 */

#include "bs_gui.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "bs_input.h"   /* must come after raylib.h - see the header */

Color BS_BG        = { 0x06, 0x0a, 0x05, 255 };
Color BS_RACK      = { 0x0c, 0x14, 0x08, 255 };
Color BS_PANEL     = { 0x18, 0x20, 0x10, 255 };
Color BS_PANEL_HI  = { 0x22, 0x2c, 0x18, 255 };
Color BS_BORDER    = { 0x2a, 0x3a, 0x1e, 255 };
Color BS_TEXT      = { 0xcd, 0xea, 0xb0, 255 };
Color BS_DIM       = { 0x8a, 0xa8, 0x78, 255 };
Color BS_ACCENT    = { 0x78, 0xb9, 0x46, 255 };
Color BS_EDGE      = { 0x3f, 0x5c, 0x28, 255 };
Color BS_ALERT     = { 0xd8, 0x4a, 0x3a, 255 };
Color BS_AMBER     = { 0xe8, 0xb2, 0x3d, 255 };
Color BS_STAR      = { 0xee, 0xcb, 0x2e, 255 };
Color BS_STAR_EDGE = { 0xa3, 0x86, 0x1a, 255 };

/* R. Triy, O. Val, H. Hex, P. Gon, G. Lobe, C. Ross, S. Tarr. */
Color BS_CABLE[BS_CABLE_COLORS] = {
    { 0x78, 0xb9, 0x46, 255 },
    { 0xc1, 0x44, 0x3a, 255 },
    { 0xd9, 0x7a, 0x2b, 255 },
    { 0x3d, 0x7d, 0xbf, 255 },
    { 0x2c, 0x4a, 0x7c, 255 },
    { 0x7a, 0x4f, 0xb5, 255 },
    { 0xee, 0xcb, 0x2e, 255 }
};
Color BS_CABLE_EDGE[BS_CABLE_COLORS] = {
    { 0x3f, 0x5c, 0x28, 255 },
    { 0x7a, 0x28, 0x1f, 255 },
    { 0x8a, 0x4d, 0x18, 255 },
    { 0x25, 0x4d, 0x75, 255 },
    { 0x16, 0x28, 0x3f, 255 },
    { 0x45, 0x27, 0x66, 255 },
    { 0xa3, 0x86, 0x1a, 255 }
};

static const Color BS_HOLE  = { 0x04, 0x07, 0x03, 255 };
static const Color BS_VISOR = { 0x9a, 0x9d, 0x94, 255 };

/* ------------------------------------------------------------------ *
 * Fonts
 *
 * Looked for rather than embedded: the OFL requires the licence to travel
 * with the font, and a build that silently bundles one is a licence problem
 * waiting to happen. assets/fonts/ ships both, and the path beside the
 * executable is tried before the working directory, because resolving a
 * relative path against the working directory only works when the program was
 * started from its own folder - which is true when you double-click it and
 * false for a shortcut, a terminal open somewhere else, or a Run box.
 * ------------------------------------------------------------------ */

/* Where an asset might be relative to the program: beside the executable
 * first, then the working directory. Resolving against the working directory
 * alone only works when the program was started from its own folder - true
 * when you double-click it, false for a shortcut, a terminal open somewhere
 * else, or a Run box. macOS puts Resources one level up and across from
 * Contents/MacOS, which is what the fourth entry is for. */
static const char *ASSET_DIRS[] = {
    "",
    "../Resources/",
    "../../",
};

const char *bs_find_asset(const char *relative, char *probe, size_t cap)
{
    const char *dir = GetApplicationDirectory();
    size_t i;

    for (i = 0; i < sizeof ASSET_DIRS / sizeof ASSET_DIRS[0]; i++) {
        snprintf(probe, cap, "%s%s%s", dir, ASSET_DIRS[i], relative);
        if (FileExists(probe)) return probe;
    }
    for (i = 0; i < sizeof ASSET_DIRS / sizeof ASSET_DIRS[0]; i++) {
        snprintf(probe, cap, "%s%s", ASSET_DIRS[i], relative);
        if (FileExists(probe)) return probe;
    }
    return 0;
}

static const char *FONT_RELATIVE[] = {
    "assets/fonts/TerminusTTF.ttf",
    "../assets/fonts/TerminusTTF.ttf",
    "TerminusTTF.ttf"
};

static const char *FONT_SYSTEM[] = {
    "/usr/share/fonts/TTF/TerminusTTF.ttf",
    "/usr/share/fonts/truetype/terminus/TerminusTTF.ttf",
    "/usr/local/share/fonts/TerminusTTF.ttf",
    "/Library/Fonts/TerminusTTF.ttf",
    "C:/Windows/Fonts/TerminusTTF.ttf"
};

static const char *find_font(char *probe, size_t cap)
{
    size_t i;
    for (i = 0; i < sizeof FONT_RELATIVE / sizeof FONT_RELATIVE[0]; i++) {
        const char *hit = bs_find_asset(FONT_RELATIVE[i], probe, cap);
        if (hit) return hit;
    }
    /* A system package would have put it somewhere absolute. */
    for (i = 0; i < sizeof FONT_SYSTEM / sizeof FONT_SYSTEM[0]; i++)
        if (FileExists(FONT_SYSTEM[i])) return FONT_SYSTEM[i];
    return 0;
}

/* Point filtering, not bilinear. Terminus is a bitmap design; smoothing it is
 * how you get the mush this font exists to avoid. */
static Font load_at(const char *path, int size, int *found)
{
    Font f = LoadFontEx(path, size, 0, 0);
    if (f.texture.id != 0 && f.glyphCount > 0) {
        SetTextureFilter(f.texture, TEXTURE_FILTER_POINT);
        *found = 1;
        return f;
    }
    return GetFontDefault();
}

void bs_ui_init(bs_ui *ui)
{
    static char probe[1024];
    memset(ui, 0, sizeof *ui);
    ui->font_name = "raylib default";

    const char *path = find_font(probe, sizeof probe);
    if (path) {
        ui->tiny  = load_at(path, BS_F_TINY,  &ui->loaded);
        ui->small = load_at(path, BS_F_SMALL, &ui->loaded);
        ui->body  = load_at(path, BS_F_BODY,  &ui->loaded);
        ui->title = load_at(path, BS_F_TITLE, &ui->loaded);
    }
    if (!ui->loaded) {
        ui->tiny = ui->small = ui->body = ui->title = GetFontDefault();
    } else {
        ui->font_name = GetFileName(path);
    }
}

void bs_ui_free(bs_ui *ui)
{
    if (!ui->loaded) return;
    UnloadFont(ui->tiny);
    UnloadFont(ui->small);
    UnloadFont(ui->body);
    UnloadFont(ui->title);
    ui->loaded = 0;
}

static Font pick(const bs_ui *ui, int size)
{
    if (size <= BS_F_TINY)  return ui->tiny;
    if (size <= BS_F_SMALL) return ui->small;
    if (size <= BS_F_BODY)  return ui->body;
    return ui->title;
}

void bs_text(const bs_ui *ui, int size, const char *s, float x, float y, Color c)
{
    Vector2 p = { x, y };
    DrawTextEx(pick(ui, size), s, p, (float)size, 0.0f, c);
}

/* Style-guide letter-spacing: 1px on labels and headings, never body text. */
void bs_text_spaced(const bs_ui *ui, int size, const char *s, float x, float y, Color c)
{
    Vector2 p = { x, y };
    DrawTextEx(pick(ui, size), s, p, (float)size, 1.0f, c);
}

float bs_measure(const bs_ui *ui, int size, const char *s, float spacing)
{
    return MeasureTextEx(pick(ui, size), s, (float)size, spacing).x;
}

void bs_text_center(const bs_ui *ui, int size, const char *s, float cx, float y, Color c)
{
    const float w = bs_measure(ui, size, s, 0.0f);
    bs_text(ui, size, s, cx - w * 0.5f, y, c);
}

void bs_text_fit(const bs_ui *ui, int size, const char *s, float cx, float y,
                 float w, Color c)
{
    char buf[64];
    size_t n = strlen(s);
    if (n >= sizeof buf) n = sizeof buf - 1;
    memcpy(buf, s, n);
    buf[n] = 0;
    while (n > 1 && bs_measure(ui, size, buf, 0.0f) > w) buf[--n] = 0;
    bs_text_center(ui, size, buf, cx, y, c);
}

/* ------------------------------------------------------------------ *
 * Chrome
 * ------------------------------------------------------------------ */

void bs_panel(Rectangle r, Color fill, Color border)
{
    DrawRectangleRounded(r, (float)BS_RADIUS / r.height, 4, fill);
    DrawRectangleRoundedLines(r, (float)BS_RADIUS / r.height, 4, border);
}

void bs_divider(float x, float y, float w)
{
    DrawRectangle((int)x, (int)y, (int)w, 1, BS_BORDER);
}

/* Null when nothing has overridden it, which is the ordinary case. */
static int     g_mouseOverride = 0;
static Vector2 g_mouse = { 0, 0 };

void bs_ui_push_mouse(Vector2 where) { g_mouse = where; g_mouseOverride = 1; }
void bs_ui_pop_mouse(void)           { g_mouseOverride = 0; }

Vector2 bs_mouse(void)
{
    return g_mouseOverride ? g_mouse : GetMousePosition();
}

int bs_ui_blocked(const bs_ui *ui)
{
    if (ui->suppress) return 1;
    /* The menu is drawn in screen space, above everything and outside any
     * transform, so it is tested against the real pointer rather than the
     * caller's idea of one. */
    return ui->menuOpen && CheckCollisionPointRec(GetMousePosition(), ui->menuRect);
}

static int button_common(bs_ui *ui, int id, Rectangle r, const char *label,
                         int enabled, Color fill, Color textCol)
{
    const Vector2 m = bs_mouse();
    const int hot = enabled && !bs_ui_blocked(ui) && CheckCollisionPointRec(m, r);
    const int down = hot && IsMouseButtonDown(MOUSE_BUTTON_LEFT);

    Color f = fill;
    if (!enabled) f = BS_PANEL;
    else if (down) f = BS_EDGE;
    else if (hot)  f = BS_PANEL_HI;

    bs_panel(r, f, enabled ? BS_BORDER : BS_PANEL_HI);

    const Color tc = enabled ? textCol : BS_EDGE;
    const float tw = bs_measure(ui, BS_F_SMALL, label, 1.0f);
    bs_text_spaced(ui, BS_F_SMALL, label,
                   r.x + (r.width - tw) * 0.5f,
                   r.y + (r.height - BS_F_SMALL) * 0.5f, tc);

    return hot && IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
}

int bs_button(bs_ui *ui, int id, Rectangle r, const char *label, int enabled)
{
    return button_common(ui, id, r, label, enabled, BS_PANEL, BS_TEXT);
}

int bs_button_lit(bs_ui *ui, int id, Rectangle r, const char *label, int lit)
{
    if (!lit) return button_common(ui, id, r, label, 1, BS_PANEL, BS_TEXT);
    return button_common(ui, id, r, label, 1, BS_ACCENT, BS_RACK);
}

int bs_info_button(bs_ui *ui, Rectangle r, int lit)
{
    const Vector2 m = bs_mouse();
    const Vector2 c = { r.x + r.width * 0.5f, r.y + r.height * 0.5f };
    const float   rad = (r.width < r.height ? r.width : r.height) * 0.5f;
    const int hot = !bs_ui_blocked(ui) && CheckCollisionPointCircle(m, c, rad);

    const Color fill = lit ? BS_ACCENT : (hot ? BS_PANEL_HI : BS_PANEL);
    DrawCircleV(c, rad, fill);
    DrawCircleLinesV(c, rad, (hot || lit) ? BS_ACCENT : BS_EDGE);

    const Color ink = lit ? BS_RACK : (hot ? BS_TEXT : BS_DIM);
    bs_text_center(ui, BS_F_SMALL, "i", c.x + 0.5f, c.y - BS_F_SMALL * 0.5f, ink);

    return hot && IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
}

/* A keyboard, drawn rather than loaded.
 *
 * Small enough that a bitmap would be a blurry rectangle on a Retina display
 * and a second file to keep in step with the interface's colours. Three white
 * keys and two black ones is the fewest that still reads as a keyboard at
 * sixteen pixels - two whites and one black reads as a window. */
int bs_keys_button(bs_ui *ui, Rectangle r, int lit)
{
    const Vector2 m = bs_mouse();
    const int hot = !bs_ui_blocked(ui) && CheckCollisionPointRec(m, r);

    const Color fill = lit ? BS_ACCENT : (hot ? BS_PANEL_HI : BS_PANEL);
    DrawRectangleRounded(r, 0.25f, 6, fill);
    DrawRectangleRoundedLines(r, 0.25f, 6, (hot || lit) ? BS_ACCENT : BS_EDGE);

    /* The glyph sits in a box inset from the button, so the rounded corner
     * never clips a key. */
    const float pad = 5.0f;
    Rectangle g = { r.x + pad, r.y + pad + 1.0f,
                    r.width - pad * 2.0f, r.height - pad * 2.0f - 2.0f };
    /* The sharps are cut out of the naturals in whatever the button is filled
     * with, rather than painted in a colour of their own. Painting them dark
     * makes them identical to the naturals when the button is lit, and the
     * glyph collapses into a block with two slots in it. */
    const Color ink   = lit ? BS_RACK : (hot ? BS_TEXT : BS_DIM);
    const Color black = fill;

    const float kw = g.width / 3.0f;
    for (int i = 0; i < 3; i++) {
        Rectangle w = { g.x + kw * (float)i + 0.5f, g.y, kw - 1.0f, g.height };
        DrawRectangleRec(w, ink);
    }
    /* Two sharps, on the seams rather than centred on a key - which is where
     * they are on the instrument, and the thing the eye actually checks. */
    const float bw = kw * 0.52f;
    for (int i = 0; i < 2; i++) {
        Rectangle b = { g.x + kw * (float)(i + 1) - bw * 0.5f, g.y,
                        bw, g.height * 0.6f };
        DrawRectangleRec(b, black);
    }

    return hot && IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
}

/* ------------------------------------------------------------------ *
 * Knob
 * ------------------------------------------------------------------ */

/* 270 degrees of travel, starting at the lower left, which is where a knob's
 * pointer sits at minimum on every piece of hardware this is imitating.
 * raylib measures clockwise from the +x axis with y running down the screen,
 * so 135 is lower-left and adding sweeps up over the top. */
static const float KNOB_START = 135.0f;
static const float KNOB_SWEEP = 270.0f;
static const float KNOB_R     = 14.0f;

static void format_value(const bs::Param *p, char *buf, size_t cap)
{
    if (p->kind == bs::PK_SWITCH && p->options) {
        const int i = p->asInt();
        snprintf(buf, cap, "%s",
                 (i >= 0 && i < p->optionCount) ? p->options[i] : "?");
        return;
    }
    if (p->kind == bs::PK_TOGGLE) {
        snprintf(buf, cap, "%s", p->on() ? "ON" : "OFF");
        return;
    }
    snprintf(buf, cap, p->fmt, (double)p->value);
}

/* Name on top, dial, readout underneath - the three bands a knob cell is made
 * of, and they have to add up to BS_KNOB_ROW with room to spare or the readout
 * runs into the name of the knob below it. */
static Vector2 knob_center(Rectangle cell)
{
    Vector2 c = { cell.x + cell.width * 0.5f, cell.y + 12.0f + KNOB_R };
    return c;
}

int bs_knob(bs_ui *ui, int id, Rectangle cell, bs::Param *p)
{
    const Vector2 m = bs_mouse();
    const Vector2 c = knob_center(cell);
    const int blocked = bs_ui_blocked(ui);
    const int hot = !blocked && CheckCollisionPointCircle(m, c, KNOB_R + 5.0f);
    int changed = 0;

    if (ui->active == id) {
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            const int fine = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
            const float speed = fine ? 0.0012f : 0.005f;
            const float t = ui->grabValue + (ui->grabY - m.y) * speed;
            const float before = p->value;
            p->setNorm(t);
            changed = (p->value != before);
        } else {
            ui->active = 0;
        }
    } else if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        const double now = GetTime();
        if (ui->lastId == id && now - ui->lastClick < 0.35) {
            /* Second click of a double: back to the value the module shipped
             * with. A knob you cannot put back is a knob you are reluctant to
             * turn. */
            p->value = p->def;
            ui->active = 0;
            ui->lastId = 0;
            changed = 1;
        } else {
            ui->active    = id;
            ui->grabY     = m.y;
            ui->grabValue = p->norm();
            ui->lastId    = id;
            ui->lastClick = now;
        }
    } else if (hot && ui->active == 0) {
        const float w = GetMouseWheelMove();
        if (w != 0.0f) {
            p->setNorm(p->norm() + w * 0.02f);
            changed = 1;
        }
    }

    /* ---- draw ---- */

    const float t   = p->norm();
    const int   lit = (ui->active == id) || hot;

    char buf[48];
    bs_text_fit(ui, BS_F_TINY, p->name, c.x, cell.y, cell.width - 2.0f, BS_DIM);

    DrawCircleV(c, KNOB_R, lit ? BS_PANEL_HI : BS_PANEL);
    DrawCircleLinesV(c, KNOB_R, lit ? BS_ACCENT : BS_EDGE);

    /* The travel, then the part of it that is used. A bipolar control fills
     * outward from the middle instead of from the end, because that is where
     * its zero is and reading it off the fill is the whole point. */
    DrawRing(c, KNOB_R + 2.0f, KNOB_R + 4.0f, KNOB_START, KNOB_START + KNOB_SWEEP,
             28, BS_BORDER);
    const int bipolar = (p->lo < 0.0f && p->hi > 0.0f);
    const float from = bipolar ? KNOB_START + KNOB_SWEEP * 0.5f : KNOB_START;
    const float to   = KNOB_START + KNOB_SWEEP * t;
    if (std::fabs(to - from) > 0.5f)
        DrawRing(c, KNOB_R + 2.0f, KNOB_R + 4.0f,
                 from < to ? from : to, from < to ? to : from, 28, BS_ACCENT);

    const float a = (KNOB_START + KNOB_SWEEP * t) * DEG2RAD;
    const Vector2 p0 = { c.x + std::cos(a) * KNOB_R * 0.30f,
                         c.y + std::sin(a) * KNOB_R * 0.30f };
    const Vector2 p1 = { c.x + std::cos(a) * KNOB_R * 0.92f,
                         c.y + std::sin(a) * KNOB_R * 0.92f };
    DrawLineEx(p0, p1, 2.0f, lit ? BS_TEXT : BS_DIM);

    format_value(p, buf, sizeof buf);
    bs_text_fit(ui, BS_F_TINY, buf, c.x, cell.y + 40.0f, cell.width - 2.0f,
                lit ? BS_TEXT : BS_DIM);

    if (changed) ui->changed = 1;
    return changed;
}

/* ------------------------------------------------------------------ *
 * Switch and toggle
 * ------------------------------------------------------------------ */

static int small_button(bs_ui *ui, Rectangle r, const char *label, int lit)
{
    const Vector2 m = bs_mouse();
    const int hot = !bs_ui_blocked(ui) && CheckCollisionPointRec(m, r);

    Color f = lit ? BS_ACCENT : (hot ? BS_PANEL_HI : BS_PANEL);
    bs_panel(r, f, hot ? BS_ACCENT : BS_BORDER);
    bs_text_fit(ui, BS_F_TINY, label, r.x + r.width * 0.5f,
                r.y + (r.height - BS_F_TINY) * 0.5f, r.width - 4.0f,
                lit ? BS_RACK : BS_TEXT);

    if (!hot) return 0;
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))  return 1;
    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) return -1;
    return 0;
}

int bs_switch(bs_ui *ui, int id, Rectangle cell, bs::Param *p)
{
    char buf[48];
    bs_text_fit(ui, BS_F_TINY, p->name, cell.x + cell.width * 0.5f, cell.y,
                cell.width - 2.0f, BS_DIM);

    Rectangle r = { cell.x + 3.0f, cell.y + 15.0f, cell.width - 6.0f, 22.0f };
    format_value(p, buf, sizeof buf);

    const int hit = small_button(ui, r, buf, 0);
    if (!hit) return 0;

    int i = p->asInt() + (hit > 0 ? 1 : -1);
    const int n = p->optionCount > 0 ? p->optionCount : 2;
    while (i < 0) i += n;
    p->value = (float)(i % n);
    ui->changed = 1;
    return 1;
}

int bs_toggle(bs_ui *ui, int id, Rectangle cell, bs::Param *p)
{
    bs_text_fit(ui, BS_F_TINY, p->name, cell.x + cell.width * 0.5f, cell.y,
                cell.width - 2.0f, BS_DIM);

    Rectangle r = { cell.x + 3.0f, cell.y + 15.0f, cell.width - 6.0f, 22.0f };
    const int hit = small_button(ui, r, p->on() ? "ON" : "OFF", p->on());
    if (hit <= 0) return 0;
    p->value = p->on() ? 0.0f : 1.0f;
    ui->changed = 1;
    return 1;
}

/* ------------------------------------------------------------------ *
 * Jack
 * ------------------------------------------------------------------ */

Vector2 bs_jack_center(Rectangle cell)
{
    Vector2 c = { cell.x + cell.width * 0.5f, cell.y + 13.0f };
    return c;
}

void bs_jack(const bs_ui *ui, Rectangle cell, const char *label,
             int plugged, Color plugColor, int hot, int isOutput)
{
    const Vector2 c = bs_jack_center(cell);
    const float r = (float)BS_JACK_R;

    /* Outputs get a filled collar, inputs a hollow one. It is the only cue
     * that survives at this size, and it is the one that matters: you cannot
     * usefully patch an output into an output. */
    DrawCircleV(c, r, isOutput ? BS_EDGE : BS_BORDER);
    DrawCircleV(c, r - 2.0f, BS_HOLE);
    if (isOutput) DrawCircleLinesV(c, r, BS_ACCENT);

    if (plugged) {
        DrawCircleV(c, r - 1.0f, plugColor);
        DrawCircleV(c, r - 4.0f, BS_HOLE);
    }
    if (hot) DrawCircleLinesV(c, r + 3.0f, BS_TEXT);

    bs_text_fit(ui, BS_F_TINY, label, c.x, cell.y + 25.0f, cell.width,
                hot ? BS_TEXT : BS_DIM);
}

/* ------------------------------------------------------------------ *
 * Meter and scope
 * ------------------------------------------------------------------ */

static void meter_bar(Rectangle r, float peak, float rms, int clip)
{
    DrawRectangleRec(r, BS_HOLE);
    DrawRectangleLinesEx(r, 1.0f, BS_BORDER);

    const float w = r.width - 2.0f;
    float f = rms * 3.0f;                 /* RMS of a synth line sits low */
    if (f > 1.0f) f = 1.0f;
    DrawRectangle((int)(r.x + 1), (int)(r.y + 1), (int)(w * f), (int)(r.height - 2),
                  clip ? BS_ALERT : BS_ACCENT);

    float p = peak;
    if (p > 1.0f) p = 1.0f;
    const float px = r.x + 1.0f + w * p;
    DrawRectangle((int)px - 1, (int)(r.y + 1), 2, (int)(r.height - 2),
                  clip ? BS_ALERT : BS_TEXT);
}

void bs_meter(Rectangle r, float peakL, float rmsL, float peakR, float rmsR, int clip)
{
    const float h = (r.height - 3.0f) * 0.5f;
    Rectangle a = { r.x, r.y, r.width, h };
    Rectangle b = { r.x, r.y + h + 3.0f, r.width, h };
    meter_bar(a, peakL, rmsL, clip);
    meter_bar(b, peakR, rmsR, clip);
}

void bs_scope(Rectangle r, const float *ring, int len, int head, Color c)
{
    DrawRectangleRec(r, BS_HOLE);
    DrawRectangleLinesEx(r, 1.0f, BS_BORDER);
    DrawLine((int)r.x + 1, (int)(r.y + r.height * 0.5f),
             (int)(r.x + r.width - 1), (int)(r.y + r.height * 0.5f), BS_BORDER);

    if (len < 2) return;
    const float half = r.height * 0.5f - 2.0f;
    const float step = (r.width - 2.0f) / (float)(len - 1);

    Vector2 prev = { 0, 0 };
    for (int i = 0; i < len; i++) {
        /* Oldest sample at the left: the ring's write head is the newest, so
         * reading forward from it walks the trace in time order. */
        float v = ring[(head + i) % len];
        if (v >  1.0f) v =  1.0f;
        if (v < -1.0f) v = -1.0f;
        Vector2 p = { r.x + 1.0f + step * (float)i,
                      r.y + r.height * 0.5f - v * half };
        if (i) DrawLineEx(prev, p, 1.5f, c);
        prev = p;
    }
}

/* The same ratios bs_star draws with, kept in one place so the icon and the
 * on-screen mark cannot come apart. */
static const float STAR_INNER = 0.421f;
static const float STAR_VIS_W = 1.158f, STAR_VIS_H = 0.358f, STAR_VIS_Y = -0.053f;
static const float STAR_STR_W = 0.737f, STAR_STR_H = 0.168f, STAR_STR_Y =  0.042f;

static void star_points(Vector2 *pts, Vector2 c, float r, float rot)
{
    for (int i = 0; i < 10; i++) {
        const float a = rot + (-90.0f + (float)i * 36.0f) * DEG2RAD;
        const float rr = r * ((i % 2 == 0) ? 1.0f : STAR_INNER);
        pts[i].x = c.x + std::cos(a) * rr;
        pts[i].y = c.y + std::sin(a) * rr;
    }
}

static bool inside_poly(const Vector2 *p, int n, float x, float y)
{
    bool in = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        if (((p[i].y > y) != (p[j].y > y)) &&
            (x < (p[j].x - p[i].x) * (y - p[i].y) / (p[j].y - p[i].y) + p[i].x))
            in = !in;
    }
    return in;
}

static float dist_to_poly(const Vector2 *p, int n, float x, float y)
{
    float best = 1e30f;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        const float dx = p[j].x - p[i].x, dy = p[j].y - p[i].y;
        const float len2 = dx * dx + dy * dy;
        float t = 0.0f;
        if (len2 > 1e-9f) {
            t = ((x - p[i].x) * dx + (y - p[i].y) * dy) / len2;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
        }
        const float qx = p[i].x + dx * t - x, qy = p[i].y + dy * t - y;
        const float d = std::sqrt(qx * qx + qy * qy);
        if (d < best) best = d;
    }
    return best;
}


/* ------------------------------------------------------------------ *
 * S. Tarr
 * ------------------------------------------------------------------ */

void bs_star(Vector2 center, float radius, float rotation)
{
    Vector2 pts[10];
    star_points(pts, center, radius, rotation);

    /* Counter-clockwise on screen means walking the points backwards, since
     * y runs down; raylib culls the other winding. */
    Vector2 fan[12];
    fan[0] = center;
    for (int i = 0; i < 10; i++) fan[1 + i] = pts[9 - i];
    fan[11] = pts[9];
    DrawTriangleFan(fan, 12, BS_STAR);

    for (int i = 0; i < 10; i++)
        DrawLineEx(pts[i], pts[(i + 1) % 10], radius * 0.03f + 1.0f, BS_STAR_EDGE);

    Rectangle visor = { center.x - radius * STAR_VIS_W * 0.5f,
                        center.y + radius * STAR_VIS_Y,
                        radius * STAR_VIS_W, radius * STAR_VIS_H };
    Rectangle strip = { center.x - radius * STAR_STR_W * 0.5f,
                        center.y + radius * STAR_STR_Y,
                        radius * STAR_STR_W, radius * STAR_STR_H };
    DrawRectangleRec(visor, BS_VISOR);
    DrawRectangleRec(strip, BS_ALERT);
}

Image bs_star_image(int size)
{
    if (size < 4) size = 4;

    Color *px = (Color *)MemAlloc((unsigned)(size * size) * (unsigned)sizeof(Color));

    const Vector2 c = { size * 0.5f, size * 0.5f };
    /* Inset, so the points do not touch the edge of the tile. A taskbar draws
     * icons hard against their neighbours and a mark that fills its square
     * looks bigger than everything beside it. */
    const float r = size * 0.46f;
    /* The SVG's hairline stroke is invisible above thumbnail sizes, so the
     * edge is a proportion of the radius instead. Deliberately without a
     * one-pixel floor: at 16 px a whole pixel of edge is an eighth of the
     * radius, and the star ends up more outline than star. Below one pixel the
     * supersampling turns it into a darkened rim, which is what it should be.
     *
     * The grey of the visor goes the same way. At small sizes a light band
     * across the middle competes with the silhouette and the whole mark reads
     * as a blob; the red stripe alone still says "S. Tarr" and leaves the star
     * a star. Simplifying rather than shrinking is what icon sets do at 16 px,
     * and this is the smallest possible version of it. */
    const float stroke = r * 0.06f;
    const bool  detail = (size >= 28);

    Vector2 pts[10];
    star_points(pts, c, r, 0.0f);

    const float visX0 = c.x - r * STAR_VIS_W * 0.5f, visX1 = c.x + r * STAR_VIS_W * 0.5f;
    const float visY0 = c.y + r * STAR_VIS_Y,        visY1 = visY0 + r * STAR_VIS_H;
    const float strX0 = c.x - r * STAR_STR_W * 0.5f, strX1 = c.x + r * STAR_STR_W * 0.5f;
    const float strY0 = c.y + r * STAR_STR_Y,        strY1 = strY0 + r * STAR_STR_H;

    /* Three by three, which is enough for a shape with no near-horizontal
     * edges and cheap enough that the largest icon is still instant. */
    enum { SS = 3 };

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            int hits = 0, rSum = 0, gSum = 0, bSum = 0;

            for (int sy = 0; sy < SS; sy++) {
                for (int sx = 0; sx < SS; sx++) {
                    const float fx = (float)x + ((float)sx + 0.5f) / (float)SS;
                    const float fy = (float)y + ((float)sy + 0.5f) / (float)SS;
                    if (!inside_poly(pts, 10, fx, fy)) continue;

                    Color k;
                    if (fx >= strX0 && fx < strX1 && fy >= strY0 && fy < strY1)
                        k = BS_ALERT;
                    else if (detail && fx >= visX0 && fx < visX1 &&
                             fy >= visY0 && fy < visY1)
                        k = BS_VISOR;
                    else if (dist_to_poly(pts, 10, fx, fy) < stroke)
                        k = BS_STAR_EDGE;
                    else
                        k = BS_STAR;

                    hits++;
                    rSum += k.r; gSum += k.g; bSum += k.b;
                }
            }

            Color out = { 0, 0, 0, 0 };
            if (hits) {
                out.r = (unsigned char)(rSum / hits);
                out.g = (unsigned char)(gSum / hits);
                out.b = (unsigned char)(bSum / hits);
                out.a = (unsigned char)((hits * 255) / (SS * SS));
            }
            px[y * size + x] = out;
        }
    }

    Image img;
    img.data    = px;
    img.width   = size;
    img.height  = size;
    img.mipmaps = 1;
    img.format  = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    return img;
}

/* ------------------------------------------------------------------ *
 * Menu
 * ------------------------------------------------------------------ */

enum { MENU_ROW = 22 };

void bs_menu_open(bs_ui *ui, Vector2 at, const char **items, int count, int tag)
{
    float w = 90.0f;
    for (int i = 0; i < count; i++) {
        const float t = bs_measure(ui, BS_F_SMALL, items[i], 1.0f) + 24.0f;
        if (t > w) w = t;
    }
    const float h = (float)(count * MENU_ROW) + 8.0f;

    /* Nudged back inside the window rather than clipped, so a right-click near
     * the bottom edge does not open a menu you cannot read. */
    float x = at.x, y = at.y;
    if (x + w > (float)GetScreenWidth())  x = (float)GetScreenWidth() - w - 4.0f;
    if (y + h > (float)GetScreenHeight()) y = (float)GetScreenHeight() - h - 4.0f;
    if (x < 4.0f) x = 4.0f;
    if (y < 4.0f) y = 4.0f;

    ui->menuOpen   = 1;
    ui->menuFresh  = 1;
    ui->menuItems  = items;
    ui->menuCount  = count;
    ui->menuTag    = tag;
    ui->menuHover  = -1;
    ui->menuAnchor = at;
    ui->menuRect   = (Rectangle){ x, y, w, h };
}

void bs_menu_close(bs_ui *ui) { ui->menuOpen = 0; ui->menuCount = 0; }

int bs_menu_take(bs_ui *ui, int *tag)
{
    if (!ui->menuOpen) return -1;

    const Vector2 m = GetMousePosition();
    ui->menuHover = -1;
    /* Measured from the first row's top edge, and only downward. Dividing a
     * small negative number by the row height and truncating gives zero, so a
     * pointer just *above* the list reads as being on its first entry - which
     * is how a right-click that opened a menu at the pointer ended up
     * selecting the item under the pointer's own corner. */
    const float rel = m.y - ui->menuRect.y - 4.0f;
    if (rel >= 0.0f && CheckCollisionPointRec(m, ui->menuRect)) {
        const int row = (int)(rel / (float)MENU_ROW);
        if (row < ui->menuCount) ui->menuHover = row;
    }

    /* A press is true for the whole frame it happens on, and the menus opened
     * by a right-click on a panel or on the rack are opened by one - so
     * without this, that same press reaches here further down the frame and
     * chooses from the menu it just opened. The module menu's first entry is
     * UNPATCH, which is why right-clicking a title bar appeared to strip its
     * cables instead of offering anything. */
    if (ui->menuFresh) { ui->menuFresh = 0; return -1; }

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) ||
        IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
        const int hit = ui->menuHover;
        if (tag) *tag = ui->menuTag;
        bs_menu_close(ui);
        return hit;
    }
    if (IsKeyPressed(KEY_ESCAPE)) bs_menu_close(ui);
    return -1;
}

void bs_ui_overlay(bs_ui *ui)
{
    if (!ui->menuOpen) return;

    const Rectangle r = ui->menuRect;
    DrawRectangle((int)r.x + 3, (int)r.y + 3, (int)r.width, (int)r.height,
                  (Color){ 0, 0, 0, 110 });
    bs_panel(r, BS_PANEL, BS_ACCENT);

    for (int i = 0; i < ui->menuCount; i++) {
        Rectangle row = { r.x + 2.0f, r.y + 4.0f + (float)(i * MENU_ROW),
                          r.width - 4.0f, (float)MENU_ROW };
        if (i == ui->menuHover) DrawRectangleRec(row, BS_EDGE);
        bs_text_spaced(ui, BS_F_SMALL, ui->menuItems[i], row.x + 8.0f,
                       row.y + (MENU_ROW - BS_F_SMALL) * 0.5f,
                       i == ui->menuHover ? BS_TEXT : BS_DIM);
    }
}

/* ------------------------------------------------------------------ *
 * Text area
 * ------------------------------------------------------------------ */

enum { TA_PAD = 6, TA_BAR = 10 };

/* Where each visual line starts, in bytes.
 *
 * Soft wrapping breaks at the last space that fits rather than mid-word when
 * there is one, and mid-word when there is not - a scratchpad in a 240 px
 * panel is nearly all short lines, and breaking those in the middle of a word
 * is the difference between notes you can read and notes you squint at. */
static void ta_lines(const std::string &s, int cols, std::vector<int> &starts)
{
    starts.clear();
    starts.push_back(0);

    int lineStart = 0, lastSpace = -1;
    for (int i = 0; i < (int)s.size(); i++) {
        if (s[(size_t)i] == '\n') {
            starts.push_back(i + 1);
            lineStart = i + 1;
            lastSpace = -1;
            continue;
        }
        if (s[(size_t)i] == ' ') lastSpace = i;
        if (i - lineStart + 1 > cols) {
            const int brk = (lastSpace > lineStart) ? lastSpace + 1 : i;
            starts.push_back(brk);
            lineStart = brk;
            lastSpace = -1;
        }
    }
}

static int ta_line_of(const std::vector<int> &starts, int caret)
{
    int lo = 0, hi = (int)starts.size() - 1;
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (starts[(size_t)mid] <= caret) lo = mid; else hi = mid - 1;
    }
    return lo;
}

static int ta_line_end(const std::string &s, const std::vector<int> &starts, int line)
{
    if (line + 1 < (int)starts.size()) {
        int e = starts[(size_t)line + 1];
        /* A hard break belongs to the line before it; a soft one does not. */
        if (e > 0 && e <= (int)s.size() && s[(size_t)e - 1] == '\n') e--;
        return e;
    }
    return (int)s.size();
}

static void ta_erase_selection(std::string &s, bs_edit *st)
{
    if (st->caret == st->sel) return;
    const int a = st->caret < st->sel ? st->caret : st->sel;
    const int b = st->caret < st->sel ? st->sel : st->caret;
    s.erase((size_t)a, (size_t)(b - a));
    st->caret = st->sel = a;
}

static void ta_insert(std::string &s, bs_edit *st, const char *text)
{
    ta_erase_selection(s, st);
    const std::string ins(text);
    s.insert((size_t)st->caret, ins);
    st->caret += (int)ins.size();
    st->sel = st->caret;
}

int bs_textarea(bs_ui *ui, int id, Rectangle r, std::string &text, bs_edit *st,
                int editable)
{
    const Vector2 m = bs_mouse();
    const int blocked = bs_ui_blocked(ui);
    const int over = !blocked && CheckCollisionPointRec(m, r);
    int changed = 0;

    const float charW = bs_measure(ui, BS_F_TINY, "M", 0.0f);
    const float lineH = (float)BS_F_TINY + 2.0f;
    const float innerW = r.width - TA_PAD * 2.0f - TA_BAR;
    int cols = (int)(innerW / (charW > 0.5f ? charW : 8.0f));
    if (cols < 4) cols = 4;

    static std::vector<int> starts;
    ta_lines(text, cols, starts);
    const int nLines = (int)starts.size();
    const float contentH = (float)nLines * lineH;
    const float viewH = r.height - TA_PAD * 2.0f;
    const float maxScroll = contentH - viewH > 0.0f ? contentH - viewH : 0.0f;

    /* ---- taking and losing focus ---- */
    if (over && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) ui->focus = id;
    else if (!over && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && ui->focus == id)
        ui->focus = 0;
    const int hot = (ui->focus == id);

    /* ---- pointer ---- */
    if (over || st->dragText || st->dragBar) {
        const float wheel = GetMouseWheelMove();
        if (wheel != 0.0f && over) st->scroll -= wheel * lineH * 3.0f;
    }

    Rectangle bar = { r.x + r.width - TA_BAR - 2.0f, r.y + 2.0f,
                      (float)TA_BAR, r.height - 4.0f };
    if (maxScroll > 0.0f) {
        const float thumbH = bar.height * (viewH / contentH);
        const float thumbY = bar.y + (bar.height - thumbH) * (st->scroll / maxScroll);
        Rectangle thumb = { bar.x, thumbY, bar.width, thumbH };
        if (!blocked && CheckCollisionPointRec(m, thumb) &&
            IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            st->dragBar = 1;
            st->dragGrab = m.y - thumbY;
        }
        if (st->dragBar) {
            if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                const float t = (m.y - st->dragGrab - bar.y) / (bar.height - thumbH);
                st->scroll = maxScroll * bs::clampf(t, 0.0f, 1.0f);
            } else {
                st->dragBar = 0;
            }
        }
    } else {
        st->dragBar = 0;
    }

    /* Caret from a pixel. Monospace, so it is a division rather than a search. */
    if (over && !st->dragBar && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        const int line = (int)((m.y - r.y - TA_PAD + st->scroll) / lineH);
        const int li = line < 0 ? 0 : (line >= nLines ? nLines - 1 : line);
        const int col = (int)((m.x - r.x - TA_PAD) / charW + 0.5f);
        const int ls = starts[(size_t)li];
        const int le = ta_line_end(text, starts, li);
        int at = ls + (col < 0 ? 0 : col);
        if (at > le) at = le;
        st->caret = st->sel = at;
        st->dragText = 1;
        st->blink = 0.0f;
    }
    if (st->dragText) {
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            const int line = (int)((m.y - r.y - TA_PAD + st->scroll) / lineH);
            const int li = line < 0 ? 0 : (line >= nLines ? nLines - 1 : line);
            const int col = (int)((m.x - r.x - TA_PAD) / charW + 0.5f);
            const int ls = starts[(size_t)li];
            const int le = ta_line_end(text, starts, li);
            int at = ls + (col < 0 ? 0 : col);
            if (at > le) at = le;
            st->caret = at;
        } else {
            st->dragText = 0;
        }
    }

    /* ---- keys ---- */
    if (hot) {
        const int ctrl  = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL) ||
                          IsKeyDown(KEY_LEFT_SUPER)   || IsKeyDown(KEY_RIGHT_SUPER);
        const int shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);

        if (ctrl && IsKeyPressed(KEY_A)) { st->sel = 0; st->caret = (int)text.size(); }
        if (ctrl && (IsKeyPressed(KEY_C) || IsKeyPressed(KEY_X)) && st->caret != st->sel) {
            const int a = st->caret < st->sel ? st->caret : st->sel;
            const int b = st->caret < st->sel ? st->sel : st->caret;
            SetClipboardText(text.substr((size_t)a, (size_t)(b - a)).c_str());
            if (editable && IsKeyPressed(KEY_X)) { ta_erase_selection(text, st); changed = 1; }
        }
        if (editable && ctrl && IsKeyPressed(KEY_V)) {
            const char *clip = GetClipboardText();
            if (clip && *clip) { ta_insert(text, st, clip); changed = 1; }
        }

        if (!ctrl) {
            int c;
            while (editable && (c = GetCharPressed()) != 0) {
                if (c < 32 || c > 126) continue;   /* ASCII, like every other surface */
                const char one[2] = { (char)c, 0 };
                ta_insert(text, st, one);
                changed = 1;
            }
            if (editable && (IsKeyPressed(KEY_ENTER) || IsKeyPressedRepeat(KEY_ENTER))) {
                ta_insert(text, st, "\n");
                changed = 1;
            }
            if (editable && (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE))) {
                if (st->caret != st->sel) ta_erase_selection(text, st);
                else if (st->caret > 0) { text.erase((size_t)--st->caret, 1); st->sel = st->caret; }
                changed = 1;
            }
            if (editable && (IsKeyPressed(KEY_DELETE) || IsKeyPressedRepeat(KEY_DELETE))) {
                if (st->caret != st->sel) ta_erase_selection(text, st);
                else if (st->caret < (int)text.size()) text.erase((size_t)st->caret, 1);
                changed = 1;
            }
        }

        int moved = 0;
        if (IsKeyPressed(KEY_LEFT)  || IsKeyPressedRepeat(KEY_LEFT))  { if (st->caret > 0) st->caret--; moved = 1; }
        if (IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT)) { if (st->caret < (int)text.size()) st->caret++; moved = 1; }
        if (IsKeyPressed(KEY_HOME)) { st->caret = starts[(size_t)ta_line_of(starts, st->caret)]; moved = 1; }
        if (IsKeyPressed(KEY_END))  { st->caret = ta_line_end(text, starts, ta_line_of(starts, st->caret)); moved = 1; }
        if (IsKeyPressed(KEY_UP)   || IsKeyPressedRepeat(KEY_UP) ||
            IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN)) {
            const int li = ta_line_of(starts, st->caret);
            const int col = st->caret - starts[(size_t)li];
            const int up = IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP);
            const int tgt = up ? li - 1 : li + 1;
            if (tgt >= 0 && tgt < nLines) {
                const int ls = starts[(size_t)tgt];
                const int le = ta_line_end(text, starts, tgt);
                st->caret = ls + col > le ? le : ls + col;
            }
            moved = 1;
        }
        if (moved) {
            if (!shift) st->sel = st->caret;
            st->blink = 0.0f;
        }
        if (changed) st->sel = st->caret;

        /* Keep the caret in view after anything that moved it. */
        if (moved || changed) {
            ta_lines(text, cols, starts);
            const float cy = (float)ta_line_of(starts, st->caret) * lineH;
            if (cy < st->scroll) st->scroll = cy;
            if (cy + lineH > st->scroll + viewH) st->scroll = cy + lineH - viewH;
        }
    }

    const float limit = (float)starts.size() * lineH - viewH;
    if (st->scroll > (limit > 0.0f ? limit : 0.0f)) st->scroll = limit > 0.0f ? limit : 0.0f;
    if (st->scroll < 0.0f) st->scroll = 0.0f;

    /* ---- draw ---- */
    DrawRectangleRec(r, BS_HOLE);
    DrawRectangleLinesEx(r, 1.0f, hot ? BS_ACCENT : BS_BORDER);

    /* Clipped by choosing what to draw rather than with a scissor: the rack
     * draws under a camera transform, and a scissor rectangle is in screen
     * pixels whatever transform is active, so one computed from these
     * coordinates would land somewhere else entirely. */
    const float clipTop = r.y + 1.0f;
    const float clipBot = r.y + r.height - 1.0f;

    const int selA = st->caret < st->sel ? st->caret : st->sel;
    const int selB = st->caret < st->sel ? st->sel : st->caret;
    const int first = (int)(st->scroll / lineH);
    const int last  = first + (int)(viewH / lineH) + 2;

    for (int li = first; li < last && li < (int)starts.size(); li++) {
        if (li < 0) continue;
        const int ls = starts[(size_t)li];
        const int le = ta_line_end(text, starts, li);
        const float ly = r.y + TA_PAD + (float)li * lineH - st->scroll;
        if (ly < clipTop - lineH || ly + lineH > clipBot) continue;

        if (selB > selA && selB > ls && selA < le) {
            const int a = selA > ls ? selA : ls;
            const int b = selB < le ? selB : le;
            Rectangle sr = { r.x + TA_PAD + (float)(a - ls) * charW, ly,
                             (float)(b - a) * charW, lineH };
            if (sr.width < 2.0f) sr.width = 2.0f;
            DrawRectangleRec(sr, BS_EDGE);
        }

        if (le > ls)
            bs_text(ui, BS_F_TINY, text.substr((size_t)ls, (size_t)(le - ls)).c_str(),
                    r.x + TA_PAD, ly, BS_TEXT);
    }

    if (hot && editable) {
        st->blink += GetFrameTime();
        if (std::fmod(st->blink, 1.0f) < 0.55f) {
            const int li = ta_line_of(starts, st->caret);
            const float cx = r.x + TA_PAD + (float)(st->caret - starts[(size_t)li]) * charW;
            const float cy = r.y + TA_PAD + (float)li * lineH - st->scroll;
            if (cy >= clipTop && cy + lineH <= clipBot)
                DrawRectangleRec((Rectangle){ cx, cy, 1.5f, lineH - 1.0f }, BS_TEXT);
        }
    }

    if (maxScroll > 0.0f) {
        DrawRectangleRec(bar, BS_PANEL);
        const float thumbH = bar.height * (viewH / contentH);
        const float thumbY = bar.y + (bar.height - thumbH) * (st->scroll / maxScroll);
        DrawRectangleRec((Rectangle){ bar.x, thumbY, bar.width, thumbH },
                         st->dragBar ? BS_ACCENT : BS_EDGE);
    }

    return changed;
}
