A polyphonic virtual modular synthesizer. Panels in a rack, jacks you drag
patch cables between, and the cables hang.

### Which file

| | |
|---|---|
| **macOS** | `bencsynth-*-macos-*.dmg` — mounts as a window; drag S. Tarr to Applications. |
| **Windows** | `bencsynth-*-windows-x86_64.zip` — unzip and run `bencsynth.exe`. |
| **Linux** | `bencsynth-*-linux-x86_64.tar.gz` — unpack and run `./bencsynth`. |

Keep the `assets` folder beside the binary: it carries the interface font and
the wordmark. Without it the program still runs, in raylib's built-in face.

The macOS build is signed ad-hoc rather than notarised, so Gatekeeper will call
it an unidentified developer the first time. Right-click the app and choose
Open to get the button that lets you through.

### Getting started

The window opens on a complete subtractive voice — two oscillators through a
mixer into a Moog-style ladder, one envelope on the cutoff and one on the
amplifier, then a delay and a reverb. Press `Z`, or click the keys.

Drag one jack to another to patch a cable. Drag a plug out of an input to pick
that cable up and move it. Right-click a jack to unplug it, or the rack itself
to add a module. The `i` button in the top right has the rest.
