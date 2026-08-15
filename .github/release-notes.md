A polyphonic virtual modular synthesizer. Panels in a rack, jacks you drag
patch cables between, and the cables hang.

**All three platforms.** Windows was the only one for v0.1.0 and v0.2.0, on the
rule that a release is a claim someone has actually run the thing. That has
stopped being true, so Linux and macOS are back.

### Which file

**Windows**

| | |
|---|---|
| **Everything** | `bencsynth-*-windows-setup.exe` — the installer. Program and plug-ins, and it upgrades an existing install rather than sitting beside it. Installs for everyone on the machine, so it asks for administrator rights once. |
| The program alone | `bencsynth-*-windows-x86_64.zip` — unzip and run `bencsynth.exe`. |
| The plug-ins alone | `bencsynth-*-windows-plugins.zip` — CLAP and VST3, with `INSTALL.md` for placing them by hand. |

**macOS** — universal, Apple Silicon and Intel.

| | |
|---|---|
| The program | `bencsynth-*-macos-universal.dmg` — drag it to Applications. |
| The plug-ins | `bencsynth-*-macos-plugins.dmg` — CLAP and **AU**, with the editor and an `Install Plug-Ins.command` that puts them where hosts look. |

**Linux** — x86_64.

| | |
|---|---|
| The program | `bencsynth-*-linux-x86_64.tar.gz` — untar and run `./bencsynth`. |
| The plug-ins | `bencsynth-*-linux-plugins.tar.gz` — CLAP, LV2 and VST3, with `INSTALL.md`. |

On Windows the installer is the one to take unless you have a reason not to. It
puts each format in the folder its hosts already search, and puts a copy of the
program beside the CLAP as its editor — which is the part that is easy to get
wrong by hand, and without which the plug-in opens to a row of sliders instead
of the rack.

Keep the `assets` folder beside the binary: it carries the interface font and
the wordmark. Without it the program still runs, in raylib's built-in face.

**macOS will say it cannot check the app for malicious software.** That is
what unnotarised means, and this is unnotarised — notarisation needs a paid
Apple Developer account. The signature is real and validates; it just
identifies nobody. Right-click → Open, once, per download. If you get
"damaged" rather than "unidentified developer", that is a different thing and
a bug — please say so.

**Linux embeds the rack via X11.** Under Wayland that goes through XWayland,
which works; native Wayland has no cross-process window embedding at all, so
there is nothing to implement against there.

### As a plugin

The plugin opens the **real rack** — cables, physics, keyboard — inside your
DAW, not a row of sliders. Notes from the host light up the keyboard and drive
the scopes; turning a knob reaches the audio without interrupting it.


- **CLAP** — all three platforms. REAPER, Bitwig, Studio One.
- **VST3** — Windows and Linux. Ableton, Cubase, FL Studio, Studio One. It is
  a shim that loads the CLAP, so **install both**.
- **AU** — macOS. Logic and GarageBand, which will load nothing else. Also a
  shim over the CLAP, so install both — the `Install Plug-Ins.command` does.
- **LV2** — Linux only. Ardour, Carla, Zrythm. It has no editor of its own; the
  rack is reachable as a parameter rather than a window.

The AU passes **`auval`** — Apple's validator, the one Logic runs before it
will list a plugin — every section, on a real Mac. In GarageBand and Logic it
appears under **BENCO → BENCsynth** in the instrument slot of a Software
Instrument track. Both hosts only scan for new AUs at launch, so if it is not
there, quit and reopen.

If you are placing files by hand, `INSTALL.md` in each plug-in pack says which
folder. The one thing to get right on every platform is that **the editor is
the program itself**, and it has to sit beside the `.clap` — that is what opens
the rack instead of a row of sliders. The Windows installer and the macOS
`Install Plug-Ins.command` both do it for you.

On Windows, also note that `%LOCALAPPDATA%` is not `%APPDATA%`; putting a
plug-in in the wrong one fails with no message at all.

The **Rack** parameter steps through all 38 presets and saves with your
project. The eight **Macro** parameters are automatable.

