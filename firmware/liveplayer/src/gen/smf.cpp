// Standard MIDI file reader/writer (SPEC §5 clock at kPPQN, §6 library files and set assets).
// The reader streams through an IFile in 512-byte chunks (files on the card are small, but may be 64 KB),
// merges every track by absolute time, keeps the earliest kMaxNotes notes and never allocates.
// Pitches are the contract (drum maps use channel 10, melodic files channel 1); the channel only pairs a
// note-off with its note-on when the same pitch sounds on several channels at once.
// Output invariants: lengthTicks is whole beats in [1, kMaxPatternBeats]; every note starts inside the loop
// (start < lengthTicks) and ends at or before it, except a note shorter than one kNoteLenUnit at the very
// end, which keeps its start and one unit (end < lengthTicks + kNoteLenUnit, as variants::packedLen does).
// Reading stops at the first event beyond the 16-beat window (nothing after it can change the pattern),
// which bounds the card work for long files; a dense file within the window is still read whole, so a
// 64 KB file costs ~128 card reads (SPEC §9.2: library files are expected to stay a few KB).
#include "smf.h"
#include <string.h>

namespace lp {
namespace {

constexpr int kChunk = 512;                          // bytes read from the IFile at a time
constexpr int kActiveMax = 64;                       // simultaneously sounding notes tracked (library max is 14)
constexpr int32_t kTimeCap = 1 << 30;                // source ticks are clamped here; far beyond any cut-off
constexpr uint32_t kTrailingBeats = 2;               // trailing silence beyond this is not part of the loop

// --- Chunked reader over IFile ---------------------------------------------------------------------------
class ChunkReader {
 public:
  explicit ChunkReader(IFile& f) : f_(f), size_(f.size()), filePos_(f.position()) {}
  uint32_t pos() const { return pos_; }
  uint32_t size() const { return size_; }
  void seek(uint32_t p) { pos_ = p > size_ ? size_ : p; }           // lazy: fill() re-syncs the file position
  bool u8(uint8_t& v) {
    if (pos_ < bufStart_ || pos_ >= bufStart_ + bufLen_) { if (!fill()) return false; }
    v = buf_[pos_ - bufStart_];
    pos_++;
    return true;
  }
  bool u16(uint16_t& v) { uint8_t a, b; if (!u8(a) || !u8(b)) return false; v = (uint16_t)((a << 8) | b); return true; }
  bool u32(uint32_t& v) { uint16_t a, b; if (!u16(a) || !u16(b)) return false; v = ((uint32_t)a << 16) | b; return true; }
  bool bytes(uint8_t* dst, int n) { for (int i = 0; i < n; i++) if (!u8(dst[i])) return false; return true; }
  // Variable-length quantity. The standard allows 4 bytes; REAPER writes a negative delta (a note that starts
  // before the item) as a 5-byte two's-complement value, so continuation bytes are consumed and the low 32
  // bits kept — the caller treats the result as signed.
  bool vlq(uint32_t& v) {
    v = 0;
    for (int i = 0; i < 8; i++) {
      uint8_t c;
      if (!u8(c)) return false;
      v = (v << 7) | (c & 0x7F);
      if (!(c & 0x80)) return true;
    }
    return false;
  }
  bool skip(uint32_t n) { if (n > size_ - pos_) { pos_ = size_; return false; } pos_ += n; return true; }
 private:
  bool fill() {
    if (pos_ >= size_) return false;
    if (filePos_ != pos_) { if (!f_.seek(pos_)) return false; filePos_ = pos_; }
    size_t n = f_.read(buf_, kChunk);
    if (n == 0) return false;
    bufStart_ = pos_;
    bufLen_ = (uint32_t)n;
    filePos_ += (uint32_t)n;
    return true;
  }
  IFile& f_;
  uint32_t size_, filePos_, pos_ = 0, bufStart_ = 0, bufLen_ = 0;
  uint8_t buf_[kChunk];
};

// --- Read-only IFile over a memory buffer (smfParseBuffer shares the streaming implementation) --------------
class MemFile : public IFile {
 public:
  MemFile(const uint8_t* d, uint32_t n) : d_(d), n_(n) {}
  bool valid() const override { return d_ != nullptr; }
  size_t read(void* dst, size_t n) override {
    if (p_ >= n_) return 0;
    if (n > n_ - p_) n = n_ - p_;
    memcpy(dst, d_ + p_, n);
    p_ += (uint32_t)n;
    return n;
  }
  size_t write(const void*, size_t) override { return 0; }
  bool seek(uint32_t pos) override { if (pos > n_) return false; p_ = pos; return true; }
  uint32_t position() const override { return p_; }
  uint32_t size() const override { return n_; }
  void close() override {}
 private:
  const uint8_t* d_;
  uint32_t n_, p_ = 0;
};

// --- Parser ------------------------------------------------------------------------------------------------
struct ActiveNote { int32_t start; uint8_t pitch; uint8_t vel; uint8_t ch; };   // 8 bytes

uint32_t ceilBeats(uint32_t ticks) { return ((ticks + kTicksPerBeat - 1) / kTicksPerBeat) * kTicksPerBeat; }

int32_t clampTime(int64_t t) { return t > kTimeCap ? kTimeCap : (t < -kTimeCap ? -kTimeCap : (int32_t)t); }

// Canonical order: start, pitch, length, velocity — deterministic for variants and round trips.
bool noteBefore(const Note& a, const Note& b) {
  if (a.start() != b.start()) return a.start() < b.start();
  if (a.pitch() != b.pitch()) return a.pitch() < b.pitch();
  if (a.len() != b.len()) return a.len() < b.len();
  return a.vel() < b.vel();
}

void sortCanonical(Pattern& p) {                       // insertion sort, at most kMaxNotes entries
  for (int i = 1; i < p.count; i++) {
    Note k = p.notes[i];
    int j = i - 1;
    while (j >= 0 && noteBefore(k, p.notes[j])) { p.notes[j + 1] = p.notes[j]; j--; }
    p.notes[j + 1] = k;
  }
}

// Makes a note end at or before len when at least one whole kNoteLenUnit fits; a blip closer than one unit
// to the loop end keeps its start (a pre-beat hit must not move earlier) and one unit, so its end may exceed
// len by up to kNoteLenUnit - 1 ticks. The sequencer stores absolute note-end ticks and only needs start < len.
void fitNote(Note& n, uint32_t len) {
  if (n.end() <= len) return;
  uint32_t avail = n.start() < len ? len - n.start() : 0;
  n.setLen((uint16_t)(avail >= (uint32_t)kNoteLenUnit ? avail - avail % kNoteLenUnit : (uint32_t)kNoteLenUnit));
}

struct Parser {
  Pattern& out;
  SmfInfo info;
  uint16_t ppq = kPPQN;
  int32_t limitSrc = kMaxPatternTicks + 1;           // smallest source tick that lies beyond the 16-beat window
  ActiveNote active[kActiveMax];
  int activeCount = 0;
  uint32_t maxEnd = 0;                               // unquantised end (kPPQN ticks, <= kMaxPatternTicks) over all notes
  int32_t maxEndSrc = 0, eotSrc = 0;                 // source ticks, for info.lengthTicksSrc
  int latestIdx = -1;                                // cached latest() while the pattern is full, -1 = unknown
  bool cut = false;                                  // something was dropped or shortened
  bool beyond = false;                               // an event lies beyond the window: the file is longer than 16 beats

