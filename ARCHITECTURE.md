# BENCsynth architecture

## The split that matters

```
src/core/     bs_dsp.h      primitives: oscillator, ladder, envelope, delay
              bs_module.h   Module, Param, Signal, panel geometry, registry
              bs_keys.h     note allocation
              bs_patch.*    modules, cables, evaluation order
              bs_modules.*  the fourteen panels
              bs_patchfile.*save and load
              bs_engine.*   the object a host holds

src/gui/      bs_gui.*      theme and widgets
              bs_rope.*     patch cable physics
              bs_rack.*     the rack: layout, patching, drawing
              bs_keyboard.* the keys and the wheels
              main.cpp      window, audio device, arrangement
```

`src/core` does not include raylib, does not allocate during `process()`, and
has no global state. That is not tidiness for its own sake — it is the entire
requirement for the plugin build, and it is much cheaper to hold to from the
start than to retrofit. `make core` and `make test` build without raylib
present at all, which is what keeps the rule honest.

`bs::Engine` is the whole public surface: give it a sample rate, hand it note
events, call `render()`. The standalone program and a future plugin differ only
in who calls those and where the notes come from.

## Signals

A signal is `float v[8][32]` plus a channel count: eight polyphonic channels of
a thirty-two frame block.

The block size is fixed and has nothing to do with what the audio device asks
for; `Engine::render` keeps whatever is left over between calls. Thirty-two
frames is short enough that control-rate modulation is not audibly stepped and
long enough that per-block overhead disappears.

**Polyphony lives in the cables.** The keyboard module decides the channel
count, and every module downstream runs at whatever width arrives at its
inputs. One VCO panel is eight oscillators when eight keys are down. The
alternative — instantiating the whole patch once per voice — means eight
copies of every knob to keep in step, and a module like the delay that is
sensibly monophonic has nowhere to live. Here it just sums its input channels
and says so in a comment.

Reading channel `c` of a signal that has fewer than `c+1` of them returns
channel 0. That single line is what makes one LFO patched into eight voices do
the obvious thing.

## Voltages

Audio ±5 V, one volt per octave with 0 V at middle C, gates 0/10 V, envelopes
0–10 V, bipolar CV ±5 V.

Working in volts rather than in normalised floats is what lets an attenuverter,
an offset and a mixer compose. A CV of "0.35" means nothing until you know what
produced it; 3.5 V into a filter with a CV depth of 0.28 octaves per volt is
just under an octave, and both ends of that sentence are on the panel.

## The graph

Connecting an output to an input points the input's `const Signal *` at the
output's buffer. Nothing is copied per block, an unpatched jack costs nothing,
and a cable is exactly the declaration it looks like: *this jack reads that
one*.

Evaluation order is a topological sort, rebuilt whenever the shape of the graph
changes. Feedback patches are legal and common — a delay into its own input, a
filter self-modulating — so a cycle cannot be an error. What the sort cannot
place is appended in id order, which means one cable in each cycle is read a
block late: 32 samples, under a millisecond, and the same compromise a hardware
patch makes with the propagation time of the cable.

Module ids and cable ids are slot indices, and a freed slot is reused
immediately. Anything caching per-cable state has to key on something else —
the rack keys its ropes on both endpoints, so a reused slot is recognised as a
different cable and hung afresh rather than snapping across the rack from
wherever the last one ended.

## Threading

One mutex, on the engine.

Structural changes — adding a module, deleting one, connecting, disconnecting —
take it, because each of them either reallocates, frees a buffer an input still
points at, or rewrites a pointer `process()` is about to read. The audio
callback takes it for the duration of a block.

Parameter knobs take nothing. A float written by the interface while the audio
thread reads it yields either the old value or the new one, and both are fine.
Making knob movement lock-free is the difference between a mutex that is taken
a few times per session and one that is taken sixty times a second per knob.

The scope's ring buffer is also read without locking. The worst a torn read can
do is put one pixel in the wrong place for one frame.

## Panels draw themselves

`bs_module.h` computes panel width and height from the module's own declared
parameters and ports; `bs_rack.cpp` lays out knobs in a grid `cols` wide, then
inputs, then outputs, in declaration order.

Nothing in the GUI knows what a VCO is. Adding a module is one class and one
line in the registry table; adding a knob to an existing module is one line in
its constructor. The alternative — a hand-placed layout per panel — is fourteen
layouts to keep in step with fourteen constructors, and they drift apart the
first time someone adds a knob.

