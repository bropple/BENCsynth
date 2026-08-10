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
#include "bs_racklib.h"
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

    /* A scratchpad's text has to survive the round trip, and the characters
     * that can break a line-based format have to survive it too: a newline
     * would end the record, and a backslash is what the escaping is made of. */
    {
        Engine e;
        e.init(48000.0f);
        const int t = e.addModule("TEXT", 40, 60);
        e.addModule("OUT", 300, 60);
        ok(t >= 0, "a TEXT module was created");

        std::string *buf = e.patch.module(t) ? e.patch.module(t)->textBuffer() : 0;
        ok(buf != 0, "the TEXT module offers a buffer");
        if (buf) {
            *buf = "line one\nline two\\ with a backslash\n\nand a blank line";

            ok(bs_patch_save(&e, PATH, status, (int)sizeof status) != 0,
               "a rack with notes saved");

            Engine f;
            f.init(48000.0f);
            ok(bs_patch_load(&f, PATH, status, (int)sizeof status) != 0,
               "a rack with notes loaded");

            std::string *back = 0;
            for (int i = 0; i < f.patch.slotCount(); i++) {
                Module *m = f.patch.module(i);
                if (m && m->typeId == "TEXT") back = m->textBuffer();
            }
            ok(back != 0, "the TEXT module came back");
            if (back) ok(*back == *buf, "the notes came back byte for byte");
        }
    }

    /* The string form is the one a plugin host will use - it is handed a blob
     * and expects one back, and never sees a path. It has to survive a round
     * trip on the largest rack there is, and a second trip has to produce the
     * same bytes: a serialiser that drifts each time it is loaded and saved
     * turns somebody's song into a slightly different song every time they
     * open it. */
    {
        int tour = -1;
        for (int i = 0; i < rackPresetCount(); i++)
            if (std::strcmp(rackPresetAt(i)->name, "GRAND TOUR") == 0) tour = i;
        ok(tour >= 0, "GRAND TOUR is there to round-trip");

        Engine a;
        a.init(48000.0f);
        a.buildPreset(tour >= 0 ? tour : 0);
        const int modsA = liveModules(a), cablesA = liveCables(a);

        const std::string text = bs_patch_to_string(&a);
        ok(text.size() > 100, "the rack serialised to something");

        Engine b;
        b.init(48000.0f);
        ok(bs_patch_from_string(&b, text.c_str(), status, (int)sizeof status) != 0,
           "it loaded back from the string");
        okf(liveModules(b) == modsA, "%.0f modules came back, expected %.0f",
            (double)liveModules(b), (double)modsA);
        okf(liveCables(b) == cablesA, "%.0f cables came back, expected %.0f",
            (double)liveCables(b), (double)cablesA);

        ok(bs_patch_to_string(&b) == text, "a second round trip is byte-identical");

        /* And it still sounds like itself. */
        okf(std::fabs(renderRms(a) - renderRms(b)) < 0.001f,
            "the reloaded rack rendered rms %.5f against the original's %.5f",
            renderRms(b), renderRms(a));
    }

    /* Rubbish in the string form is refused rather than half-applied. */
    {
        Engine e;
        e.init(48000.0f);
        e.buildDefaultPatch();
        const int before = liveModules(e);
        ok(bs_patch_from_string(&e, "not a rack at all\n", status,
                                (int)sizeof status) == 0,
           "a string that is not a rack is refused");
        okf(liveModules(e) == before,
            "the rack still has %.0f modules after a refused load, expected %.0f",
            (double)liveModules(e), (double)before);
        ok(bs_patch_from_string(&e, 0, status, (int)sizeof status) == 0,
           "a null string is refused rather than crashing");
    }

    /* Audio rides inside the file. A rack refers to its samples by path,
     * which is useless the moment the rack is handed to somebody else - so
     * the file carries the audio after the text and a NUL, and loading
     * restores any sample whose path no longer opens. The NUL matters for
     * the past as much as the future: every loader ever shipped parses the
     * text as a C string and stops there, so an old build opens a new file
     * and simply has no audio in it. */
    {
        const char *wavPath = "bs-test-tmp/bs-test-embed.wav";
        {
            const int rate = 44100, n = rate / 5;
            std::vector<short> pcm((size_t)n * 2);
            for (int i = 0; i < n; i++) {
                const double v = std::sin(2.0 * 3.14159265358979 * 330.0 * i / rate);
                pcm[(size_t)i * 2 + 0] = (short)(v * 20000.0);
                pcm[(size_t)i * 2 + 1] = (short)(v * 20000.0);
            }
            FILE *f = std::fopen(wavPath, "wb");
            ok(f != 0, "wrote the sample fixture");
            if (f) {
                struct P {
                    static void u32(FILE *g, unsigned v)
                    { unsigned char b[4] = { (unsigned char)v, (unsigned char)(v >> 8),
                                             (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
                      std::fwrite(b, 1, 4, g); }
                    static void u16(FILE *g, unsigned v)
                    { unsigned char b[2] = { (unsigned char)v, (unsigned char)(v >> 8) };
                      std::fwrite(b, 1, 2, g); }
                };
                const unsigned data = (unsigned)(pcm.size() * 2);
                std::fwrite("RIFF", 1, 4, f); P::u32(f, 36 + data);
                std::fwrite("WAVEfmt ", 1, 8, f); P::u32(f, 16);
                P::u16(f, 1); P::u16(f, 2); P::u32(f, (unsigned)rate);
                P::u32(f, (unsigned)rate * 4); P::u16(f, 4); P::u16(f, 16);
                std::fwrite("data", 1, 4, f); P::u32(f, data);
                std::fwrite(&pcm[0], 2, pcm.size(), f);
                std::fclose(f);
            }
        }

        /* Scoped, so the engine lets go of its shared copy before the load
         * below: the sample cache keeps a file alive as long as any module
         * holds it, and a cache hit would let the reload "work" with the
         * restore broken. A rack handed to somebody else is a fresh process
         * with a cold cache, and that is the case being tested. */
        {
            Engine e;
            e.init(48000.0f);
            e.clear();
            const int smp = e.addModule("SAMPLE", 40, 60);
            const int out = e.addModule("OUT", 300, 60);
            e.connect(smp, 0, out, 0);
            std::string why;
            ok(e.loadSample(smp, wavPath, &why), "the fixture wav loads");
            ok(bs_patch_save(&e, PATH, status, (int)sizeof status) != 0,
               "a rack with a sample saved");
        }

        /* The file: text, a NUL, then the audio. */
        std::string blob;
        {
            FILE *f = std::fopen(PATH, "rb");
            char buf[4096]; size_t n;
            while (f && (n = std::fread(buf, 1, sizeof buf, f)) > 0)
                blob.append(buf, n);
            if (f) std::fclose(f);
        }
        const size_t nul = blob.find('\0');
        ok(nul != std::string::npos, "the saved file carries an audio section");
        ok(nul + 5 < blob.size() && std::memcmp(&blob[nul + 1], "BSAU", 4) == 0,
           "and it is the layout the plugin state uses");

        /* What a build from before this feature does with the file: parse the
         * same bytes as a C string. It must see the whole rack and nothing of
         * the audio. */
        {
            Engine old;
            old.init(48000.0f);
            ok(bs_patch_from_string(&old, blob.c_str(), status,
                                    (int)sizeof status) != 0,
               "an old loader reads the new file");
            okf(liveModules(old) == 2, "and sees %.0f modules, expected %.0f",
                (double)liveModules(old), 2.0);
        }

        /* The source file is gone; the copy in the rack file is not. */
        std::remove(wavPath);
        const std::string dest = std::string(userSampleDir()) +
                                 "/bs-test-embed.wav";
        std::remove(dest.c_str());

        Engine f;
        f.init(48000.0f);
        ok(bs_patch_load(&f, PATH, status, (int)sizeof status) != 0,
           "the rack loaded with its source file deleted");
        Module *m = 0;
        for (int i = 0; i < f.patch.slotCount(); i++)
            if (f.patch.module(i) && f.patch.module(i)->typeId == "SAMPLE")
                m = f.patch.module(i);
        ok(m != 0, "the sampler came back");
        ok(m && m->textLoaded(), "and its audio came back out of the rack file");

        /* And it sounds - nothing on TRIG means play. */
        if (m) {
            std::vector<float> buf(24000 * 2, 0.0f);
            f.render(&buf[0], 24000);
            double s = 0.0;
            for (size_t i = 0; i < buf.size(); i++) s += (double)buf[i] * buf[i];
            const double rms = std::sqrt(s / (double)buf.size());
            okf(rms > 0.001, "the restored sample plays at rms %.4f, "
                "expected over %.3f", rms, 0.001);
        }
        std::remove(dest.c_str());

        /* A rack with no samples saves the bytes it always did: no NUL. */
        {
            Engine g;
            g.init(48000.0f);
            g.buildDefaultPatch();
            ok(bs_patch_save(&g, PATH, status, (int)sizeof status) != 0,
               "a rack with no samples saved");
            std::string b2;
            FILE *fp = std::fopen(PATH, "rb");
            char buf[4096]; size_t n;
            while (fp && (n = std::fread(buf, 1, sizeof buf, fp)) > 0)
                b2.append(buf, n);
            if (fp) std::fclose(fp);
            ok(b2.find('\0') == std::string::npos,
               "and its file carries no audio section");
        }
    }

    std::remove(PATH);
    std::remove(DIR);
}
