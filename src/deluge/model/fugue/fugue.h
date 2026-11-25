/*
 * This file is part of The Synthstrom Audible Deluge Firmware.
 *
 * The Synthstrom Audible Deluge Firmware is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "definitions_cxx.hpp"
#include "model/sync.h"
#include <cstdint>

class InstrumentClip;
class Output;
class Song;

namespace deluge::fugue {

struct FugueNote;
struct FugueScale;
struct FugueTransform;

constexpr int32_t kNumVoices = 3;  // voices 2, 3 and 4; voice 1 is the clip you are in
constexpr int32_t kNumTempos = 15; // 1/8 1/7 ... 1/2, 1, 2 ... 7 8
constexpr int32_t kTempoUnity = 7;

constexpr int32_t kMaxPitch = 14;
constexpr int32_t kMaxOctave = 4;

/// VoiceSettings::reso until the user picks a value: stands for a 16th note at the song's resolution
constexpr uint8_t kResoDefault = 255;

/// The sync menu value of a real 16th note in the current song
uint8_t defaultReso();
/// Sync menu value -> ticks. 0 means "no rounding grid".
int32_t resoToTicks(uint8_t reso);

enum class Destination : uint8_t {
	CLIP,  // the next three slots below, on the same track
	TRACK, // three cloned instruments, same row as the source
	BOTH,  // the three cloned instruments, plus three variants below every clip of that row: a 4x4 block
};

/// Arpeggiator and randomizer settings a voice can carry. Values are in menu units, the same ones the stock arp and
/// randomizer menus show.
enum class Extra : uint8_t {
	ARP_MODE, // ArpPreset: off, up, down, both, random, walk, custom
	ARP_SYNC, // a sync menu value
	ARP_GATE,
	ARP_RHYTHM,
	LOCK,
	NOTE_PROBABILITY,
	VELOCITY_SPREAD,
	BASS_PROBABILITY,
};
constexpr int32_t kNumExtras = 8;

/// An Extra that has not been touched: the voice keeps whatever the source clip has
constexpr int16_t kExtraFromSource = -1;

struct VoiceSettings {
	bool on{true};
	int8_t pitch{0};
	int8_t octave{0};
	bool invert{false};
	SequenceDirection direction{SequenceDirection::FORWARD};
	int16_t rotate{0};          // squares at the clip's current zoom
	uint8_t tempo{kTempoUnity}; // index into the tempo table
	uint8_t reso{kResoDefault}; // a sync menu value (0 = off), or kResoDefault
	int16_t extras[kNumExtras]{kExtraFromSource, kExtraFromSource, kExtraFromSource, kExtraFromSource,
	                           kExtraFromSource, kExtraFromSource, kExtraFromSource, kExtraFromSource};
};

enum class ApplyResult : uint8_t {
	DONE,
	NEEDS_CONFIRMATION, // a target already holds notes; nothing has been written
	FAILED,
};

/// Fugue: copies the notes of the current clip into up to three further clips, each with its own offsets.
/// Everything in here lives for the session only - nothing is saved with the song.
class Fugue {
public:
	Destination destination{Destination::CLIP};
	VoiceSettings voices[kNumVoices];

	/// Whether the destination setting can be TRACK for this clip (synth and MIDI yes, CV no)
	static bool canCloneTrack(InstrumentClip* clip);

	/// Rolls new offsets for every voice that is on. Writes nothing to the song.
	void randomize();

	/// What the menu shows for one of a voice's arp / randomizer settings: its own value once set, the current
	/// clip's until then.
	int32_t getExtra(int32_t voiceIndex, Extra extra);
	void setExtra(int32_t voiceIndex, Extra extra, int32_t value);

	static int32_t readClipExtra(InstrumentClip* clip, Extra extra);
	static void writeClipExtra(InstrumentClip* clip, Extra extra, int32_t value);

	/// Writes the voices. Shows its own popups. With overwriteConfirmed false it stops before writing anything if a
	/// target already holds notes.
	ApplyResult apply(bool overwriteConfirmed);

private:
	// The instruments TRACK created last time, and what they were created for
	Song* rememberedSong{nullptr};
	Output* rememberedSource{nullptr};
	Output* rememberedVoice[kNumVoices]{};

	struct SourceNote;

	void buildTransform(const VoiceSettings& voice, int32_t clipLength, FugueTransform* transform) const;
	Output* getRememberedVoiceOutput(InstrumentClip* source, int32_t voiceIndex);
	VoiceSettings makeVariant(const VoiceSettings& base, bool scaleActive, InstrumentClip* source) const;
	InstrumentClip* createVoiceClip(InstrumentClip* source, int32_t section, InstrumentClip* insertBelow);
	void discardVoiceClip(InstrumentClip* clip);
	Output* createVoiceInstrument(InstrumentClip* source, InstrumentClip* voiceClip);
	bool writeVoice(InstrumentClip* source, InstrumentClip* target, bool targetIsNew, const FugueNote* notes,
	                int32_t numNotes, int32_t newLength, const SourceNote* sourceNotes, const VoiceSettings& voice);
};

extern Fugue fugue;

} // namespace deluge::fugue