  explicit Parser(Pattern& o) : out(o) {}

  void setPpq(uint16_t division) {                   // toTicks(src) > kMaxPatternTicks  <=>  src >= limitSrc
    ppq = division;
    limitSrc = (int32_t)(((int64_t)(kMaxPatternTicks + 1) * ppq - ppq / 2 + kPPQN - 1) / kPPQN);
  }

  uint32_t toTicks(int32_t src) const {              // source PPQ -> kPPQN, rounded to nearest, capped just past the window
    if (src <= 0) return 0;
    uint64_t t = ((uint64_t)src * kPPQN + ppq / 2) / ppq;
    return t > (uint64_t)kMaxPatternTicks + 1 ? (uint32_t)kMaxPatternTicks + 1 : (uint32_t)t;   // wrap-free arithmetic below
  }

  int latest() const {                               // index of the latest-starting kept note (last among ties)
    int best = 0;
    for (int i = 1; i < out.count; i++) if (out.notes[i].start() >= out.notes[best].start()) best = i;
    return best;
  }

  void emit(int32_t startSrc, int32_t endSrc, uint8_t pitch, uint8_t vel) {
    if (endSrc > maxEndSrc) maxEndSrc = endSrc;
    uint32_t s = toTicks(startSrc), e = toTicks(endSrc);                         // both <= kMaxPatternTicks + 1
    if (e <= s) e = s + 1;                                                       // minimum length 1 tick
    if (maxEnd < e) maxEnd = e > (uint32_t)kMaxPatternTicks ? (uint32_t)kMaxPatternTicks : e;
    if (s >= (uint32_t)kMaxPatternTicks) { cut = true; return; }                // starts at or beyond the cut: dropped
    if (e > (uint32_t)kMaxPatternTicks) { e = kMaxPatternTicks; cut = true; }    // crosses the cut: clipped (a sub-unit
    Note n = Note::make((uint16_t)s, (uint16_t)(e - s), pitch, vel);             // tail there keeps its start, see fitNote)
    int slot = out.count;
    if (slot >= kMaxNotes) {                                                     // keep the earliest kMaxNotes
      cut = true;
      if (latestIdx < 0) latestIdx = latest();                                   // one scan per eviction, not per note
      if (s >= out.notes[latestIdx].start()) return;
      slot = latestIdx;
      latestIdx = -1;
    } else {
      out.count++;
    }
    out.notes[slot] = n;
  }

