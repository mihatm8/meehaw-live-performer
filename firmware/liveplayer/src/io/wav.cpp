// meehaw live performer — WAV reading: one-shots into µ-law pool slots, vocal streaming (SPEC §5, §6, §9).
// Loop-task only (all SD I/O happens there, SPEC §7), so the 2 KB chunk buffer is a file-scope static
// instead of a stack frame. It is shared by the loader and the streamer: neither keeps anything in it
// between calls.
#include "wav.h"
#include "../core/config.h"
#include "../dsp/ulaw.h"
#include <string.h>
#include <math.h>

namespace lp {

namespace {

constexpr int kChunkSamples = (int)(kLoaderChunkBytes / 2);   // 1024 int16 = 2 KB
int16_t s_chunk[kChunkSamples];

inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
inline bool tag(const uint8_t* p, const char* t) { return memcmp(p, t, 4) == 0; }

// Saturating float -> int16 with round-half-away. A NaN compares false with everything and its int
// conversion is undefined, so it becomes silence (gainDb is clamped before powf, this is belt and braces).
inline int16_t clamp16(float v) {
  if (v != v) return 0;
  if (v > 32767.0f) return 32767;
  if (v < -32768.0f) return -32768;
  return (int16_t)(v >= 0.0f ? v + 0.5f : v - 0.5f);
}

// Parses the 'fmt ' chunk body (first 16 bytes, plus the WAVE_FORMAT_EXTENSIBLE sub-format when present).
bool parseFmt(IFile& f, uint32_t size, WavInfo& info) {
  uint8_t b[40];
  if (size < 16) return false;
  uint32_t want = size < sizeof(b) ? size : (uint32_t)sizeof(b);
  if (f.read(b, want) != want) return false;
  uint16_t format = rd16(b);
  info.channels = rd16(b + 2);
  info.sampleRate = rd32(b + 4);
  info.bits = rd16(b + 14);
  if (format == 0xFFFE) {                              // extensible: sub-format GUID starts with the real tag
    if (want < 26) return false;
    format = rd16(b + 24);
  }
  return format == 1;                                  // PCM only
}

} // namespace

// RIFF walk: finds 'fmt ' and 'data' wherever they sit, skipping LIST/fact/etc. (chunks are word-aligned).
bool wavReadHeader(IFile& f, WavInfo& info) {
  info = WavInfo();
  if (!f.valid() || !f.seek(0)) return false;
  uint8_t h[12];
  if (f.read(h, 12) != 12 || !tag(h, "RIFF") || !tag(h + 8, "WAVE")) return false;
  const uint32_t fileSize = f.size();
  bool haveFmt = false, haveData = false;
  uint32_t pos = 12;
  while (pos + 8 <= fileSize) {
    if (!f.seek(pos) || f.read(h, 8) != 8) break;
    uint32_t size = rd32(h + 4);
    uint32_t body = pos + 8;
    if (tag(h, "fmt ")) {
      if (!parseFmt(f, size, info)) return false;
      haveFmt = true;
    } else if (tag(h, "data")) {
      info.dataOffset = body;
      info.dataBytes = size > fileSize - body ? fileSize - body : size;   // streaming writers put 0xFFFFFFFF here
      haveData = true;
    }
    if (haveFmt && haveData) break;
    if (size > fileSize - body) break;                 // truncated chunk: nothing valid can follow
    pos = body + size + (size & 1);
  }
  if (!haveFmt || !haveData) return false;
  if (info.bits != 16 || (info.channels != 1 && info.channels != 2) || info.sampleRate == 0) return false;
  info.dataBytes -= info.dataBytes % (info.channels * 2u);   // whole frames only
  return f.seek(info.dataOffset);
}

// One-shot loader (SPEC §5 sample pool, §9.1 cap fade, §9.2 SD budget). begin() validates and seeks; step()
// does the work one 2 KB chunk at a time so the generator can interleave vocal refills (SPEC §9.2).
bool UlawLoader::begin(IFile& f, const WavInfo& info, uint8_t* dst, uint32_t capSamples, float gainDb) {
  finish();
  i_ = 0;
  if (!dst || !capSamples || info.bits != 16 || (info.channels != 1 && info.channels != 2) || !info.sampleRate) return false;
  if (!f.valid() || !f.seek(info.dataOffset)) return false;
  channels_ = info.channels;
  srcFrames_ = info.dataBytes / (channels_ * 2u);     // frames in the data chunk (bounded below to the cap's need)
  srcRead_ = 0;
  if (!srcFrames_) return false;
  const uint32_t srcRate = info.sampleRate;
  outRate_ = (uint32_t)kSampleRate;
  // Output length at kSampleRate (floor), then the cap.
  const uint32_t outTotal = srcRate == outRate_ ? srcFrames_ : (uint32_t)(((uint64_t)srcFrames_ * outRate_) / srcRate);
  if (!outTotal) return false;
  const bool cut = outTotal > capSamples;
  nOut_ = cut ? capSamples : outTotal;
  // Read only the source frames the output needs: the nearest frame of the last output sample, plus one.
  const uint64_t need = (((uint64_t)(nOut_ - 1) * srcRate + outRate_ / 2) / outRate_) + 1;
  if (need < srcFrames_) srcFrames_ = (uint32_t)need;
  fadeN_ = cut ? kCapFadeSamples : 0;                  // SPEC §9.1: 10 ms fade at the cap only
  if (fadeN_ > nOut_) fadeN_ = nOut_;
  fadeStep_ = fadeN_ > 1 ? 1.0f / (float)(fadeN_ - 1) : 0.0f;   // ramp 1 -> exactly 0 over fadeN samples
  // gainDb comes from the slot table today but lp/config.txt later: a NaN means 0 dB, the rest is clamped.
  if (gainDb != gainDb) gainDb = 0.0f;
  if (gainDb < kMinGainDb) gainDb = kMinGainDb;
  if (gainDb > kMaxGainDb) gainDb = kMaxGainDb;
  gain_ = powf(10.0f, gainDb / 20.0f);
  // Nearest-frame stepping without overflow: src = round(i * srcRate / outRate) via an error accumulator.
  stepInt_ = srcRate / outRate_; stepRem_ = srcRate % outRate_;
  src_ = 0; err_ = outRate_ / 2;
  f_ = &f; dst_ = dst;
  return true;
}

// Reads whole 2 KB chunks until maxBytes is reached (at least one) and encodes every output sample whose
// nearest source frame lies inside them. A short read (data claimed beyond EOF) ends the load early.
bool UlawLoader::step(uint32_t maxBytes) {
  if (done()) return false;
  uint32_t readBytes = 0;
  const uint32_t frameBytes = channels_ * 2u;
  while (i_ < nOut_) {
    if (srcRead_ >= srcFrames_) break;                 // source exhausted by a short read: the rest is missing
    if (readBytes >= maxBytes && readBytes) return true;   // budget for this step spent, more remains
    uint32_t frames = kLoaderChunkBytes / frameBytes;
    if (frames > srcFrames_ - srcRead_) frames = srcFrames_ - srcRead_;
    const size_t bytes = (size_t)frames * frameBytes;
    const size_t got = f_->read(s_chunk, bytes);
    const uint32_t gotFrames = (uint32_t)(got / frameBytes);
    readBytes += (uint32_t)got;
    const uint32_t chunkStart = srcRead_, chunkEnd = srcRead_ + gotFrames;
    const bool shortRead = gotFrames < frames;         // data claimed beyond EOF: stop after this chunk
    srcRead_ = shortRead ? srcFrames_ : chunkEnd;
    if (!gotFrames) break;
    const bool lastChunk = srcRead_ >= srcFrames_;
    for (; i_ < nOut_; i_++) {
      uint32_t src = src_;
      if (src >= chunkEnd) {
        if (!lastChunk || shortRead) break;            // needs the next chunk, or the data is missing
        src = chunkEnd - 1;                            // rounding may step past the end: hold the last frame
      }
      int32_t v = channels_ == 1 ? s_chunk[src - chunkStart]
                                 : ((int32_t)s_chunk[2 * (src - chunkStart)] + (int32_t)s_chunk[2 * (src - chunkStart) + 1]) / 2;
      float sv = (float)v * gain_;
      if (i_ + fadeN_ >= nOut_) sv *= (float)(nOut_ - 1 - i_) * fadeStep_;   // linear fade ending at 0
      dst_[i_] = ulawEncode(clamp16(sv));
      src_ += stepInt_; err_ += stepRem_;
      if (err_ >= outRate_) { err_ -= outRate_; src_++; }
    }
  }
  finish();
  return false;
}

uint32_t wavLoadUlaw(IFile& f, const WavInfo& info, uint8_t* dst, uint32_t capSamples, float gainDb) {
  UlawLoader ld;
  if (!ld.begin(f, info, dst, capSamples, gainDb)) return 0;
  while (ld.step(0xFFFFFFFFu)) {}
  return ld.written();
}

// Streaming reader (SPEC §5 VocalStream): continues from the file position, never crosses the data end.
uint32_t wavReadMono(IFile& f, const WavInfo& info, int16_t* dst, uint32_t n) {
  if (!dst || !n || info.bits != 16 || (info.channels != 1 && info.channels != 2)) return 0;
  if (!f.valid()) return 0;
  const uint32_t frameBytes = info.channels * 2u;
  const uint32_t dataEnd = info.dataOffset + info.dataBytes;
  uint32_t pos = f.position();
  if (pos < info.dataOffset) { if (!f.seek(info.dataOffset)) return 0; pos = info.dataOffset; }
  if (pos >= dataEnd) return 0;
  uint32_t framesAvail = (dataEnd - pos) / frameBytes;
  if (n > framesAvail) n = (uint32_t)framesAvail;
  if (info.channels == 1) {
    size_t got = f.read(dst, (size_t)n * 2);
    return (uint32_t)(got / 2);
  }
  uint32_t done = 0;
  while (done < n) {
    uint32_t frames = n - done;
    if (frames > (uint32_t)kChunkSamples / 2) frames = kChunkSamples / 2;
    size_t got = f.read(s_chunk, (size_t)frames * 4);
    uint32_t gotFrames = (uint32_t)(got / 4);
    for (uint32_t i = 0; i < gotFrames; i++) dst[done + i] = (int16_t)(((int32_t)s_chunk[2 * i] + (int32_t)s_chunk[2 * i + 1]) / 2);
    done += gotFrames;
    if (gotFrames < frames) break;
  }
  return done;
}

} // namespace lp