So is any knob you choose: **right-click a knob → EXPOSE** and it takes one of
sixteen slots the host sees by name — `VCF CUTOFF` rather than `Param 12`. The
choice is saved with the project, and the host is told to re-read the names, so
they change as you change them.

Everything else saves too — the patch, where the rack was scrolled and how far
it was zoomed. You do not have to save inside BENCsynth for the DAW's project
to come back the way you left it.

**MPE controllers work per note.** A Seaboard, a Linnstrument or a Push sends
bend, pressure and slide for each finger separately, and BENCsynth carries all
of it — `V/OCT` follows each note's own bend, and `PRESS` and `SLIDE` on the
KBD panel are cables like any other. Two notes can bend in opposite
directions.

**A hardware knob row can drive the macros.** Set `CC BASE` on a MACRO panel
to the first CC your controller sends and its eight knobs take the eight
macros. Or assign one at a time: right-click a macro knob, choose **LEARN MIDI
CC**, and turn the control you want — this works inside a DAW too, where the
knob and the MIDI are in different processes. Either way it saves with the
rack.

**The standalone takes MIDI now.** Plug a keyboard in and play it; there is
nothing to configure, and the status line says what it connected to.

### New in v0.7.2

- **Windows: it installs for everyone on the machine now**, into
  `C:\Program Files\BENCO\BENCsynth`, and asks for administrator rights once
  to do it. It used to install into `%LOCALAPPDATA%\Programs` for the person
  running it and no rights were needed. One copy in the place Windows keeps
  programs is the better arrangement for a machine somebody else also sits at.

  The plug-ins moved with it, from the per-user folders to
  `%COMMONPROGRAMFILES%\CLAP`, `\VST3` and `\LV2`. Hosts search both, so
  anything you placed by hand still works — but an elevated installer writing
  `%LOCALAPPDATA%` writes the profile of whoever answered the UAC prompt,
  which on a machine with a separate administrator account is not the person
  who will open the DAW.

  A previous per-user install is not removed by this one; they sit in
  different places and Windows will list both. Uninstall the old one first, or
  afterwards, from Apps & Features.

- **The BENCO folder.** All three of these now live under one, rather than a
  row of BENC-somethings across Program Files reading as unrelated things from
  unrelated people. The uninstaller takes that folder too, when it is empty
  and this was the last one in it.

### New in v0.7.1

- **It opens ten and a half seconds faster on Windows**, on the machines where
  that was happening. Nothing was wrong with the synthesiser: raylib wakes
  GLFW's joystick subsystem just before it creates the window, and on Windows
  that enumerates every game controller the machine has ever had attached —
  present or not, each answered by its own driver at its own pace. A rig with
  a lot of things plugged into it pays for all of them, before the window
  appears. This program has never read a joystick, so it no longer asks.

  If a raylib program is slow to open on your machine, Windows' own Game
  Controllers panel (`joy.cpl`) will be slow too — the same enumeration, with
  no raylib anywhere near it.

- **The multisampling hint is gone.** Measured at 703 pixels of a 972,800
  pixel window — seven hundredths of one per cent — because this interface is
  rectangles and text. It also asks the driver to go looking for a
  multisampled pixel format at startup, which some are slow about.

- **The VST3 build works again.** The Steinberg SDK was being cloned from
  `master` while clap-wrapper was pinned, and on 11 August the SDK moved to
  v3.8.1 and stopped compiling against it. Both halves are pinned now.

- **The demo tracks are in the repository**, under `demo/` — the sources, not
  the audio. Each renders about 120 MB of stems and a mixdown from a few tens
  of kilobytes of code, and it comes back bit for bit, so the code is the part
  worth keeping.

### New in v0.7.0

