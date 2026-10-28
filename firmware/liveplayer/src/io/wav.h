// WAV reading for one-shots (into µ-law pool slots) and vocal streaming (SPEC §5/§6, §9.1, §9.2).
#pragma once
#include <stdint.h>
#include "fs.h"
#include "../core/config.h"
namespace lp {
struct WavInfo { uint32_t sampleRate = 0; uint16_t channels = 0, bits = 0; uint32_t dataOffset = 0, dataBytes = 0; };
bool wavReadHeader(IFile& f, WavInfo& info);            // RIFF/WAVE PCM 16-bit; finds the data chunk

constexpr int kCapFadeMs = 10;                           // SPEC §9.1: the loader only applies a 10 ms fade at the cap
constexpr uint32_t kCapFadeSamples = (uint32_t)(kCapFadeMs * kSampleRate / 1000);   // 220
constexpr float kMinGainDb = -40.0f, kMaxGainDb = 24.0f; // gainDb is clamped here (NaN -> 0 dB) before powf
constexpr uint32_t kLoaderChunkBytes = 2048;             // one SD read of the loader (1024 int16)

// Resumable one-shot loader (SPEC §9.2: no SD burst on the loop task may exceed 20 ms). begin() checks the
// format and seeks to the data; each step(maxBytes) reads at most maxBytes from the file (rounded up to whole
// 2 KB chunks) and encodes every output sample those bytes yield: mono-izes stereo by averaging, steps other
// sample rates to 22050 by the nearest source frame, applies gainDb, fades the last kCapFadeMs linearly to
// zero when the file is cut at capSamples, µ-law encodes into dst. No state refers to the shared chunk
// buffer between steps, so wavReadMono (vocal refills) may run between them. Cost: a 22050 mono 400 ms kick
// is 17 640 B = 9 chunks, i.e. ~ 9-18 ms of SD at 1-2 MB/s plus 2.2 ms of µ-law encode; step(4096) keeps
// each burst at 2 chunks. A 44.1 kHz stereo source of the same duration reads 4x the bytes.
class UlawLoader {
 public:
  bool begin(IFile& f, const WavInfo& info, uint8_t* dst, uint32_t capSamples, float gainDb);
  bool step(uint32_t maxBytes = 4096);                   // true while more work remains
  bool done() const { return f_ == nullptr; }
  uint32_t written() const { return i_; }                // samples in dst so far (final count once done)
 private:
  IFile* f_ = nullptr; uint8_t* dst_ = nullptr;
  uint32_t channels_ = 1, srcFrames_ = 0, srcRead_ = 0;  // source frames to read (data, bounded by the cap) / consumed
  uint32_t outRate_ = 1, stepInt_ = 1, stepRem_ = 0;     // nearest-frame resampler: src += stepInt + carry
  uint32_t src_ = 0, err_ = 0;
  uint32_t nOut_ = 0, fadeN_ = 0, i_ = 0;
  float gain_ = 1.0f, fadeStep_ = 0.0f;
  void finish() { f_ = nullptr; }
};

// Reads up to capSamples samples (mono, converting stereo by averaging, resampling other rates by nearest) and
// stores them µ-law encoded into dst. Applies gainDb and a kCapFadeMs fade to zero at the cut. Returns samples
// written. One non-resumable call (= UlawLoader begin + step until done): see the cost note above.
uint32_t wavLoadUlaw(IFile& f, const WavInfo& info, uint8_t* dst, uint32_t capSamples, float gainDb);
// Reads up to n int16 mono samples from the current position into dst (for streaming). Returns samples read.
uint32_t wavReadMono(IFile& f, const WavInfo& info, int16_t* dst, uint32_t n);
}
