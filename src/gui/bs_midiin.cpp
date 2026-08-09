/*
 * BENCsynth - MIDI in. See bs_midiin.h for the shape and why.
 */

#include "bs_midiin.h"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace bs {

namespace {

/* A ring, written by whatever thread the platform gives us and read by the
 * interface. Three bytes an entry; 256 entries is four seconds of very fast
 * playing and costs nothing. */
const int RING = 256;

struct In {
    unsigned char       msg[RING][3];
    std::atomic<int>    writePos;
    std::atomic<int>    readPos;
    char                name[128];
    int                 ports;

    In() : writePos(0), readPos(0), ports(0) { name[0] = 0; }
};

In g;

void push(const unsigned char *m)
{
    const int w = g.writePos.load(std::memory_order_relaxed);
    const int r = g.readPos.load(std::memory_order_acquire);
    if (w - r >= RING) return;          /* full: drop, rather than wrap over */
    std::memcpy(g.msg[w % RING], m, 3);
    g.writePos.store(w + 1, std::memory_order_release);
}

}  /* namespace */

int  midiInPop(unsigned char *out)
{
    const int r = g.readPos.load(std::memory_order_relaxed);
    const int w = g.writePos.load(std::memory_order_acquire);
    if (r == w) return 0;
    std::memcpy(out, g.msg[r % RING], 3);
    g.readPos.store(r + 1, std::memory_order_release);
    return 1;
}

const char *midiInName() { return g.name; }

}  /* namespace bs */

/* ------------------------------------------------------------------ *
 * Linux: the ALSA sequencer
 *
 * The sequencer rather than rawmidi, because it is what every application on
 * the system connects through - a keyboard, a DAW's output, a virtual port
 * from something else. Subscribed to everything that announces itself, so a
 * device plugged in before the program started is simply there.
 * ------------------------------------------------------------------ */
#if defined(__linux__)

#include <alsa/asoundlib.h>
#include <poll.h>
#include <pthread.h>
#include <vector>

