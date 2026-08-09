/*
 * Write a built-in rack out as a .bencsynth file.
 *
 * Exists so a test can have a user rack without one being committed or
 * hand-written. The plugin's Rack chooser now lists whatever is in the user's
 * rack folder, and checking that means putting something there first.
 *
 *   bencsynth-save-rack CLASSIC out.bencsynth
 *   bencsynth-save-rack 0       out.bencsynth
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

    char status[256] = "";
    if (!bs_patch_save(&e, argv[2], status, sizeof status)) {
        std::fprintf(stderr, "could not write %s: %s\n", argv[2], status);
        return 1;
    }
    std::printf("wrote %s (%s)\n", argv[2], bs::rackPresetAt(idx)->name);
    return 0;
}
