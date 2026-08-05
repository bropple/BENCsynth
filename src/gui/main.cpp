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
#include "bs_version.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>

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

static int draw_header(bs_ui *ui, Rectangle r, const bs::Engine *eng, int infoOpen)
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

    /* The information button takes the corner, so the readouts stop short of
     * it rather than running underneath. */
    Rectangle info = { r.x + r.width - 34.0f, r.y + (r.height - 24.0f) * 0.5f,
                       24.0f, 24.0f };
    const int hit = bs_info_button(ui, info, infoOpen);

    /* Sounding voices rather than allocated ones: the number that means
     * anything while playing is how many are audible. Read through the
     * published counters - the voice array belongs to the audio thread. */
    char buf[128];
    std::snprintf(buf, sizeof buf, "VOICES %d/%d    LOAD %2.0f%%    %d Hz",
                  eng->voicesSounding(), eng->voicesAllocated(),
                  (double)(eng->load * 100.0f), SAMPLE_RATE);
    const float w = bs_measure(ui, BS_F_SMALL, buf, 1.0f);
    bs_text_spaced(ui, BS_F_SMALL, buf, info.x - w - 16.0f,
                   r.y + (r.height - BS_F_SMALL) * 0.5f, BS_DIM);

    return hit;
}

/* ------------------------------------------------------------------ *
 * Information
 * ------------------------------------------------------------------ */

/* The BENCO wordmark, white on transparent, tinted to phosphor when it is
 * drawn. Zero id means it was not found, and the panel simply does without. */
static Texture2D g_logo = { 0, 0, 0, 0, 0 };

static void load_logo(void)
{
    char probe[1024];
    const char *path = bs_find_asset("assets/brand/BENCO_Logo_Terminal.png",
                                     probe, sizeof probe);
    if (!path) return;
    Image img = LoadImage(path);
    if (img.data == 0) return;
    g_logo = LoadTextureFromImage(img);
    UnloadImage(img);
    if (g_logo.id != 0) SetTextureFilter(g_logo, TEXTURE_FILTER_BILINEAR);
}

