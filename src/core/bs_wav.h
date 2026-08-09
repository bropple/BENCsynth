/*
 * BENCsynth - reading a .wav.
 *
 * Enough of the format to open what people actually have: PCM at 8, 16, 24 or
 * 32 bits, IEEE float at 32 or 64, any channel count, any rate. Not enough to
 * be a media library - no compression, no ADPCM, no chunk nobody uses.
 *
 * Everything comes back as interleaved stereo float, because that is what the
 * rack carries and converting once at load is cheaper than deciding per
 * sample. A mono file is copied to both sides; anything wider than stereo is
 * folded down, since a modular has two output jacks and no opinion about 5.1.
 *
 * The file's own sample rate comes back with it rather than being resampled
 * here. A sampler has to resample anyway to play at a pitch, so doing it twice
 * would only lose more.
 */

#ifndef BS_WAV_H
#define BS_WAV_H

#include <string>
#include <vector>

namespace bs {

struct WavData {
    std::vector<float> samples;   /* interleaved stereo */
    int    frames;
    double rate;                  /* the file's, not the engine's */

    WavData() : frames(0), rate(0.0) {}
};

/* Returns false and fills `err` if the file will not open, is not a WAV, or is
 * in a form this does not read. A caller showing that string to somebody is
 * the whole reason it exists. */
bool wavLoad(const char *path, WavData *out, std::string *err,
             int maxSeconds = 120);

}  /* namespace bs */

#endif /* BS_WAV_H */
