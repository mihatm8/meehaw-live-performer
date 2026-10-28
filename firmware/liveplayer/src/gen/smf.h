// Standard MIDI file parser (format 0/1, any PPQ) into a Pattern (SPEC §6). Streams from IFile with a small buffer;
// keeps the first kMaxNotes notes by start time, merges tracks, ignores channel 10 distinction (pitches are the contract).
#pragma once
#include "../core/types.h"
#include "../io/fs.h"
namespace lp {
struct SmfInfo { uint16_t ppq = 96; float bpm = 0; uint32_t lengthTicksSrc = 0; uint16_t notes = 0; bool truncated = false; };
// Length: musical length rounded UP to whole beats, min 1 beat, max kMaxPatternBeats (longer files are cut).
bool smfParse(IFile& f, Pattern& out, SmfInfo* info = nullptr);
bool smfParseBuffer(const uint8_t* data, uint32_t size, Pattern& out, SmfInfo* info = nullptr);
// Writer for set assets / editor exports: type-0 file at kPPQN.
bool smfWrite(IFile& f, const Pattern& p, float bpm);
}