static int draw_info(bs_ui *ui, Rectangle screen)
{
    DrawRectangleRec(screen, (Color){ 0, 0, 0, 190 });

    /* Sized to what is in it. A panel that stretches to the window leaves a
     * third of itself empty, which reads as something failing to load. */
    float pw = screen.width - 120.0f;
    float ph = screen.height - 90.0f;
    if (pw > 660.0f) pw = 660.0f;
    if (ph > 484.0f) ph = 484.0f;
    Rectangle p = { screen.x + (screen.width - pw) * 0.5f,
                    screen.y + (screen.height - ph) * 0.5f, pw, ph };
    bs_panel(p, BS_PANEL, BS_ACCENT);

    float y = p.y + 24.0f;

    /* The wordmark, then the star, then the name: house first, mascot second,
     * program third, which is the order they belong in. */
    if (g_logo.id != 0) {
        float lw = pw * 0.42f;
        if (lw > 260.0f) lw = 260.0f;
        const float lh = lw * (float)g_logo.height / (float)g_logo.width;
        DrawTexturePro(g_logo,
                       (Rectangle){ 0, 0, (float)g_logo.width, (float)g_logo.height },
                       (Rectangle){ p.x + (pw - lw) * 0.5f, y, lw, lh },
                       (Vector2){ 0, 0 }, 0.0f, BS_TEXT);
        y += lh + 16.0f;
    } else {
        bs_text_center(ui, BS_F_BODY, "BENCO HOLDINGS", p.x + pw * 0.5f, y, BS_TEXT);
        y += 30.0f;
    }

    bs_star((Vector2){ p.x + pw * 0.5f, y + 26.0f }, 26.0f, 0.0f);
    y += 60.0f;

    char line[128];
    std::snprintf(line, sizeof line, "BENCsynth %s", BS_VERSION_STRING);
    bs_text_center(ui, BS_F_TITLE, line, p.x + pw * 0.5f, y, BS_TEXT);
    y += 34.0f;
    bs_text_center(ui, BS_F_SMALL, "a polyphonic virtual modular synthesizer",
                   p.x + pw * 0.5f, y, BS_DIM);
    y += 22.0f;
    bs_text_center(ui, BS_F_SMALL, "Copyright (c) 2026 Ben Ropple",
                   p.x + pw * 0.5f, y, BS_TEXT);
    y += 20.0f;
    bs_text_center(ui, BS_F_TINY, "mascot: S. Tarr", p.x + pw * 0.5f, y, BS_EDGE);
    y += 22.0f;

    bs_divider(p.x + 24.0f, y, pw - 48.0f);
    y += 12.0f;

    /* Two columns, because one column of fourteen lines is a wall and the
     * gestures and the keys are two different things to learn. */
    static const char *PATCHING[] = {
        "DRAG jack to jack     patch a cable",
        "DRAG a plug off an    move that cable",
        "  input",
        "RIGHT-CLICK a jack    unplug it",
        "RIGHT-CLICK the rack  add a module",
        "RIGHT-CLICK a title   module menu",
        "DRAG a module title   move the panel",
        "DRAG / WHEEL a knob   turn it",
        "SHIFT-DRAG a knob     turn it slowly",
        "DOUBLE-CLICK a knob   back to default",
        "DRAG empty rack       pan",
        "WHEEL on the rack     zoom"
    };
    static const char *KEYS[] = {
        "Z S X D C V G B ...   lower octave",
        "Q 2 W 3 E R 5 T ...   upper octave",
        "[  ]                  shift octave",
        "SPACE                 sustain",
        "ESC                   all notes off",
        "F1                    this window",
        "RACKS                 the factory patches",
        "raylib - zlib licence",
        "Terminus TTF - SIL Open Font Licence",
        "",
        "github.com/bropple/BENCsynth"
    };
    const int np = (int)(sizeof PATCHING / sizeof PATCHING[0]);
    const int nk = (int)(sizeof KEYS / sizeof KEYS[0]);
    const float colW = (pw - 60.0f) * 0.5f;

    bs_text_spaced(ui, BS_F_TINY, "PATCHING", p.x + 24.0f, y, BS_ACCENT);
    bs_text_spaced(ui, BS_F_TINY, "PLAYING", p.x + 36.0f + colW, y, BS_ACCENT);
    y += 16.0f;

    for (int i = 0; i < np; i++)
        bs_text(ui, BS_F_TINY, PATCHING[i], p.x + 24.0f, y + (float)i * 14.0f, BS_DIM);
    for (int i = 0; i < nk; i++)
        bs_text(ui, BS_F_TINY, KEYS[i], p.x + 36.0f + colW, y + (float)i * 14.0f,
                i >= nk - 4 ? BS_EDGE : BS_DIM);

    bs_text_spaced(ui, BS_F_TINY, "F1, or click outside, to close",
                   p.x + pw - 222.0f, p.y + ph - 22.0f, BS_EDGE);

    /* Released rather than pressed: the click that opened the window is still
     * going on when this runs on the same frame, and reacting to the press
     * would shut it in the act of opening. And only outside the panel - a
     * click on the text you are trying to read is not a request to dismiss
     * it. */
    return IsMouseButtonReleased(MOUSE_BUTTON_LEFT) &&
           !CheckCollisionPointRec(GetMousePosition(), p);
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
        bs_rack_patch_replaced(rack);
        app->statusAge = 0.0f;
    }
    x += 76.0f;

    DrawRectangle((int)x, (int)y + 3, 1, (int)h - 6, BS_BORDER);
    x += 9.0f;

    b = (Rectangle){ x, y, 74.0f, h };
    if (bs_button(ui, 8006, b, "RACKS", 1)) {
        bs_keyboard_release_all(kb, eng);
        bs_rack_preset_menu(rack, ui, (Vector2){ b.x, b.y + h });
    }
    x += 80.0f;

    b = (Rectangle){ x, y, 62.0f, h };
    if (bs_button(ui, 8007, b, "CLEAR", 1)) {
        bs_keyboard_release_all(kb, eng);
        eng->clear();
        bs_rack_patch_replaced(rack);
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
    const char *iconDir = 0;
    const char *startRack = 0;
    int openInfo = 0;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--shot") == 0 && i + 1 < argc) shot = argv[++i];
        else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            shotFrames = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--info") == 0) openInfo = 1;
        else if (std::strcmp(argv[i], "--icons") == 0 && i + 1 < argc)
            iconDir = argv[++i];
        else if (std::strcmp(argv[i], "--rack") == 0 && i + 1 < argc)
            startRack = argv[++i];
        else loadPath = argv[i];
    }

    /* Writing the icon files needs no window and no GL context, so it happens
     * before either exists. `tools/make-icons.sh` calls this and then packs
     * the results into a .ico; the .icns is assembled from the same PNGs by
     * the macOS bundle script. */
    if (iconDir) {
        static const int SIZES[] = { 16, 24, 32, 48, 64, 128, 256, 512 };
        int failed = 0;
        for (int i = 0; i < (int)(sizeof SIZES / sizeof SIZES[0]); i++) {
            Image img = bs_star_image(SIZES[i]);
            char path[512];
            std::snprintf(path, sizeof path, "%s/star-%d.png", iconDir, SIZES[i]);
            if (!ExportImage(img, path)) {
                std::fprintf(stderr, "cannot write %s\n", path);
                failed = 1;
            }
            UnloadImage(img);
        }
        return failed;
    }

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    InitWindow(WIN_W, WIN_H, "BENCsynth");
    SetWindowMinSize(WIN_MIN_W, WIN_MIN_H);
    SetTargetFPS(60);
    /* Escape is a panic button here, not a way out. A synthesizer that
     * vanishes when you reach for the key that silences it is a synthesizer
     * you learn not to trust. */
    SetExitKey(KEY_NULL);

    /* The window icon, on the platforms where a program can set its own.
     * Windows takes it from a named resource compiled into the executable
     * instead - see src/gui/bencsynth.rc - and macOS takes it from the .app
     * bundle, because GLFW's Cocoa backend ignores this call entirely: a bare
     * Mach-O executable has no Dock identity to hang an icon on.
     *
     * Several sizes rather than one, so the titlebar and the taskbar each pick
     * a rasterisation made for them rather than scaling a single image. */
    {
        static const int SIZES[] = { 16, 24, 32, 48, 64, 128 };
        const int n = (int)(sizeof SIZES / sizeof SIZES[0]);
        Image icons[6];
        for (int i = 0; i < n; i++) icons[i] = bs_star_image(SIZES[i]);
        SetWindowIcons(icons, n);
        for (int i = 0; i < n; i++) UnloadImage(icons[i]);
    }

    bs_ui ui;
    bs_ui_init(&ui);
    load_logo();

    bs_rack rack;
    bs_rack_init(&rack);

    bs_keyboard kb;
    bs_keyboard_init(&kb);

    bs_app app;
    std::memset(&app, 0, sizeof app);
    app.slot = 1;
    app.about = openInfo;

    g_engine.init((float)SAMPLE_RATE);
    if (loadPath) {
        if (!bs_patch_load(&g_engine, loadPath, app.status, (int)sizeof app.status))
            g_engine.buildDefaultPatch();
        bs_rack_patch_replaced(&rack);
    } else if (startRack) {
        /* By name or by index, the same spelling the offline renderer takes. */
        int which = -1;
        for (int i = 0; i < bs::rackPresetCount(); i++)
            if (TextIsEqual(bs::rackPresetAt(i)->name, startRack)) which = i;
        if (which < 0) which = std::atoi(startRack);
        if (which < 0 || which >= bs::rackPresetCount()) which = 0;
        g_engine.buildPreset(which);
        say(&app, bs::rackPresetAt(which)->blurb);
    } else {
        g_engine.buildDefaultPatch();
        say(&app, "default rack - press Z, or click the keys, or try RACKS");
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

        /* A mouse release is true for the whole frame it happens on, and the
         * information window is opened by one - so without this the same
         * release that opens it reaches the dismiss test further down, finds
         * the pointer outside the panel (it is on the button in the header),
         * and shuts it again before anything is drawn. The window appeared for
         * a single frame and looked like it was refusing to open. */
        int justOpened = 0;
        if (draw_header(&ui, header, &g_engine, app.about)) {
            app.about = !app.about;
            justOpened = app.about;
        }

        ui.suppress = app.about;
        draw_toolbar(&app, &ui, &rack, &kb, &g_engine, toolbar, rview);
        bs_rack_frame(&rack, &ui, &g_engine, rview, dt);
        bs_keyboard_frame(&kb, &ui, &g_engine, keys);
        ui.suppress = 0;

        if (app.about) {
            Rectangle screen = { 0, 0, W, H };
            if (draw_info(&ui, screen) && !justOpened) app.about = 0;
        }

        if (bs_rack_menu(&rack, &ui, &g_engine) && rack.presetLoaded >= 0) {
            const bs::RackPreset *rp = bs::rackPresetAt(rack.presetLoaded);
            if (rp) {
                char line[160];
                std::snprintf(line, sizeof line, "%s - %s", rp->name, rp->blurb);
                say(&app, line);
            }
        }
        bs_ui_overlay(&ui);

        EndDrawing();

        if (shot && ++frame >= shotFrames) {
            TakeScreenshot(shot);
            break;
        }
    }

    bs_keyboard_release_all(&kb, &g_engine);
    if (g_logo.id != 0) UnloadTexture(g_logo);
    UnloadAudioStream(stream);
    CloseAudioDevice();
    bs_ui_free(&ui);
    CloseWindow();
    return 0;
}
