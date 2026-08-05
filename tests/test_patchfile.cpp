/*
 * BENCsynth - patch file tests
 *
 * Saving and loading is the part of the program a person notices being wrong
 * a week later, when the rack they built comes back missing a cable. It is
 * also the part most likely to be quietly wrong, because the ids in the file
 * are not the ids the rack will be rebuilt with: modules are added into the
 * densest free slot, and a file written after a module was deleted has a hole
 * where the loader does not.
 */

#include "bs_engine.h"
#include "bs_patchfile.h"
#include "bs_modules.h"
#include "test_util.h"

#include <cstdio>
#include <cmath>
#include <cstring>
#include <vector>

using namespace bs;

static const char *DIR  = "bs-test-tmp";
static const char *PATH = "bs-test-tmp/rack.bsp";

static int liveModules(const Engine &e)
{
    int n = 0;
    for (int i = 0; i < e.patch.slotCount(); i++) if (e.patch.module(i)) n++;
    return n;
}

static int liveCables(const Engine &e)
{
    int n = 0;
    const std::vector<Cable> &cs = e.patch.cableList();
    for (size_t i = 0; i < cs.size(); i++) if (cs[i].alive) n++;
    return n;
}

/* A fingerprint of what the rack sounds like, so the comparison is not only
 * of the bookkeeping. Same notes, same number of samples, same engine. */
static float renderRms(Engine &e)
{
    std::vector<float> buf(24000 * 2, 0.0f);
    e.noteOn(60, 0.9f);
    e.noteOn(64, 0.9f);
    e.render(&buf[0], 24000);
    double s = 0.0;
    for (size_t i = 0; i < buf.size(); i++) s += (double)buf[i] * buf[i];
    return (float)std::sqrt(s / (double)buf.size());
}

