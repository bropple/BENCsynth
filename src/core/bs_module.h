/*
 * BENCsynth - module and signal model
 *
 * A module is a panel with knobs, input jacks and output jacks, and a process()
 * that fills its outputs from its inputs once per block. That is the whole
 * abstraction. Nothing here knows about drawing, and nothing here allocates
 * during process(), so the same objects that back the rack on screen can be
 * driven from a plugin's audio callback unchanged.
 *
 * Polyphony is carried in the cables rather than in the graph. A signal is not
 * one value per sample but up to BS_MAX_POLY of them, and a module processes
 * however many channels arrive at its inputs. This is how a modular can be
 * polyphonic at all without the patch being duplicated eight times: one VCO
 * panel, one set of knobs, eight independent oscillators inside it, because
 * eight pitches came down the cable. The keyboard module is where the channel
 * count is decided, and everything downstream inherits it.
 */

#ifndef BS_MODULE_H
#define BS_MODULE_H

#include "bs_dsp.h"

#include <string>
#include <vector>

namespace bs {

class KeyboardState;

enum {
    BS_BLOCK    = 32,   /* frames per process() call                        */
    BS_MAX_POLY = 8,    /* channels a cable can carry                       */
    /* How many controls the MACRO module offers, and so how many parameters a
     * plugin wrapper can promise a host before it knows what rack it will be
     * asked to hold. */
    BS_MACROS   = 8
};

/* ------------------------------------------------------------------ *
 * Signal
 * ------------------------------------------------------------------ */

struct Signal {
    float v[BS_MAX_POLY][BS_BLOCK];
    int   channels;

    Signal() : channels(1) { clear(); }

    void clear()
    {
        for (int c = 0; c < BS_MAX_POLY; c++)
            for (int i = 0; i < BS_BLOCK; i++) v[c][i] = 0.0f;
    }

    void setChannels(int n)
    {
        channels = n < 1 ? 1 : (n > BS_MAX_POLY ? BS_MAX_POLY : n);
    }

    /* Reading channel c of a signal that has fewer than c+1 of them returns
     * channel 0. That is what makes a single LFO patched into eight voices do
     * the obvious thing rather than modulating only the first one. */
    float get(int c, int i) const { return v[c < channels ? c : 0][i]; }

    /* Every channel summed, for the modules that are monophonic by nature -
     * the output stage, the delay, the reverb. */
    float sum(int i) const
    {
        float s = 0.0f;
        for (int c = 0; c < channels; c++) s += v[c][i];
        return s;
    }
};

/* What the host's transport is doing, when there is one.
 *
 * A modular has no idea what tempo anything is at, which is fine on a bench
 * and wrong in a session: a delay set to 0.352 s and a clock set to 130 BPM
 * are both approximately right and both drift against the bar within a few
 * seconds. The plugin fills this in once per block from the host; the
 * standalone leaves bpm at zero, which every module reads as "no host, use
 * your own knob".
 */
struct Transport {
    float bpm;        /* zero when nothing is telling us */
    int   playing;
    Transport() : bpm(0.0f), playing(0) {}
};

/* The signal an unpatched input reads. Silence, permanently. Having one of
 * these means process() never has to branch on whether a jack is connected
 * except where the distinction actually matters musically. */
const Signal &zeroSignal();

/* ------------------------------------------------------------------ *
 * Parameters
 * ------------------------------------------------------------------ */

enum ParamKind {
    PK_KNOB,     /* continuous, drawn as a knob with a readout           */
    PK_SWITCH,   /* one of N named positions                             */
    PK_TOGGLE    /* off/on, drawn as a latching button                   */
};

enum ParamCurve {
    PC_LIN,      /* knob travel is proportional to value                 */
    PC_EXP,      /* constant ratio per degree - cutoffs, rates, times    */
    PC_QUAD      /* squared - envelope times, where the useful range is
                  * bunched at the bottom but the top has to reach       */
};

struct Param {
    const char *name;
    float lo, hi, def, value;
    int   kind;
    int   curve;
    const char *fmt;    /* printf format for the readout, value substituted */
    const char *const *options;
    int   optionCount;
    /* Snap increment. Zero is continuous; an octave switch drawn as a knob
     * wants 1, a semitone trim wants 1, a fine trim wants nothing. Applied on
     * the way in from the widget, so the stored value is always a value the
     * panel could display exactly. */
    float quant;

    Param()
        : name(""), lo(0.0f), hi(1.0f), def(0.0f), value(0.0f),
          kind(PK_KNOB), curve(PC_LIN), fmt("%.2f"),
          options(0), optionCount(0), quant(0.0f) {}

    /* Knob travel, 0..1. The curve lives here rather than in the widget so
     * that a preset stores the value a person would read off the panel, not
     * the position of a control that happened to draw it. */
    float norm() const
    {
        if (hi <= lo) return 0.0f;
        switch (curve) {
        case PC_EXP:
            if (lo > 0.0f) return std::log(value / lo) / std::log(hi / lo);
            break;
        case PC_QUAD: {
            const float t = (value - lo) / (hi - lo);
            return std::sqrt(t < 0.0f ? 0.0f : t);
        }
        default: break;
        }
        return (value - lo) / (hi - lo);
    }

