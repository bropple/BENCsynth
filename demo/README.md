# Demo tracks

Three pieces of music made out of nothing but BENCsynth factory racks. They
are here as evidence: a synthesiser's claim to be usable is worth what someone
made with it, and not much otherwise.

| | | |
|---|---|---|
| **nightdrive** | synthwave, 104 BPM, D minor, 28 bars | `Dm - Bb - F - C` |
| **starfall** | see `starfall/src` | |
| **supernova** | hardcore, see `supernova/src` | |

## The audio is not in the repository

Only the code that makes it. Each track renders to about 120 MB of stems plus
a mixdown, and all of it comes back exactly from these few tens of kilobytes
of source — the mixdown checked bit for bit against the one that was rendered
by hand, same md5. Committing four hundred megabytes to save sixteen seconds
of rendering is not a trade worth making, and it would be permanent.

## Rendering one

From the top of the repository:

```
make core

g++ -std=c++17 -O2 -Isrc/core demo/nightdrive/src/synthwave.cpp libbencsynth.a -o nd
g++ -std=c++17 -O2 -Isrc/core demo/nightdrive/src/mixsw.cpp     libbencsynth.a -o ndmix

./nd    demo/nightdrive        # writes the nine layers, about 16 seconds
./ndmix demo/nightdrive        # sums them into nightdrive.wav
```

The other two are the same shape with different filenames — each has one
source that renders the layers and one that mixes them, named in its own
`src/`.

Both take the output directory as their one argument and default to the
current one, so they can be pointed anywhere.

## Why two programs

Rendering is seventeen seconds and balancing is a dozen attempts. Splitting
them means changing a gain costs a second rather than a minute. The mixer also
reports what each layer contributes *where it actually plays* — a layer silent
for half the track has a whole-file RMS that says nothing about how loud it
sounds.
