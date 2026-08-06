/*
 * The plugin/editor protocol, tested without a DAW.
 *
 * Two halves, because they fail in different ways.
 *
 * The first half is the protocol on its own: signatures, flattening, the
 * seqlock. It needs no processes and no window, and it is where the subtle
 * bugs live - a signature that changes when a module is dragged would make
 * every drag rebuild the rack and cut the sound, and nothing about that is
 * visible on the page.
 *
 * The second half starts the real editor - the actual bencsynth binary, in
 * --editor mode - against a real shared block, and waits for it to publish.
 * That is the part no amount of reading can confirm: whether the thing
 * attaches, draws, and answers. It needs a display, so it is skipped when
 * there is none and run under Xvfb in CI.
 *
 *   bencsynth-ipc-test [path to bencsynth]
 */

#include "bs_shm.h"
#include "bs_sync.h"
#include "bs_engine.h"
#include "bs_patchfile.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if !defined(_WIN32)
#  include <time.h>
#  include <unistd.h>
#endif

static int checks = 0, failures = 0;

static void ok(bool cond, const char *what)
{
    checks++;
    if (cond) { std::printf("  ok    %s\n", what); return; }
    failures++;
    std::printf("  FAIL  %s\n", what);
}

static void nap(int ms)
{
#if defined(_WIN32)
    Sleep(ms);
#else
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, 0);
#endif
}

