#include "bs_patchfile.h"
#include "bs_modules.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

/* The only platform-dependent lines in the core, and they are here rather than
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

/* ------------------------------------------------------------------ *
 * Text
 * ------------------------------------------------------------------ */

static void appendf(std::string &s, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    s += buf;
}

std::string bs_patch_to_string(bs::Engine *eng)
{
    std::string out;
    appendf(out, "%s %d\n", MAGIC, VERSION);

    for (int id = 0; id < eng->patch.slotCount(); id++) {
        Module *m = eng->patch.module(id);
        if (!m) continue;

        appendf(out, "M %d %s %.1f %.1f %d", id, m->typeId.c_str(),
                (double)m->x, (double)m->y, m->paramCount());
        for (int p = 0; p < m->paramCount(); p++)
            appendf(out, " %.6g", (double)m->params[(size_t)p].value);
        out += '\n';

        /* A scratchpad's contents, escaped onto one line. The format is
         * line-based, so a newline in the text would otherwise end the record
         * and the rest of the note would be read back as garbage. */
        const std::string *note = m->textBuffer();
        if (note && !note->empty()) {
            appendf(out, "X %d ", id);
            for (size_t k = 0; k < note->size(); k++) {
                const char c = (*note)[k];
                if (c == '\\')      out += "\\\\";
                else if (c == '\n') out += "\\n";
                else if (c == '\r') { }
                else                out += c;
            }
            out += '\n';
        }
    }

    const std::vector<Cable> &cs = eng->patch.cableList();
    for (size_t i = 0; i < cs.size(); i++) {
        if (!cs[i].alive) continue;
        appendf(out, "C %d %d %d %d %d\n", cs[i].src, cs[i].srcPort,
                cs[i].dst, cs[i].dstPort, cs[i].color);
    }

    /* Where it was being looked at, if anyone has looked at it. */
    if (eng->patch.viewZoom > 0.0f)
        appendf(out, "V %.2f %.2f %.4f\n", (double)eng->patch.viewX,
                (double)eng->patch.viewY, (double)eng->patch.viewZoom);

    /* Which knobs this rack hands to the host. Written last and read by slot,
     * so an older file without them simply has none - and a newer file opened
     * by an older build skips a record letter it does not know. */
    for (int i = 0; i < bs::BS_EXPOSED; i++) {
        const bs::Exposed &e = eng->patch.exposed[i];
        if (e.module < 0) continue;
        appendf(out, "E %d %d %d\n", i, e.module, e.param);
    }
    return out;
}

/* Reads one line, without its terminator, and advances `p` past it. */
static bool nextLine(const char *&p, std::string &line)
{
    if (!p || !*p) return false;
    const char *e = p;
    while (*e && *e != '\n') e++;
    line.assign(p, (size_t)(e - p));
    if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
    p = *e ? e + 1 : e;
    return true;
}