namespace bs {
namespace {

snd_seq_t   *g_seq   = 0;
int          g_port  = -1;
pthread_t    g_thread;
std::atomic<bool> g_run(false);

void *midiThread(void *)
{
    /* Polled with a timeout rather than blocked on a read.
     *
     * Blocking was the obvious thing and it was wrong: closing the sequencer
     * to wake the thread frees a structure the thread is sitting inside, and
     * the first version aborted with a corrupted heap the moment it shut
     * down. Now the thread owns its own exit - it notices g_run and returns,
     * and only then does anyone close anything. */
    const int nfds = snd_seq_poll_descriptors_count(g_seq, POLLIN);
    if (nfds <= 0) return 0;
    std::vector<struct pollfd> pfd((size_t)nfds);

    while (g_run.load(std::memory_order_acquire)) {
        snd_seq_poll_descriptors(g_seq, &pfd[0], (unsigned)nfds, POLLIN);
        if (poll(&pfd[0], (nfds_t)nfds, 100) <= 0) continue;

        /* Every event that is waiting, not one of them.
         *
         * ALSA reads from the descriptor into a buffer of its own, so several
         * events arrive together and only the first makes the socket
         * readable. Taking one per poll left the rest sitting in that buffer
         * until some later event happened to knock them loose - which looked
         * exactly like MIDI not working, because the note that woke the poll
         * was a subscription notice and the note-on was second in line. */
        while (snd_seq_event_input_pending(g_seq, 1) > 0) {
        snd_seq_event_t *ev = 0;
        if (snd_seq_event_input(g_seq, &ev) < 0 || !ev) break;

        unsigned char m[3] = { 0, 0, 0 };
        switch (ev->type) {
        case SND_SEQ_EVENT_NOTEON:
            m[0] = 0x90 | (ev->data.note.channel & 0x0f);
            m[1] = ev->data.note.note; m[2] = ev->data.note.velocity; break;
        case SND_SEQ_EVENT_NOTEOFF:
            m[0] = 0x80 | (ev->data.note.channel & 0x0f);
            m[1] = ev->data.note.note; m[2] = ev->data.note.velocity; break;
        case SND_SEQ_EVENT_CONTROLLER:
            m[0] = 0xb0 | (ev->data.control.channel & 0x0f);
            m[1] = (unsigned char)ev->data.control.param;
            m[2] = (unsigned char)ev->data.control.value; break;
        case SND_SEQ_EVENT_PITCHBEND: {
            /* ALSA centres this on zero; MIDI centres it on 8192. */
            const int v = ev->data.control.value + 8192;
            m[0] = 0xe0 | (ev->data.control.channel & 0x0f);
            m[1] = (unsigned char)(v & 0x7f);
            m[2] = (unsigned char)((v >> 7) & 0x7f); break;
        }
        case SND_SEQ_EVENT_CHANPRESS:
            m[0] = 0xd0 | (ev->data.control.channel & 0x0f);
            m[1] = (unsigned char)ev->data.control.value; break;
        default:
            continue;
        }
        push(m);
        }
    }
    return 0;
}

}  /* namespace */

int midiInOpen()
{
    if (g_seq) return g.ports;
    if (snd_seq_open(&g_seq, "default", SND_SEQ_OPEN_INPUT, 0) < 0) {
        g_seq = 0;
        return 0;
    }
    snd_seq_set_client_name(g_seq, "BENCsynth");
    g_port = snd_seq_create_simple_port(
        g_seq, "in",
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_APPLICATION | SND_SEQ_PORT_TYPE_MIDI_GENERIC);
    if (g_port < 0) { snd_seq_close(g_seq); g_seq = 0; return 0; }

    /* Connect to every hardware input that exists. Anything else on the
     * system can still connect to us by name afterwards. */
    snd_seq_client_info_t *cinfo;
    snd_seq_port_info_t   *pinfo;
    snd_seq_client_info_alloca(&cinfo);
    snd_seq_port_info_alloca(&pinfo);
    snd_seq_client_info_set_client(cinfo, -1);
    while (snd_seq_query_next_client(g_seq, cinfo) >= 0) {
        const int client = snd_seq_client_info_get_client(cinfo);
        if (client == snd_seq_client_id(g_seq) || client == 0) continue;
        snd_seq_port_info_set_client(pinfo, client);
        snd_seq_port_info_set_port(pinfo, -1);
        while (snd_seq_query_next_port(g_seq, pinfo) >= 0) {
            const unsigned caps = snd_seq_port_info_get_capability(pinfo);
            if ((caps & SND_SEQ_PORT_CAP_READ) == 0) continue;
            if ((caps & SND_SEQ_PORT_CAP_SUBS_READ) == 0) continue;
            if (snd_seq_connect_from(g_seq, g_port, client,
                                     snd_seq_port_info_get_port(pinfo)) < 0)
                continue;
            if (!g.name[0])
                std::snprintf(g.name, sizeof g.name, "%s",
                              snd_seq_client_info_get_name(cinfo));
            g.ports++;
        }
    }

    snd_seq_nonblock(g_seq, 1);
    g_run.store(true, std::memory_order_release);
    pthread_create(&g_thread, 0, midiThread, 0);
    return g.ports;
}

void midiInClose()
{
    if (!g_seq) return;
    /* Join before closing, never after: the thread reads through g_seq, and
     * freeing it underneath was a use-after-free that aborted on shutdown. */
    g_run.store(false, std::memory_order_release);
    pthread_join(g_thread, 0);
    snd_seq_close(g_seq);
    g_seq = 0;
    g.ports = 0;
    g.name[0] = 0;
}

}  /* namespace bs */

/* ------------------------------------------------------------------ *
 * Windows: winmm, already linked for audio
 * ------------------------------------------------------------------ */
#elif defined(_WIN32)

#include <windows.h>
#include <mmsystem.h>

