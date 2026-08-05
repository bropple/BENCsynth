/*
 * BENCsynth - DSP primitives
 *
 * Header-only, freestanding, and deliberately free of any notion of a module,
 * a patch or a GUI. Everything here is a small struct with state and a step()
 * that advances it by one sample. The module layer above composes these; the
 * plugin wrapper that eventually exists will link the same code.
 *
 * Voltage conventions, borrowed from hardware modular and kept consistently
 * everywhere above this file:
 *
 *   audio      +/-5 V nominal
 *   V/oct      0 V is middle C, one volt per octave
 *   gate       0 V low, 10 V high
 *   envelope   0..10 V
 *   bipolar CV +/-5 V
 *
 * Sticking to volts rather than normalised floats is what makes an
 * attenuverter, an offset and a mixer behave the way the panel says they will
 * regardless of what is plugged into them.
 */

#ifndef BS_DSP_H
#define BS_DSP_H

#include <cmath>
#include <cstdint>

namespace bs {

/* ------------------------------------------------------------------ *
 * Scalar helpers
 * ------------------------------------------------------------------ */

inline float clampf(float x, float lo, float hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

/* Wraps into [0,1) for one-turn-at-most excursions, which is all a phase
 * accumulator ever needs. fmodf would also work and costs more. */
inline float wrap01(float x)
{
    while (x >= 1.0f) x -= 1.0f;
    while (x <  0.0f) x += 1.0f;
    return x;
}

/* Middle C. MIDI note 60, and the zero point of the V/oct scale. */
static const float BS_C4 = 261.6255653f;

inline float voltsToHz(float v)   { return BS_C4 * std::exp2(v); }
inline float noteToVolts(float n) { return (n - 60.0f) * (1.0f / 12.0f); }
inline float hzToVolts(float hz)  { return std::log2(hz / BS_C4); }

/* A cheap tanh that keeps the shape that matters - unit slope at zero, hard
 * flattening past |2| - without the libm call in the inner loop of a filter
 * that evaluates it four times per sample. */
inline float softClip(float x)
{
    if (x < -3.0f) return -1.0f;
    if (x >  3.0f) return  1.0f;
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/* ------------------------------------------------------------------ *
 * Band-limited step and ramp
 *
 * A naive saw or square is a discontinuity dropped into a sampled signal, and
 * the harmonics it needs above Nyquist come back as inharmonic aliases -
 * audible as a metallic ringing that slides the wrong way as you play up the
 * keyboard. polyBLEP subtracts a two-sample approximation of that step's
 * band-limited counterpart, which is not perfect but costs almost nothing and
 * removes the worst of it.
 *
 * polyBLAMP is the same trick one derivative up, for the corners of a
 * triangle, where the value is continuous but the slope is not.
 * ------------------------------------------------------------------ */

inline float polyblep(float t, float dt)
{
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

inline float polyblamp(float t, float dt)
{
    if (t < dt) {
        t = t / dt - 1.0f;
        return -t * t * t * (1.0f / 3.0f);
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt + 1.0f;
        return t * t * t * (1.0f / 3.0f);
    }
    return 0.0f;
}

/* ------------------------------------------------------------------ *
 * Oscillator
 *
 * One phase accumulator producing all four shapes at once, which is what a
 * 921-style hardware VCO does - the shapes are simultaneous outputs of one
 * core, not a waveform selector switch. That is why the module has four jacks
 * rather than one jack and a shape knob.
 * ------------------------------------------------------------------ */

struct BlepOsc {
    float phase;
    float sync_last;     /* previous sync input, for edge detection */

    BlepOsc() : phase(0.0f), sync_last(0.0f) {}

    void reset() { phase = 0.0f; sync_last = 0.0f; }

    /* dt is cycles per sample (frequency / sample rate); pw is pulse width in
     * (0,1). Outputs are +/-1 and are scaled to volts by the caller. */
    void step(float dt, float pw,
              float *saw, float *pulse, float *tri, float *sine)
    {
        dt = clampf(dt, 0.0f, 0.45f);
        pw = clampf(pw, 0.02f, 0.98f);

        phase += dt;
        if (phase >= 1.0f) phase -= 1.0f;

        const float p = phase;

        if (saw) {
            float s = 2.0f * p - 1.0f;
            s -= polyblep(p, dt);
            *saw = s;
        }
        if (pulse) {
            float s = (p < pw) ? 1.0f : -1.0f;
            s += polyblep(p, dt);
            s -= polyblep(wrap01(p - pw), dt);
            *pulse = s;
        }
        if (tri) {
            /* 4*|p-0.5|-1 peaks at +1 when p is 0 and at -1 when p is 0.5, so
             * the slope goes +4 -> -4 at the first corner and -4 -> +4 at the
             * second. The correction carries the sign of that change. */
            float s = 4.0f * std::fabs(p - 0.5f) - 1.0f;
            s -= 8.0f * dt * polyblamp(p, dt);
            s += 8.0f * dt * polyblamp(wrap01(p + 0.5f), dt);
            *tri = s;
        }
        if (sine) {
            *sine = std::sin(6.2831853071795864f * p);
        }
    }

    /* Hard sync: a rising edge through 1 V restarts the cycle. Returns true on
     * the frame it fired, so the caller can suppress the blep it would
     * otherwise have inserted at the natural wrap. */
    bool syncEdge(float in)
    {
        const bool rise = (sync_last < 1.0f && in >= 1.0f);
        sync_last = in;
        if (rise) phase = 0.0f;
        return rise;
    }
};

/* ------------------------------------------------------------------ *
 * Moog ladder filter, topology-preserving transform
 *
 * Four one-pole lowpasses in series with a global feedback path. Solved for
 * the instantaneous feedback rather than delaying it by a sample, which is
 * what keeps the resonance in tune and stable as the cutoff sweeps - the
 * naive one-sample-delay ladder detunes badly at high cutoff and blows up
 * when you push resonance and cutoff at once, and that combination is
 * precisely what people reach for this filter to do.
 *
 * Saturation sits in the feedback path, so the self-oscillation limits itself
 * instead of running away, and driving the input harder thickens the tone
 * rather than clipping the output stage.
 * ------------------------------------------------------------------ */

struct Ladder {
    float s[4];
    float sr;
    /* The two-pole tap, left behind by the last step(). A real ladder brings
     * the intermediate stages out to their own jacks and the gentler slope is
     * a different instrument, so there is no reason to make the caller build a
     * second filter to get at it. */
    float tap2;

    Ladder() : sr(48000.0f), tap2(0.0f) { reset(); }

    void reset() { s[0] = s[1] = s[2] = s[3] = 0.0f; tap2 = 0.0f; }
    void setSampleRate(float rate) { sr = rate; reset(); }

    /* cutoff in Hz, res 0..1 where 1 is the edge of self-oscillation,
     * drive is a linear pre-gain. */
    float step(float in, float cutoff, float res, float drive)
    {
        cutoff = clampf(cutoff, 8.0f, sr * 0.45f);

        const float g = std::tan(3.14159265358979f * cutoff / sr);
        const float G = g / (1.0f + g);
        const float k = clampf(res, 0.0f, 1.08f) * 4.0f;

        /* Resonance thins the low end on a real ladder because the feedback
         * subtracts the lowpassed signal from the input. Half-compensating
         * keeps that character audible without the patch losing its bottom
         * every time the knob moves. */
        const float x = softClip(in * drive) * (1.0f + 0.5f * k * 0.25f);

        const float G2 = G * G, G3 = G2 * G, G4 = G3 * G;
        const float z0 = (1.0f - G) * s[0];
        const float z1 = (1.0f - G) * s[1];
        const float z2 = (1.0f - G) * s[2];
        const float z3 = (1.0f - G) * s[3];

        /* Saturating the state term is what keeps the loop bounded past the
         * point where a linear ladder runs away. Above k = 4 the linear
         * system has poles outside the unit circle and the exact solve below
         * will happily follow them to infinity; compressing what comes back
         * round the loop turns that into a limit cycle instead, which is what
         * self-oscillation is on the hardware and why it settles at a level
         * rather than destroying the speaker. The instantaneous part of the
         * feedback stays linear because the solve accounts for it exactly. */
        const float S = softClip(G3 * z0 + G2 * z1 + G * z2 + z3);
        const float u = (x - k * S) / (1.0f + k * G4);

        float y = u, v;
        for (int i = 0; i < 4; i++) {
            v    = G * (y - s[i]);
            y    = v + s[i];
            s[i] = y + v;
            if (i == 1) tap2 = y;
        }
        return y;
    }
};

/* One-pole lowpass, for tone controls and CV smoothing where a ladder is
 * four times more filter than the job needs. */
struct OnePole {
    float z, a;
    OnePole() : z(0.0f), a(0.5f) {}
    void setCutoff(float hz, float sr)
    {
        const float g = std::tan(3.14159265358979f * clampf(hz, 1.0f, sr * 0.45f) / sr);
        a = g / (1.0f + g);
    }
    float lp(float x) { z += a * (x - z); return z; }
    float hp(float x) { return x - lp(x); }
    void reset() { z = 0.0f; }
};

/* Removes the DC a saturating filter or an asymmetric waveform leaves behind,
 * which otherwise eats headroom at the output stage without being audible on
 * its own. */
struct DCBlock {
    float x1, y1, r;
    DCBlock() : x1(0.0f), y1(0.0f), r(0.9995f) {}
    void setSampleRate(float sr) { r = 1.0f - 20.0f / sr; }
    float step(float x) { const float y = x - x1 + r * y1; x1 = x; y1 = y; return y; }
};

/* ------------------------------------------------------------------ *
 * Envelope
 *
 * Analogue envelopes charge a capacitor toward a rail and are stopped at the
 * threshold, so the segments are exponential and the attack is fastest at the
 * start. Aiming past the target and cutting off early is what produces that
 * shape; a linear ramp to the target instead gives the flat, synthetic attack
 * that digital envelopes are recognisable by.
 * ------------------------------------------------------------------ */

struct ADSR {
    enum Stage { IDLE, ATTACK, DECAY, SUSTAIN, RELEASE };

    float value;
    int   stage;
    float sr;

    ADSR() : value(0.0f), stage(IDLE), sr(48000.0f) {}

    void setSampleRate(float rate) { sr = rate; }
    void reset() { value = 0.0f; stage = IDLE; }

    void gate(bool on)
    {
        if (on) {
            if (stage == IDLE || stage == RELEASE) stage = ATTACK;
        } else if (stage != IDLE) {
            stage = RELEASE;
        }
    }

    /* Retriggering from the top rather than from zero: a legato line that
     * retriggers should not click, and a hardware envelope does not reset its
     * capacitor either. */
    void retrigger() { stage = ATTACK; }

    /* Times in seconds, sustain 0..1. */
    float step(float a, float d, float s, float r)
    {
        /* 1 - exp(-1/(t*sr)) is the per-sample coefficient of a one-pole
         * settling to 63% in t seconds; the overshoot targets below turn that
         * into a segment that *finishes* in roughly t. */
        switch (stage) {
        case ATTACK: {
            const float k = 1.0f - std::exp(-1.0f / (0.3f * a * sr + 1.0f));
            value += k * (1.2f - value);
            if (value >= 1.0f) { value = 1.0f; stage = DECAY; }
            break;
        }
        case DECAY: {
            const float k = 1.0f - std::exp(-1.0f / (0.3f * d * sr + 1.0f));
            value += k * (s - 0.08f - value);
            if (value <= s + 0.001f) { value = s; stage = SUSTAIN; }
            break;
        }
        case SUSTAIN:
            value += 0.02f * (s - value);
            break;
        case RELEASE: {
            const float k = 1.0f - std::exp(-1.0f / (0.3f * r * sr + 1.0f));
            value += k * (-0.08f - value);
            if (value <= 0.0005f) { value = 0.0f; stage = IDLE; }
            break;
        }
        default:
            value = 0.0f;
            break;
        }
        return clampf(value, 0.0f, 1.0f);
    }

    bool active() const { return stage != IDLE; }
};

/* ------------------------------------------------------------------ *
 * Noise
 * ------------------------------------------------------------------ */

struct Noise {
    uint32_t state;
    float b0, b1, b2, b3, b4, b5, b6;

    Noise() : state(0x1234567u), b0(0), b1(0), b2(0), b3(0), b4(0), b5(0), b6(0) {}

    void seed(uint32_t s) { state = s ? s : 0x1234567u; }

    float white()
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return (float)(int32_t)state * (1.0f / 2147483648.0f);
    }

    /* Paul Kellet's economy pink filter: a bank of one-poles whose corner
     * frequencies are spaced to approximate a 3 dB/octave slope. */
    float pink()
    {
        const float w = white();
        b0 = 0.99886f * b0 + w * 0.0555179f;
        b1 = 0.99332f * b1 + w * 0.0750759f;
        b2 = 0.96900f * b2 + w * 0.1538520f;
        b3 = 0.86650f * b3 + w * 0.3104856f;
        b4 = 0.55000f * b4 + w * 0.5329522f;
        b5 = -0.7616f * b5 - w * 0.0168980f;
        const float out = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362f;
        b6 = w * 0.115926f;
        return out * 0.15f;
    }
};

/* ------------------------------------------------------------------ *
 * Delay line with fractional read
 * ------------------------------------------------------------------ */

struct Delay {
    float *buf;
    int    size, w;

    Delay() : buf(0), size(0), w(0) {}
    ~Delay() { delete[] buf; }

    void alloc(int frames)
    {
        delete[] buf;
        size = frames < 2 ? 2 : frames;
        buf  = new float[size];
        clear();
    }
    void clear() { for (int i = 0; i < size; i++) buf[i] = 0.0f; w = 0; }

    void write(float x) { buf[w] = x; if (++w >= size) w = 0; }

    float read(float delaySamples) const
    {
        float d = clampf(delaySamples, 1.0f, (float)(size - 2));
        float rp = (float)w - d;
        while (rp < 0.0f) rp += (float)size;
        const int   i = (int)rp;
        const float f = rp - (float)i;
        const int   j = (i + 1 >= size) ? 0 : i + 1;
        return lerpf(buf[i], buf[j], f);
    }

private:
    Delay(const Delay &);
    Delay &operator=(const Delay &);
};

/* ------------------------------------------------------------------ *
 * Reverb - four combs into two allpasses, per side
 *
 * A Schroeder reverb rather than anything larger. It is the size of algorithm
 * that fits a rack module: enough to put a patch in a room, not so much that
 * it becomes the instrument.
 * ------------------------------------------------------------------ */

struct Reverb {
    static const int NCOMB = 4;
    static const int NAP   = 2;

    Delay comb[2][NCOMB];
    Delay ap[2][NAP];
    float combZ[2][NCOMB];
    float combLen[NCOMB];
    float apLen[NAP];
    float sr;

    Reverb() : sr(48000.0f) {}

    void setSampleRate(float rate)
    {
        sr = rate;
        /* Mutually prime-ish lengths in milliseconds, so the comb resonances
         * do not pile up on the same frequencies. */
        static const float cms[NCOMB] = { 29.7f, 37.1f, 41.1f, 43.7f };
        static const float ams[NAP]   = { 5.0f, 1.7f };
        for (int side = 0; side < 2; side++) {
            /* A few samples of offset between channels is what makes the tail
             * stereo without a second set of constants. */
            const float skew = side ? 1.021f : 1.0f;
            for (int i = 0; i < NCOMB; i++) {
                combLen[i] = cms[i] * 0.001f * sr;
                comb[side][i].alloc((int)(combLen[i] * skew) + 8);
                combZ[side][i] = 0.0f;
            }
            for (int i = 0; i < NAP; i++) {
                apLen[i] = ams[i] * 0.001f * sr;
                ap[side][i].alloc((int)(apLen[i] * skew) + 8);
            }
        }
    }

    void clear()
    {
        for (int side = 0; side < 2; side++) {
            for (int i = 0; i < NCOMB; i++) { comb[side][i].clear(); combZ[side][i] = 0.0f; }
            for (int i = 0; i < NAP; i++) ap[side][i].clear();
        }
    }

    /* size 0..1 maps to feedback, damp 0..1 rolls the tail off. */
    void step(float in, float size, float damp, float *outL, float *outR)
    {
        const float fb = 0.7f + 0.28f * clampf(size, 0.0f, 1.0f);
        const float d  = clampf(damp, 0.0f, 0.95f);
        float out[2] = { 0.0f, 0.0f };

        for (int side = 0; side < 2; side++) {
            const float skew = side ? 1.021f : 1.0f;
            float acc = 0.0f;
            for (int i = 0; i < NCOMB; i++) {
                const float y = comb[side][i].read(combLen[i] * skew);
                combZ[side][i] = lerpf(y, combZ[side][i], d);
                comb[side][i].write(in + combZ[side][i] * fb);
                acc += y;
            }
            acc *= 0.25f;
            for (int i = 0; i < NAP; i++) {
                const float bufOut = ap[side][i].read(apLen[i] * skew);
                const float v      = acc + bufOut * 0.5f;
                ap[side][i].write(v);
                acc = bufOut - v * 0.5f;
            }
            out[side] = acc;
        }
        *outL = out[0];
        *outR = out[1];
    }
};

/* ------------------------------------------------------------------ *
 * Portamento / slew
 * ------------------------------------------------------------------ */

struct Slew {
    float value;
    Slew() : value(0.0f) {}
    void reset(float v) { value = v; }
    /* time is seconds to traverse one octave; 0 is instant. */
    float step(float target, float time, float sr)
    {
        if (time <= 0.0001f) { value = target; return value; }
        const float k = 1.0f - std::exp(-1.0f / (0.3f * time * sr + 1.0f));
        value += k * (target - value);
        return value;
    }
};

} /* namespace bs */

#endif /* BS_DSP_H */