- **The bow and the reed make notes.** They have not since v0.4.2. What
  BOWED actually produced was a DC offset — at middle C, a constant 1.0487 V
  moving three ten-thousandths peak to peak. Three releases described it as
  working, including this file, because every check that could have caught it
  measured a *level*, and a constant passes a level check especially well: it
  is perfectly uniform, it sustains forever, and it never dies at the bottom
  of the keyboard.

  Four faults were stacked. The servo fed the returning wave back into the
  loop with its DC included, and a delay loop with a gain near one has a DC
  gain near a thousand. It replaced only the string's declared loss and not
  the damping filter's, which at BOWED's own brightness eats 28% per round
  trip — so the bow died at dark settings and sang at bright ones. Stability
  at the top of the keyboard had been bought by weakening the servo, which
  left the top octave oscillating correctly at 17 dB down. And the damping
  corner sat close enough to the fundamental around notes 87–91 that the
  string lost more per round trip than any servo would replace.

  Now: every note from the bottom of the keyboard to the top oscillates, in
  tune within a couple of cents above note 48, and at a level that matches
  the hammer driving the identical string — so switching `EXCITE` changes the
  timbre and not the volume. **BLOW** had the same faults, shares the same
  string, and is fixed by the same change.

- **A saved `.bencsynth` carries its audio.** The plug-in's state has
  embedded a rack's samples since v0.5.0, but a rack saved to a file still
  carried only paths — so handing somebody the file handed them the silence
  where their drums were. The audio now rides in the file itself. Older
  versions open the new files and simply find no audio in them.

- **NSEQ** — eight steps advanced by the *notes*, not by a clock. VOICE rolls
  a die per note; this is the deliberate half. Each note takes the next step
  and keeps it for as long as it sounds, so "the fourth note of every bar,
  harder" is literally a knob: steps 1 1 1 8, `LEN` 4, `RESET` from whatever
  defines your bar. Polyphonic — a chord's notes take successive steps and
  each holds its own.

- **KRELL never sampled anything.** Its end-of-cycle pulse was wired into the
  sample-and-hold's signal input instead of its clock, so the rack played one
  pitch, metronomically, for four releases — while its own notes panel
  explained the self-generating rhythm at length. Measured: 1 distinct pitch
  over twenty seconds before, 14 after.

- **GRAND TOUR's sampler reached nothing.** Its inputs were patched and its
  outputs went nowhere, under a comment claiming it went through the same
  filter as everything else. Right-click, LOAD SAMPLE, silence. It is
  connected now.

- **CLK's EXT jack works.** It says an external clock takes over when
  patched, but "external" was inferred from the voltage — and a real clock
  sits at exactly zero between its pulses, so the internal oscillator slipped
  its own ticks into every gap. A 1 Hz external clock into a 120 BPM panel
  gave 21 edges in ten seconds instead of 10.

- **A dimmed panel is really dimmed.** With the About panel or the file
  browser open, the on-screen keyboard underneath still played notes and the
  wheels still moved.

- **Opening a rack that fails to load no longer claims the filename.** It
  did, which meant the next SAVE wrote your current rack over the file that
  had just refused to open.

- **The sampler stops trusting the file's header.** A `.wav` claiming a
  four-gigabyte data chunk had all of it allocated before the read loop
  noticed the file was a hundred bytes long, a four-billion-hertz sample rate
  blew up the same allocation, and a truncated format chunk was read anyway
  so the sample width came off uninitialised stack. This is the one module
  that opens arbitrary files somebody else made.

- **Five races and a deadlock in the plug-in**, found by reading rather than
  by crashing — which is the right time. Closing the editor window while
  something played could take the host's audio thread down with it; the
  MIDI-CC path walked a list the main thread reallocates. The engine also
  allocated inside `render()` when the graph changed, which is the one thing
  the audio thread promises never to do.

- **Two tests that could not fail now can.** One ended in `|| true`; the
  other checked that a saved default rack restored into a default rack, which
  a `restore()` that did nothing passed perfectly.

### New in v0.6.0

- **The sampler shows its waveform**, with a marker where `START` lands.
- **One copy per file.** Two samplers on the same recording used to hold two
  copies of it; a drum rack built from a folder held the folder once per pad.
- **A saved project carries its audio.** The plug-in's state now contains the
  sample files themselves, so a project that moves to another machine — or
  whose samples get tidied away — still plays. Up to 32 MB; past that the path
  travels and the audio does not.