    void setNorm(float t)
    {
        t = clampf(t, 0.0f, 1.0f);
        bool done = false;
        switch (curve) {
        case PC_EXP:
            if (lo > 0.0f) { value = lo * std::pow(hi / lo, t); done = true; }
            break;
        case PC_QUAD:
            value = lo + (hi - lo) * t * t;
            done = true;
            break;
        default: break;
        }
        if (!done) value = lo + (hi - lo) * t;
        if (quant > 0.0f)
            value = lo + quant * std::floor((value - lo) / quant + 0.5f);
        value = clampf(value, lo, hi);
    }

    int   asInt() const { return (int)(value + 0.5f); }
    bool  on() const { return value >= 0.5f; }
};

struct PortInfo {
    const char *name;    /* short, uppercase, fits under a jack */
};

/* ------------------------------------------------------------------ *
 * Module
 * ------------------------------------------------------------------ */

class Module {
public:
    Module()
        : hp(6), cols(2), sr(48000.0f), st(1.0f / 48000.0f), xport(0),
          id(-1), x(0.0f), y(0.0f) {}
    virtual ~Module() {}

    /* Fills every output signal with BS_BLOCK frames. Called once per block,
     * in dependency order, from the audio thread. Must not allocate. */
    virtual void process() = 0;

    /* Clears every bit of state that is not a parameter. */
    virtual void reset() {}

    void setSampleRate(float rate)
    {
        sr = rate;
        st = 1.0f / rate;
        onSampleRate();
        reset();
    }

    /* ---- construction helpers, called from a subclass constructor ---- */

    void configure(const char *type, const char *name, int widthUnits, int columns = 2)
    {
        typeId = type;
        title  = name;
        hp     = widthUnits;
        cols   = columns;
    }

    int addKnob(const char *name, float lo, float hi, float def,
                const char *fmt = "%.2f", int curve = PC_LIN, float quant = 0.0f)
    {
        Param p;
        p.name = name; p.lo = lo; p.hi = hi; p.def = def; p.value = def;
        p.kind = PK_KNOB; p.curve = curve; p.fmt = fmt; p.quant = quant;
        params.push_back(p);
        return (int)params.size() - 1;
    }

    int addSwitch(const char *name, const char *const *opts, int count, int def)
    {
        Param p;
        p.name = name; p.lo = 0.0f; p.hi = (float)(count - 1);
        p.def = (float)def; p.value = (float)def;
        p.kind = PK_SWITCH; p.options = opts; p.optionCount = count;
        p.fmt = "%.0f";
        params.push_back(p);
        return (int)params.size() - 1;
    }

    int addToggle(const char *name, bool def)
    {
        Param p;
        p.name = name; p.lo = 0.0f; p.hi = 1.0f;
        p.def = def ? 1.0f : 0.0f; p.value = p.def;
        p.kind = PK_TOGGLE; p.fmt = "%.0f";
        params.push_back(p);
        return (int)params.size() - 1;
    }

    int addInput(const char *name)
    {
        PortInfo pi; pi.name = name;
        inInfo.push_back(pi);
        ins.push_back(0);
        return (int)inInfo.size() - 1;
    }

    int addOutput(const char *name)
    {
        PortInfo pi; pi.name = name;
        outInfo.push_back(pi);
        outs.push_back(Signal());
        return (int)outInfo.size() - 1;
    }

    /* ---- process()-time accessors ---- */

    float pv(int i) const { return params[(size_t)i].value; }
    int   pi(int i) const { return params[(size_t)i].asInt(); }
    bool  pb(int i) const { return params[(size_t)i].on(); }

    bool patched(int i) const { return ins[(size_t)i] != 0; }
    const Signal &in(int i) const
    {
        const Signal *s = ins[(size_t)i];
        return s ? *s : zeroSignal();
    }
    Signal &out(int i) { return outs[(size_t)i]; }

    /* The channel count this module should run at: the widest thing plugged
     * into it, and at least one. */
    int polyIn() const
    {
        int n = 1;
        for (size_t i = 0; i < ins.size(); i++)
            if (ins[i] && ins[i]->channels > n) n = ins[i]->channels;
        return n;
    }

    void setAllOutputChannels(int n)
    {
        for (size_t i = 0; i < outs.size(); i++) outs[i].setChannels(n);
    }

    void resetParams()
    {
        for (size_t i = 0; i < params.size(); i++) params[i].value = params[i].def;
    }

    /* The output stage is the one module the engine has to reach into: the
     * rack is a graph with no privileged node, but something has to hand two
     * channels to the sound card. A module that is an output returns its
     * block here and the engine sums whatever it finds, so a patch with two
     * output modules, or none, is a patch rather than an error. */
    virtual const float *masterL() const { return 0; }
    virtual const float *masterR() const { return 0; }

