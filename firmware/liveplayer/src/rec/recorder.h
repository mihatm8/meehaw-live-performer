// meehaw live performer — session recorder (SPEC §9.23). Records the performance as INPUT EVENTS on the engine's
// tick clock (key presses / releases and the gyro FX commands), not audio: a session is a few KB on the card
// (/lp/rec/rec_<NNN>.lprec, 8 bytes per event, appended while recording). A session is rendered to a WAV
// (/lp/wav/rec_<NNN>.wav, 22050 Hz mono int16) OFFLINE: the sketch stops feeding the speaker, resets the engine,
// the generator's Rng and the library bags to the state saved in the session header (seed + genre + key + tempo),
// then renders block after block on the loop task, replaying each recorded event at its tick through Ui::onKey
// (so quantisation, generation, loads and pages behave exactly as live), pumping the streams between blocks and
// writing every 256-sample block straight to the file (512 B, no extra buffer). Rendering runs as fast as the
// card allows (about 1–2 × real time); the speaker stays silent. Deterministic as long as the card content is
// unchanged; an asynchronous generate may land one bar later or earlier than live (documented caveat).
// Memory: one 32-event write-behind buffer (256 B; 64 events / 512 B before the 2026-10-16 RAM diet) + state; the session
// list is two 64-bit masks (bit NNN = rec_NNN.lprec / .wav exists) and ONE 8-byte name buffer, never a name table;
// nothing allocated after boot.
#pragma once
#include <stdint.h>
#include "../core/types.h"
#include "../io/fs.h"
#include "../io/keys.h"

namespace lp {

class Engine; class Generator; class Sets; class Ui;

struct RecEvent {                    // 8 bytes on the card, little-endian
  uint32_t tick;                     // engine tick (96 PPQN) at which the event happened
  uint8_t type;                      // 0 key press, 1 key release, 2 gyro command (x = filter -128..127, y = reverb 0..255, extra = echo 0..255), 3 output volume
  uint8_t x, y;                      // key column / row (types 0 / 1), or payload (types 2 / 3)
  uint8_t extra;                     // type 2: echo send 0..255; type 3: volume 0..255
};

struct RecHeader {                   // 64 bytes at the start of a session file
  char magic[8];                     // "LPREC1\0\0"
  uint32_t seed;                     // Rng seed restored before replay
  uint16_t bpm; int8_t keyRoot; uint8_t set1Autoplay;
  char genre[32];
  uint32_t events; uint32_t lengthTicks;   // written on stop (0 while recording / after a power loss: the file is still replayable to its last event)
  uint8_t reserved[8];
};

class Recorder {
 public:
  static constexpr const char* kRecDir = "/lp/rec";
  static constexpr const char* kWavDir = "/lp/wav";
  static constexpr int kMaxSessions = 64;             // rec_000 … rec_063
  static constexpr int kBuf = 32;                     // events buffered before a card write (256 B; 64 → 32 in the 2026-10-16 RAM diet) — also the render's chunk size

  void init(IFileSystem* fs, Engine* eng, Generator* gen, Sets* sets, Ui* ui);

  // ---- recording (live) ----
  bool startRecording();                              // creates the next rec_<NNN>.lprec, writes the header (seed, genre, key, tempo), returns false when the card / dir is missing or 64 sessions exist
  void stopRecording();                               // flushes, rewrites the header with the counts
  bool recording() const { return recording_; }
  void onInput(const RecEvent& e);                    // called by the Ui for every key event and gyro / volume command while recording (cheap, buffered)
  uint32_t recordedEvents() const { return count_; }

  // ---- sessions (the RECORDER page, SHIFT + save 1) ----
  int sessionCount();                                 // lists /lp/rec (one walk, cached until a change)
  const char* sessionName(int i);                     // "rec_007"
  bool sessionInfo(int i, RecHeader& out);            // header of session i (one card read)
  bool deleteSession(int i);                          // unlinks the .lprec and its .wav (if any); refuses while recording or rendering
  bool hasWav(int i);