namespace bs {
namespace {

HMIDIIN g_handles[16];
int     g_count = 0;

void CALLBACK midiProc(HMIDIIN, UINT msg, DWORD_PTR, DWORD_PTR p1, DWORD_PTR)
{
    if (msg != MIM_DATA) return;
    const unsigned char m[3] = { (unsigned char)(p1 & 0xff),
                                 (unsigned char)((p1 >> 8) & 0x7f),
                                 (unsigned char)((p1 >> 16) & 0x7f) };
    push(m);
}

}  /* namespace */

int midiInOpen()
{
    if (g_count) return g.ports;
    const UINT n = midiInGetNumDevs();
    for (UINT i = 0; i < n && g_count < 16; i++) {
        HMIDIIN h = 0;
        if (midiInOpen(&h, i, (DWORD_PTR)midiProc, 0, CALLBACK_FUNCTION) != MMSYSERR_NOERROR)
            continue;
        midiInStart(h);
        g_handles[g_count++] = h;
        if (!g.name[0]) {
            MIDIINCAPSA caps;
            if (midiInGetDevCapsA(i, &caps, sizeof caps) == MMSYSERR_NOERROR)
                std::snprintf(g.name, sizeof g.name, "%s", caps.szPname);
        }
        g.ports++;
    }
    return g.ports;
}

void midiInClose()
{
    for (int i = 0; i < g_count; i++) {
        midiInStop(g_handles[i]);
        midiInClose(g_handles[i]);
    }
    g_count = 0;
    g.ports = 0;
    g.name[0] = 0;
}

}  /* namespace bs */

/* ------------------------------------------------------------------ *
 * macOS: CoreMIDI
 * ------------------------------------------------------------------ */
#elif defined(__APPLE__)

#include <CoreMIDI/CoreMIDI.h>

namespace bs {
namespace {

MIDIClientRef g_client = 0;
MIDIPortRef   g_in     = 0;

void readProc(const MIDIPacketList *pkts, void *, void *)
{
    const MIDIPacket *p = &pkts->packet[0];
    for (unsigned i = 0; i < pkts->numPackets; i++) {
        /* A packet can hold several messages back to back. Only the three
         * lengths this program cares about are unpacked. */
        unsigned j = 0;
        while (j < p->length) {
            const unsigned char st = p->data[j];
            if (st < 0x80) { j++; continue; }
            unsigned need = 3;
            const unsigned char hi = st & 0xf0;
            if (hi == 0xc0 || hi == 0xd0) need = 2;
            if (st >= 0xf0) break;                  /* system: not ours */
            if (j + need > p->length) break;
            unsigned char m[3] = { st, 0, 0 };
            m[1] = p->data[j + 1];
            m[2] = need > 2 ? p->data[j + 2] : 0;
            push(m);
            j += need;
        }
        p = MIDIPacketNext(p);
    }
}

}  /* namespace */

int midiInOpen()
{
    if (g_client) return g.ports;
    if (MIDIClientCreate(CFSTR("BENCsynth"), 0, 0, &g_client) != noErr) return 0;
    if (MIDIInputPortCreate(g_client, CFSTR("in"), readProc, 0, &g_in) != noErr) {
        MIDIClientDispose(g_client);
        g_client = 0;
        return 0;
    }
    const ItemCount n = MIDIGetNumberOfSources();
    for (ItemCount i = 0; i < n; i++) {
        MIDIEndpointRef src = MIDIGetSource(i);
        if (!src) continue;
        if (MIDIPortConnectSource(g_in, src, 0) != noErr) continue;
        if (!g.name[0]) {
            CFStringRef nm = 0;
            if (MIDIObjectGetStringProperty(src, kMIDIPropertyDisplayName, &nm) == noErr && nm) {
                CFStringGetCString(nm, g.name, sizeof g.name, kCFStringEncodingUTF8);
                CFRelease(nm);
            }
        }
        g.ports++;
    }
    return g.ports;
}

void midiInClose()
{
    if (!g_client) return;
    MIDIPortDispose(g_in);
    MIDIClientDispose(g_client);
    g_client = 0; g_in = 0;
    g.ports = 0;
    g.name[0] = 0;
}

}  /* namespace bs */

#else

namespace bs {
int  midiInOpen()  { return 0; }
void midiInClose() {}
}

#endif
