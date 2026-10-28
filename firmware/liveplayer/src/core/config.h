// meehaw live performer — compile-time constants (SPEC §2, §5). Everything sized here is static.
#pragma once
#include <stdint.h>

namespace lp {

constexpr int kSampleRate = 22050;
constexpr int kBlock = 256;                 // samples per render block
constexpr int kPPQN = 96;                   // ticks per quarter note
constexpr int kBeatsPerBar = 4;
constexpr int kTicksPerBeat = kPPQN;
constexpr int kTicksPerBar = kPPQN * kBeatsPerBar;          // 384
constexpr int kMaxPatternBeats = 16;
constexpr int kMaxPatternTicks = kMaxPatternBeats * kPPQN;  // 1536 (fits 12 bits)
constexpr int kMaxNotes = 192;              // per Pattern
constexpr int kNoteLenUnit = 12;            // ticks per length unit (1/32 note)

constexpr int kSets = 2;
constexpr int kTracksPerSet = 8;
constexpr int kTracks = kSets * kTracksPerSet;
constexpr int kVariants = 4;

constexpr int kMinBpm = 40;
constexpr int kMaxBpm = 240;
constexpr int kDefaultBpm = 125;                // most House loops and the rap bank are rendered at 125 (docs/LOOPS.md); config.txt bpm= overrides at boot

// Sample pool (SPEC §9.1): one µ-law pool per set, 8 slots, replaced slot by slot through a staging buffer.
constexpr int kSampleSlots = 8;             // 0 kick 1 snare 2 hh 3 oh 4..7 perc A..D
constexpr int kCapKickMs = 400, kCapSnareMs = 350, kCapHhMs = 120, kCapOhMs = 350, kCapPercMs = 250;
constexpr int kCapMaxSamples = kCapKickMs * kSampleRate / 1000;                      // 8820 = staging size
constexpr int kPoolBytesPerSet = (kCapKickMs + kCapSnareMs + kCapHhMs + kCapOhMs + 4 * kCapPercMs) * kSampleRate / 1000;  // 48 952
constexpr int kStagingBytes = kCapMaxSamples;
constexpr int kSlotFadeSamples = 64;

constexpr int kSynthVoicesPerSet = 16;      // chord 4, pad 4, lead 2, bass 2, fx 4
constexpr int kSampleVoicesPerSet = 16;     // two instances per slot (retrigger/choke fades, SPEC §9.1)
constexpr int kVocalRingBytes = 8192;       // int16 samples ring per set (2 x 4096 B)
// Loop mode (SPEC §9.19): two loop slots device-wide besides the two vocal streams. Stream s: 0 / 1 = set s's vocal
// stream, kSets + k = loop slot k. Loop files are rendered at kLoopBpm (docs/LOOPS.md), whole bars, 22050 mono int16.
constexpr int kLoopSlots = 2;
constexpr int kStreams = kSets + kLoopSlots;
constexpr int kLoopRingBytes = 6144;                      // int16 ring per loop slot: 3072 samples = 139 ms, refilled in kLoopChunkSamples chunks
constexpr int kLoopChunkSamples = 1024;                   // one 2 KB card read per refill (space >= this many samples)
constexpr int kMaxLoopBars = 8;
constexpr int kMaxLoopTicks = kMaxLoopBars * kTicksPerBar;   // 3072: a streaming track's clip may be this long (patterns stay <= kMaxPatternTicks)
constexpr int kLoopBpm = 125;                             // rate = bpm / kLoopBpm, as the vocal's bpm / fileBpm
constexpr int kLoopLowRamBytes = 6 * 1024;                // loop mode is refused below this much free heap (RAM diet 2026-10-16: was 10 KB; an open card file costs no heap since the POSIX SD layer, FatFs' LFN scratch ≈ 1 KB per open is the only runtime heap user)
// Vocal clip (SPEC §9.16): a cycle of window + pause bars. The phrase plays from its start for the window (shorter
// phrases end early, longer ones fade out at its end), then silence for the pause (0, 1, 2 or 4 bars, default 1).
constexpr int kVocalWindowBars = 2;
constexpr int kVocalWindowTicks = kVocalWindowBars * kTicksPerBar;              // 768
constexpr int kVocalPauseMaxBars = 4;
constexpr int kVocalPauseDefault = 1;
constexpr int kMaxVocalClipTicks = (16 + kVocalPauseMaxBars) * kTicksPerBar;   // 7680: a 16-bar window ('<n>bars' token, SPEC §9.16 / §9.18.1) + the longest pause; fits the engine's 16-bit clip length

constexpr int kEchoSamples = 10240;         // 464 ms at 22050: 1/4 note up to 129 BPM, longer divisions fold (SPEC §9.13)
constexpr int kLookaheadSamples = 64;       // master limiter lookahead (SPEC §9.11)
constexpr int kParamSmoothMs = 10;
constexpr int kMaxSdBurstMs = 20;           // SPEC §9.2
constexpr int kMaxRamps = 8;
constexpr int kCmdQueueSize = 128;
constexpr int kRampBarsDefault = 4;

constexpr int kKeyCols = 14;
constexpr int kKeyRows = 4;
constexpr int kKeyRepeatDelayMs = 350;
constexpr int kKeyRepeatIntervalMs = 80;
constexpr int kHoldToggleMs = 400;          // gyro fx press-and-hold
constexpr int kParamPageMs = 1200;          // full-screen parameter page auto-return
constexpr int kMaxPathLen = 160;
constexpr int kMaxNameLen = 96;
// Card file handles (SPEC §9.19f / §9.19g): the device mounts the card with this many FatFs file slots (SdFs::kMaxOpen, each a
// preallocated 4.2 KB FIL) and the host file system models the same pool. Budget: one per live stream (kStreams), two for
// new-file stream switches waiting for their bar line (Generator::kPendingSwitches) and one for the job's own kit / index /
// SMF / set file. The generator makes a new-file stream open only while one handle stays free after it.
constexpr int kMaxOpenFiles = 7;

} // namespace lp