  // ---- offline render ----
  bool startRender(int i);                            // opens the session and /lp/wav/rec_<NNN>.wav, asks the sketch to take the engine offline; false when busy / missing
  bool rendering() const { return rendering_; }
  int renderPercent() const;                          // 0..100 by replayed ticks against the header's lengthTicks (or events)
  void cancelRender();                                // closes the files, deletes the partial WAV, returns to live
  // One render step, called by the sketch's loop() while rendering() (the render task is parked meanwhile): replays due events
  // through Ui::onKey, renders ONE engine block into `block`, writes it to the WAV, pumps the streams; returns false when done.
  bool renderStep(int16_t* block);
  void step();                                        // housekeeping each loop(): flush the event buffer when the card is free (one bounded write)
  // The replay's clock while rendering(): the recorded time axis in ms (the engine's sample clock since startRender, continued
  // from the platform's millis when g_microsHook is set). renderStep ticks the Ui with it; the sketch passes it to Ui::draw and
  // to the live key / IMU events it still delivers meanwhile, so the Ui runs on ONE clock during a render (hold timers, pages).
  uint32_t renderNowMs() const;
  // True while renderStep delivers the session's events (and their synthesised repeats) to Ui::onKey: the Ui tells a replayed
  // event from a live one by it (a live key during a render is only the ` cancel hold; a replayed ` never cancels, a replayed
  // save / SHIFT + save 1 / drive hold never writes the card or reboots). False between steps, when the sketch's live keys arrive.
  bool replaying() const { return replaying_; }
  // The header seed of the session being recorded or rendered (0 when neither runs): the generator is re-seeded with it at
  // both starts, and the Ui may seed its own load Rng from it at both starts too, so a random LOAD inside a recording picks
  // the same preset on replay (its Rng is otherwise seeded from the platform clock, which no replay reproduces). Both starts
  // also call Sets::resetLoadCycle() (SPEC §9.26 masks and the §9.8 last names cleared), so the replayed load's candidates
  // are the recording's — on the device the Sets object keeps its state between the two, unlike two fresh boots.
  uint32_t sessionSeed() const { return seed_; }

 private:
  static constexpr int kHeld = 4;                     // replayed keys held at once whose KeyMatrix repeats are synthesised (SPEC §3 hold repeats)
  static constexpr int kOpenRetrySteps = 128;         // render steps a failed (transient) open of the session file is retried before the render gives up (SPEC §9.19g: the 7-handle pool can be full for a few steps)
  struct Held { uint8_t x = 0, y = 0, used = 0, pad = 0; uint32_t nextMs = 0; };
  IFileSystem* fs_ = nullptr; Engine* eng_ = nullptr; Generator* gen_ = nullptr; Sets* sets_ = nullptr; Ui* ui_ = nullptr;
  bool recording_ = false, rendering_ = false, replaying_ = false;   // replaying_: inside a replayed / synthesised key's Ui::onKey (see replaying())
  RecEvent buf_[kBuf]; uint8_t bufN_ = 0; uint32_t count_ = 0; uint32_t startTick_ = 0;   // recording: the write-behind buffer; rendering: one kBuf-event chunk of the session file
  IFile* rec_ = nullptr; IFile* wav_ = nullptr; uint32_t written_ = 0;   // WAV data bytes written (header patched on finish); rec_ is open for the whole recording, only transiently while rendering
  int8_t cur_ = -1; uint32_t replayed_ = 0; uint32_t total_ = 0; uint32_t lengthTicks_ = 0;   // session being rendered: events delivered / in the file, its length
  uint32_t base_ = 0, t0_ = 0; uint32_t chunkStart_ = 0; uint8_t chunkN_ = 0; uint8_t first_ = 0; uint8_t tail_ = 0; uint8_t openFails_ = 0; uint32_t tailLeft_ = 0;   // replay tick base (t0_ = the bar line the restart lands on), buf_'s first event index, the first block / tail state, failed chunk opens in a row
  uint32_t clock0_ = 0, ms0_ = 0;                    // renderNowMs: engine sample clock and platform ms at startRender
  uint32_t seed_ = 0;                                 // sessionSeed()
  Held held_[kHeld];
  uint64_t recMask_ = 0, wavMask_ = 0; int8_t sessions_ = -1; char name_[8];   // bit n: rec_<n>.lprec / rec_<n>.wav exists (one walk each, cached until a change)
  // helpers
  void scan();
  int index(int i);                                   // session i (by number) → its NNN, -1 when out of range
  void flush();
  bool readChunk();
  bool deliverDue(uint32_t tick, uint32_t ms);        // every loaded event whose tick has come, in file order; false when a chunk read failed (retried next step)
  void deliver(const RecEvent& e, uint32_t ms);
  void repeats(uint32_t ms);
  void uiKey(uint8_t x, uint8_t y, KeyEvent::Kind kind, uint32_t ms);   // one key to Ui::onKey as a replayed event (replaying() true for the call)
  void restoreLive(const RecHeader& h, uint32_t ms);  // startRender: the Ui / engine state the header's reserved[4..7] describe
  void holdPress(uint8_t x, uint8_t y, uint32_t ms); void holdRelease(uint8_t x, uint8_t y);
  void finishRender(bool keepWav);
};

} // namespace lp