  void noteOn(int32_t t, uint8_t pitch, uint8_t vel, uint8_t ch) {
    if (activeCount == kActiveMax) {                                             // overflow: close the oldest, report it
      cut = true;
      emit(active[0].start, t, active[0].pitch, active[0].vel);
      memmove(&active[0], &active[1], sizeof(ActiveNote) * (kActiveMax - 1));
      activeCount--;
    }
    active[activeCount++] = ActiveNote{t, pitch, vel, ch};
  }

  // Closes the oldest sounding note of that pitch on the same channel (FIFO); a note-off whose channel has no
  // such note falls back to the oldest note of that pitch on any channel.
  void noteOff(int32_t t, uint8_t pitch, uint8_t ch) {
    int hit = -1;
    for (int i = 0; i < activeCount && hit < 0; i++) if (active[i].pitch == pitch && active[i].ch == ch) hit = i;
    for (int i = 0; i < activeCount && hit < 0; i++) if (active[i].pitch == pitch) hit = i;
    if (hit < 0) return;
    emit(active[hit].start, t, pitch, active[hit].vel);
    memmove(&active[hit], &active[hit + 1], sizeof(ActiveNote) * (activeCount - hit - 1));
    activeCount--;
  }

  void closeAll(int32_t t) {
    for (int i = 0; i < activeCount; i++) emit(active[i].start, t, active[i].pitch, active[i].vel);
    activeCount = 0;
  }

  // Meta event (after the 0xFF byte). Returns false on a read error; sets eot at end-of-track.
  bool metaEvent(ChunkReader& r, int32_t time, bool& eot) {
    uint8_t type;
    uint32_t len;
    if (!r.u8(type) || !r.vlq(len)) return false;
    if (type == 0x2F) { eot = true; if (time > eotSrc) eotSrc = time; return true; }
    if (type == 0x51 && len == 3 && !(info.bpm > 0.0f)) {                       // first set_tempo wins
      uint8_t m[3];
      if (!r.bytes(m, 3)) return false;
      uint32_t mpq = ((uint32_t)m[0] << 16) | ((uint32_t)m[1] << 8) | m[2];
      if (mpq) info.bpm = 60000000.0f / (float)mpq;
      return true;
    }
    return r.skip(len);                                                          // text, markers, key, ...
  }

  // Sysex and system common / real-time messages carry no notes and are skipped.
  bool systemEvent(ChunkReader& r, uint8_t b) {
    if (b == 0xF0 || b == 0xF7) { uint32_t len; return r.vlq(len) && r.skip(len); }
    if (b >= 0xF8) return true;
    return r.skip((b == 0xF1 || b == 0xF3) ? 1 : (b == 0xF2 ? 2 : 0));
  }

  // One MTrk chunk ending at byte offset end. Malformed data ends the track early; what was read is kept.
  // Reading stops at the first event beyond the window: a note-on there is dropped, a note-off there clips
  // its note at the cut (so sounding notes are closed at that time with the same result), and the EOT is at
  // or after it, so the loop is 16 beats regardless. Only a set_tempo after 16 beats would be missed.
  void track(ChunkReader& r, uint32_t end) {
    int32_t time = 0;
    uint8_t status = 0;
    activeCount = 0;
    while (r.pos() < end) {
      uint32_t delta;
      uint8_t b, d1, d2;
      if (!r.vlq(delta) || !r.u8(b)) break;
      time = clampTime((int64_t)time + (int32_t)delta);                          // 5-byte VLQs are signed
      if (time >= limitSrc) { beyond = true; if (time > eotSrc) eotSrc = time; break; }
      if (b >= 0xF0) {                                                           // meta / sysex / system
        bool eot = false;
        if (!(b == 0xFF ? metaEvent(r, time, eot) : systemEvent(r, b)) || eot) break;
        continue;
      }
      if (b < 0x80) { if (status == 0) break; d1 = b; }                          // running status
      else { status = b; if (!r.u8(d1)) break; }
      if (d1 & 0x80) break;                                                      // data byte expected
      uint8_t kind = status & 0xF0;
      if (kind == 0xC0 || kind == 0xD0) continue;                                // program / channel pressure
      if (!r.u8(d2) || (d2 & 0x80)) break;
      uint8_t ch = status & 0x0F;
      if (kind == 0x90 && d2 > 0) noteOn(time, d1, d2, ch);
      else if (kind == 0x80 || kind == 0x90) noteOff(time, d1, ch);              // velocity 0 = note off
    }
    closeAll(time);                                 // unterminated notes end at this track's EOT, last event or exit
  }

