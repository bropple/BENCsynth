#include "bs_patchfile.h"
#include "bs_modules.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

/* The only platform-dependent line in the core, and it is here rather than
 * taken from raylib so that saving a rack does not drag a windowing library
 * into the part of the program a plugin links. */
#ifdef _WIN32
#  include <direct.h>
#  define BS_MKDIR(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#  define BS_MKDIR(p) mkdir(p, 0777)
#endif

using bs::Module;
using bs::Cable;

static const char *MAGIC = "BENCSYNTH";
static const int   VERSION = 1;

const char *bs_patch_slot_path(int slot)
{
    static char path[64];
    if (slot < 1) slot = 1;
    if (slot > 8) slot = 8;
    std::snprintf(path, sizeof path, "patches/rack-%d.bsp", slot);
    return path;
}

/* ------------------------------------------------------------------ */

int bs_patch_save(bs::Engine *eng, const char *path, char *status, int cap)
{
    /* The directory is made on the way past rather than at startup, so a
     * program that is only ever played does not leave a folder behind. One
     * level deep is all the slot paths need; a failure here is ignored,
     * because the fopen below is the check that matters and it gives a better
     * message. */
    const char *slash = std::strrchr(path, '/');
    if (slash) {
        const std::string dir(path, (size_t)(slash - path));
        if (!dir.empty()) BS_MKDIR(dir.c_str());
    }

    FILE *f = std::fopen(path, "wb");
    if (!f) {
        std::snprintf(status, (size_t)cap, "cannot write %s", path);
        return 0;
    }

    std::fprintf(f, "%s %d\n", MAGIC, VERSION);

    int modules = 0, cables = 0;
    for (int id = 0; id < eng->patch.slotCount(); id++) {
        const Module *m = eng->patch.module(id);
        if (!m) continue;
        std::fprintf(f, "M %d %s %.1f %.1f %d", id, m->typeId.c_str(),
                     (double)m->x, (double)m->y, m->paramCount());
        for (int p = 0; p < m->paramCount(); p++)
            std::fprintf(f, " %.6g", (double)m->params[(size_t)p].value);
        std::fputc('\n', f);
        modules++;
    }

    const std::vector<Cable> &cs = eng->patch.cableList();
    for (size_t i = 0; i < cs.size(); i++) {
        if (!cs[i].alive) continue;
        std::fprintf(f, "C %d %d %d %d %d\n", cs[i].src, cs[i].srcPort,
                     cs[i].dst, cs[i].dstPort, cs[i].color);
        cables++;
    }

    std::fclose(f);
    std::snprintf(status, (size_t)cap, "saved %s - %d modules, %d cables",
                  path, modules, cables);
    return 1;
}

/* ------------------------------------------------------------------ */

int bs_patch_load(bs::Engine *eng, const char *path, char *status, int cap)
{
    FILE *f = std::fopen(path, "rb");
    if (!f) {
        std::snprintf(status, (size_t)cap, "no patch in %s", path);
        return 0;
    }

    char magic[32] = "";
    int  version = 0;
    if (std::fscanf(f, "%31s %d", magic, &version) != 2 ||
        std::strcmp(magic, MAGIC) != 0) {
        std::fclose(f);
        std::snprintf(status, (size_t)cap, "%s is not a BENCsynth rack", path);
        return 0;
    }
    if (version > VERSION) {
        std::fclose(f);
        std::snprintf(status, (size_t)cap, "%s was written by a later version", path);
        return 0;
    }

    eng->clear();

    /* Saved ids are not reused ids: the file may have holes where modules
     * were deleted, and add() hands out the densest slot it can. So the two
     * numbering schemes are kept apart and the cables are translated. */
    std::vector<int> remap;
    int modules = 0, cables = 0, skipped = 0;

    char line[4096];
    std::fgets(line, sizeof line, f);           /* rest of the header line */
    while (std::fgets(line, sizeof line, f)) {
        if (line[0] == 'M') {
            int savedId = 0, nparams = 0;
            char type[32] = "";
            float x = 0.0f, y = 0.0f;
            int consumed = 0;
            if (std::sscanf(line, "M %d %31s %f %f %d%n",
                            &savedId, type, &x, &y, &nparams, &consumed) != 5) continue;

            const int id = eng->addModule(type, x, y);
            if ((int)remap.size() <= savedId) remap.resize((size_t)savedId + 1, -1);
            remap[(size_t)savedId] = id;
            if (id < 0) { skipped++; continue; }
            modules++;

            Module *m = eng->patch.module(id);
            const char *p = line + consumed;
            for (int i = 0; i < nparams; i++) {
                char *end = 0;
                const double v = std::strtod(p, &end);
                if (end == p) break;
                p = end;
                /* A file written before a knob existed simply runs out of
                 * values, and the knobs it never knew about keep their
                 * defaults. A file written after one was removed has values
                 * with nowhere to go, and they are dropped. */
                if (i < m->paramCount()) m->params[(size_t)i].value = (float)v;
            }
        } else if (line[0] == 'C') {
            int s = 0, sp = 0, d = 0, dp = 0, col = 0;
            if (std::sscanf(line, "C %d %d %d %d %d", &s, &sp, &d, &dp, &col) < 4) continue;
            if (s < 0 || d < 0 || s >= (int)remap.size() || d >= (int)remap.size()) continue;
            if (remap[(size_t)s] < 0 || remap[(size_t)d] < 0) continue;
            const int cid = eng->connect(remap[(size_t)s], sp, remap[(size_t)d], dp);
            if (cid >= 0) {
                Cable *c = eng->patch.cable(cid);
                if (c) c->color = col;
                cables++;
            }
        }
    }
    std::fclose(f);

    if (skipped)
        std::snprintf(status, (size_t)cap,
                      "loaded %s - %d modules, %d cables, %d unknown module(s) dropped",
                      path, modules, cables, skipped);
    else
        std::snprintf(status, (size_t)cap, "loaded %s - %d modules, %d cables",
                      path, modules, cables);
    return 1;
}