- ~~**The bow sustains.**~~ This was wrong, and stayed wrong until v0.7.0.
  What it actually shipped was a DC offset holding a perfectly steady level —
  see v0.7.0 above.

### New in v0.5.2

- **The file browser takes a typed path**, so a sample living somewhere else on
  the disk is one paste away rather than a lot of clicking, and **NEW FOLDER**
  makes one — the whole path if you typed several levels that do not exist yet.

### New in v0.5.1

- **A file picker that works everywhere.** BENCsynth draws its own now, in its
  own window — no `zenity`, no `kdialog`, nothing to install. It is used on
  Linux always, and on every platform when running inside a DAW, where asking
  the desktop to spawn a dialog is at best rude and at worst refused by a
  sandboxed host. Windows and macOS still get their own system dialog when
  running standalone, because there it is the right thing.
- **`ROOT` on the sampler** — which key plays the file untouched. Set it to the
  note the recording actually is and the keyboard is in tune with it: play that
  key and you hear the file, play a fifth up and you hear it a fifth up. Reads
  out as a note name rather than a number.

### New in v0.5.0

- **SAMPLE** — load a `.wav` and play it like an oscillator. Right-click the
  module → **LOAD SAMPLE**. It is polyphonic and pitched at one volt per
  octave, so a chord plays the file at several speeds at once, and it accounts
  for the file's own sample rate — a 44.1 kHz recording plays at the pitch it
  was made at. `LOOP`, a `START` position with its own CV, and an `EOS` pulse
  when a voice reaches the end, so one sample can start the next thing.

  The path is saved with the rack and travels to the plugin with it, so a
  sampler in a DAW project finds its file again. PCM at 8, 16, 24 and 32 bits
  and float at 32 and 64 are read; compressed files are refused by name rather
  than played as noise.

### New in v0.4.2

- **The bow works at every pitch now.** v0.4.0's BOWED had a dead band five
  semitones wide centred on middle C, where a note would start, ring for half a
  second and stop — at any bow pressure. The friction curve was too sharp; a
  gentler one transfers energy the same way all the way up the keyboard. Held
  notes also hold instead of fading, and the attack is quicker.

- **Learn stops listening after ten seconds.** Arming it and walking away used
  to mean the next thing to send a CC claimed the knob — possibly a sustain
  pedal, an hour later.
- **Learn says so when there is nowhere to put it**, instead of swallowing the
  assignment and leaving the knob at zero.
- **A rack with two MACRO panels now agrees with itself.** Learning on the
  second panel's knob was quietly writing the first panel's.

### New in v0.4.1

- **MIDI learn works inside a DAW**, not only in the standalone. v0.4.0 shipped
  it half-connected: the knob is in one process and the MIDI is in the other,
  and only the standalone had both. Right-click a MACRO knob → **LEARN MIDI
  CC** and turn something on your controller, wherever you are running it.

### New in v0.4.0

- **MIDI in, for the standalone.** A keyboard plugged into the machine used to
  do nothing at all — MIDI only reached BENCsynth through a host. ALSA on
  Linux, winmm on Windows, CoreMIDI on macOS; no new dependency and nothing to
  install. The status line names what it found.
- **A bow and a reed**, on the same waveguide as the piano. `EXCITE` on STRING
  picks between HAMMER, BOW and BLOW. A struck string decays; a bowed or blown
  one keeps going for as long as you hold the key, because you are still
  putting energy into it. New rack: **BOWED**.
- **VOICE** — a different number for every voice. The one thing a rack of
  hardware cannot do: there a module is a voice, so making the third note
  behave unlike the first means patching it differently. `IDX`, a per-note
  `RND` fixed when the note lands, `AGE`, and `PRESS`. It is what lets a
  sequencer play expressively rather than evenly.
- **Learn a MIDI CC per knob**, in the standalone *and* in a DAW. Right-click a
  MACRO knob → **LEARN MIDI CC**, turn something on your controller, done.
  `CC BASE` still claims a whole row of eight for a controller that sends one.

