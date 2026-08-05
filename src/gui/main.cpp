/*
 * BENCsynth - a virtual modular synthesizer
 *
 * The window, the audio device, and the arrangement of the three things that
 * make up the interface: a header, a rack, and a keyboard.
 *
 * Audio runs from raylib's stream callback straight into bs::Engine, which
 * owns the lock that keeps the callback and the mouse out of each other's
 * way. Nothing in src/core knows this file exists, which is the arrangement
 * that lets the same rack eventually run inside a plugin.
 */

#include "bs_gui.h"
#include "bs_rack.h"
#include "bs_keyboard.h"
#include "bs_patchfile.h"
#include "bs_engine.h"
#include "bs_modules.h"

#include <cmath>
#include <cstdio>
#include <cstring>

enum {
    WIN_W = 1440, WIN_H = 900,
    WIN_MIN_W = 900, WIN_MIN_H = 620,
    HEADER_H = 46, TOOLBAR_H = 38, KEYS_H = 132,
    SAMPLE_RATE = 48000
};

/* The audio callback needs the engine and cannot be handed a parameter, so it
 * comes through here. One synthesizer per process; there is no arrangement
 * under which a second would be useful. */
static bs::Engine g_engine;

static void audio_cb(void *buffer, unsigned int frames)
{
    g_engine.render((float *)buffer, (int)frames);
}

/* ------------------------------------------------------------------ *
 * Header
 * ------------------------------------------------------------------ */

