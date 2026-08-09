/*
 * BENCsynth - the in-window file browser. See bs_browser.h for why it exists.
 */

#include "bs_browser.h"
#include "bs_input.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Entry {
    std::string name;
    bool        dir;
};

struct Browser {
    bool                active;
    std::string         dir;
    std::string         ext;      /* "" for everything */
    std::string         title;
    std::vector<Entry>  entries;
    int                 sel;
    int                 top;      /* first visible row */
    bool                saving;   /* a name to type, and OPEN becomes SAVE */
    std::string         name;
    bs_edit             nameEdit;
    double              lastClickAt;
    int                 lastClickRow;

    Browser() : active(false), sel(-1), top(0), saving(false),
                lastClickAt(-1.0), lastClickRow(-1)
    { std::memset(&nameEdit, 0, sizeof nameEdit); }
};

Browser g;

/* raylib hands back full paths already, so this is only needed for the name
 * typed into the save field. Forward slashes: Windows takes them everywhere it
 * takes backslashes, and raylib's own path helpers emit them. */
std::string join(const std::string &dir, const std::string &name)
{
    if (dir.empty()) return name;
    const char last = dir[dir.size() - 1];
    if (last == '/' || last == '\\') return dir + name;
    return dir + "/" + name;
}

bool matches(const std::string &name, const std::string &ext)
{
    if (ext.empty()) return true;
    return IsFileExtension(name.c_str(), ("." + ext).c_str());
}

void scan()
{
    g.entries.clear();
    g.sel = -1;
    g.top = 0;

    /* raylib's, rather than dirent and FindFirstFile behind an #if. It is
     * already linked, it is already cross-platform, and doing it by hand meant
     * including windows.h into a file that also includes raylib - which
     * redefines Rectangle, CloseWindow and ShowCursor and will not compile.
     *
     * Everything, then filtered here. LoadDirectoryFilesEx takes an extension
     * filter and a "DIR" token to keep directories, but the two together want
     * a spelling this got wrong first time and the list came back empty.
     * Asking for everything and deciding here needs no guessing. */
    FilePathList files = LoadDirectoryFiles(g.dir.c_str());
    for (unsigned i = 0; i < files.count; i++) {
        const char *full = files.paths[i];
        if (!full || !*full) continue;
        const char *name = GetFileName(full);
        if (!name || !*name) continue;
        /* Hidden files stay hidden. Somebody who wants one knows where it is
         * and can put the path in the rack file. */
        if (name[0] == '.') continue;
        Entry e;
        e.name = name;
        e.dir  = !IsPathFile(full);
        if (!e.dir && !matches(e.name, g.ext)) continue;
        g.entries.push_back(e);
    }
    UnloadDirectoryFiles(files);

    /* Directories first, then names, so the list does not reshuffle itself
     * between one visit and the next. */
    std::sort(g.entries.begin(), g.entries.end(),
              [](const Entry &a, const Entry &b) {
                  if (a.dir != b.dir) return a.dir;
                  return a.name < b.name;
              });
}

const char *homeDir()
{
    const char *h = getenv("HOME");
    if (!h || !*h) h = getenv("USERPROFILE");
    if (!h || !*h) h = GetWorkingDirectory();
    return h ? h : ".";
}

}  /* namespace */

void bs_browser_open(const char *startDir, const char *ext, const char *title)
{
    g.active = true;
    g.ext    = ext ? ext : "";
    g.title  = title ? title : "Open";
    g.dir    = startDir && *startDir ? startDir : homeDir();
    if (!DirectoryExists(g.dir.c_str())) g.dir = homeDir();
    g.saving = false;
    g.name.clear();
    g.lastClickAt = -1.0;
    g.lastClickRow = -1;
    scan();
}

void bs_browser_save(const char *startDir, const char *ext, const char *title,
                     const char *suggest)
{
    bs_browser_open(startDir, ext, title);
    g.saving = true;
    g.name = suggest ? suggest : "";
    std::memset(&g.nameEdit, 0, sizeof g.nameEdit);
    g.nameEdit.caret = g.nameEdit.sel = (int)g.name.size();
}