    /* Where played notes come from. The engine calls this on every module it
     * adds; all but the keyboard interface ignore it. */
    virtual void bindKeys(KeyboardState *) {}

    /* Panel space this module needs beyond its knobs and jacks - a display,
     * a meter. Zero for everything that is only controls. */
    virtual int extraPanelHeight() const { return 0; }

    /* A module that holds free text - a scratchpad - returns it here, so that
     * saving a patch carries it and the rack knows to draw an editor. Null for
     * everything else, which is everything else. */
    virtual std::string *textBuffer() { return 0; }

    /* Called after something outside has written to that buffer - the patch
     * loader, or the interface. A module whose text names a file on disk
     * reloads here, on whatever thread did the writing, which is never the
     * audio thread. */
    virtual void onTextChanged() {}

    /* Whether that worked, for a module whose text names a file. Empty means
     * nothing to report; a module with no file always says nothing. */
    virtual bool        textLoaded() const { return true; }
    virtual const char *textStatus() const { return ""; }

    int inputCount() const  { return (int)inInfo.size(); }
    int outputCount() const { return (int)outInfo.size(); }
    int paramCount() const  { return (int)params.size(); }

    /* ---- data ---- */

    std::string typeId;   /* stable across versions; what a patch file stores */
    std::string title;    /* what the panel says                             */
    int   hp;             /* panel width in rack units                       */
    int   cols;           /* knob columns, for the panel auto-layout         */

    std::vector<Param>    params;
    std::vector<PortInfo> inInfo, outInfo;
    std::vector<Signal>   outs;
    std::vector<const Signal *> ins;   /* null when the jack is empty */

    float sr, st;

    /* Borrowed from the patch, which owns it. Null only before a module is
     * added to one. */
    const Transport *xport;

    /* The host's tempo, or the fallback if there is no host. Modules ask for
     * it this way so none of them has to know whether one exists. */
    float hostBpm(float fallback) const
    {
        return (xport && xport->bpm > 1.0f) ? xport->bpm : fallback;
    }

    /* Identity and rack position. Placement is not a DSP concern, but it is a
     * property of this module rather than of a parallel structure the GUI
     * would have to keep in step through every add and delete, and it is what
     * a saved patch has to restore alongside the cables. */
    int   id;
    float x, y;

protected:
    virtual void onSampleRate() {}
};

/* ------------------------------------------------------------------ *
 * Panel geometry
 *
 * A panel is laid out from the module's own declarations rather than from a
 * second description of where each control goes: knobs in a grid `cols` wide,
 * then the input jacks, then the output jacks. Fourteen modules would
 * otherwise be fourteen hand-placed layouts to keep in step with fourteen
 * constructors, and they would drift apart the first time a knob was added.
 *
 * It lives here, next to the declarations it reads, because the rack is not
 * the only thing that needs the answer - the default patch has to know how
 * tall a module is to place the row beneath it, and a second copy of these
 * constants in the GUI is a second copy that can be wrong.
 * ------------------------------------------------------------------ */

enum {
    BS_HP          = 20,   /* pixels per rack unit of panel width  */
    BS_PANEL_TOP   = 28,   /* title bar                            */
    BS_KNOB_ROW    = 54,
    BS_JACK_ROW    = 42,
    BS_JACK_GAP    = 10,   /* the rule between inputs and outputs  */
    BS_PANEL_PAD   = 10
};

inline int panelJackCols(const Module &m)
{
    int c = m.hp / 2;
    if (c < 2) c = 2;
    if (c > 4) c = 4;
    return c;
}

inline int panelWidth(const Module &m) { return m.hp * BS_HP; }

inline int panelHeight(const Module &m)
{
    const int cols = m.cols < 1 ? 1 : m.cols;
    const int jc   = panelJackCols(m);
    const int krow = (m.paramCount() + cols - 1) / cols;
    const int irow = (m.inputCount()  + jc - 1) / jc;
    const int orow = (m.outputCount() + jc - 1) / jc;

    int h = BS_PANEL_TOP + BS_PANEL_PAD;
    h += krow * BS_KNOB_ROW;
    h += m.extraPanelHeight();
    if (irow) h += irow * BS_JACK_ROW;
    if (irow && orow) h += BS_JACK_GAP;
    if (orow) h += orow * BS_JACK_ROW;
    h += BS_PANEL_PAD;
    return h;
}

/* ------------------------------------------------------------------ *
 * Registry
 * ------------------------------------------------------------------ */

struct ModuleType {
    const char *id;
    const char *name;     /* menu label */
    const char *group;    /* menu section */
    Module *(*make)();
};

int               moduleTypeCount();
const ModuleType *moduleTypeAt(int i);
Module           *createModule(const char *typeId);

} /* namespace bs */

#endif /* BS_MODULE_H */
