# BENCsynth architecture

## The split that matters

```
src/core/     bs_dsp.h      primitives: oscillator, ladder, envelope, delay
              bs_module.h   Module, Param, Signal, panel geometry, registry
              bs_keys.h     note allocation and the event queue
              bs_version.h  one place for the version number
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

Evaluation order is a topological sort — Kahn's algorithm over a compressed
adjacency list, O(V+E) — rebuilt whenever the shape of the graph changes, and
rebuilt eagerly on the editing thread so that `process()` never allocates.

Feedback patches are legal and common — a delay into its own input, a
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

Three tiers, and which one a thing belongs to is decided by what it costs to be
wrong about it.

**Structural changes take a mutex.** Adding a module, deleting one, connecting,
disconnecting: each either reallocates, frees a buffer an input still points
at, or rewrites a pointer `process()` is about to read. The audio callback
takes the same mutex for the duration of one block — 32 frames, so a 512-frame
device buffer acquires it sixteen times per callback. Uncontended, that is
nanoseconds, and it keeps the critical section to one block's work rather than
the whole buffer.

**Parameter knobs take nothing.** A float written by the interface while the
audio thread reads it yields either the old value or the new one, and both are
fine. Locking sixty times a second per knob to avoid that is the worse trade.

**Note events go through a wait-free queue.** `KeyboardState` — the voice
allocator — belongs to the audio thread and nothing else touches it.
`Engine::noteOn` and friends push onto a single-producer single-consumer ring,
and `Engine::render` drains it at the top of each block before anything reads
it.

That last one started as the middle tier and was wrong. The interface was
writing the voice array while the audio thread read *and wrote* the same
fields, which is a data race however small the fields are — and the reasoning
that made it look acceptable was the knob reasoning applied where it did not
hold. Ordinary knobs are one independent float; a voice allocation is a set of
fields that have to agree with each other.

Two consequences worth keeping:

- The interface can no longer read the voice array at all, so the two things it
  legitimately wants — how many voices are sounding, how many are allocated —
  are published as atomics once a block. The on-screen keyboard lights its keys
  from what it is itself holding, which is both thread-safe and the more
  accurate answer: it lights keys that are down, not voices still ringing out
  their release.
- Retrigger flags are cleared by the engine after every module has run, not by
  the keyboard module. Clearing them in the module was wrong the moment a rack
  had two keyboard panels — the first to run cleared them before the second
  could see them.

Events carry no sample offset, so everything posted between two blocks lands at
the start of the next one: 32 samples of quantisation, well under a
millisecond. A plugin wanting sample-accurate MIDI adds the offset to the event
and splits the block on it, and nothing else has to change. That is the other
reason for the queue — it is the shape a host's event list arrives in anyway.

The scope's ring buffer is still read without locking. The worst a torn read
can do is put one pixel in the wrong place for one frame.

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
is how the window gets checked without anyone sitting in front of it — CI runs
it under Xvfb, so every push proves the window comes up, the font loads and the
default rack draws.

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

## Icons, and where each platform gets one

Three mechanisms, because there is no one mechanism that works.

- **Windows** reads it from a resource compiled into the executable, and GLFW
  looks that resource up *by the name* `GLFW_ICON`. An icon declared the usual
  way, with a numeric id, is invisible to it: the build succeeds, Explorer
  shows the right icon, and the titlebar and taskbar stay generic with nothing
  warning you. `tools/check_win_icon.py` walks the PE resource directory of the
  built `.exe` and asserts the name, because a grep of the source cannot tell
  you whether `windres` ran.
- **macOS** takes it from the `.app` bundle. GLFW's Cocoa backend ignores
  `SetWindowIcon` outright — a bare Mach-O executable has no Dock identity to
  hang an icon on — so `tools/macos-app.sh` builds an `.icns` and a bundle
  around the binary.
- **Linux/X11** takes it from `_NET_WM_ICON`, which the program sets at
  startup.

The mark itself is rasterised from the same geometry that draws S. Tarr in the
header — `bs_star_image()` and `bs_star()` share their constants — so the icon
in the taskbar cannot drift from the one in the window, and a build that has
lost its assets folder still has an icon. `bencsynth --icons <dir>` writes the
PNG set with no window and no GL context; `tools/make-icons.sh` calls that and
packs the `.ico`.

At 16 and 24 pixels the mark is simplified rather than shrunk: the visor's grey
band is dropped and only the red stripe survives, because a light band across
the middle at that size competes with the silhouette and the whole thing reads
as a blob. The edge stroke is proportional with no one-pixel floor for the same
reason — a whole pixel of outline at 16 px is an eighth of the radius.

## Packaging

The macOS path is the one with the traps, all of them inherited from BENCmouth
having hit them first:

- **The bundle must be signed after it is assembled.** The linker ad-hoc signs
  the executable on its own, and that signature covers the executable alone.
  Once the same file sits inside a bundle, macOS validates it against the
  bundle — `Info.plist`, `Resources`, the lot — the standalone signature does
  not cover any of that, and Gatekeeper reports a perfectly intact bundle as
  "damaged and can't be opened", with no option to open it anyway. Signing
  after everything is in place makes the signature cover what it is checked
  against.
- **The disk image is styled with dmgbuild, not AppleScript.** A `.dmg`'s
  window layout lives in a `.DS_Store`, and the usual way to produce one is to
  mount the image and ask Finder to arrange it. That needs an interactive
  session, so it cannot work on a runner: the image comes out unstyled and
  nothing says so until someone opens it. dmgbuild writes the `.DS_Store`
  directly. The release job mounts the finished image and asserts the
  `.DS_Store` is there, so a fallback to the unstyled path fails the build
  rather than shipping.
- **raylib is built from source and linked statically.** Homebrew's is a dylib
  under `/opt/homebrew`, and a release binary linked against it aborts at launch
  on every machine that has just downloaded it. The job runs `otool -L` and
  fails on anything outside `/usr/lib` and `/System`.
- **A `.app` ships as a disk image, not a tarball.** A `.app` is a directory
  Finder draws as one icon; a tarball of one arrives looking like a folder
  called `Contents`, and the person who unpacks it has no idea which part is
  the program.
