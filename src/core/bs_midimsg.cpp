/*
 * BENCsynth - one MIDI message, applied. See bs_midimsg.h.
 */

#include "bs_midimsg.h"

namespace bs {

bool applyMidi(Engine &eng, MpeState &mpe, const unsigned char *m, int len,
               int atFrame, MidiMirror mirror, void *user)
{
    if (!m || len < 1) return false;
    const int ch = m[0] & 0x0f;

    switch (m[0] & 0xf0) {
    case 0x90:
        if (len < 3) return false;
        /* Note-on with zero velocity is a note-off, and has been since running
         * status made that cheaper to send. */
        if (m[2] > 0) {
            mpe.noteOn(ch, m[1]);
            eng.noteOn(m[1], (float)m[2] / 127.0f, atFrame);
            if (mirror) mirror(user, NE_NOTE_ON, m[1], (float)m[2] / 127.0f);
        } else {
            mpe.noteOff(ch, m[1]);
            eng.noteOff(m[1], atFrame);
            if (mirror) mirror(user, NE_NOTE_OFF, m[1], 0.0f);
        }
        return true;

    case 0x80:
        if (len < 2) return false;
        mpe.noteOff(ch, m[1]);
        eng.noteOff(m[1], atFrame);
        if (mirror) mirror(user, NE_NOTE_OFF, m[1], 0.0f);
        return true;

    case 0xe0: {
        if (len < 3) return false;
        const int raw = ((int)m[2] << 7) | (int)m[1];      /* 0..16383 */
        const float bend = (float)(raw - 8192) / 8192.0f;
        /* On a channel carrying one note, this bend is that note's - which is
         * all MPE is. Anywhere else it is the wheel. */
        const int mn = mpe.noteOn(ch);
        if (mn >= 0) {
            eng.noteExpression(mn, 0, bend * mpe.bendRange, atFrame);
        } else {
            eng.setBend(bend, atFrame);
            if (mirror) mirror(user, NE_BEND, 0, bend);
        }
        return true;
    }

    case 0xd0: {   /* channel pressure - per-note under MPE */
        if (len < 2) return false;
        const int mn = mpe.noteOn(ch);
        if (mn >= 0) eng.noteExpression(mn, 1, (float)m[1] / 127.0f, atFrame);
        return true;
    }

    case 0xb0:
        if (len < 3) return false;
        if (m[1] == 1) {
            eng.setMod((float)m[2] / 127.0f, atFrame);
            if (mirror) mirror(user, NE_MOD, 0, (float)m[2] / 127.0f);
        } else if (m[1] == 74) {
            /* The Y axis. MPE puts it here by convention. */
            const int mn = mpe.noteOn(ch);
            if (mn >= 0) eng.noteExpression(mn, 2, (float)m[2] / 127.0f, atFrame);
        } else if (m[1] == 64) {
            eng.setSustain(m[2] >= 64, atFrame);
            if (mirror) mirror(user, NE_SUSTAIN, 0, m[2] >= 64 ? 1.0f : 0.0f);
        } else if (m[1] == 123 || m[1] == 120) {
            eng.panic();
            mpe.reset();
            if (mirror) mirror(user, NE_ALL_OFF, 0, 0.0f);
        } else {
            return false;
        }
        return true;

    default:
        return false;
    }
}

}  /* namespace bs */