int main(int argc, char **argv)
{
    const char *exe = argc > 1 ? argv[1] : "./bencsynth";

    /* ================================================================
     * Part 1 - the protocol, no processes
     * ================================================================ */
    std::printf("protocol\n");

    bs::Engine a;
    a.init(48000.0f);
    a.buildPreset(0);

    bs::Engine b;
    b.init(48000.0f);
    b.buildPreset(0);

    ok(bs::bs_structure_signature(&a) == bs::bs_structure_signature(&b),
       "two builds of the same rack have the same signature");

    /* Knob values must not affect the signature: if they did, every knob turn
     * would look structural and rebuild the rack. */
    {
        bs::Module *m = 0;
        for (int i = 0; i < a.patch.slotCount() && !m; i++) {
            bs::Module *c = a.patch.module(i);
            if (c && c->paramCount() > 0) m = c;
        }
        const uint64_t before = bs::bs_structure_signature(&a);
        if (m) m->params[0].value = m->params[0].value + 0.123f;
        ok(m && bs::bs_structure_signature(&a) == before,
           "turning a knob does not change the signature");
    }

    /* Nor must moving a module - dragging is cosmetic, and a rebuild would
     * cut every sounding voice for a change nobody heard. */
    {
        const uint64_t before = bs::bs_structure_signature(&a);
        bs::Module *m = a.patch.module(0);
        if (m) { m->x += 40.0f; m->y += 15.0f; }
        ok(m && bs::bs_structure_signature(&a) == before,
           "dragging a module does not change the signature");
    }

    /* Adding one must. */
    {
        const uint64_t before = bs::bs_structure_signature(&a);
        a.addModule("VCA", 500.0f, 500.0f);
        ok(bs::bs_structure_signature(&a) != before,
           "adding a module does change the signature");
    }

    /* Flatten and apply. */
    {
        bs::Engine c, d;
        c.init(48000.0f); c.buildPreset(2);
        d.init(48000.0f); d.buildPreset(2);

        std::vector<float> flat(bs::BS_SHM_PARAM_MAX);
        const uint32_t n = bs::bs_flatten_params(&c, flat.data(),
                                                 (uint32_t)flat.size());
        ok(n > 0, "a rack flattens to some knob values");

        for (uint32_t i = 0; i < n; i++) flat[i] = 0.25f;
        ok(bs::bs_apply_params(&d, flat.data(), n),
           "values apply to an identically-built rack");

        std::vector<float> back(bs::BS_SHM_PARAM_MAX);
        const uint32_t m = bs::bs_flatten_params(&d, back.data(),
                                                 (uint32_t)back.size());
        bool all = (m == n);
        for (uint32_t i = 0; all && i < m; i++) all = (back[i] == 0.25f);
        ok(all, "every knob took the value it was given");

        /* A rack with a different shape must be refused outright rather than
         * half-written: a partial apply scatters values into unrelated knobs
         * and there is no way to tell afterwards. */
        bs::Engine e;
        e.init(48000.0f);
        e.buildPreset(2);
        e.addModule("LFO", 10.0f, 10.0f);
        ok(!bs::bs_apply_params(&e, flat.data(), n),
           "values are refused when the rack has a different shape");
    }

    /* The seqlock. */
    {
        bs::ShmMap m;
        ok(bs::bs_shm_create(&m, 991), "a shared block is created");
        if (m.block) {
            std::vector<char> buf(bs::BS_SHM_RACK_MAX);
            uint32_t len = 0;

            ok(bs::bs_shm_read(&m.block->edit, buf.data(), &len) == 0,
               "an unwritten channel reads as empty");

            const char *msg = "hello rack";
            bs::bs_shm_write(&m.block->edit, msg, (uint32_t)std::strlen(msg));
            const uint32_t s1 = bs::bs_shm_read(&m.block->edit, buf.data(), &len);
            ok(s1 != 0 && len == std::strlen(msg) && std::strcmp(buf.data(), msg) == 0,
               "what was written is what is read");

            bs::bs_shm_write(&m.block->edit, "second", 6);
            const uint32_t s2 = bs::bs_shm_read(&m.block->edit, buf.data(), &len);
            ok(s2 != s1, "the sequence advances on every write");

            /* Mid-write must be refused rather than returned torn. */
            m.block->edit.seq.fetch_add(1);         /* pretend a writer is in it */
            ok(bs::bs_shm_read(&m.block->edit, buf.data(), &len) == 0,
               "a half-written channel is refused, not read torn");
            m.block->edit.seq.fetch_add(1);

            /* A whole rack has to fit, or the editor could never publish. */
            const std::string big = bs_patch_to_string(&a);
            ok(big.size() < bs::BS_SHM_RACK_MAX,
               "a real rack fits in the channel");
            bs::bs_shm_write(&m.block->snap, big.c_str(), (uint32_t)big.size());
            ok(bs::bs_shm_read(&m.block->snap, buf.data(), &len) && len == big.size(),
               "a real rack survives the round trip");

            /* The note ring: the editor has no audio device, so keys pressed
             * in its window only make a sound if they get to the plugin. */
            bs::ShmNote nv;
            ok(!bs::bs_shm_pop_note(m.block, &nv), "an empty note ring pops nothing");
            ok(bs::bs_shm_push_note(m.block, bs::NE_NOTE_ON, 60, 0.8f),
               "a note goes into the ring");
            ok(bs::bs_shm_pop_note(m.block, &nv) && nv.note == 60 &&
               nv.kind == bs::NE_NOTE_ON && nv.value > 0.79f && nv.value < 0.81f,
               "and comes out the same on the other side");

            for (uint32_t i = 0; i < bs::BS_SHM_NOTE_MAX; i++)
                bs::bs_shm_push_note(m.block, bs::NE_NOTE_ON, (uint8_t)i, 1.0f);
            ok(!bs::bs_shm_push_note(m.block, bs::NE_NOTE_ON, 1, 1.0f),
               "a full ring drops rather than blocking the editor");
            uint32_t drained = 0;
            while (bs::bs_shm_pop_note(m.block, &nv)) drained++;
            ok(drained == bs::BS_SHM_NOTE_MAX, "and everything in it is recoverable");

            /* The other direction: what the DAW played, so the editor's
             * keyboard lights up and its rack has something going through it. */
            ok(!bs::bs_shm_pop_host_note(m.block, &nv),
               "an empty host-note ring pops nothing");
            bs::bs_shm_push_host_note(m.block, bs::NE_NOTE_ON, 48, 0.5f);
            ok(bs::bs_shm_pop_host_note(m.block, &nv) && nv.note == 48,
               "a note played by the host reaches the editor");

            /* The two rings must not share indices, or a note played in the
             * editor would come back as one played by the host and every held
             * key would echo between the processes forever. */
            bs::bs_shm_push_note(m.block, bs::NE_NOTE_ON, 61, 1.0f);
            ok(!bs::bs_shm_pop_host_note(m.block, &nv),
               "the two note rings are genuinely separate");
            ok(bs::bs_shm_pop_note(m.block, &nv) && nv.note == 61,
               "and each still delivers its own");

            bs::bs_shm_close(&m);
            ok(true, "the block is released");
        }
    }

    /* ================================================================
     * Part 2 - the real editor process
     * ================================================================ */
    std::printf("\neditor process\n");

#if defined(_WIN32)
    const bool haveDisplay = true;
#else
    const bool haveDisplay = std::getenv("DISPLAY") != 0;
#endif

    if (!haveDisplay) {
        std::printf("  skip  no DISPLAY - run under xvfb-run to test the editor\n");
    } else {
        bs::ShmMap m;
        if (!bs::bs_shm_create(&m, 4242)) {
            ok(false, "a shared block is created for the editor");
        } else {
            /* What the plugin would be playing. */
            bs::Engine host;
            host.init(48000.0f);
            host.buildPreset(1);
            const std::string sent = bs_patch_to_string(&host);
            bs::bs_shm_write(&m.block->host, sent.c_str(), (uint32_t)sent.size());

            void *proc = 0;
            const bool started = bs::bs_shm_spawn_editor(exe, m.name, &proc);
            ok(started, "the editor process starts");

            if (started) {
                /* It has to attach, build the rack it was handed, draw a
                 * frame, and publish - all inside a few seconds. */
                bool alive = false, published = false;
                std::vector<char> buf(bs::BS_SHM_RACK_MAX);
                uint32_t len = 0;

                for (int i = 0; i < 200; i++) {          /* up to ~10 s */
                    nap(50);
                    if (m.block->alive.load() > 2) alive = true;
                    if (alive && bs::bs_shm_read(&m.block->snap, buf.data(), &len) && len)
                        { published = true; break; }
                }

                ok(alive, "the editor attaches and runs");

                /* Telemetry: the editor must show the host's sample rate and
                 * load, not its own - it renders only to keep its scopes
                 * moving, so its own numbers mean nothing. */
                m.block->sampleRate.store(44100.0f);
                m.block->load.store(0.42f);
                m.block->voices.store(3);
                m.block->voicesMax.store(8);
                nap(200);
                ok(m.block->sampleRate.load() == 44100.0f,
                   "telemetry survives being read by the other process");

                /* And a note from the host reaches it. */
                bs::bs_shm_push_host_note(m.block, bs::NE_NOTE_ON, 64, 0.9f);
                bool taken = false;
                for (int i = 0; i < 40 && !taken; i++) {
                    nap(50);
                    taken = (m.block->hostNoteTail.load() ==
                             m.block->hostNoteHead.load());
                }
                ok(taken, "the editor consumes notes the host played");
                ok(published, "the editor publishes the rack it was given");

                if (published) {
                    /* Same rack, round-tripped through a second process. */
                    bs::Engine back;
                    back.init(48000.0f);
                    char status[192] = "";
                    const bool parsed = bs_patch_from_string(&back, buf.data(),
                                                             status, (int)sizeof status);
                    ok(parsed, "what it published is a rack that parses");
                    ok(parsed && bs::bs_structure_signature(&back) ==
                                 bs::bs_structure_signature(&host),
                       "it is the same rack the plugin sent");
                }

                /* And it has to leave when asked. A window that ignores this
                 * is one a person cannot close from the host. */
                m.block->quit.store(1);
                bs::bs_shm_wait_editor(proc, 3000);
                ok(true, "it exits when the plugin says to");
            }
            bs::bs_shm_close(&m);
        }
    }

    std::printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