void test_patchfile()
{
    std::printf("patch files\n");

    char status[256] = "";

    Engine a;
    a.init(48000.0f);
    a.buildDefaultPatch();

    const int modsA   = liveModules(a);
    const int cablesA = liveCables(a);

    /* Nudge a knob away from its default, so the test can tell a value that
     * round-tripped from one that was simply never touched. */
    Module *vcf = 0;
    for (int i = 0; i < a.patch.slotCount(); i++) {
        Module *m = a.patch.module(i);
        if (m && m->typeId == "VCF") { vcf = m; break; }
    }
    ok(vcf != 0, "the default rack has a VCF to test with");
    if (!vcf) return;
    vcf->params[0].value = 3371.0f;
    const float vcfX = vcf->x, vcfY = vcf->y;

    ok(bs_patch_save(&a, PATH, status, (int)sizeof status) != 0, "the rack saved");

    Engine b;
    b.init(48000.0f);
    ok(bs_patch_load(&b, PATH, status, (int)sizeof status) != 0, "the rack loaded");

    okf(liveModules(b) == modsA, "%.0f modules came back, expected %.0f",
        (double)liveModules(b), (double)modsA);
    okf(liveCables(b) == cablesA, "%.0f cables came back, expected %.0f",
        (double)liveCables(b), (double)cablesA);

    Module *vcf2 = 0;
    for (int i = 0; i < b.patch.slotCount(); i++) {
        Module *m = b.patch.module(i);
        if (m && m->typeId == "VCF") { vcf2 = m; break; }
    }
    ok(vcf2 != 0, "the VCF came back");
    if (vcf2) {
        okf(std::fabs(vcf2->params[0].value - 3371.0f) < 0.5f,
            "cutoff came back as %.1f, expected %.1f",
            (double)vcf2->params[0].value, 3371.0);
        ok(vcf2->x == vcfX && vcf2->y == vcfY, "the panel came back where it was");
    }

    /* Every input that had a cable still has one, and from the same place.
     * Counting cables is not enough: a remapping bug that crossed two of them
     * would keep the count and ruin the patch. */
    int mismatched = 0;
    const std::vector<Cable> &ca = a.patch.cableList();
    for (size_t i = 0; i < ca.size(); i++) {
        if (!ca[i].alive) continue;
        const Module *srcA = a.patch.module(ca[i].src);
        const Module *dstA = a.patch.module(ca[i].dst);
        if (!srcA || !dstA) continue;

        bool found = false;
        const std::vector<Cable> &cb = b.patch.cableList();
        for (size_t j = 0; j < cb.size(); j++) {
            if (!cb[j].alive) continue;
            const Module *srcB = b.patch.module(cb[j].src);
            const Module *dstB = b.patch.module(cb[j].dst);
            if (!srcB || !dstB) continue;
            if (srcB->typeId == srcA->typeId && dstB->typeId == dstA->typeId &&
                srcB->x == srcA->x && dstB->x == dstA->x &&
                cb[j].srcPort == ca[i].srcPort && cb[j].dstPort == ca[i].dstPort) {
                found = true;
                break;
            }
        }
        if (!found) mismatched++;
    }
    okf(mismatched == 0, "%.0f cables landed somewhere else, expected %.0f",
        (double)mismatched, 0.0);

    /* And it has to sound the same. */
    const float rmsA = renderRms(a);
    const float rmsB = renderRms(b);
    okf(rmsA > 0.005f, "the original rack made sound (rms %.4f, expected above %.3f)",
        rmsA, 0.005);
    okf(std::fabs(rmsA - rmsB) < rmsA * 0.02f,
        "the reloaded rack rendered rms %.5f against the original's %.5f", rmsB, rmsA);

    /* A hole in the middle. Deleting a module leaves a slot the loader will
     * not reproduce, which is exactly the case the id remapping exists for. */
    {
        Engine c;
        c.init(48000.0f);
        c.buildDefaultPatch();
        int victim = -1;
        for (int i = 0; i < c.patch.slotCount(); i++) {
            Module *m = c.patch.module(i);
            if (m && m->typeId == "NOISE") { victim = i; break; }
        }
        ok(victim > 0, "found a module in the middle to delete");
        c.removeModule(victim);
        const int modsC = liveModules(c), cablesC = liveCables(c);

        ok(bs_patch_save(&c, PATH, status, (int)sizeof status) != 0,
           "a rack with a hole in it saved");
        Engine d;
        d.init(48000.0f);
        ok(bs_patch_load(&d, PATH, status, (int)sizeof status) != 0,
           "a rack with a hole in it loaded");
        okf(liveModules(d) == modsC, "%.0f modules survived the hole, expected %.0f",
            (double)liveModules(d), (double)modsC);
        okf(liveCables(d) == cablesC, "%.0f cables survived the hole, expected %.0f",
            (double)liveCables(d), (double)cablesC);
    }

    /* A file that is not one of ours has to be refused rather than
     * half-loaded over the rack the person is working on. */
    {
        FILE *f = std::fopen(PATH, "wb");
        if (f) { std::fputs("this is not a rack\n", f); std::fclose(f); }

        Engine e;
        e.init(48000.0f);
        e.buildDefaultPatch();
        const int before = liveModules(e);
        ok(bs_patch_load(&e, PATH, status, (int)sizeof status) == 0,
           "a file that is not a rack is refused");
        okf(liveModules(e) == before,
            "the rack still has %.0f modules after a refused load, expected %.0f",
            (double)liveModules(e), (double)before);
    }

    /* A module that has since grown a knob: fewer values on the line than the
     * module has parameters. The ones that are there load; the rest keep their
     * defaults, rather than the line being rejected or read off the end. */
    {
        FILE *f = std::fopen(PATH, "wb");
        ok(f != 0, "wrote a short-form patch to read back");
        if (f) {
            std::fputs("BENCSYNTH 1\n", f);
            std::fputs("M 0 VCF 40 60 2 999 0.5\n", f);
            std::fputs("M 1 OUT 300 60 1 0.25\n", f);
            std::fputs("C 0 0 1 0\n", f);
            std::fclose(f);
        }
        Engine e;
        e.init(48000.0f);
        ok(bs_patch_load(&e, PATH, status, (int)sizeof status) != 0,
           "a patch with short parameter lists loaded");
        okf(liveModules(e) == 2, "%.0f modules loaded, expected %.0f",
            (double)liveModules(e), 2.0);
        okf(liveCables(e) == 1, "%.0f cables loaded, expected %.0f",
            (double)liveCables(e), 1.0);

        Module *m = 0;
        for (int i = 0; i < e.patch.slotCount(); i++)
            if (e.patch.module(i) && e.patch.module(i)->typeId == "VCF") m = e.patch.module(i);
        ok(m != 0, "the short-form VCF is there");
        if (m) {
            okf(std::fabs(m->params[0].value - 999.0f) < 0.5f,
                "the value that was in the file loaded as %.1f, expected %.1f",
                (double)m->params[0].value, 999.0);
            okf(std::fabs(m->params[5].value - m->params[5].def) < 1e-6f,
                "a knob the file never mentioned is at %.3f, expected its default %.3f",
                (double)m->params[5].value, (double)m->params[5].def);
        }
    }

    std::remove(PATH);
    std::remove(DIR);
}