int bs_patch_from_string(bs::Engine *eng, const char *text,
                         char *status, int cap)
{
    if (!text) {
        std::snprintf(status, (size_t)cap, "nothing to load");
        return 0;
    }

    const char *p = text;
    std::string line;

    char magic[32] = "";
    int  version = 0;
    if (!nextLine(p, line) ||
        std::sscanf(line.c_str(), "%31s %d", magic, &version) != 2 ||
        std::strcmp(magic, MAGIC) != 0) {
        std::snprintf(status, (size_t)cap, "not a BENCsynth rack");
        return 0;
    }
    if (version > VERSION) {
        std::snprintf(status, (size_t)cap, "written by a later version");
        return 0;
    }

    eng->clear();

    /* Saved ids are not reused ids: the text may have holes where modules were
     * deleted, and add() hands out the densest slot it can. So the two
     * numbering schemes are kept apart and the cables are translated. */
    std::vector<int> remap;
    int modules = 0, cables = 0, skipped = 0;

    while (nextLine(p, line)) {
        const char *l = line.c_str();

        if (l[0] == 'M') {
            int savedId = 0, nparams = 0;
            char type[32] = "";
            float x = 0.0f, y = 0.0f;
            int consumed = 0;
            if (std::sscanf(l, "M %d %31s %f %f %d%n",
                            &savedId, type, &x, &y, &nparams, &consumed) != 5) continue;
            if (savedId < 0 || savedId > 4096) continue;

            const int id = eng->addModule(type, x, y);
            if ((int)remap.size() <= savedId) remap.resize((size_t)savedId + 1, -1);
            remap[(size_t)savedId] = id;
            if (id < 0) { skipped++; continue; }
            modules++;

            Module *m = eng->patch.module(id);
            const char *q = l + consumed;
            for (int i = 0; i < nparams; i++) {
                char *end = 0;
                const double v = std::strtod(q, &end);
                if (end == q) break;
                q = end;
                /* Text written before a knob existed simply runs out of
                 * values, and the knobs it never knew about keep their
                 * defaults. Text written after one was removed has values with
                 * nowhere to go, and they are dropped. */
                if (i < m->paramCount()) m->params[(size_t)i].value = (float)v;
            }

        } else if (l[0] == 'X') {
            int savedId = 0, consumed = 0;
            if (std::sscanf(l, "X %d%n", &savedId, &consumed) != 1) continue;
            if (savedId < 0 || savedId >= (int)remap.size()) continue;
            if (remap[(size_t)savedId] < 0) continue;
            Module *m = eng->patch.module(remap[(size_t)savedId]);
            std::string *note = m ? m->textBuffer() : 0;
            if (!note) continue;

            note->clear();
            const char *q = l + consumed;
            if (*q == ' ') q++;
            for (; *q; q++) {
                if (*q != '\\') { note->push_back(*q); continue; }
                q++;
                if (*q == 'n')       note->push_back('\n');
                else if (*q == '\\') note->push_back('\\');
                else if (*q == 0)    break;
                else                 note->push_back(*q);
            }

            /* A path is not a note: whatever it names has to be
             * opened before the next block asks for its audio. */
            m->onTextChanged();
        } else if (l[0] == 'C') {
            int s = 0, sp = 0, d = 0, dp = 0, col = 0;
            if (std::sscanf(l, "C %d %d %d %d %d", &s, &sp, &d, &dp, &col) < 4) continue;
            if (s < 0 || d < 0 || s >= (int)remap.size() || d >= (int)remap.size()) continue;
            if (remap[(size_t)s] < 0 || remap[(size_t)d] < 0) continue;
            const int cid = eng->connect(remap[(size_t)s], sp, remap[(size_t)d], dp);
            if (cid >= 0) {
                Cable *c = eng->patch.cable(cid);
                if (c) c->color = col;
                cables++;
            }

        } else if (l[0] == 'V') {
            float vx = 0.0f, vy = 0.0f, vz = 0.0f;
            if (std::sscanf(l, "V %f %f %f", &vx, &vy, &vz) != 3) continue;
            if (vz > 0.02f && vz < 50.0f) {
                eng->patch.viewX = vx;
                eng->patch.viewY = vy;
                eng->patch.viewZoom = vz;
            }

        } else if (l[0] == 'E') {
            /* Through the same remap as the cables: ids in the file are the
             * ones it was written with, and a rack loaded into a patch that
             * already has modules gets different ones. */
            int slot = 0, mod = 0, par = 0;
            if (std::sscanf(l, "E %d %d %d", &slot, &mod, &par) != 3) continue;
            if (slot < 0 || slot >= bs::BS_EXPOSED) continue;
            if (mod < 0 || mod >= (int)remap.size() || remap[(size_t)mod] < 0) continue;
            const Module *m = eng->patch.module(remap[(size_t)mod]);
            if (!m || par < 0 || par >= m->paramCount()) continue;
            eng->patch.exposed[slot].module = remap[(size_t)mod];
            eng->patch.exposed[slot].param  = par;
        }
    }

    if (skipped)
        std::snprintf(status, (size_t)cap,
                      "%d modules, %d cables, %d unknown module(s) dropped",
                      modules, cables, skipped);
    else
        std::snprintf(status, (size_t)cap, "%d modules, %d cables",
                      modules, cables);
    return 1;
}

/* ------------------------------------------------------------------ *
 * Files
 * ------------------------------------------------------------------ */

/* Everything up to the last separator, so a status line can name the file
 * without the path it came down. */
static const char *baseName(const char *path)
{
    const char *a = std::strrchr(path, '/');
    const char *b = std::strrchr(path, '\\');
    const char *s = a > b ? a : b;
    return s ? s + 1 : path;
}

int bs_patch_save(bs::Engine *eng, const char *path, char *status, int cap)
{
    /* One level of directory, made on the way past rather than at startup, so
     * a program that is only ever played leaves no folder behind. A failure
     * here is ignored: the fopen below is the check that matters and it gives
     * the better message. */
    const char *slash = std::strrchr(path, '/');
    if (slash) {
        const std::string dir(path, (size_t)(slash - path));
        if (!dir.empty()) BS_MKDIR(dir.c_str());
    }

    const std::string text = bs_patch_to_string(eng);

    FILE *f = std::fopen(path, "wb");
    if (!f) {
        std::snprintf(status, (size_t)cap, "cannot write %s", baseName(path));
        return 0;
    }
    const size_t n = std::fwrite(text.data(), 1, text.size(), f);
    const int    ok = (n == text.size());
    std::fclose(f);

    if (!ok) {
        std::snprintf(status, (size_t)cap, "could not finish writing %s",
                      baseName(path));
        return 0;
    }
    std::snprintf(status, (size_t)cap, "saved %s", baseName(path));
    return 1;
}

int bs_patch_load(bs::Engine *eng, const char *path, char *status, int cap)
{
    FILE *f = std::fopen(path, "rb");
    if (!f) {
        std::snprintf(status, (size_t)cap, "cannot open %s", baseName(path));
        return 0;
    }

    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    std::fclose(f);

    char detail[192] = "";
    if (!bs_patch_from_string(eng, text.c_str(), detail, (int)sizeof detail)) {
        std::snprintf(status, (size_t)cap, "%s - %s", baseName(path), detail);
        return 0;
    }
    std::snprintf(status, (size_t)cap, "%s - %s", baseName(path), detail);
    return 1;
}