  // Musical length (header comment of smf.h). A file that goes on beyond the window was a loop longer than
  // 16 beats: it is cut to exactly kMaxPatternBeats so the bars stay aligned, and reported as truncated.
  // Otherwise: max note end (over every note, kept or not), or end-of-track when it is at most kTrailingBeats
  // of silence further (raw ticks), rounded up to whole beats, at least one beat.
  void finish() {
    uint32_t len;
    if (beyond) {
      len = kMaxPatternTicks;
      cut = true;
    } else {
      uint32_t eot = toTicks(eotSrc);                                            // <= kMaxPatternTicks here
      len = ceilBeats(eot > maxEnd && eot - maxEnd <= kTrailingBeats * kTicksPerBeat ? eot : maxEnd);
      if (len < (uint32_t)kTicksPerBeat) len = kTicksPerBeat;
    }
    out.lengthTicks = (uint16_t)len;
    for (int i = 0; i < out.count; i++) fitNote(out.notes[i], len);
    sortCanonical(out);
    int32_t srcLen = maxEndSrc > eotSrc ? maxEndSrc : eotSrc;                   // for a cut file: the exit time (a lower bound)
    info.lengthTicksSrc = srcLen > 0 ? (uint32_t)srcLen : 0;
    info.ppq = ppq;
    info.notes = out.count;
    info.truncated = cut;
  }
};

// --- Writer --------------------------------------------------------------------------------------------------
class Sink {                                           // counts bytes (f == nullptr) or writes them in 64-byte runs
 public:
  explicit Sink(IFile* f) : f_(f) {}
  void put(uint32_t b) { if (f_) { buf_[n_++] = (uint8_t)b; if (n_ == (int)sizeof(buf_)) flush(); } total_++; }
  void put16(uint32_t v) { put(v >> 8); put(v); }
  void put32(uint32_t v) { put16(v >> 16); put16(v); }
  void vlq(uint32_t v) {
    uint8_t tmp[5];
    int n = 0;
    do { tmp[n++] = (uint8_t)(v & 0x7F); v >>= 7; } while (v);
    while (n) { n--; put(tmp[n] | (n ? 0x80 : 0)); }
  }
  void flush() { if (f_ && n_) { if (f_->write(buf_, (size_t)n_) != (size_t)n_) ok_ = false; n_ = 0; } }
  uint32_t total() const { return total_; }
  bool ok() const { return ok_; }
 private:
  IFile* f_;
  uint8_t buf_[64];
  int n_ = 0;
  uint32_t total_ = 0;
  bool ok_ = true;
};

bool bitGet(const uint8_t* bits, int i) { return (bits[i >> 3] >> (i & 7)) & 1; }
void bitSet(uint8_t* bits, int i) { bits[i >> 3] |= (uint8_t)(1 << (i & 7)); }

// A note that starts inside the loop is cut at the loop end on export (as REAPER cuts notes at the item end),
// so a sub-unit tail that overruns lengthTicks (see fitNote) re-parses to the same packed note. The length
// itself round-trips unless the pattern came from a cut file and ends in more than kTrailingBeats of silence:
// an SMF cannot say "this was longer", so such an export re-parses to its note-based length (set files carry
// the loop length separately, SPEC §9.8 lp.track.N.len).
uint32_t offTick(const Note& n, uint32_t len) { return n.start() < len && n.end() > len ? len : n.end(); }

// Track body: tempo and 4/4 at tick 0, then note on/off events in time order (offs before ons on the same
// tick so a retriggered pitch re-parses as two notes), then end of track at the pattern length.
void writeTrackBody(Sink& s, const Pattern& p, uint32_t mpq) {
  s.put(0); s.put(0xFF); s.put(0x51); s.put(3); s.put(mpq >> 16); s.put(mpq >> 8); s.put(mpq);
  s.put(0); s.put(0xFF); s.put(0x58); s.put(4); s.put(4); s.put(2); s.put(24); s.put(8);
  uint8_t onDone[(kMaxNotes + 7) / 8] = {0}, offDone[(kMaxNotes + 7) / 8] = {0};
  int n = p.count > kMaxNotes ? kMaxNotes : p.count;
  uint32_t last = 0;
  for (int step = 0; step < 2 * n; step++) {
    int onIdx = -1, offIdx = -1;
    uint32_t onT = 0, offT = 0;
    for (int i = 0; i < n; i++) {
      const Note& k = p.notes[i];
      if (!bitGet(onDone, i)) { if (onIdx < 0 || k.start() < onT) { onIdx = i; onT = k.start(); } }
      else if (!bitGet(offDone, i)) { uint32_t e = offTick(k, p.lengthTicks); if (offIdx < 0 || e < offT) { offIdx = i; offT = e; } }
    }
    bool off = offIdx >= 0 && (onIdx < 0 || offT <= onT);
    uint32_t t = off ? offT : onT;
    s.vlq(t - last);
    last = t;
    if (off) { s.put(0x80); s.put(p.notes[offIdx].pitch()); s.put(0x40); bitSet(offDone, offIdx); }
    else { s.put(0x90); s.put(p.notes[onIdx].pitch()); s.put(p.notes[onIdx].vel()); bitSet(onDone, onIdx); }
  }
  uint32_t eot = p.lengthTicks > last ? p.lengthTicks : last;
  s.vlq(eot - last);
  s.put(0xFF); s.put(0x2F); s.put(0);
}

} // namespace

bool smfParse(IFile& f, Pattern& out, SmfInfo* info) {
  out.clear();
  if (!f.valid()) return false;
  ChunkReader r(f);
  uint8_t id[4];
  uint32_t hlen;
  uint16_t format, ntracks, division;
  if (!r.bytes(id, 4) || memcmp(id, "MThd", 4) != 0 || !r.u32(hlen) || hlen < 6) return false;
  if (!r.u16(format) || !r.u16(ntracks) || !r.u16(division)) return false;
  if (format > 1 || division == 0 || (division & 0x8000)) return false;        // SMPTE division unsupported
  if (hlen > 6 && !r.skip(hlen - 6)) return false;
  Parser p(out);
  p.setPpq(division);
  int found = 0;
  while (found < ntracks && r.size() - r.pos() >= 8) {
    uint32_t clen;
    if (!r.bytes(id, 4) || !r.u32(clen)) break;
    uint32_t start = r.pos();
    uint32_t end = clen > r.size() - start ? r.size() : start + clen;
    if (memcmp(id, "MTrk", 4) == 0) { found++; p.track(r, end); }              // other chunks are skipped
    r.seek(end);
  }
  if (found == 0) return false;
  p.finish();
  if (info) *info = p.info;
  return true;
}

bool smfParseBuffer(const uint8_t* data, uint32_t size, Pattern& out, SmfInfo* info) {
  MemFile f(data, size);
  return smfParse(f, out, info);
}

// Tempos outside kWriteMinBpm..kWriteMaxBpm (NaN and infinities included) are written as the default: the
// 24-bit set_tempo cannot hold anything below 3.58 BPM and the float -> uint32_t cast must stay in range.
constexpr float kWriteMinBpm = 4.0f, kWriteMaxBpm = 1000.0f;

bool smfWrite(IFile& f, const Pattern& p, float bpm) {
  if (!f.valid()) return false;
  if (!(bpm >= kWriteMinBpm) || bpm > kWriteMaxBpm) bpm = (float)kDefaultBpm;
  uint32_t mpq = (uint32_t)(60000000.0f / bpm + 0.5f);                          // 60 000 .. 15 000 000
  uint32_t bodyLen;
  { Sink counter(nullptr); writeTrackBody(counter, p, mpq); bodyLen = counter.total(); }
  Sink s(&f);
  s.put('M'); s.put('T'); s.put('h'); s.put('d'); s.put32(6); s.put16(0); s.put16(1); s.put16(kPPQN);
  s.put('M'); s.put('T'); s.put('r'); s.put('k'); s.put32(bodyLen);
  writeTrackBody(s, p, mpq);
  s.flush();
  return s.ok();
}

} // namespace lp
