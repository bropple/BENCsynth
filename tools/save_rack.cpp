/*
 * Write a built-in rack out as a .bencsynth file.
 *
 * Exists so a test can have a user rack without one being committed or
 * hand-written. The plugin's Rack chooser now lists whatever is in the user's
 * rack folder, and checking that means putting something there first.
 *
 *   bencsynth-save-rack CLASSIC out.bencsynth
 *   bencsynth-save-rack 0       out.bencsynth
 *   bencsynth-save-rack "GRAND TOUR" out.bencsynth --cc-base 20
 *
 * --cc-base sets the MACRO panel's CC BASE knob before saving, which is how
 * the host test gets a rack whose macros answer to a controller.
 */

#include "bs_engine.h"
#include "bs_patchfile.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char **argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <rack name or index> <path>\n", argv[0]);
        return 2;
    }

    int idx = -1;
    for (int i = 0; i < bs::rackPresetCount(); i++) {
        const bs::RackPreset *rp = bs::rackPresetAt(i);
        if (rp && std::strcmp(rp->name, argv[1]) == 0) { idx = i; break; }
    }
    if (idx < 0) {
        char *end = 0;
        const long n = std::strtol(argv[1], &end, 10);
        if (end && !*end && n >= 0 && n < bs::rackPresetCount()) idx = (int)n;
    }
    if (idx < 0) {
        std::fprintf(stderr, "no rack called '%s'\n", argv[1]);
        return 1;
    }

    bs::Engine e;
    e.init(48000.0f);
    e.buildPreset(idx);

    for (int i = 3; i + 1 < argc; i++) {
        if (std::strcmp(argv[i], "--cc-base") != 0) continue;
        const int base = std::atoi(argv[i + 1]);
        bool set = false;
        for (int k = 0; k < e.patch.slotCount(); k++) {
            bs::Module *m = e.patch.module(k);
            if (!m || m->typeId != "MACRO") continue;
            if (m->paramCount() <= bs::BS_MACROS) continue;
            m->params[(size_t)bs::BS_MACROS].value = (float)base;
            set = true;
        }
        if (!set) {
            std::fprintf(stderr, "no MACRO panel in '%s' to assign a CC to\n", argv[1]);
            return 1;
        }
        std::printf("CC BASE = %d\n", base);
    }

    char status[256] = "";
    if (!bs_patch_save(&e, argv[2], status, sizeof status)) {
        std::fprintf(stderr, "could not write %s: %s\n", argv[2], status);
        return 1;
    }
    std::printf("wrote %s (%s)\n", argv[2], bs::rackPresetAt(idx)->name);
    return 0;
}
