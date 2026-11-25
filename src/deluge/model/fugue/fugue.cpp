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

#include "model/fugue/fugue.h"
#include "definitions_cxx.hpp"
#include "gui/l10n/l10n.h"
#include "gui/menu_item/value_scaling.h"
#include "hid/display/display.h"
#include "memory/general_memory_allocator.h"
#include "model/action/action_logger.h"
#include "model/clip/instrument_clip.h"
#include "model/fugue/fugue_transform.h"
#include "model/instrument/instrument.h"
#include "model/instrument/midi_instrument.h"
#include "model/model_stack.h"
#include "model/note/note.h"
#include "model/note/note_row.h"
#include "model/song/song.h"
#include "modulation/arpeggiator.h"
#include "modulation/params/param.h"
#include "modulation/params/param_manager.h"
#include "modulation/params/param_set.h"
#include "storage/storage_manager.h"
#include "util/d_string.h"
#include "util/functions.h"
#include <algorithm>

namespace deluge::fugue {

Fugue fugue{};

struct Fugue::SourceNote {
	int32_t y;
	int32_t pos;
	int32_t length;
	uint8_t velocity;
	uint8_t probability;
	uint8_t fill;
	Iterance iterance;
};

namespace {

struct TempoRatio {
	int8_t timeMul;
	int8_t timeDiv;
};

// 1/8 ... 1/2, 1, 2 ... 8 - a slower tempo makes the clip longer
constexpr TempoRatio kTempoRatios[kNumTempos] = {{8, 1}, {7, 1}, {6, 1}, {5, 1}, {4, 1}, {3, 1}, {2, 1}, {1, 1},
                                                 {1, 2}, {1, 3}, {1, 4}, {1, 5}, {1, 6}, {1, 7}, {1, 8}};

/// One clip Apply is going to write
struct Job {
	int8_t column; // 0 = the source's own track, 1..3 = the track of voice 2..4
	int8_t row;    // how many sections below the source
	int8_t voice;  // the voice whose column this is, or -1 for the source's own column
	VoiceSettings settings;
	InstrumentClip* existing;
};

// Three voices, plus (BOTH only) three variants under the source and under each voice
constexpr int32_t kMaxJobs = kNumVoices + (kNumVoices + 1) * 3;

void popup(l10n::String string) {
	display->displayPopup(l10n::get(string));
}

InstrumentClip* findSessionClip(Output* output, int32_t section, Clip* exclude) {
	for (int32_t i = 0; i < currentSong->sessionClips.getNumElements(); i++) {
		Clip* clip = currentSong->sessionClips.getClipAtIndex(i);
		if (clip != exclude && clip->output == output && clip->section == section
		    && clip->type == ClipType::INSTRUMENT) {
			return static_cast<InstrumentClip*>(clip);
		}
	}
	return nullptr;
}

bool isOutputInSong(Output* output) {
	for (Output* thisOutput = currentSong->firstOutput; thisOutput != nullptr; thisOutput = thisOutput->next) {
		if (thisOutput == output) {
			return true;
		}
	}
	return false;
}

void getScale(InstrumentClip* clip, FugueScale* scale) {
	const MusicalKey& key = currentSong->key;
	int32_t size = key.modeNotes.count();
	scale->active = clip->isScaleModeClip() && size >= 1 && size <= 12;
	if (!scale->active) {
		return;
	}
	scale->root = key.rootNote;
	scale->size = size;
	for (int32_t i = 0; i < size; i++) {
		scale->notes[i] = key.modeNotes[i];
	}
}

Error makeFilePath(Instrument* instrument, String* name, String* path) {
	path->set(&instrument->dirPath);
	Error error = path->concatenate("/");
	if (error == Error::NONE) {
		error = path->concatenate(name);
	}
	if (error == Error::NONE) {
		error = path->concatenate(".XML");
	}
	return error;
}

} // namespace

int32_t resoToTicks(uint8_t reso) {
	if (reso == kResoDefault) {
		reso = defaultReso();
	}
	SyncLevel level = syncValueToSyncLevel(reso);
	if (level == SYNC_LEVEL_NONE) {
		return 0;
	}
	int32_t ticks = 3 << (MAX_SYNC_LEVEL - level);
	switch (syncValueToSyncType(reso)) {
	case SYNC_TYPE_TRIPLET:
		ticks = ticks * 2 / 3;
		break;
	case SYNC_TYPE_DOTTED:
		ticks = ticks * 3 / 2;
		break;
	default:
		break;
	}
	return std::max<int32_t>(ticks, 1);
}

uint8_t defaultReso() {
	// A real 16th note, whatever the song's resolution is
	int32_t sixteenth = currentSong->getSixteenthNoteLength();
	for (int32_t level = SYNC_LEVEL_WHOLE; level <= MAX_SYNC_LEVEL; level++) {
		if ((3 << (MAX_SYNC_LEVEL - level)) == sixteenth) {
			return static_cast<uint8_t>(level); // even note lengths start at menu value 0
		}
	}
	return static_cast<uint8_t>(SYNC_LEVEL_16TH);
}

bool Fugue::canCloneTrack(InstrumentClip* clip) {
	if (clip == nullptr || clip->output == nullptr) {
		return false;
	}
	switch (clip->output->type) {
	case OutputType::SYNTH:
		return true;
	case OutputType::MIDI_OUT:
		// MPE zones and internal destinations have no channel suffixes
		return static_cast<MIDIInstrument*>(clip->output)->getChannel() < 16;
	default:
		// CV: one instrument per CV channel, and the output is monophonic anyway
		return false;
	}
}

void Fugue::randomize() {
	// A "tame wiggle": a fresh roll inside small ranges. random(n) returns 0..n inclusive.
	for (VoiceSettings& voice : voices) {
		if (!voice.on) {
			continue;
		}
		voice.pitch = random(14) - 7;
		voice.octave = random(2) - 1;
		voice.invert = (random(3) == 0);
		switch (random(3)) {
		case 2:
			voice.direction = SequenceDirection::REVERSE;
			break;
		case 3:
			voice.direction = SequenceDirection::PINGPONG;
			break;
		default:
			voice.direction = SequenceDirection::FORWARD;
			break;
		}
		voice.rotate = random(8) - 4;
		switch (random(3)) {
		case 0:
			voice.tempo = kTempoUnity - 1; // 1/2
			break;
		case 3:
			voice.tempo = kTempoUnity + 1; // 2
			break;
		default:
			voice.tempo = kTempoUnity;
			break;
		}

		// The arp and randomizer settings get rolled too, just as mildly. From here on the voice has its own
		// values for them instead of the source clip's.
		auto setExtraValue = [&voice](Extra extra, int32_t value) { voice.extras[util::to_underlying(extra)] = value; };
		// Arp: off half the time, otherwise up, down, both, random or walk
		setExtraValue(Extra::ARP_MODE, (random(1) == 0) ? 0 : (1 + random(4)));
		// Sync: an 8th, a 16th or a 32nd, the 16th twice as likely
		int32_t sync = defaultReso() + ((random(3) == 0) ? -1 : (random(2) == 0) ? 1 : 0);
		setExtraValue(Extra::ARP_SYNC, std::clamp<int32_t>(sync, SYNC_LEVEL_WHOLE, MAX_SYNC_LEVEL));
		setExtraValue(Extra::ARP_GATE, 15 + random(25));
		// Rhythm: none half the time, otherwise any pattern
		setExtraValue(Extra::ARP_RHYTHM, (random(1) == 0) ? 0 : (1 + random(kMaxMenuValue - 1)));
		setExtraValue(Extra::LOCK, (random(3) == 0) ? 1 : 0);
		setExtraValue(Extra::NOTE_PROBABILITY, 35 + random(15)); // 50 = every note plays
		setExtraValue(Extra::VELOCITY_SPREAD, random(15));
		setExtraValue(Extra::BASS_PROBABILITY, (random(1) == 0) ? 0 : random(20));
	}
}

int32_t Fugue::readClipExtra(InstrumentClip* clip, Extra extra) {
	using namespace deluge::modulation::params;
	if (clip == nullptr || clip->output == nullptr) {
		return 0;
	}
	ArpeggiatorSettings& arp = clip->arpSettings;
	// A synth keeps these as parameters of the clip; MIDI and CV clips keep them in the arp settings
	UnpatchedParamSet* unpatched =
	    (clip->output->type == OutputType::SYNTH) ? clip->paramManager.getUnpatchedParamSet() : nullptr;

	switch (extra) {
	case Extra::ARP_MODE:
		return util::to_underlying(arp.preset);
	case Extra::ARP_SYNC: // Same sum as the sync menu item uses
		return static_cast<int32_t>(arp.syncType)
		       + (static_cast<int32_t>(arp.syncLevel) - (arp.syncType != SYNC_TYPE_EVEN ? 1 : 0));
	case Extra::LOCK:
		return arp.randomizerLock ? 1 : 0;
	case Extra::ARP_GATE:
		return computeCurrentValueForStandardMenuItem(unpatched ? unpatched->getValue(UNPATCHED_ARP_GATE) : arp.gate);
	case Extra::ARP_RHYTHM:
		return unpatched ? computeCurrentValueForStandardMenuItem(unpatched->getValue(UNPATCHED_ARP_RHYTHM))
		                 : computeCurrentValueForUnsignedMenuItem(arp.rhythm);
	case Extra::NOTE_PROBABILITY:
		return unpatched ? computeCurrentValueForStandardMenuItem(unpatched->getValue(UNPATCHED_NOTE_PROBABILITY))
		                 : computeCurrentValueForUnsignedMenuItem(arp.noteProbability);
	case Extra::VELOCITY_SPREAD:
		return unpatched ? computeCurrentValueForStandardMenuItem(unpatched->getValue(UNPATCHED_SPREAD_VELOCITY))
		                 : computeCurrentValueForUnsignedMenuItem(arp.spreadVelocity);
	case Extra::BASS_PROBABILITY:
		return unpatched ? computeCurrentValueForStandardMenuItem(unpatched->getValue(UNPATCHED_ARP_BASS_PROBABILITY))
		                 : computeCurrentValueForUnsignedMenuItem(arp.bassProbability);
	}
	return 0;
}

void Fugue::writeClipExtra(InstrumentClip* clip, Extra extra, int32_t value) {
	using namespace deluge::modulation::params;
	if (clip == nullptr || clip->output == nullptr) {
		return;
	}
	ArpeggiatorSettings& arp = clip->arpSettings;
	UnpatchedParamSet* unpatched =
	    (clip->output->type == OutputType::SYNTH) ? clip->paramManager.getUnpatchedParamSet() : nullptr;

	auto setParam = [&](int32_t param, uint32_t* midiCvField) {
		value = std::clamp<int32_t>(value, 0, kMaxMenuValue);
		if (unpatched != nullptr) {
			unpatched->params[param].setCurrentValueBasicForSetup(computeFinalValueForStandardMenuItem(value));
		}
		else {
			*midiCvField = computeFinalValueForUnsignedMenuItem(value);
		}
	};

	switch (extra) {
	case Extra::ARP_MODE:
		arp.preset = static_cast<ArpPreset>(std::clamp<int32_t>(value, 0, util::to_underlying(ArpPreset::CUSTOM)));
		arp.updateSettingsFromCurrentPreset();
		arp.flagForceArpRestart = true;
		break;
	case Extra::ARP_SYNC:
		value = std::clamp<int32_t>(value, 0, NUM_SYNC_VALUES - 1);
		arp.syncType = syncValueToSyncType(value);
		arp.syncLevel = syncValueToSyncLevel(value);
		break;
	case Extra::LOCK:
		arp.randomizerLock = (value != 0);
		break;
	case Extra::ARP_GATE:
		value = std::clamp<int32_t>(value, 0, kMaxMenuValue);
		if (unpatched != nullptr) {
			unpatched->params[UNPATCHED_ARP_GATE].setCurrentValueBasicForSetup(
			    computeFinalValueForStandardMenuItem(value));
		}
		else {
			arp.gate = computeFinalValueForStandardMenuItem(value);
		}
		break;
	case Extra::ARP_RHYTHM:
		setParam(UNPATCHED_ARP_RHYTHM, &arp.rhythm);
		break;
	case Extra::NOTE_PROBABILITY:
		setParam(UNPATCHED_NOTE_PROBABILITY, &arp.noteProbability);
		break;
	case Extra::VELOCITY_SPREAD:
		setParam(UNPATCHED_SPREAD_VELOCITY, &arp.spreadVelocity);
		break;
	case Extra::BASS_PROBABILITY:
		setParam(UNPATCHED_ARP_BASS_PROBABILITY, &arp.bassProbability);
		break;
	}
}

int32_t Fugue::getExtra(int32_t voiceIndex, Extra extra) {
	int16_t value = voices[voiceIndex].extras[util::to_underlying(extra)];
	if (value != kExtraFromSource) {
		return value;
	}
	// Not set for this voice yet: show what the clip you are in has, since that is what the voice will get
	return readClipExtra(getCurrentInstrumentClip(), extra);
}

void Fugue::setExtra(int32_t voiceIndex, Extra extra, int32_t value) {
	voices[voiceIndex].extras[util::to_underlying(extra)] = value;
}

/// A variant of a clip for the rows below it (BOTH): two of its settings, picked at random, moved a little.
/// Tempo is never one of them, so the variants keep the length of the clip above.
VoiceSettings Fugue::makeVariant(const VoiceSettings& base, bool scaleActive, InstrumentClip* source) const {
	VoiceSettings variant = base;

	// What the clip at the top of the column has for an arp / randomizer setting
	auto baseExtra = [&](Extra extra) -> int32_t {
		int16_t value = base.extras[util::to_underlying(extra)];
		return (value != kExtraFromSource) ? value : readClipExtra(source, extra);
	};
	auto setExtraValue = [&variant](Extra extra, int32_t value) { variant.extras[util::to_underlying(extra)] = value; };
	// Moves a 0-50 setting by `amount` up or down, turning round at the ends of the range
	auto nudgeExtra = [&](Extra extra, int32_t amount, bool up) {
		int32_t value = std::clamp<int32_t>(baseExtra(extra), 0, kMaxMenuValue);
		int32_t moved = value + (up ? amount : -amount);
		if (moved < 0 || moved > kMaxMenuValue) {
			moved = value - (up ? amount : -amount);
		}
		setExtraValue(extra, std::clamp<int32_t>(moved, 0, kMaxMenuValue));
	};

	enum Candidate : int8_t { PITCH, ROTATE, OCTAVE, INVERT, DIRECTION, ARP_MODE, NOTE_PROBABILITY, RHYTHM, BASS };
	int8_t candidates[9] = {PITCH, ROTATE, OCTAVE, INVERT, DIRECTION, ARP_MODE, NOTE_PROBABILITY};
	int32_t numCandidates = 7;
	// Arp rhythm and bass probability only do something while the arp is running
	if (baseExtra(Extra::ARP_MODE) != 0) {
		candidates[numCandidates++] = RHYTHM;
		candidates[numCandidates++] = BASS;
	}

	// Two different ones. random(n) returns 0..n inclusive.
	int32_t first = random(numCandidates - 1);
	int32_t second = (first + 1 + random(numCandidates - 2)) % numCandidates;

	for (int32_t index : {first, second}) {
		bool up = (random(1) == 1);
		switch (candidates[index]) {
		case PITCH: { // Jump to a chord tone - a third, a fifth or a seventh away
			static constexpr int8_t scaleJumps[3] = {2, 4, 6};
			static constexpr int8_t semitoneJumps[3] = {4, 7, 10};
			int32_t jump = scaleActive ? scaleJumps[random(2)] : semitoneJumps[random(2)];
			variant.pitch = base.pitch + (up ? jump : -jump);
			break;
		}
		case ROTATE: { // 1 to 8 squares either way
			int32_t squares = 1 + random(7);
			variant.rotate = base.rotate + (up ? squares : -squares);
			break;
		}
		case OCTAVE: { // One up or down, staying inside the menu's range
			int32_t octave = base.octave + (up ? 1 : -1);
			if (octave > kMaxOctave || octave < -kMaxOctave) {
				octave = base.octave - (up ? 1 : -1);
			}
			variant.octave = octave;
			break;
		}
		case INVERT:
			variant.invert = !base.invert;
			break;
		case DIRECTION: // On to the next mode
			switch (base.direction) {
			case SequenceDirection::FORWARD:
				variant.direction = SequenceDirection::REVERSE;
				break;
			case SequenceDirection::REVERSE:
				variant.direction = SequenceDirection::PINGPONG;
				break;
			default:
				variant.direction = SequenceDirection::FORWARD;
				break;
			}
			break;
		case ARP_MODE: { // Any other one of off, up, down, both, random, walk
			int32_t mode = std::clamp<int32_t>(baseExtra(Extra::ARP_MODE), 0, 5);
			setExtraValue(Extra::ARP_MODE, (mode + 1 + random(4)) % 6);
			break;
		}
		case NOTE_PROBABILITY:
			nudgeExtra(Extra::NOTE_PROBABILITY, 5 + random(5), up);
			break;
		case RHYTHM: // A pattern a few places along the list
			nudgeExtra(Extra::ARP_RHYTHM, 1 + random(3), up);
			break;
		default: // BASS
			nudgeExtra(Extra::BASS_PROBABILITY, 5 + random(5), up);
			break;
		}
	}
	return variant;
}

void Fugue::buildTransform(const VoiceSettings& voice, int32_t clipLength, FugueTransform* transform) const {
	int32_t tempo = std::clamp<int32_t>(voice.tempo, 0, kNumTempos - 1);
	transform->invert = voice.invert;
	transform->pitchSteps = voice.pitch;
	transform->octaves = voice.octave;
	// Rotate counts squares at the clip's current zoom. A whole lap is no rotation at all.
	int64_t rotateTicks = (int64_t)voice.rotate * (int64_t)currentSong->xZoom[NAVIGATION_CLIP];
	transform->rotateTicks = (clipLength > 0) ? (int32_t)(rotateTicks % clipLength) : 0;
	transform->timeMul = kTempoRatios[tempo].timeMul;
	transform->timeDiv = kTempoRatios[tempo].timeDiv;
	transform->gridTicks = resoToTicks(voice.reso);
}

/// The instrument TRACK made for this voice last time, if it is still there
Output* Fugue::getRememberedVoiceOutput(InstrumentClip* source, int32_t voiceIndex) {
	if (rememberedSong != currentSong || rememberedSource != source->output) {
		return nullptr;
	}
	Output* output = rememberedVoice[voiceIndex];
	if (output == nullptr || output == source->output || !isOutputInSong(output)
	    || output->type != source->output->type) {
		return nullptr;
	}
	return output;
}

/// Makes an empty copy of the source clip (same sound settings, length, scale mode and colour) and puts it in the
/// song directly below `insertBelow`.
InstrumentClip* Fugue::createVoiceClip(InstrumentClip* source, int32_t section, InstrumentClip* insertBelow) {
	if (!currentSong->sessionClips.ensureEnoughSpaceAllocated(1)) {
		display->displayError(Error::INSUFFICIENT_RAM);
		return nullptr;
	}

	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStackWithTimelineCounter* modelStack =
	    setupModelStackWithSong(modelStackMemory, currentSong)->addTimelineCounter(source);

	Error error = source->clone(modelStack, false);
	if (error != Error::NONE) {
		display->displayError(error);
		return nullptr;
	}

	// clone() has pointed the model stack at the new clip
	auto* newClip = static_cast<InstrumentClip*>(modelStack->getTimelineCounter());
	newClip->section = section;
	newClip->onKeyboardScreen = false;
	newClip->onAutomationClipView = false;

	// Notes, MPE and automation all go: the voice gets its own notes, and automation is not copied
	newClip->clear(nullptr, modelStack, true, true);

	int32_t index = currentSong->sessionClips.getIndexForClip(insertBelow);
	if (index < 0) {
		index = 0;
	}
	// Can't fail - we ensured enough space in advance
	currentSong->sessionClips.insertClipAtIndex(newClip, index);
	return newClip;
}

/// Takes a clip made by createVoiceClip() back out of the song
void Fugue::discardVoiceClip(InstrumentClip* clip) {
	int32_t index = currentSong->sessionClips.getIndexForClip(clip);
	if (index >= 0) {
		currentSong->sessionClips.deleteAtIndex(index);
	}
	clip->~InstrumentClip();
	delugeDealloc(clip);
}

/// Gives the voice clip an instrument of its own: a clone of the source's. Returns it, or nullptr (after showing why).
Output* Fugue::createVoiceInstrument(InstrumentClip* source, InstrumentClip* voiceClip) {
	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStackWithTimelineCounter* modelStack =
	    setupModelStackWithSong(modelStackMemory, currentSong)->addTimelineCounter(voiceClip);

	Instrument* newInstrument = nullptr;

	// MIDI: same channel, next free suffix
	if (source->output->type == OutputType::MIDI_OUT) {
		auto* sourceInstrument = static_cast<MIDIInstrument*>(source->output);
		int32_t channel = sourceInstrument->getChannel();
		int32_t suffix = sourceInstrument->channelSuffix + 1;
		for (; suffix < 26; suffix++) {
			if (currentSong->getInstrumentFromPresetSlot(OutputType::MIDI_OUT, channel, suffix, nullptr, nullptr, false)
			    == nullptr) {
				break;
			}
		}
		if (suffix < 0 || suffix >= 26) {
			popup(l10n::String::STRING_FOR_NO_UNUSED_CHANNELS_AVAILABLE);
			return nullptr;
		}

		bool instrumentAlreadyInSong = false;
		newInstrument = currentSong->getNonAudioInstrumentToSwitchTo(
		    OutputType::MIDI_OUT, Availability::INSTRUMENT_UNUSED, channel, suffix, &instrumentAlreadyInSong);
		if (newInstrument == nullptr) {
			return nullptr; // It has shown its own popup
		}

		// Same steps as stock grid view when it copies a MIDI clip to a new track
		voiceClip->output = newInstrument;
		Error error = voiceClip->setNonAudioInstrument(newInstrument, currentSong);
		if (error != Error::NONE) {
			voiceClip->output = source->output;
			display->displayError(error);
			return nullptr;
		}
		if (!instrumentAlreadyInSong) {
			currentSong->addOutput(newInstrument);
		}
	}

	// Synth: read the preset from the card again, like "Clone" in the preset browser does
	else {
		auto* sourceInstrument = static_cast<Instrument*>(source->output);

		String filePath;
		FilePointer filePointer;
		Error error = makeFilePath(sourceInstrument, &sourceInstrument->name, &filePath);
		if (error != Error::NONE) {
			display->displayError(error);
			return nullptr;
		}
		if (!StorageManager::fileExists(filePath.get(), &filePointer)) {
			// Only presets that have a file on the card can be cloned
			display->displayError(Error::FILE_NOT_SAVED);
			return nullptr;
		}

		// A name nobody uses yet, neither in the song nor on the card: "name 2", "name 3" ...
		String newName;
		bool foundName = false;
		for (int32_t number = 2; number < 100 && !foundName; number++) {
			newName.set(&sourceInstrument->name);
			error = newName.concatenate(" ");
			if (error == Error::NONE) {
				error = newName.concatenateInt(number);
			}
			if (error != Error::NONE) {
				display->displayError(error);
				return nullptr;
			}
			if (currentSong->getInstrumentFromPresetSlot(OutputType::SYNTH, 0, -1, newName.get(),
			                                             sourceInstrument->dirPath.get(), true, true)
			    != nullptr) {
				continue;
			}
			String newPath;
			if (makeFilePath(sourceInstrument, &newName, &newPath) == Error::NONE
			    && StorageManager::fileExists(newPath.get())) {
				continue;
			}
			foundName = true;
		}
		if (!foundName) {
			display->displayError(Error::UNSPECIFIED);
			return nullptr;
		}

		error =
		    StorageManager::loadInstrumentFromFile(currentSong, nullptr, OutputType::SYNTH, false, &newInstrument,
		                                           &filePointer, &sourceInstrument->name, &sourceInstrument->dirPath);
		if (error != Error::NONE || newInstrument == nullptr) {
			display->displayError(error != Error::NONE ? error : Error::UNSPECIFIED);
			return nullptr;
		}
		newInstrument->name.set(&newName);
		newInstrument->editedByUser = true; // So the song saves it in full - there is no file of that name

		display->displayLoadingAnimationText("Loading", false, true);
		error = newInstrument->loadAllAudioFiles(true);
		display->removeLoadingAnimation();
		if (error != Error::NONE) {
			currentSong->deleteOutput(newInstrument);
			display->displayError(error);
			return nullptr;
		}

		// The voice clip takes the source clip's current knob values (without its automation)
		error = voiceClip->changeInstrument(modelStack, newInstrument, nullptr, InstrumentRemoval::NONE, source);
		if (error != Error::NONE) {
			currentSong->deleteOutput(newInstrument);
			display->displayError(error);
			return nullptr;
		}
		currentSong->addOutput(newInstrument);
	}

	if (newInstrument->getActiveClip() == nullptr) {
		newInstrument->setActiveClip(modelStack);
	}
	return newInstrument;
}

/// Replaces the target's notes with this voice's. Sound settings and automation of the target are left alone.
bool Fugue::writeVoice(InstrumentClip* source, InstrumentClip* target, bool targetIsNew, const FugueNote* notes,
                       int32_t numNotes, int32_t newLength, const SourceNote* sourceNotes, const VoiceSettings& voice) {
	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStackWithTimelineCounter* modelStack =
	    setupModelStackWithSong(modelStackMemory, currentSong)->addTimelineCounter(target);

	if (!targetIsNew) {
		target->stopAllNotesPlaying(modelStack);
		target->clear(nullptr, modelStack, false, true); // notes and MPE, not automation

		// Don't let chromatic notes rewrite the song's scale through a scale-mode target
		if (target->inScaleMode && !source->inScaleMode) {
			target->inScaleMode = false;
		}
	}

	// Every row of a voice follows the voice's clip length and direction
	for (int32_t i = 0; i < target->noteRows.getNumElements(); i++) {
		NoteRow* noteRow = target->noteRows.getElement(i);
		noteRow->loopLengthIfIndependent = 0;
		noteRow->sequenceDirectionMode = SequenceDirection::OBEY_PARENT;
	}

	if (target->loopLength != newLength) {
		currentSong->setClipLength(target, newLength, nullptr);
	}

	// The notes come sorted by pitch, so each row is filled in one go, in position order
	bool allWritten = true;
	for (int32_t i = 0; i < numNotes; i++) {
		const FugueNote& note = notes[i];
		const SourceNote& sourceNote = sourceNotes[note.tag];

		ModelStackWithNoteRow* modelStackWithNoteRow =
		    target->getOrCreateNoteRowForYNote(note.y, modelStack, nullptr, nullptr);
		NoteRow* noteRow = modelStackWithNoteRow->getNoteRowAllowNull();
		if (noteRow == nullptr) {
			allWritten = false; // Out of RAM
			break;
		}
		noteRow->attemptNoteAdd(note.pos, note.length, sourceNote.velocity, sourceNote.probability, sourceNote.iterance,
		                        sourceNote.fill, modelStackWithNoteRow, nullptr);
	}

	target->setSequenceDirectionMode(modelStack, voice.direction);

	// Scroll so the lowest note sits on the second row from the bottom
	if (numNotes > 0) {
		target->yScroll = target->getYVisualFromYNote(notes[0].y, currentSong) - 1;
	}

	if (!allWritten) {
		display->displayError(Error::INSUFFICIENT_RAM);
	}
	return allWritten;
}

ApplyResult Fugue::apply(bool overwriteConfirmed) {
	using enum l10n::String;

	InstrumentClip* source = getCurrentInstrumentClip();
	if (source == nullptr || source->type != ClipType::INSTRUMENT || source->output == nullptr) {
		popup(STRING_FOR_FUGUE_CANT);
		return ApplyResult::FAILED;
	}
	OutputType outputType = source->output->type;
	bool typeOk =
	    (outputType == OutputType::SYNTH || outputType == OutputType::MIDI_OUT || outputType == OutputType::CV);
	if (!typeOk || source->isArrangementOnlyClip()) {
		popup(STRING_FOR_FUGUE_CANT);
		return ApplyResult::FAILED;
	}

	bool anyVoiceOn = false;
	for (const VoiceSettings& voice : voices) {
		anyVoiceOn = anyVoiceOn || voice.on;
	}
	if (!anyVoiceOn) {
		popup(STRING_FOR_FUGUE_NO_VOICES);
		return ApplyResult::FAILED;
	}

	// CV clips and MIDI clips on an MPE zone can't be cloned to other tracks: they always get CLIP
	Destination effectiveDestination = canCloneTrack(source) ? destination : Destination::CLIP;
	bool toTracks = (effectiveDestination != Destination::CLIP);
	bool withVariants = (effectiveDestination == Destination::BOTH);

	// --- Take a snapshot of the source notes ------------------------------------------------------------------------
	int32_t maxNotes = 0;
	for (int32_t i = 0; i < source->noteRows.getNumElements(); i++) {
		maxNotes += source->noteRows.getElement(i)->notes.getNumElements();
	}
	if (maxNotes == 0) {
		popup(STRING_FOR_FUGUE_NO_NOTES);
		return ApplyResult::FAILED;
	}

	auto* sourceNotes =
	    static_cast<SourceNote*>(GeneralMemoryAllocator::get().allocLowSpeed(maxNotes * sizeof(SourceNote)));
	auto* workNotes =
	    static_cast<FugueNote*>(GeneralMemoryAllocator::get().allocLowSpeed(maxNotes * sizeof(FugueNote)));
	if (sourceNotes == nullptr || workNotes == nullptr) {
		if (sourceNotes != nullptr) {
			delugeDealloc(sourceNotes);
		}
		if (workNotes != nullptr) {
			delugeDealloc(workNotes);
		}
		display->displayError(Error::INSUFFICIENT_RAM);
		return ApplyResult::FAILED;
	}

	int32_t numSourceNotes = 0;
	int32_t sourceLength = source->loopLength;
	for (int32_t i = 0; i < source->noteRows.getNumElements(); i++) {
		NoteRow* noteRow = source->noteRows.getElement(i);
		// A row with its own length only contributes what lies inside it and inside the clip
		int32_t rowLength = noteRow->loopLengthIfIndependent ? noteRow->loopLengthIfIndependent : sourceLength;
		rowLength = std::min(rowLength, sourceLength);
		for (int32_t n = 0; n < noteRow->notes.getNumElements() && numSourceNotes < maxNotes; n++) {
			Note* note = noteRow->notes.getElement(n);
			if (note->pos < 0 || note->pos >= rowLength) {
				continue;
			}
			SourceNote& sourceNote = sourceNotes[numSourceNotes++];
			sourceNote.y = noteRow->getNoteCode();
			sourceNote.pos = note->pos;
			sourceNote.length = std::clamp<int32_t>(note->getLength(), 1, sourceLength);
			sourceNote.velocity = note->getVelocity();
			sourceNote.probability = note->getProbability();
			sourceNote.fill = note->getFill();
			sourceNote.iterance = note->getIterance();
		}
	}

	FugueScale scale;
	getScale(source, &scale);

	// Runs one clip's maths into workNotes
	auto transformNotes = [&](const VoiceSettings& settings, int32_t* newLength, FugueError* error) {
		for (int32_t i = 0; i < numSourceNotes; i++) {
			workNotes[i] = {sourceNotes[i].y, sourceNotes[i].pos, sourceNotes[i].length, i};
		}
		FugueTransform transform;
		buildTransform(settings, sourceLength, &transform);
		return fugueApplyTransform(workNotes, numSourceNotes, sourceLength, scale, transform, newLength, error);
	};

	ApplyResult result = ApplyResult::DONE;
	Job jobs[kMaxJobs];
	int32_t numJobs = 0;
	// Which instrument each column is on. Column 0 is the source's; the others are known once they exist.
	Output* columnOutput[kNumVoices + 1]{};
	columnOutput[0] = source->output;

	// --- Check everything before writing anything -------------------------------------------------------------------
	if (numSourceNotes == 0) {
		popup(STRING_FOR_FUGUE_NO_NOTES);
		result = ApplyResult::FAILED;
	}

	// The voices themselves
	for (int32_t v = 0; v < kNumVoices && result == ApplyResult::DONE; v++) {
		if (!voices[v].on) {
			continue;
		}

		int32_t newLength;
		FugueError error;
		transformNotes(voices[v], &newLength, &error);
		if (error == FugueError::PITCH_OUT_OF_RANGE) {
			popup(STRING_FOR_FUGUE_OUT_OF_RANGE);
			result = ApplyResult::FAILED;
			break;
		}
		if (error == FugueError::TOO_LONG) {
			popup(STRING_FOR_FUGUE_TOO_LONG);
			result = ApplyResult::FAILED;
			break;
		}

		Job& job = jobs[numJobs++];
		job.column = toTracks ? (v + 1) : 0;
		job.row = toTracks ? 0 : (v + 1);
		job.voice = v;
		job.settings = voices[v];
		job.existing = nullptr;
		if (toTracks) {
			columnOutput[v + 1] = getRememberedVoiceOutput(source, v);
		}
	}

	// BOTH: three variants below the source and below each voice, each based on the clip at the top of its column
	if (withVariants && result == ApplyResult::DONE) {
		VoiceSettings sourceSettings; // no offsets at all: the source as it is
		if (source->sequenceDirectionMode == SequenceDirection::REVERSE
		    || source->sequenceDirectionMode == SequenceDirection::PINGPONG) {
			sourceSettings.direction = source->sequenceDirectionMode;
		}

		for (int32_t column = 0; column <= kNumVoices; column++) {
			if (column > 0 && !voices[column - 1].on) {
				continue;
			}
			const VoiceSettings& base = (column == 0) ? sourceSettings : voices[column - 1];

			for (int32_t row = 1; row <= 3; row++) {
				Job& job = jobs[numJobs++];
				job.column = column;
				job.row = row;
				job.voice = column - 1;
				job.existing = nullptr;

				// Roll again if a variant would leave the note range. The top clip itself is known to fit.
				job.settings = base;
				for (int32_t attempt = 0; attempt < 12; attempt++) {
					VoiceSettings variant = makeVariant(base, scale.active, source);
					int32_t newLength;
					FugueError error;
					transformNotes(variant, &newLength, &error);
					if (error == FugueError::NONE) {
						job.settings = variant;
						break;
					}
				}
			}
		}
	}

	// Where each clip goes, and whether something is there already
	bool needsConfirmation = false;
	for (int32_t j = 0; j < numJobs && result == ApplyResult::DONE; j++) {
		Job& job = jobs[j];
		int32_t section = source->section + job.row;
		if (section >= kMaxNumSections) {
			popup(STRING_FOR_FUGUE_NO_ROOM);
			result = ApplyResult::FAILED;
			break;
		}
		if (columnOutput[job.column] != nullptr) {
			job.existing = findSessionClip(columnOutput[job.column], section, source);
		}
		if (job.existing != nullptr && job.existing->containsAnyNotes()) {
			needsConfirmation = true;
		}
	}

	if (result == ApplyResult::DONE && needsConfirmation && !overwriteConfirmed) {
		result = ApplyResult::NEEDS_CONFIRMATION;
	}

	// --- Write the clips --------------------------------------------------------------------------------------------
	if (result == ApplyResult::DONE) {
		if (toTracks && (rememberedSong != currentSong || rememberedSource != source->output)) {
			rememberedSong = currentSong;
			rememberedSource = source->output;
			for (Output*& output : rememberedVoice) {
				output = nullptr;
			}
		}

		// Where new clips are put in the song's clip list (this is what row view shows): a column's clips go
		// below one another, and each new voice track goes below the one before.
		InstrumentClip* columnLast[kNumVoices + 1]{};
		columnLast[0] = source;
		InstrumentClip* lastTop = source;

		for (int32_t j = 0; j < numJobs; j++) {
			Job& job = jobs[j];
			int32_t column = job.column;
			InstrumentClip* target = job.existing;
			bool targetIsNew = false;

			if (target == nullptr) {
				InstrumentClip* insertBelow = (columnLast[column] != nullptr) ? columnLast[column] : lastTop;
				target = createVoiceClip(source, source->section + job.row, insertBelow);
				if (target == nullptr) {
					result = ApplyResult::FAILED;
					break;
				}
				targetIsNew = true;

				if (column > 0) {
					// The column's instrument exists already: move the new clip onto it
					if (columnOutput[column] != nullptr) {
						char modelStackMemory[MODEL_STACK_MAX_SIZE];
						ModelStackWithTimelineCounter* modelStack =
						    setupModelStackWithSong(modelStackMemory, currentSong)->addTimelineCounter(target);
						Error error =
						    target->changeInstrument(modelStack, static_cast<Instrument*>(columnOutput[column]),
						                             nullptr, InstrumentRemoval::NONE, source);
						if (error != Error::NONE) {
							discardVoiceClip(target);
							display->displayError(error);
							result = ApplyResult::FAILED;
							break;
						}
					}
					// Or it is the first clip of a new voice track
					else {
						Output* newOutput = createVoiceInstrument(source, target);
						if (newOutput == nullptr) {
							discardVoiceClip(target);
							result = ApplyResult::FAILED;
							break;
						}
						columnOutput[column] = newOutput;
						rememberedVoice[column - 1] = newOutput;
					}
				}
			}

			columnLast[column] = target;
			if (job.row == 0) {
				lastTop = target;
			}

			// workNotes was last filled by the checks above, for some other clip
			int32_t newLength;
			FugueError error;
			int32_t numNotes = transformNotes(job.settings, &newLength, &error);
			if (error != FugueError::NONE
			    || !writeVoice(source, target, targetIsNew, workNotes, numNotes, newLength, sourceNotes,
			                   job.settings)) {
				result = ApplyResult::FAILED;
				break;
			}

			// Arp and randomizer settings this clip has of its own (set in the voice's menu, rolled by RAND, or
			// changed by a variant). The rest stays as cloned from the source.
			for (int32_t e = 0; e < kNumExtras; e++) {
				int16_t value = job.settings.extras[e];
				if (value != kExtraFromSource) {
					writeClipExtra(target, static_cast<Extra>(e), value);
				}
			}
		}

		// Like cloning a clip in song view, this can't be undone, and older undo steps may no longer fit
		actionLogger.deleteAllLogs();

		if (result == ApplyResult::DONE) {
			popup(STRING_FOR_FUGUE_DONE);
		}
	}

	delugeDealloc(sourceNotes);
	delugeDealloc(workNotes);
	return result;
}

} // namespace deluge::fugue
