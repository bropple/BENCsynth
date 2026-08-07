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
#include "bs_shm.h"
#include "bs_embed.h"
#include "bs_log.h"
#include "bs_sync.h"
#include "bs_filedlg.h"
#include "bs_engine.h"
#include "bs_modules.h"
#include "bs_version.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include "bs_input.h"   /* must come after raylib.h - see the header */

enum {
    /* One size for every buffer that holds a path, so that copying one into
     * another cannot lose the end of it. */
    BS_PATH = 2048,

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

/* Negative / zero mean "this process is the whole program" - see the status
 * line below for why an editor must not report its own. */
static float g_hostLoad = -1.0f;
static float g_hostRate = 0.0f;

/* Returns 1 for the information button, 2 for the keyboard toggle. */
static int draw_header(bs_ui *ui, Rectangle r, const bs::Engine *eng,
                       int infoOpen, int keysOn)
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

    /* The keyboard toggle sits inside it, so the corner still belongs to the
     * information button and nothing moves when this one appears. */
    Rectangle keysBtn = { info.x - 32.0f, info.y, 28.0f, 24.0f };
    const int keysHit = bs_keys_button(ui, keysBtn, keysOn);

    /* Sounding voices rather than allocated ones: the number that means
     * anything while playing is how many are audible. Read through the
     * published counters - the voice array belongs to the audio thread. */
    char buf[128];
    /* While editing a plugin the interesting numbers belong to the other
     * process: this one renders only to keep its own scopes moving, so its
     * load is meaningless and its sample rate is a compiled-in guess. */
    std::snprintf(buf, sizeof buf, "VOICES %d/%d    LOAD %2.0f%%    %d Hz",
                  eng->voicesSounding(), eng->voicesAllocated(),
                  (double)((g_hostLoad >= 0.0f ? g_hostLoad : eng->load) * 100.0f),
                  (int)(g_hostRate > 0.0f ? g_hostRate : (float)SAMPLE_RATE));
    const float w = bs_measure(ui, BS_F_SMALL, buf, 1.0f);
    bs_text_spaced(ui, BS_F_SMALL, buf, keysBtn.x - w - 16.0f,
                   r.y + (r.height - BS_F_SMALL) * 0.5f, BS_DIM);

    return keysHit ? 2 : (hit ? 1 : 0);
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
        "RIGHT-CLICK a title   unpatch/reset/remove",
        "DELETE over a module  remove it",
        "DRAG a module title   move the panel",
        "DRAG / WHEEL a knob   turn it",
        "SHIFT-DRAG a knob     turn it slowly",
        "DOUBLE-CLICK a knob   back to default",
        "DRAG empty rack       pan",
        "WHEEL on the rack     zoom",
        "CLICK a NOTES panel   type in it; ESC leaves"
    };
    static const char *KEYS[] = {
        "Z S X D C V G B ...   lower octave",
        "Q 2 W 3 E R 5 T ...   upper octave",
        "[  ]                  shift octave",
        "SPACE                 sustain",
        "ESC                   all notes off",
        "F1                    this window",
        "CTRL-S / CTRL-O       save / open a rack",
        "CTRL-SHIFT-S          save as",
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
    /* The file this rack came from or was last written to. Empty until it has
     * one, which is what makes SAVE ask the first time and not after. */
    char  path[BS_PATH];
    char  status[192];
    float statusAge;
    int   about;
    /* The keyboard at the bottom. On by default in the program and off inside
     * a host, where the DAW already has one and the rack is the part you came
     * for - but only defaulted, not removed, because clicking a key to hear
     * what a patch does is worth keeping either way. */
    int   showKeys;
} bs_app;

/* The name shown in the title bar and on the SAVE button's tooltip-in-spirit.
 * Everything after the last separator, or a reminder that there is not one. */
static const char *app_name(const bs_app *app)
{
    if (!app->path[0]) return "untitled";
    const char *a = std::strrchr(app->path, '/');
    const char *b = std::strrchr(app->path, '\\');
    const char *s = a > b ? a : b;
    return s ? s + 1 : app->path;
}