int bs_browser_active(void) { return g.active ? 1 : 0; }

int bs_browser_frame(bs_ui *ui, Rectangle screen, char *out, int cap)
{
    if (!g.active) return 0;

    /* Not suppress: bs_button honours it, so setting it here would block this
     * panel's own buttons. main() sets it before the rack draws, which is
     * where blocking the rack has to happen anyway. */
    ui->suppress = 0;

    const float w = screen.width  < 760.0f ? screen.width  - 40.0f : 720.0f;
    const float h = screen.height < 560.0f ? screen.height - 40.0f : 520.0f;
    const Rectangle box = { (screen.width - w) * 0.5f,
                            (screen.height - h) * 0.5f, w, h };

    DrawRectangleRec(screen, (Color){ 0, 0, 0, 170 });
    bs_panel(box, BS_PANEL, BS_BORDER);

    const float pad = 12.0f;
    bs_text(ui, BS_F_SMALL, g.title.c_str(), box.x + pad, box.y + 10.0f, BS_TEXT);

    /* The path, trimmed from the left - the end of it is the part that says
     * where you are. */
    std::string shown = g.dir;
    while (bs_measure(ui, BS_F_TINY, shown.c_str(), 1.0f) > w - pad * 2 &&
           shown.size() > 4)
        shown = "..." + shown.substr(4);
    bs_text(ui, BS_F_TINY, shown.c_str(), box.x + pad, box.y + 32.0f, BS_DIM);

    const float rowH   = 22.0f;
    const float listY  = box.y + 52.0f;
    const float listH  = h - 52.0f - 46.0f - (g.saving ? 34.0f : 0.0f);
    const int   rows   = (int)(listH / rowH);
    const Rectangle list = { box.x + pad, listY, w - pad * 2, listH };
    bs_panel(list, BS_PANEL_HI, BS_BORDER);

    /* ".." is row -1, always available and always at the top. */
    const int total = (int)g.entries.size() + 1;
    if (g.top > total - rows) g.top = total - rows;
    if (g.top < 0) g.top = 0;

    const Vector2 m = GetMousePosition();
    const bool overList = CheckCollisionPointRec(m, list);
    const float wheel = GetMouseWheelMove();
    if (overList && wheel != 0.0f) {
        g.top -= (int)(wheel * 3.0f);
        if (g.top > total - rows) g.top = total - rows;
        if (g.top < 0) g.top = 0;
    }

    int chosenRow = -2;                 /* -2 nothing, -1 is ".." */
    for (int i = 0; i < rows; i++) {
        const int idx = g.top + i;
        if (idx >= total) break;
        const Rectangle r = { list.x + 1.0f, listY + (float)i * rowH,
                              list.width - 2.0f, rowH };
        const bool hot = CheckCollisionPointRec(m, r);
        const bool sel = (idx - 1) == g.sel && idx > 0;
        if (sel)      DrawRectangleRec(r, BS_EDGE);
        else if (hot) DrawRectangleRec(r, BS_PANEL);

        const char *label;
        Color col;
        if (idx == 0) { label = ".."; col = BS_DIM; }
        else {
            const Entry &e = g.entries[(size_t)(idx - 1)];
            label = e.name.c_str();
            col = e.dir ? BS_TEXT : BS_DIM;
        }
        char line[320];
        if (idx > 0 && g.entries[(size_t)(idx - 1)].dir)
            std::snprintf(line, sizeof line, "%s/", label);
        else
            std::snprintf(line, sizeof line, "%s", label);
        bs_text(ui, BS_F_TINY, line, r.x + 8.0f, r.y + 5.0f, col);

        if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            const double now = GetTime();
            const bool dbl = (g.lastClickRow == idx) && (now - g.lastClickAt < 0.4);
            g.lastClickAt = now;
            g.lastClickRow = idx;
            if (idx == 0) { if (dbl) chosenRow = -1; else g.sel = -1; }
            else {
                g.sel = idx - 1;
                /* Saving: a click on a file offers to overwrite it, which is
                 * what putting its name in the field means. */
                if (g.saving && !g.entries[(size_t)(idx - 1)].dir) {
                    g.name = g.entries[(size_t)(idx - 1)].name;
                    g.nameEdit.caret = g.nameEdit.sel = (int)g.name.size();
                }
                if (dbl) chosenRow = idx - 1;
            }
        }
    }

    /* A thumb, only when there is more than fits. */
    if (total > rows) {
        const float frac = (float)rows / (float)total;
        const float th = list.height * frac;
        const float ty = list.y + list.height * ((float)g.top / (float)total);
        DrawRectangleRec((Rectangle){ list.x + list.width - 5.0f, ty, 4.0f, th },
                         BS_BORDER);
    }

    if (g.saving) {
        const Rectangle nr = { list.x, list.y + list.height + 6.0f,
                               list.width, 24.0f };
        bs_text(ui, BS_F_TINY, "NAME", nr.x, nr.y - 12.0f, BS_DIM);
        bs_textarea(ui, 9110, nr, g.name, &g.nameEdit, 1);
    }

    /* Buttons. */
    const float by = box.y + h - 36.0f;
    int result = 0;
    if (bs_button(ui, 9101, (Rectangle){ box.x + pad, by, 70.0f, 26.0f }, "UP", 1))
        chosenRow = -1;

    const bool canOpen = g.sel >= 0 && g.sel < (int)g.entries.size();
    const bool intoDir = canOpen && g.entries[(size_t)g.sel].dir;
    if (g.saving) {
        /* Entering a directory still has to be possible while saving, so the
         * button says which of the two it is about to do. */
        std::string trimmed = g.name;
        while (!trimmed.empty() && trimmed[0] == ' ') trimmed.erase(0, 1);
        while (!trimmed.empty() && trimmed[trimmed.size() - 1] == ' ')
            trimmed.erase(trimmed.size() - 1);
        const bool canSave = !trimmed.empty();
        if (bs_button(ui, 9102,
                      (Rectangle){ box.x + w - pad - 90.0f, by, 90.0f, 26.0f },
                      intoDir ? "ENTER" : "SAVE",
                      (intoDir || canSave) ? 1 : 0)) {
            if (intoDir) chosenRow = g.sel;
            else if (canSave) {
                if (!g.ext.empty() && !matches(trimmed, g.ext))
                    trimmed += "." + g.ext;
                std::snprintf(out, (size_t)cap, "%s",
                              join(g.dir, trimmed).c_str());
                g.active = false;
                return 1;
            }
        }
    } else if (bs_button(ui, 9102,
                  (Rectangle){ box.x + w - pad - 90.0f, by, 90.0f, 26.0f },
                  intoDir ? "ENTER" : "OPEN",
                  canOpen ? 1 : 0) && canOpen)
        chosenRow = g.sel;

    if (bs_button(ui, 9103,
                  (Rectangle){ box.x + w - pad - 190.0f, by, 90.0f, 26.0f },
                  "CANCEL", 1) ||
        IsKeyPressed(KEY_ESCAPE)) {
        g.active = false;
        return -1;
    }

    if (chosenRow == -1) {
        g.dir = GetPrevDirectoryPath(g.dir.c_str());
        scan();
        return 0;
    }
    if (chosenRow >= 0 && chosenRow < (int)g.entries.size()) {
        const Entry &e = g.entries[(size_t)chosenRow];
        const std::string full = join(g.dir, e.name);
        if (e.dir) {
            g.dir = full;
            scan();
            return 0;
        }
        std::snprintf(out, (size_t)cap, "%s", full.c_str());
        g.active = false;
        result = 1;
    }
    return result;
}