static void draw_header(bs_ui *ui, Rectangle r, const bs::Engine *eng)
{
    DrawRectangleRec(r, BS_RACK);
    DrawRectangle((int)r.x, (int)(r.y + r.height - 1), (int)r.width, 1, BS_BORDER);

    const Vector2 starAt = { r.x + 30.0f, r.y + r.height * 0.5f };
    bs_star(starAt, 15.0f, 0.0f);

    /* One glowing title per screen, and this is it: the style guide's soft
     * green text-shadow, spread by drawing the word again a pixel out in each
     * direction at low alpha, which is what a blur radius amounts to when the
     * only tool is a text call. */
    const char *name = "BENCsynth";
    static const float OFF[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
    for (int i = 0; i < 4; i++) {
        const Color g = { BS_ACCENT.r, BS_ACCENT.g, BS_ACCENT.b, 60 };
        bs_text_spaced(ui, BS_F_TITLE, name, 56.0f + OFF[i][0] * 2.0f,
                       r.y + 7.0f + OFF[i][1] * 2.0f, g);
    }
    bs_text_spaced(ui, BS_F_TITLE, name, 56.0f, r.y + 7.0f, BS_TEXT);

    const float nameW = bs_measure(ui, BS_F_TITLE, name, 1.0f);
    bs_text_spaced(ui, BS_F_TINY, "BENCO HOLDINGS   MODULAR SERIES",
                   58.0f + nameW + 14.0f, r.y + 19.0f, BS_EDGE);

    /* Right-hand readouts. Held voices rather than allocated ones, because
     * the number that means anything while playing is how many are sounding. */
    int held = 0;
    for (int i = 0; i < bs::BS_MAX_POLY; i++) if (eng->keys.v[i].gate) held++;

    char buf[128];
    std::snprintf(buf, sizeof buf, "VOICES %d/%d    LOAD %2.0f%%    %d Hz",
                  held, eng->keys.channels(), (double)(eng->load * 100.0f),
                  SAMPLE_RATE);
    const float w = bs_measure(ui, BS_F_SMALL, buf, 1.0f);
    bs_text_spaced(ui, BS_F_SMALL, buf, r.x + r.width - w - 14.0f,
                   r.y + (r.height - BS_F_SMALL) * 0.5f, BS_DIM);
}

/* ------------------------------------------------------------------ *
 * About
 * ------------------------------------------------------------------ */

static void draw_about(bs_ui *ui, Rectangle screen)
{
    DrawRectangleRec(screen, (Color){ 0, 0, 0, 170 });

    Rectangle p = { screen.x + screen.width * 0.5f - 290.0f,
                    screen.y + screen.height * 0.5f - 190.0f, 580.0f, 380.0f };
    bs_panel(p, BS_PANEL, BS_ACCENT);

    bs_star((Vector2){ p.x + 84.0f, p.y + 96.0f }, 52.0f, 0.0f);

    bs_text_spaced(ui, BS_F_TITLE, "BENCsynth", p.x + 160.0f, p.y + 36.0f, BS_TEXT);
    bs_text_spaced(ui, BS_F_SMALL, "A VIRTUAL MODULAR SYNTHESIZER",
                   p.x + 162.0f, p.y + 74.0f, BS_DIM);
    bs_text_spaced(ui, BS_F_SMALL, "MASCOT: S. TARR", p.x + 162.0f, p.y + 96.0f, BS_EDGE);
    bs_text_spaced(ui, BS_F_SMALL, "BENCO HOLDINGS", p.x + 162.0f, p.y + 118.0f, BS_EDGE);

    bs_divider(p.x + 20.0f, p.y + 168.0f, p.width - 40.0f);

    static const char *LINES[] = {
        "DRAG a jack to another jack        patch a cable",
        "DRAG a plug out of an input        move that cable",
        "RIGHT-CLICK a jack                 unplug it",
        "RIGHT-CLICK the rack               add a module",
        "RIGHT-CLICK a module title         module menu",
        "DRAG a module title                move the panel",
        "DRAG or WHEEL on a knob            turn it",
        "SHIFT-DRAG a knob                  turn it slowly",
        "DOUBLE-CLICK a knob                back to default",
        "",
        "Z S X D C ... and Q 2 W 3 E ...    play",
        "[ and ]                            octave",
        "SPACE                              sustain",
        "ESC                                all notes off"
    };
    const int n = (int)(sizeof LINES / sizeof LINES[0]);
    for (int i = 0; i < n; i++)
        bs_text(ui, BS_F_TINY, LINES[i], p.x + 24.0f,
                p.y + 182.0f + (float)i * 14.0f, i % 2 ? BS_DIM : BS_TEXT);

    bs_text_spaced(ui, BS_F_TINY, "F1 or click to close", p.x + p.width - 152.0f,
                   p.y + p.height - 22.0f, BS_EDGE);
}

/* ------------------------------------------------------------------ *
 * Toolbar
 * ------------------------------------------------------------------ */

typedef struct {
    int   slot;
    char  status[192];
    float statusAge;
    int   about;
} bs_app;

static void say(bs_app *app, const char *msg)
{
    std::snprintf(app->status, sizeof app->status, "%s", msg);
    app->statusAge = 0.0f;
}

static void draw_toolbar(bs_app *app, bs_ui *ui, bs_rack *rack, bs_keyboard *kb,
                         bs::Engine *eng, Rectangle r, Rectangle rackView)
{
    DrawRectangleRec(r, BS_RACK);
    DrawRectangle((int)r.x, (int)(r.y + r.height - 1), (int)r.width, 1, BS_BORDER);

    const float y = r.y + 6.0f;
    const float h = 26.0f;
    float x = 10.0f;

    Rectangle b;

    b = (Rectangle){ x, y, 62.0f, h };
    if (bs_button(ui, 8001, b, "ADD", 1))
        bs_rack_add_menu(rack, ui, (Vector2){ b.x, b.y + h }, rackView);
    x += 68.0f;

    DrawRectangle((int)x, (int)y + 3, 1, (int)h - 6, BS_BORDER);
    x += 9.0f;

    b = (Rectangle){ x, y, 24.0f, h };
    if (bs_button(ui, 8002, b, "-", app->slot > 1)) app->slot--;
    x += 26.0f;

    char slot[24];
    std::snprintf(slot, sizeof slot, "RACK %d", app->slot);
    b = (Rectangle){ x, y, 68.0f, h };
    bs_panel(b, BS_PANEL, BS_BORDER);
    bs_text_center(ui, BS_F_SMALL, slot, b.x + b.width * 0.5f,
                   b.y + (h - BS_F_SMALL) * 0.5f, BS_TEXT);
    x += 70.0f;

    b = (Rectangle){ x, y, 24.0f, h };
    if (bs_button(ui, 8003, b, "+", app->slot < 8)) app->slot++;
    x += 32.0f;

    b = (Rectangle){ x, y, 62.0f, h };
    if (bs_button(ui, 8004, b, "SAVE", 1)) {
        bs_patch_save(eng, bs_patch_slot_path(app->slot), app->status,
                      (int)sizeof app->status);
        app->statusAge = 0.0f;
    }
    x += 68.0f;

    b = (Rectangle){ x, y, 62.0f, h };
    if (bs_button(ui, 8005, b, "LOAD", 1)) {
        bs_keyboard_release_all(kb, eng);
        bs_patch_load(eng, bs_patch_slot_path(app->slot), app->status,
                      (int)sizeof app->status);
        app->statusAge = 0.0f;
    }
    x += 76.0f;

    DrawRectangle((int)x, (int)y + 3, 1, (int)h - 6, BS_BORDER);
    x += 9.0f;

    b = (Rectangle){ x, y, 84.0f, h };
    if (bs_button(ui, 8006, b, "DEFAULT", 1)) {
        bs_keyboard_release_all(kb, eng);
        eng->buildDefaultPatch();
        bs_rack_home(rack);
        say(app, "default rack restored");
    }
    x += 90.0f;

    b = (Rectangle){ x, y, 62.0f, h };
    if (bs_button(ui, 8007, b, "CLEAR", 1)) {
        bs_keyboard_release_all(kb, eng);
        eng->clear();
        say(app, "rack emptied - right-click to add a module");
    }
    x += 68.0f;

    b = (Rectangle){ x, y, 62.0f, h };
    if (bs_button(ui, 8008, b, "PANIC", 1)) {
        bs_keyboard_release_all(kb, eng);
        eng->panic();
        say(app, "all notes off");
    }
    x += 68.0f;

    b = (Rectangle){ x, y, 62.0f, h };
    if (bs_button_lit(ui, 8009, b, "HELP", app->about)) app->about = !app->about;
    x += 76.0f;

    /* The status line fades rather than disappearing, so a message that has
     * been read stops competing with the rack for attention but is still
     * there if you look back. */
    if (app->status[0]) {
        unsigned char a = 255;
        if (app->statusAge > 6.0f) a = 0;
        else if (app->statusAge > 3.0f)
            a = (unsigned char)(255.0f * (1.0f - (app->statusAge - 3.0f) / 3.0f));
        if (a) {
            const Color c = { BS_DIM.r, BS_DIM.g, BS_DIM.b, a };
            bs_text(ui, BS_F_SMALL, app->status, x, y + (h - BS_F_SMALL) * 0.5f, c);
        }
    }
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    /* A screenshot mode, so the window can be checked from a build script
     * without anyone sitting in front of it. */
    const char *shot = 0;
    int shotFrames = 90;
    const char *loadPath = 0;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--shot") == 0 && i + 1 < argc) shot = argv[++i];
        else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            shotFrames = std::atoi(argv[++i]);
        else loadPath = argv[i];
    }

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    InitWindow(WIN_W, WIN_H, "BENCsynth");
    SetWindowMinSize(WIN_MIN_W, WIN_MIN_H);
    SetTargetFPS(60);
    /* Escape is a panic button here, not a way out. A synthesizer that
     * vanishes when you reach for the key that silences it is a synthesizer
     * you learn not to trust. */
    SetExitKey(KEY_NULL);

    bs_ui ui;
    bs_ui_init(&ui);

    bs_rack rack;
    bs_rack_init(&rack);

    bs_keyboard kb;
    bs_keyboard_init(&kb);

    bs_app app;
    std::memset(&app, 0, sizeof app);
    app.slot = 1;

    g_engine.init((float)SAMPLE_RATE);
    if (loadPath) {
        if (!bs_patch_load(&g_engine, loadPath, app.status, (int)sizeof app.status))
            g_engine.buildDefaultPatch();
    } else {
        g_engine.buildDefaultPatch();
        say(&app, "default rack - press Z, or click the keys");
    }

    InitAudioDevice();
    SetAudioStreamBufferSizeDefault(512);
    AudioStream stream = LoadAudioStream(SAMPLE_RATE, 32, 2);
    SetAudioStreamCallback(stream, audio_cb);
    PlayAudioStream(stream);
    if (!IsAudioDeviceReady())
        say(&app, "no audio device - the rack still patches, it just cannot be heard");

    /* A screenshot of an idle rack shows a dead meter and a flat scope, which
     * is the least informative picture of a synthesizer there is. */
    if (shot) {
        g_engine.noteOn(52, 0.9f);
        g_engine.noteOn(59, 0.85f);
        g_engine.noteOn(64, 0.8f);
        g_engine.noteOn(67, 0.75f);
    }

    int frame = 0;
    while (!WindowShouldClose()) {
        const float dt = GetFrameTime();
        const float W = (float)GetScreenWidth();
        const float H = (float)GetScreenHeight();

        app.statusAge += dt;
        ui.changed = 0;

        Rectangle header  = { 0, 0, W, (float)HEADER_H };
        Rectangle toolbar = { 0, (float)HEADER_H, W, (float)TOOLBAR_H };
        Rectangle keys    = { 0, H - (float)KEYS_H, W, (float)KEYS_H };
        Rectangle rview   = { 0, (float)(HEADER_H + TOOLBAR_H), W,
                              H - (float)(HEADER_H + TOOLBAR_H + KEYS_H) };
        if (rview.height < 80.0f) rview.height = 80.0f;

        if (IsKeyPressed(KEY_F1)) app.about = !app.about;
        if (IsKeyPressed(KEY_ESCAPE)) {
            bs_keyboard_release_all(&kb, &g_engine);
            g_engine.panic();
            say(&app, "all notes off");
        }

        /* Typing is off while the help is up, so reading it does not play a
         * chord at whoever is reading. */
        bs_keyboard_typing(&kb, &g_engine, !app.about);

        BeginDrawing();
        ClearBackground(BS_BG);

        draw_header(&ui, header, &g_engine);

        ui.suppress = app.about;
        draw_toolbar(&app, &ui, &rack, &kb, &g_engine, toolbar, rview);
        bs_rack_frame(&rack, &ui, &g_engine, rview, dt);
        bs_keyboard_frame(&kb, &ui, &g_engine, keys);
        ui.suppress = 0;

        if (app.about) {
            Rectangle screen = { 0, 0, W, H };
            draw_about(&ui, screen);
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) app.about = 0;
        }

        bs_rack_menu(&rack, &ui, &g_engine);
        bs_ui_overlay(&ui);

        EndDrawing();

        if (shot && ++frame >= shotFrames) {
            TakeScreenshot(shot);
            break;
        }
    }

    bs_keyboard_release_all(&kb, &g_engine);
    UnloadAudioStream(stream);
    CloseAudioDevice();
    bs_ui_free(&ui);
    CloseWindow();
    return 0;
}
