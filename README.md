# BENCsynth

A polyphonic virtual modular synthesizer. Panels in a rack, jacks you drag
patch cables between, and the cables hang.

![The default rack](docs/rack.png)

Fourteen module types, eight-voice polyphony carried in the cables themselves,
a Moog-style ladder filter, and a keyboard along the bottom you can play with
the mouse or with the computer keyboard.

The mascot is **S. Tarr**, who appears in the header and in the help panel and
otherwise stays out of the way.

---

## Building

```
make            # the synthesizer
make test       # the core tests - no raylib, no sound card needed
make render     # a phrase through the default rack, written to a .wav
make info       # what the build decided about raylib
```

raylib 5.5 is the only dependency, and the build looks for it in three places,
most specific first:

| Where | How |
|---|---|
| `vendor/raylib/` | drop a prefix in the tree with `include/` and `lib/` |
| `RAYLIB=/prefix` | `RAYLIB=/opt/raylib make` |
| pkg-config | a system package |

`vendor/` is gitignored, so a fresh clone has to supply one of the three.
Building raylib from source into `vendor/raylib` is the path with the fewest
surprises:

```
git clone --depth 1 https://github.com/raysan5/raylib
cmake -S raylib -B raylib/build -DCMAKE_INSTALL_PREFIX=$PWD/vendor/raylib \
      -DBUILD_EXAMPLES=OFF -DBUILD_SHARED_LIBS=OFF
cmake --build raylib/build --target install -j
```

`make test`, `make core` and `make render` need none of it — the DSP core does
not link raylib, deliberately.

---

## Playing it

The window opens on a complete subtractive voice: two oscillators through a
mixer into the ladder filter, one envelope on the cutoff and one on the
amplifier, then a delay and a reverb into the output stage.

**Keyboard**

| | |
|---|---|
| `Z S X D C V G B H N J M` | the lower octave |
| `Q 2 W 3 E R 5 T 6 Y 7 U` | the octave above it |
| `[` `]` | shift octave |
| `SPACE` | sustain |
| `ESC` | all notes off |

Clicking the on-screen keys works too, and dragging across them glides. How
far down a key you click sets the velocity.

**Patching**

| | |
|---|---|
| drag jack → jack | patch a cable |
| drag a plug out of an input | pick that cable up and move it |
| right-click a jack | unplug it |
| right-click the rack | add a module |
| right-click a module title | unpatch / reset knobs / remove |
| drag a module title | move the panel |
| drag, or wheel, on a knob | turn it |
| shift-drag a knob | turn it slowly |
| double-click a knob | back to its default |
| drag empty rack, or middle-drag | pan |
| wheel / shift-wheel | scroll |

Outputs fan out to as many inputs as you like; an input takes one cable, and a
second one replaces the first. Patching backwards — from an input, looking for
a source — works as well.

`RACK 1..8` with `SAVE` and `LOAD` are eight numbered slots under `patches/`.

---

## The modules

| | |
|---|---|
| **KBD** | keyboard interface — pitch, gate, trigger, velocity, mod, bend. Glide, octave, bend range, voice count, poly/mono/legato. The only source of polyphony. |
| **VCO** | band-limited oscillator; saw, pulse, triangle and sine all at once, as a hardware VCO has them. Exponential FM, PWM, hard sync. |
| **LFO** | the same core, slow. Four shapes, rate CV, sync, unipolar switch. |
| **NOISE / S&H** | white and pink noise, and a sample-and-hold that samples its own noise when nothing is patched into it. |
| **VCF** | Moog ladder, four-pole, with a two-pole tap. Two attenuverted cutoff CVs, keyboard tracking, drive, resonance to past self-oscillation. |
| **VCA** | linear or exponential, with a CV depth. |
| **ADSR** | gate, retrigger and velocity in; envelope and its inverse out. |
| **MIX** | four in with levels, a master, and an inverted output. |
| **MULT** | two buffered multiples, one in to three out. |
| **ATT** | two attenuverters with offsets. |
| **DLY** | echo — time with CV, feedback, tone, mix. |
| **RVB** | a small room. Size, damping, mix, stereo out. |
| **SCOPE** | two traces with a time base, so you can see what you patched. |
| **OUT** | the output stage, with a peak-and-RMS meter. |

Voltages follow hardware convention throughout: audio at ±5 V, one volt per
octave with 0 V at middle C, gates at 10 V, envelopes 0–10 V. That is what
makes an attenuverter, an offset and a mixer behave the way the panel says
they will no matter what you plug into them.

---

## How the polyphony works

The patch is not duplicated per voice. A cable carries up to eight channels,
the KBD module decides how many, and every module downstream processes
whatever arrives — so one VCO panel really is eight oscillators when eight
keys are down. Polyphonic cables are drawn thicker than mono ones, which is
how you can see where the voices end and the mono tail begins.

---

## Layout

```
src/core/      the synthesizer. No raylib, no globals, no allocation in
               process(). This is what a plugin will link.
src/gui/       the window: theme, widgets, rack, cable physics, keyboard.
tests/         core tests
tools/         the offline renderer
```

`ARCHITECTURE.md` goes into why it is arranged that way, and what has to
happen for the VST build.

---

## Third-party

- **raylib** — zlib/libpng licence. Not vendored in the repository; see above.
- **Terminus TTF** — SIL Open Font License, in `assets/fonts/` with its
  `OFL.txt`. Looked for on disk at startup, with raylib's built-in font as the
  fallback, so a build with the font missing still comes up.

The `style/` folder holds the BENCO design guide and is gitignored.