Panel geometry lives in the core rather than the GUI because the GUI is not the
only thing that needs it: the default patch has to know how tall a module is to
place the row beneath it, and a second copy of those constants is a second copy
that can be wrong.

Two modules need more than knobs and jacks — the scope's screen and the output
meter. They declare the extra height with a virtual, and the rack special-cases
exactly those two by type id. Two special cases is cheaper than a general
drawing protocol that nothing else would use.

## The cables

Each cable is sixteen point masses under gravity, integrated with Verlet and
relaxed against a **maximum**-length constraint per segment.

Maximum rather than fixed is the whole thing: a cable is inextensible but
perfectly happy to bunch up, so segments are only ever pulled together, never
pushed apart. That one asymmetry is the difference between something that hangs
and something that behaves like a spring.

Verlet rather than stored velocities because the ends are pinned to moving
jacks. Moving an endpoint *is* giving it velocity — the implied `v` is wherever
it was last frame — so dragging a panel makes its cables swing without a line
of code that says so.

Three numbers in `bs_rope.cpp` were arrived at by measurement rather than by
taste, and the reasoning is worth keeping:

- **Slack is solved for, not chosen.** A catenary's sag goes as
  `sqrt(3 · span · slack / 8)`, so "20% longer than the gap" droops gently
  across a narrow panel and falls off the bottom of the window across a wide
  rack. Inverting that for a fixed 38-pixel sag is what makes a cable look like
  the same cable wherever it is patched.
- **The relaxation sweeps alternate direction.** Gauss-Seidel propagates a
  correction one point per sweep in whichever direction it runs, so sweeping
  one way only, sixteen points need sixteen sweeps before the far end has heard
  about the near end at all. The symptom was a cable that sagged further every
  frame.
- **Gravity is low — roughly lunar.** Not an aesthetic choice. The relaxation
  never fully satisfies the constraint in the iterations available, and the
  steady-state error is proportional to how hard gravity pushes against it: at
  earth-like values a long cable settles a third longer than it is supposed to
  be. Since an inextensible cable's resting shape is set by its length and not
  by gravity, weakening gravity costs only settling time.

## Testing what can be wrong quietly

`tests/test_core.cpp` runs against the same objects the synthesizer runs on,
with no window and no sound card. It covers the things that fail silently: an
oscillator half an octave out, a filter that goes unstable at the top of its
resonance range, a voice allocator that loses a note when it steals one, a
patch that stops making sound after a module is deleted, a feedback loop that
runs away.

The broadest one is cheapest: every module in the registry, run at three
parameter positions with nothing patched into it, has to produce a finite
signal. It costs one line per module added and catches a new module dividing by
an unpatched jack.

`tools/render_wav.cpp` writes a phrase to a WAV through the real engine, which
is how the thing gets *listened* to on a machine with no audio device — and how
a change to the filter gets judged by ear rather than by argument.

`./bencsynth --shot out.png` renders a few frames and screenshots itself, which
is how the window gets checked without anyone sitting in front of it.

## The plugin

The core is already plugin-shaped. What is left is the wrapper:

1. **A wrapper target.** CLAP is the smaller job — a C API, no SDK build. VST3
   needs Steinberg's SDK and its own class hierarchy. Either links
   `libbencsynth.a` unchanged.
2. **MIDI in.** `Engine::noteOn/noteOff/setBend/setMod/setSustain` are already
   the entire interface; the wrapper translates the host's event list and has to
   split blocks at event boundaries for sample-accurate timing, which
   `render()` does not do yet.
3. **State.** `bs_patchfile.cpp` is already in the core and already
   raylib-free. It still writes to a path, so it wants a string-in/string-out
   pair beside the file pair, for a host that hands over a stream.
4. **Parameters.** A host wants a flat, stable list of automatable parameters.
   The rack's parameters are neither — they come and go with modules. The usual
   answer is a fixed bank of macro parameters that patch onto rack knobs.
5. **The editor.** raylib owns its window, which is not what a plugin editor
   is. Either an embedded-window backend, or the GUI is redrawn against the
   host's surface. This is the large piece of work, and it is the reason the
   split above is worth holding to now rather than later — a headless plugin
   that loads racks saved by the standalone is useful long before the editor
   exists.