static void app_retitle(const bs_app *app)
{
    /* Generous, because a path can be as long as the platform allows and the
     * compiler is right that a smaller one would silently lose the end of it. */
    char t[BS_PATH + 64];
    std::snprintf(t, sizeof t, "BENCsynth - %s", app_name(app));
    SetWindowTitle(t);
}

static void say(bs_app *app, const char *msg)
{
    std::snprintf(app->status, sizeof app->status, "%s", msg);
    app->statusAge = 0.0f;
}

/* Where the dialogs start, and where a rack goes when there is no dialog to
 * ask with. Beside the program rather than in a home directory: this is a
 * portable folder someone unzipped, and the racks belong with it. */
static const char *patch_dir(void)
{
    static char dir[BS_PATH];
    if (!dir[0]) std::snprintf(dir, sizeof dir, "%spatches", GetApplicationDirectory());
    return dir;
}

static void app_save(bs_app *app, bs::Engine *eng, int askForName)
{
    char chosen[BS_PATH];

    if (askForName || !app->path[0]) {
        char suggest[BS_PATH + 64];
        std::snprintf(suggest, sizeof suggest, "%s.%s",
                      app->path[0] ? app_name(app) : "rack", BS_PATCH_EXT);

        const int r = bs_save_dialog(GetWindowHandle(), "Save rack", suggest,
                                     BS_PATCH_DESC, BS_PATCH_EXT,
                                     chosen, sizeof chosen);
        if (r == BS_DLG_CANCELLED) return;
        if (r == BS_DLG_UNAVAILABLE) {
            /* No zenity, no kdialog. Rather than refuse, put it somewhere
             * findable and say exactly where - a machine without a file dialog
             * is not a machine that should be unable to save. */
            /* Assembled and then copied with a length rather than printed:
             * snprintf truncating a path is correct here, but there is no way
             * to say so that -Wformat-truncation believes, and a warning
             * everyone learns to scroll past is worse than three lines. */
            std::string fallback = std::string(patch_dir()) + "/rack." BS_PATCH_EXT;
            if (fallback.size() >= sizeof chosen) fallback.resize(sizeof chosen - 1);
            std::memcpy(chosen, fallback.c_str(), fallback.size() + 1);
        }
        std::snprintf(app->path, sizeof app->path, "%s", chosen);
    }

    /* The dialogs do not all add the extension, and a rack called `bass` that
     * the open dialog then filters out of view is worse than a rack called
     * `bass.bencsynth`. */
    const size_t n = std::strlen(app->path);
    const size_t e = std::strlen(BS_PATCH_EXT) + 1;
    if (n < e || std::strcmp(app->path + n - e, "." BS_PATCH_EXT) != 0) {
        std::snprintf(app->path + n, sizeof app->path - n, ".%s", BS_PATCH_EXT);
    }

    bs_patch_save(eng, app->path, app->status, (int)sizeof app->status);
    app->statusAge = 0.0f;
    app_retitle(app);
}

