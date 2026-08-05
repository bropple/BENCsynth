<p align="center">
  <img src="assets/brand/BENCO_Logo_README.png" alt="BENCO Holdings" width="420">
</p>

# BENCsynth

[![build](https://github.com/bropple/BENCsynth/actions/workflows/ci.yml/badge.svg)](https://github.com/bropple/BENCsynth/actions/workflows/ci.yml)
[![license: MIT](https://img.shields.io/badge/license-MIT-78b946)](LICENSE)

A polyphonic virtual modular synthesizer. Panels in a rack, jacks you drag
patch cables between, and the cables hang.

![The default rack](docs/rack.png)

**[Download a build](https://github.com/bropple/BENCsynth/releases/latest)** for
Linux, macOS or Windows — or `make` it. Every push also builds all three and
keeps them: the
[latest run](https://github.com/bropple/BENCsynth/actions/workflows/ci.yml)
has a `.tar.gz`, a `.dmg` and a `.zip` attached.

**[Hear it](docs/bencsynth-demo.wav)** — a phrase through the default rack,
rendered offline by `make render`, which needs neither a window nor a sound
card.

Fourteen module types, eight-voice polyphony carried in the cables themselves,
a Moog-style ladder filter, and a keyboard along the bottom you can play with
the mouse or with the computer keyboard.

The mascot is **S. Tarr**, who is the program's icon, sits in the header, and
otherwise stays out of the way.

<p align="center">
  <img src="docs/info.png" alt="The information window" width="760">
</p>

---

## Building

```
make            # the synthesizer
make test       # the core tests - no raylib, no sound card needed
make render     # a phrase through the default rack, written to a .wav
make icons      # regenerate assets/icon from the star geometry in the code
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
| `F1`, or the `i` in the corner | what everything does |

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
| wheel on the rack | zoom, toward the pointer |
| shift-wheel | scroll sideways |

Outputs fan out to as many inputs as you like; an input takes one cable, and a
second one replaces the first. Patching backwards — from an input, looking for
a source — works as well.

`RACK 1..8` with `SAVE` and `LOAD` are eight numbered slots under `patches/`.

---

## The factory racks

`RACKS` in the toolbar replaces everything with one of eight worked examples.
A modular's problem is not that it cannot make a sound, it is that it makes no
sound until you already know what you are doing — and the fastest way to learn
what resonance does is to be handed a rack where it is already doing something
and turn the knob.

| | |
|---|---|
| **CLASSIC** | two oscillators, ladder, delay and reverb — what the window opens on |
| **INIT** | one oscillator, one envelope, one amplifier. A place to start building |
| **BASS** | legato mono, a sine an octave down under the saw, short filter thump |
| **SQUARE LEAD** | pulse width on an LFO, glide and echo |
| **SAW PAD** | eight voices, two saws pulled a few cents apart, long reverb |
| **PLUCK** | no sustain on either envelope, resonant filter chirp |
| **DRONE** | the filter is past self-oscillation, so it *is* the oscillator. Makes sound with nothing held down; turn RES down and it stops |
| **WIND** | noise through a resonant filter and no oscillator anywhere, played from the keyboard |

`./bencsynth --rack PLUCK` opens straight into one, and
`./bencsynth-render out.wav PLUCK` renders a phrase through it without a window
or a sound card.

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
tools/         the offline renderer, and the icon and packaging scripts
assets/        the interface font, the BENCO wordmark, the S. Tarr icons
```

`make icons` regenerates `assets/icon` from `bs_star_image()` — the same code
that draws the mark in the window — so the icon in your taskbar cannot drift
from the one in the `.ico`. It needs ImageMagick; the results are committed, so
a build does not.

`ARCHITECTURE.md` goes into why it is arranged that way, and what has to
happen for the VST build.

---

## Third-party

- **raylib** — zlib/libpng licence. Not vendored in the repository; see above.
- **Terminus TTF** — SIL Open Font License, in `assets/fonts/` with its
  `OFL.txt`. Looked for on disk at startup, with raylib's built-in font as the
  fallback, so a build with the font missing still comes up.

Full attribution is in [NOTICE](NOTICE). BENCsynth itself is MIT — see
[LICENSE](LICENSE).

The `style/` folder holds the BENCO design guide and is gitignored.