### New in v0.3.0

- **The racks you saved are in the host's chooser.** The Rack parameter used to
  offer only the 37 presets compiled in; it now lists whatever is in your rack
  folder as well. Built-in entries keep their positions, so old projects are
  unaffected.
- **MPE and CLAP note expressions.** Per-note bend, pressure and slide, with
  `PRESS` and `SLIDE` added to KBD and a note's own bend folded into `V/OCT`.
  Two notes can bend in opposite directions at once — which is the thing a
  single pitch wheel cannot do, and most of why MPE exists. A cable already
  carries every voice separately, so a rack patches per-note expression like
  anything else.
- **Record to a .wav.** A `REC` button on the toolbar writes the master bus.
  Takes are named for the moment they started. If the disk falls behind, the
  block is dropped rather than the audio stalling, and the count is reported
  when you stop.
- **A hardware knob row drives the macros.** A MACRO panel's new `CC BASE`
  knob claims eight consecutive CCs. It saves with the rack, and the plugin
  reports the changes back to the host so your project stores them.

### New in v0.2.3

- **The AU is validated.** `auval` passes every section; it shipped in v0.2.2
  unproven, and now it is not.
- **The rack now appears in Logic and GarageBand.** v0.2.2's AU loaded, played
  and automated, but its window sat on "starting the rack" forever. CLAP says a
  host calls create, set_parent, then show, and show is where the editor
  process gets started — clap-wrapper's AU view never calls show. The editor
  now starts on whichever of the two arrives.
- **One version, everywhere.** v0.2.2 reported three different numbers for one
  plugin — a component saying 1.1.1, wrapping a CLAP saying 0.1.0, in a release
  tagged v0.2.2. Everything now reads `src/core/bs_version.h`, and the release
  refuses to build a tag that disagrees with it.

### New in v0.2.2

- **AU on macOS**, so Logic and GarageBand can load it.
- **PING on DLY.** Two identical delay lines are stereo only in that nothing
  gets summed; they still repeat in place. With PING on, the source enters the
  left line and the repeats alternate — measured 100/0, 4/96, 97/3, 3/97.

### New in v0.2.1

- **Linux and macOS builds**, per the above.
- **The macOS executable is universal now.** The plug-ins always were, but the
  executable is the plug-in's editor — so a universal CLAP on an Intel Mac used
  to load and then never open its window.
- **DLY and CHORUS carry stereo through.** Both summed their input, so a rack
  that had placed its voices across the field lost them at the effect. Both
  gained an `IN R`; DLY gained a second delay line with `OUT R` and `WET R`.
  Patches saved by v0.2.0 load unchanged — the new ports are appended.

### New in v0.2.0

- **An installer**, which is the headline. It upgrades in place; the old
  version does not have to be removed first.
- **Thirteen more module types**, thirty in all: a plucked-string waveguide, a
  state-variable filter, a function generator, a quantizer, a clock, an
  eight-step sequencer, logic, a switch, a wavefolder, a bitcrusher, a chorus,
  a slew limiter and a stereo panner among them.
- **Eight more racks**, thirty-seven in all — including **PIANO** and
  **GRAND**, which are struck strings rather than a filter pretending.
- **Musical typing** while the plugin has focus, a keyboard you can fold away
  with the icon in the top right, and a rack list that scrolls and sorts.
- **Tempo sync** — CLK and DLY can follow the host's BPM in note values.
- **Five bugs** an audit turned up, three of them serious: a restored rack
  being wiped on activate, which made saved projects come back silent; clock
  divisions that did not divide; and an envelope's end-of-cycle that stuck on.

### Getting started

Press **Z** or click the keys. **RACKS** on the toolbar loads a preset; each
one comes with a NOTES panel explaining what it does and one thing to try.
Right-click a module's title bar for its menu, right-click the empty rack to
add a module, and drag between jacks to patch. Scroll to zoom.

Start with **CLASSIC**, then **PIANO**, then **GRAND TOUR** — which uses every
module type at once and is more of a demonstration than an instrument.