static void app_open(bs_app *app, bs::Engine *eng, bs_rack *rack, bs_keyboard *kb)
{
    char chosen[BS_PATH];
    const int r = bs_open_dialog(GetWindowHandle(), "Open rack", patch_dir(),
                                 BS_PATCH_DESC, BS_PATCH_EXT,
                                 chosen, sizeof chosen);
    if (r != BS_DLG_OK) {
        if (r == BS_DLG_UNAVAILABLE)
            std::snprintf(app->status, sizeof app->status,
                          "no file dialog here - install zenity or kdialog, or "
                          "pass a rack on the command line");
        app->statusAge = 0.0f;
        return;
    }

    bs_keyboard_release_all(kb, eng);
    if (bs_patch_load(eng, chosen, app->status, (int)sizeof app->status))
        std::snprintf(app->path, sizeof app->path, "%s", chosen);
    bs_rack_patch_replaced(rack);
    bs_rack_restore_view(rack, eng->patch);
    app->statusAge = 0.0f;
    app_retitle(app);
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

    b = (Rectangle){ x, y, 62.0f, h };
    if (bs_button(ui, 8002, b, "OPEN", 1)) app_open(app, eng, rack, kb);
    x += 68.0f;

    b = (Rectangle){ x, y, 62.0f, h };
    if (bs_button(ui, 8003, b, "SAVE", 1)) app_save(app, eng, 0);
    x += 68.0f;

    b = (Rectangle){ x, y, 78.0f, h };
    if (bs_button(ui, 8004, b, "SAVE AS", 1)) app_save(app, eng, 1);
    x += 92.0f;

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
    /* Set when this process is a plugin's editor rather than the program: the
     * rack lives in shared memory, the plugin makes the sound, and this window
     * is the only part of BENCsynth a DAW cannot draw for itself. */
    const char *editorShm = 0;
    /* No window of our own: the frames go to the plugin, which draws them
     * inside the host's. Told rather than discovered - see bs_shm_spawn_editor. */
    int offscreen = 0;
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
        else if (std::strcmp(argv[i], "--editor") == 0 && i + 1 < argc)
            editorShm = argv[++i];
        else if (std::strcmp(argv[i], "--offscreen") == 0) offscreen = 1;
        else if (argv[i][0] == '-' && argv[i][1] == '-') {
            /* Not a file. Treating it as one is how an editor that predates a
             * flag ends up opening a window instead of refusing to start,
             * which is a great deal harder to diagnose than this line. */
            std::fprintf(stderr, "bencsynth: unknown option %s\n", argv[i]);
            return 2;
        }
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

    /* Embedded in a host's window: no decorations of our own, because the
     * host draws the frame and a title bar inside an FX rack looks like a bug.
     * The handle is read before the window exists so the flag can be set. */
    unsigned long long embedParent = 0;
    if (editorShm) {
        bs::ShmMap peek;
        if (bs::bs_shm_open(&peek, editorShm)) {
            embedParent = peek.block->embedParent.load();
            bs::bs_shm_close(&peek);
        }
    }

    /* Three ways this window can exist:
     *
     *   the program        a normal window
     *   embedded (Win/X11) undecorated, reparented into the host's
     *   offscreen (macOS)  hidden, and the frames are shipped as pixels
     *
     * The third is not a preference. An NSView cannot be handed to another
     * process, so on macOS the editor draws into a texture and the plugin
     * displays it - see src/plugin/bs_cocoa.h. */
    const unsigned fbMode = (unsigned)(editorShm && offscreen);

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    if (embedParent) SetConfigFlags(FLAG_WINDOW_UNDECORATED);
    if (fbMode)      SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(WIN_W, WIN_H, "BENCsynth");
    /* Again, after the fact. The config flag is a hint given before the window
     * exists and a backend is free to ignore it; this one is an instruction to
     * a window that does. A stray top-level window here is not a cosmetic
     * problem - it is the rack appearing outside the host, clickable, and not
     * following it around. */
    if (fbMode) SetWindowState(FLAG_WINDOW_HIDDEN);
    if (editorShm)
        bs::bs_log("editor: attached to %s, offscreen=%d, built %s %s",
                   editorShm, (int)fbMode, __DATE__, __TIME__);
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
    app.about = openInfo;
    app.showKeys = 1;          /* lowered below when this is a plugin's editor */

    g_engine.init((float)SAMPLE_RATE);
    if (loadPath) {
        if (bs_patch_load(&g_engine, loadPath, app.status, (int)sizeof app.status))
            std::snprintf(app.path, sizeof app.path, "%s", loadPath);
        else
            g_engine.buildDefaultPatch();
        bs_rack_patch_replaced(&rack);
        bs_rack_restore_view(&rack, g_engine.patch);
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

    /* ---- editor mode ------------------------------------------------ *
     *
     * Attach to the plugin's shared block and take the rack from it. No audio
     * device: the plugin is already making the sound, and opening a second one
     * here would be a second synthesizer playing the same notes a few
     * milliseconds out of step with the first. */
    bs::ShmMap shm;
    bool     editing     = false;
    uint32_t seenHostSeq = 0;
    uint32_t editSeq     = 0;
    uint64_t lastSig     = 0;
    std::vector<float> lastParams;
    std::vector<char>  shmText;
    std::vector<float> flat;

    if (editorShm) {
        if (!bs::bs_shm_open(&shm, editorShm)) {
            std::fprintf(stderr, "BENCsynth: cannot attach to %s\n", editorShm);
            CloseWindow();
            return 1;
        }
        editing = true;
        app.showKeys = 0;      /* the host has a keyboard; the rack wants the room */
        /* Offscreen there is no window to receive a click, so the interface
         * reads the events the host forwarded instead. */
        if (fbMode) bs_input_attach(shm.block, 0, 0);
        shmText.resize(bs::BS_SHM_RACK_MAX);
        flat.resize(bs::BS_SHM_PARAM_MAX);
        say(&app, "editing a plugin - the host is making the sound");

        /* Become a child of the host's window. SetParent works across process
         * boundaries, which is the whole reason the editor can be a separate
         * process and still appear inside a DAW's FX rack. The handle crosses
         * as a void* because windows.h and raylib cannot share a file - see
         * src/plugin/bs_embed.h. */
        if (embedParent) {
            const uint32_t w = shm.block->wantW.load();
            const uint32_t h = shm.block->wantH.load();
            bs::bs_embed_window(GetWindowHandle(), embedParent,
                                (int)(w ? w : (uint32_t)WIN_W),
                                (int)(h ? h : (uint32_t)WIN_H));
        }
    }

    app_retitle(&app);

    AudioStream stream = { 0 };
    if (!editing) {
        InitAudioDevice();
        SetAudioStreamBufferSizeDefault(512);
        stream = LoadAudioStream(SAMPLE_RATE, 32, 2);
        SetAudioStreamCallback(stream, audio_cb);
        PlayAudioStream(stream);
        if (!IsAudioDeviceReady())
            say(&app, "no audio device - the rack still patches, it just cannot be heard");
    }

    /* A screenshot of an idle rack shows a dead meter and a flat scope, which
     * is the least informative picture of a synthesizer there is. */
    if (shot) {
        g_engine.noteOn(52, 0.9f);
        g_engine.noteOn(59, 0.85f);
        g_engine.noteOn(64, 0.8f);
        g_engine.noteOn(67, 0.75f);
    }

    /* Offscreen: everything is drawn into this and read back, rather than
     * being swapped to a window nobody can see. */
    RenderTexture2D fbTarget = { 0 };
    int fbW = 0, fbH = 0;
    std::vector<unsigned char> fbScratch;

    int frame = 0;
    while (!WindowShouldClose()) {
        /* Before anything asks. Turns the events that arrived since the last
         * frame into the held-and-edge state the interface expects. */
        bs_input_frame();
        /* ---- editor: take whatever the plugin changed ---------------- */
        if (editing) {
            bs::ShmBlock *b = shm.block;
            b->alive.fetch_add(1, std::memory_order_relaxed);
            if (b->quit.load(std::memory_order_acquire)) break;

            /* The host changed the rack under us - a preset was chosen from
             * the DAW's parameter list, or a project was loaded. Whatever is
             * on screen is stale. */
            uint32_t len = 0;
            const uint32_t hs = bs::bs_shm_read(&b->host, shmText.data(), &len);
            if (hs && hs != seenHostSeq && len) {
                char status[192] = "";
                if (bs_patch_from_string(&g_engine, shmText.data(), status,
                                         (int)sizeof status)) {
                    bs_rack_patch_replaced(&rack);
                    bs_rack_restore_view(&rack, g_engine.patch);
                    lastSig = 0;               /* republish, structure and all */
                    lastParams.clear();
                }
                seenHostSeq = hs;
            }
        }
        const float dt = GetFrameTime();
        const float W = (float)GetScreenWidth();
        const float H = (float)GetScreenHeight();

        app.statusAge += dt;
        ui.changed = 0;

        Rectangle header  = { 0, 0, W, (float)HEADER_H };
        Rectangle toolbar = { 0, (float)HEADER_H, W, (float)TOOLBAR_H };
        const float keysH = app.showKeys ? (float)KEYS_H : 0.0f;
        Rectangle keys    = { 0, H - keysH, W, keysH };
        Rectangle rview   = { 0, (float)(HEADER_H + TOOLBAR_H), W,
                              H - ((float)(HEADER_H + TOOLBAR_H) + keysH) };
        if (rview.height < 80.0f) rview.height = 80.0f;

        if (IsKeyPressed(KEY_F1)) app.about = !app.about;
        /* F2 for the keyboard, next to F1 for the help. It starts hidden
         * inside a host, and a shortcut is how you get it back without going
         * to the toolbar - the same route the toolbar button takes. */
        if (IsKeyPressed(KEY_F2) && ui.focus == 0) {
            bs_keyboard_release_all(&kb, &g_engine);
            app.showKeys = !app.showKeys;
            say(&app, app.showKeys ? "keyboard shown (F2)"
                                   : "keyboard hidden (F2) - the rack gets the room");
        }

        /* Ctrl-S and Ctrl-O, because everything else does. Not while a
         * scratchpad has the keyboard, where Ctrl-S is somebody's muscle
         * memory for nothing and Ctrl-O would open a dialog over their
         * typing. */
        if (ui.focus == 0) {
            const int ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL) ||
                             IsKeyDown(KEY_LEFT_SUPER)   || IsKeyDown(KEY_RIGHT_SUPER);
            const int shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
            if (ctrl && IsKeyPressed(KEY_S)) app_save(&app, &g_engine, shift);
            if (ctrl && IsKeyPressed(KEY_O)) app_open(&app, &g_engine, &rack, &kb);
        }
        if (IsKeyPressed(KEY_ESCAPE)) {
            /* While a scratchpad has the keyboard, Escape is how you get out
             * of it. Silencing the rack from inside a text field would be a
             * surprise, and there would be no other way to stop typing. */
            if (ui.focus) {
                ui.focus = 0;
            } else {
                bs_keyboard_release_all(&kb, &g_engine);
                g_engine.panic();
                say(&app, "all notes off");
            }
        }

        /* Typing is off while the help is up, so reading it does not play a
         * chord at whoever is reading - and off while a scratchpad has the
         * keyboard, or writing a note into one would play it as well. */
        bs_keyboard_typing(&kb, &g_engine, !app.about && ui.focus == 0);

        BeginDrawing();
        if (fbMode && fbTarget.id != 0) BeginTextureMode(fbTarget);
        ClearBackground(BS_BG);

        /* A mouse release is true for the whole frame it happens on, and the
         * information window is opened by one - so without this the same
         * release that opens it reaches the dismiss test further down, finds
         * the pointer outside the panel (it is on the button in the header),
         * and shuts it again before anything is drawn. The window appeared for
         * a single frame and looked like it was refusing to open. */
        int justOpened = 0;
        const int headerHit = draw_header(&ui, header, &g_engine, app.about,
                                          app.showKeys);
        if (headerHit == 1) {
            app.about = !app.about;
            justOpened = app.about;
        } else if (headerHit == 2) {
            bs_keyboard_release_all(&kb, &g_engine);
            app.showKeys = !app.showKeys;
            say(&app, app.showKeys ? "keyboard shown"
                                   : "keyboard hidden - the rack gets the room");
        }

        ui.suppress = app.about;
        draw_toolbar(&app, &ui, &rack, &kb, &g_engine, toolbar, rview);
        bs_rack_frame(&rack, &ui, &g_engine, rview, dt);
        if (app.showKeys) bs_keyboard_frame(&kb, &ui, &g_engine, keys);
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

        if (fbMode && fbTarget.id != 0) EndTextureMode();
        EndDrawing();

        /* Read the frame back and hand it over.
         *
         * raylib stores a render texture the way OpenGL does, bottom row
         * first, and it is RGBA. The surface on the other side wants BGRA with
         * the top row first. Both corrections happen here rather than there:
         * the plugin's copy runs on the host's main thread, where a per-pixel
         * loop over a million pixels is somebody's dropped audio buffer. */
        if (fbMode && fbTarget.id != 0 && editing) {
            Image img = LoadImageFromTexture(fbTarget.texture);
            if (img.data && img.format == PIXELFORMAT_UNCOMPRESSED_R8G8B8A8) {
                bs::ShmBlock *b = shm.block;
                const int w = img.width, h = img.height;
                const unsigned char *src = (const unsigned char *)img.data;

                const uint32_t sq = b->fbSeq.load(std::memory_order_relaxed);
                b->fbSeq.store(sq + 1, std::memory_order_release);
                std::atomic_thread_fence(std::memory_order_release);

                b->fbW = (uint32_t)w;
                b->fbH = (uint32_t)h;
                b->fbStride = (uint32_t)w * 4u;
                for (int y = 0; y < h; y++) {
                    const unsigned char *s = src + (size_t)(h - 1 - y) * (size_t)w * 4;
                    unsigned char *d = b->fb + (size_t)y * (size_t)w * 4;
                    for (int x = 0; x < w; x++) {
                        d[x * 4 + 0] = s[x * 4 + 2];   /* B */
                        d[x * 4 + 1] = s[x * 4 + 1];   /* G */
                        d[x * 4 + 2] = s[x * 4 + 0];   /* R */
                        d[x * 4 + 3] = 255;
                    }
                }

                std::atomic_thread_fence(std::memory_order_release);
                b->fbSeq.store(sq + 2, std::memory_order_release);
            }
            UnloadImage(img);
        }

        /* ---- editor: publish whatever changed ------------------------ *
         *
         * Two channels, because the two kinds of change cost different
         * amounts on the other end. A new module or a new cable means the
         * plugin has to rebuild, which allocates and cuts every sounding
         * voice - unavoidable, and rare. A knob move is a float, and must not
         * cost that: sending it structurally would make a filter sweep sound
         * like the patch being reloaded sixty times a second.
         *
         * The signature covers structure only. Dragging a module changes the
         * rack text but not the signature, so it travels as a snapshot and
         * never rebuilds anything. */
        if (editing) {
            bs::ShmBlock *b = shm.block;

            /* Notes played here go to the plugin, which is the thing with an
             * audio device. Draining is not optional: in editor mode nothing
             * calls render(), so anything left in this queue would sit there
             * until it filled and then be silently dropped forever. */
            /* Offscreen: the host's size arrives the same way, but there is
             * no window to resize - the render target is what has to match. */
            if (fbMode) {
                int w = (int)b->wantW.load(std::memory_order_acquire);
                int h = (int)b->wantH.load(std::memory_order_acquire);
                if (w < 320) w = 320;
                if (h < 240) h = 240;
                if (w > (int)bs::BS_SHM_FB_MAX_W) w = (int)bs::BS_SHM_FB_MAX_W;
                if (h > (int)bs::BS_SHM_FB_MAX_H) h = (int)bs::BS_SHM_FB_MAX_H;
                if (w != fbW || h != fbH) {
                    if (fbTarget.id != 0) UnloadRenderTexture(fbTarget);
                    fbTarget = LoadRenderTexture(w, h);
                    fbW = w; fbH = h;
                    fbScratch.assign((size_t)w * (size_t)h * 4, 0);
                    /* The interface lays itself out from the window size, so
                     * the hidden window has to agree with the target or the
                     * keyboard ends up off the bottom of the picture. */
                    SetWindowSize(w, h);
                }
            }

            /* The host owns the geometry when embedded, and it resizes its
             * own window, not ours - so the size arrives here instead. */
            if (embedParent) {
                const int w = (int)b->wantW.load(std::memory_order_acquire);
                const int h = (int)b->wantH.load(std::memory_order_acquire);
                if (w > 0 && h > 0 &&
                    (w != GetScreenWidth() || h != GetScreenHeight())) {
                    SetWindowSize(w, h);
                    /* And the window itself, which on X11 is a child of the
                     * host's and does not follow GLFW's idea of its size. */
                    bs::bs_embed_resize(GetWindowHandle(), w, h);
                }
            }

            bs::NoteEvent ne;
            while (g_engine.events.pop(&ne)) {
                g_engine.keys.apply(ne);      /* keep the on-screen keys honest */
                bs_shm_push_note(b, ne.kind, ne.note, ne.value);
            }

            /* What the DAW played. Applied to this engine so the on-screen
             * keyboard lights up and the rack has something going through it -
             * and deliberately not pushed back the other way, or a held note
             * would bounce between the processes forever. */
            bs::ShmNote hn;
            while (bs::bs_shm_pop_host_note(b, &hn)) {
                bs::NoteEvent he;
                he.kind   = hn.kind;
                he.note   = hn.note;
                he.value  = hn.value;
                he.offset = 0;
                g_engine.keys.apply(he);
            }

            g_hostLoad = b->load.load(std::memory_order_relaxed);
            g_hostRate = b->sampleRate.load(std::memory_order_relaxed);
            if (g_hostRate > 0.0f && g_engine.patch.sampleRate() != g_hostRate)
                g_engine.patch.setSampleRate(g_hostRate);

            /* Render, and throw the audio away.
             *
             * The scopes, meters and envelope displays are modules reading
             * their own inputs - there is no signal in this process unless
             * this process produces one, and a scope that never moves while
             * the DAW plays through the rack is worse than no scope. So the
             * editor runs the same graph with the same notes and discards the
             * result. The picture is this render, not the plugin's, so a
             * waveform can sit at a different phase than what you hear; the
             * shape and the level are right, which is what a scope is for.
             *
             * Paced by the frame clock, and clamped: a stalled window must not
             * come back and render a second of audio in one go. */
            {
                static float carry = 0.0f;
                carry += dt * g_engine.patch.sampleRate();
                int want = (int)carry;
                if (want > 8192) want = 8192;        /* ~170 ms, then give up */
                carry -= (float)want;
                float scratch[2 * 256];
                while (want > 0) {
                    const int n = want > 256 ? 256 : want;
                    g_engine.render(scratch, n);
                    want -= n;
                }
            }

            const uint64_t sig = bs::bs_structure_signature(&g_engine);
            const uint32_t n   = bs::bs_flatten_params(&g_engine, flat.data(),
                                                       (uint32_t)flat.size());

            if (sig != lastSig) {
                const std::string t = bs_patch_to_string(&g_engine);
                bs::bs_shm_write(&b->edit, t.c_str(), (uint32_t)t.size());
                bs::bs_shm_write(&b->snap, t.c_str(), (uint32_t)t.size());
                editSeq = b->edit.seq.load(std::memory_order_acquire);
                lastSig = sig;

                /* Values are published against the structure they belong to,
                 * or the plugin would write them into a rack that has not
                 * been rebuilt yet and scatter them across the wrong knobs. */
                std::memcpy(b->params, flat.data(), n * sizeof(float));
                b->paramCount = n;
                b->paramStructSeq.store(editSeq, std::memory_order_release);
                b->paramSeq.fetch_add(1, std::memory_order_release);
                lastParams.assign(flat.begin(), flat.begin() + n);
            } else {
                bool moved = lastParams.size() != n;
                for (uint32_t i = 0; !moved && i < n; i++)
                    moved = lastParams[i] != flat[i];
                if (moved) {
                    std::memcpy(b->params, flat.data(), n * sizeof(float));
                    b->paramCount = n;
                    b->paramStructSeq.store(editSeq, std::memory_order_release);
                    b->paramSeq.fetch_add(1, std::memory_order_release);
                    lastParams.assign(flat.begin(), flat.begin() + n);

                    /* The host saves the snapshot, so knob positions have to
                     * reach it too - but only the plugin's *save* reads this,
                     * never its audio path, so it is free. */
                    const std::string t = bs_patch_to_string(&g_engine);
                    bs::bs_shm_write(&b->snap, t.c_str(), (uint32_t)t.size());
                }
            }
        }

        if (shot && ++frame >= shotFrames) {
            TakeScreenshot(shot);
            break;
        }
    }

    bs_keyboard_release_all(&kb, &g_engine);
    if (g_logo.id != 0) UnloadTexture(g_logo);
    if (editing) {
        /* One last full publish, so a window closed with unsaved knob
         * positions still hands them over before the block goes away. */
        const std::string t = bs_patch_to_string(&g_engine);
        bs::bs_shm_write(&shm.block->snap, t.c_str(), (uint32_t)t.size());
        bs::bs_shm_close(&shm);
    } else {
        UnloadAudioStream(stream);
        CloseAudioDevice();
    }
    bs_ui_free(&ui);
    CloseWindow();
    return 0;
}
