/*
 * BENCsynth - the user's own racks. See bs_racklib.h for where they live.
 */

#include "bs_racklib.h"
#include "bs_patchfile.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#  include <direct.h>
#  define BS_SEP '\\'
#else
#  include <dirent.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  define BS_SEP '/'
#endif

namespace bs {

static std::vector<UserRack> g_racks;

/* mkdir -p, for the two or three components these paths have. */
static void makeDirs(const std::string &path)
{
    for (size_t i = 1; i <= path.size(); i++) {
        if (i != path.size() && path[i] != '/' && path[i] != '\\') continue;
        const std::string part = path.substr(0, i);
#if defined(_WIN32)
        _mkdir(part.c_str());
#else
        mkdir(part.c_str(), 0755);
#endif
    }
}

const char *userRackDir(bool create)
{
    static std::string dir;
    static bool made = false;

    if (dir.empty()) {
        if (const char *env = std::getenv("BENCSYNTH_RACKS")) {
            dir = env;
        } else {
#if defined(_WIN32)
            const char *base = std::getenv("APPDATA");
            if (base) dir = std::string(base) + "\\BENCsynth\\racks";
#elif defined(__APPLE__)
            const char *home = std::getenv("HOME");
            if (home) dir = std::string(home) +
                            "/Library/Application Support/BENCsynth/racks";
#else
            /* The XDG default is ~/.local/share, and honouring the variable
             * costs one getenv. */
            if (const char *xdg = std::getenv("XDG_DATA_HOME"))
                dir = std::string(xdg) + "/bencsynth/racks";
            else if (const char *home = std::getenv("HOME"))
                dir = std::string(home) + "/.local/share/bencsynth/racks";
#endif
        }
    }
    if (create && !made && !dir.empty()) { makeDirs(dir); made = true; }
    return dir.c_str();
}

/* Everything in one directory that ends in the patch extension. */
static void scanOne(const char *dirPath, std::vector<UserRack> &out)
{
    if (!dirPath || !*dirPath) return;

    const std::string ext = std::string(".") + BS_PATCH_EXT;

#if defined(_WIN32)
    WIN32_FIND_DATAA fd;
    const std::string glob = std::string(dirPath) + "\\*" + ext;
    HANDLE h = FindFirstFileA(glob.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string name = fd.cFileName;
#else
    DIR *d = opendir(dirPath);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        std::string name = e->d_name;
        if (name.size() <= ext.size()) continue;
        if (name.compare(name.size() - ext.size(), ext.size(), ext) != 0) continue;
#endif
        UserRack r;
        r.name = name.substr(0, name.size() - ext.size());
        r.path = std::string(dirPath) + BS_SEP + name;
        if (!r.name.empty()) out.push_back(r);
#if defined(_WIN32)
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    }
    closedir(d);
#endif
}

int scanUserRacks(const char *extra)
{
    g_racks.clear();
    scanOne(userRackDir(false), g_racks);
    scanOne(extra, g_racks);

    /* Sorted, so a host's list is the same on the next run. Then deduped by
     * name with the first winning, which makes the per-user folder shadow a
     * portable one rather than the list showing the rack twice. */
    std::stable_sort(g_racks.begin(), g_racks.end(),
                     [](const UserRack &a, const UserRack &b) {
                         return a.name < b.name;
                     });
    g_racks.erase(std::unique(g_racks.begin(), g_racks.end(),
                              [](const UserRack &a, const UserRack &b) {
                                  return a.name == b.name;
                              }),
                  g_racks.end());
    return (int)g_racks.size();
}

int userRackCount() { return (int)g_racks.size(); }

const UserRack *userRackAt(int i)
{
    return (i >= 0 && i < (int)g_racks.size()) ? &g_racks[(size_t)i] : 0;
}

}  /* namespace bs */
