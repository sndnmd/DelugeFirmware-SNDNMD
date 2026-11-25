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
#include "gui/context_menu/fugue_overwrite.h"
#include "gui/l10n/l10n.h"
#include "gui/menu_item/integer.h"
#include "gui/menu_item/menu_item.h"
#include "gui/menu_item/selection.h"
#include "gui/menu_item/submenu.h"
#include "gui/menu_item/sync_level.h"
#include "gui/menu_item/toggle.h"
#include "gui/ui/ui.h"
#include "hid/display/display.h"
#include "hid/display/oled.h"
#include "model/clip/instrument_clip.h"
#include "model/fugue/fugue.h"
#include "model/settings/runtime_feature_settings.h"
#include "model/song/song.h"
#include "modulation/arpeggiator_rhythms.h"
#include <algorithm>

// Menu items for the Fugue community feature. All of them just read and write deluge::fugue::fugue; the work
// happens in model/fugue/fugue.cpp.

namespace deluge::gui::menu_item::fugue_menu {

using deluge::fugue::fugue;

/// The "Fugue" entry of the clip menu. Hidden unless the community feature is switched on.
class Root final : public Submenu {
public:
	using Submenu::Submenu;

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		return runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::Fugue);
	}
};

/// CLIP (three clips below on the same track), TRAC (three cloned instruments) or BOTH (a 4x4 block)
class Destination final : public Selection {
public:
	using Selection::Selection;

	static bool tracksAllowed() { return deluge::fugue::Fugue::canCloneTrack(getCurrentInstrumentClip()); }

	void readCurrentValue() override {
		int32_t value = tracksAllowed() ? util::to_underlying(fugue.destination) : 0;
		this->setValue(std::clamp<int32_t>(value, 0, 2));
	}
	void writeCurrentValue() override {
		fugue.destination = static_cast<deluge::fugue::Destination>(std::clamp<int32_t>(this->getValue(), 0, 2));
	}
	deluge::vector<std::string_view> getOptions(OptType optType) override {
		(void)optType;
		using enum l10n::String;
		// A CV clip can't be cloned to other tracks, so it only gets the one choice
		if (!tracksAllowed()) {
			return {l10n::getView(STRING_FOR_FUGUE_DEST_CLIPS)};
		}
		return {l10n::getView(STRING_FOR_FUGUE_DEST_CLIPS), l10n::getView(STRING_FOR_FUGUE_DEST_TRACKS),
		        l10n::getView(STRING_FOR_FUGUE_DEST_BOTH)};
	}
};

/// Voice on / off
class VoiceOn final : public Toggle {
public:
	VoiceOn(l10n::String newName, uint8_t newVoice) : Toggle(newName, newName), voice(newVoice) {}

	void readCurrentValue() override { this->setValue(fugue.voices[voice].on); }
	void writeCurrentValue() override { fugue.voices[voice].on = this->getValue(); }

private:
	uint8_t voice;
};

/// Melodic inversion on / off
class VoiceInvert final : public Toggle {
public:
	VoiceInvert(l10n::String newName, uint8_t newVoice) : Toggle(newName, newName), voice(newVoice) {}

	void readCurrentValue() override { this->setValue(fugue.voices[voice].invert); }
	void writeCurrentValue() override { fugue.voices[voice].invert = this->getValue(); }
	void getColumnLabel(StringBuf& label) override { label.append("Inv"); }

private:
	uint8_t voice;
};

/// Pitch (scale steps), octave or rotate (squares at the current zoom)
class VoiceNumber final : public Integer {
public:
	enum class Kind : uint8_t { PITCH, OCTAVE, ROTATE };

	VoiceNumber(l10n::String newName, uint8_t newVoice, Kind newKind)
	    : Integer(newName, newName), voice(newVoice), kind(newKind) {}

	void readCurrentValue() override {
		const deluge::fugue::VoiceSettings& settings = fugue.voices[voice];
		int32_t value = (kind == Kind::PITCH)    ? settings.pitch
		                : (kind == Kind::OCTAVE) ? settings.octave
		                                         : settings.rotate;
		this->setValue(std::clamp(value, getMinValue(), getMaxValue()));
	}
	void writeCurrentValue() override {
		deluge::fugue::VoiceSettings& settings = fugue.voices[voice];
		int32_t value = this->getValue();
		switch (kind) {
		case Kind::PITCH:
			settings.pitch = value;
			break;
		case Kind::OCTAVE:
			settings.octave = value;
			break;
		case Kind::ROTATE:
			settings.rotate = value;
			break;
		}
	}
	void getColumnLabel(StringBuf& label) override {
		label.append((kind == Kind::PITCH) ? "Pitch" : (kind == Kind::OCTAVE) ? "Oct" : "Rot");
	}

protected:
	[[nodiscard]] int32_t getMaxValue() const override {
		switch (kind) {
		case Kind::PITCH:
			return deluge::fugue::kMaxPitch;
		case Kind::OCTAVE:
			return deluge::fugue::kMaxOctave;
		default: {
			// One less than the number of squares the clip has at the current zoom
			InstrumentClip* clip = getCurrentInstrumentClip();
			int32_t zoom = currentSong->xZoom[NAVIGATION_CLIP];
			if (clip == nullptr || zoom < 1) {
				return 1;
			}
			return std::clamp<int32_t>(clip->loopLength / zoom - 1, 1, 999);
		}
		}
	}
	[[nodiscard]] int32_t getMinValue() const override { return -getMaxValue(); }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return NUMBER; }

private:
	uint8_t voice;
	Kind kind;
};

/// Sets the voice clip's play direction
class VoiceDirection final : public Selection {
public:
	VoiceDirection(l10n::String newName, uint8_t newVoice) : Selection(newName, newName), voice(newVoice) {}

	void readCurrentValue() override {
		int32_t value = util::to_underlying(fugue.voices[voice].direction);
		this->setValue(std::clamp<int32_t>(value, 0, 2));
	}
	void writeCurrentValue() override {
		fugue.voices[voice].direction = static_cast<SequenceDirection>(this->getValue());
	}
	deluge::vector<std::string_view> getOptions(OptType optType) override {
		if (optType == OptType::SHORT || display->have7SEG()) {
			return {"FWD", "REV", "PING"};
		}
		using enum l10n::String;
		return {l10n::getView(STRING_FOR_FORWARD), l10n::getView(STRING_FOR_REVERSED),
		        l10n::getView(STRING_FOR_PING_PONG)};
	}
	void getColumnLabel(StringBuf& label) override { label.append("Dir"); }

private:
	uint8_t voice;
};

/// Speed of the voice against the source. The voice clip gets longer or shorter to match.
class VoiceTempo final : public Selection {
public:
	VoiceTempo(l10n::String newName, uint8_t newVoice) : Selection(newName, newName), voice(newVoice) {}

	void readCurrentValue() override {
		this->setValue(std::clamp<int32_t>(fugue.voices[voice].tempo, 0, deluge::fugue::kNumTempos - 1));
	}
	void writeCurrentValue() override { fugue.voices[voice].tempo = this->getValue(); }
	deluge::vector<std::string_view> getOptions(OptType optType) override {
		(void)optType;
		// The 7-segment display has no slash
		if (display->have7SEG()) {
			return {"1-8", "1-7", "1-6", "1-5", "1-4", "1-3", "1-2", "1", "2", "3", "4", "5", "6", "7", "8"};
		}
		return {"1/8", "1/7", "1/6", "1/5", "1/4", "1/3", "1/2", "1", "2", "3", "4", "5", "6", "7", "8"};
	}
	bool wrapAround() override { return false; }

private:
	uint8_t voice;
};

/// Rounding grid for a voice that runs faster than the source. Off = exact to the tick.
class VoiceReso final : public SyncLevel {
public:
	VoiceReso(l10n::String newName, uint8_t newVoice) : SyncLevel(newName, newName), voice(newVoice) {}

	void readCurrentValue() override {
		uint8_t reso = fugue.voices[voice].reso;
		if (reso == deluge::fugue::kResoDefault) {
			reso = deluge::fugue::defaultReso();
		}
		this->setValue(std::min<int32_t>(reso, NUM_SYNC_VALUES - 1));
	}
	void writeCurrentValue() override { fugue.voices[voice].reso = this->getValue(); }
	void getColumnLabel(StringBuf& label) override {
		// SyncLevel puts the note length in the label; keep that, it is the value
		SyncLevel::getColumnLabel(label);
	}

private:
	uint8_t voice;
};

// --- Arp and randomizer settings of a voice --------------------------------------------------------------------------
// Until one is changed here it shows, and the voice gets, what the clip you are in has.

/// Arp mode of the voice's clips: the stock list
class VoiceArpMode final : public Selection {
public:
	VoiceArpMode(l10n::String newName, uint8_t newVoice) : Selection(newName, newName), voice(newVoice) {}

	void readCurrentValue() override {
		this->setValue(std::clamp<int32_t>(fugue.getExtra(voice, deluge::fugue::Extra::ARP_MODE), 0, 6));
	}
	void writeCurrentValue() override { fugue.setExtra(voice, deluge::fugue::Extra::ARP_MODE, this->getValue()); }
	deluge::vector<std::string_view> getOptions(OptType optType) override {
		(void)optType;
		using enum l10n::String;
		return {l10n::getView(STRING_FOR_OFF),   l10n::getView(STRING_FOR_UP),     l10n::getView(STRING_FOR_DOWN),
		        l10n::getView(STRING_FOR_BOTH),  l10n::getView(STRING_FOR_RANDOM), l10n::getView(STRING_FOR_WALK),
		        l10n::getView(STRING_FOR_CUSTOM)};
	}
	void getColumnLabel(StringBuf& label) override { label.append("Arp"); }

private:
	uint8_t voice;
};

/// Arp rate of the voice's clips, as a note length
class VoiceArpSync final : public SyncLevel {
public:
	VoiceArpSync(l10n::String newName, uint8_t newVoice) : SyncLevel(newName, newName), voice(newVoice) {}

	void readCurrentValue() override {
		int32_t value = fugue.getExtra(voice, deluge::fugue::Extra::ARP_SYNC);
		this->setValue(std::clamp<int32_t>(value, 0, NUM_SYNC_VALUES - 1));
	}
	void writeCurrentValue() override { fugue.setExtra(voice, deluge::fugue::Extra::ARP_SYNC, this->getValue()); }

private:
	uint8_t voice;
};

/// Arp gate, arp rhythm, note probability, velocity spread or bass probability: 0 to 50 like the stock items
class VoiceExtraNumber final : public Integer {
public:
	VoiceExtraNumber(l10n::String newName, uint8_t newVoice, deluge::fugue::Extra newExtra, char const* newLabel)
	    : Integer(newName, newName), voice(newVoice), extra(newExtra), columnLabel(newLabel) {}

	void readCurrentValue() override {
		this->setValue(std::clamp<int32_t>(fugue.getExtra(voice, extra), 0, kMaxMenuValue));
	}
	void writeCurrentValue() override { fugue.setExtra(voice, extra, this->getValue()); }
	void getColumnLabel(StringBuf& label) override { label.append(columnLabel); }

	// The rhythm shows its pattern, as the stock arp rhythm item does
	void drawValue() override {
		if (isRhythm()) {
			display->setScrollingText(arpRhythmPatternNames[rhythmIndex()]);
		}
		else {
			Integer::drawValue();
		}
	}
	void drawInteger(int32_t textWidth, int32_t textHeight, int32_t yPixel) override {
		if (isRhythm()) {
			char text[12];
			snprintf(text, sizeof(text), "%d: %s", (int)rhythmIndex(), arpRhythmPatternNames[rhythmIndex()]);
			hid::display::OLED::main.drawStringCentred(text, yPixel + OLED_MAIN_TOPMOST_PIXEL, textWidth, textHeight);
		}
		else {
			Integer::drawInteger(textWidth, textHeight, yPixel);
		}
	}
	void renderInHorizontalMenu(const SlotPosition& slot) override {
		if (isRhythm()) {
			hid::display::OLED::main.drawStringCentered(arpRhythmPatternNames[rhythmIndex()], slot.start_x,
			                                            slot.start_y + kHorizontalMenuSlotYOffset, kTextSpacingX,
			                                            kTextSpacingY, slot.width);
		}
		else {
			Integer::renderInHorizontalMenu(slot);
		}
	}

protected:
	[[nodiscard]] int32_t getMaxValue() const override { return kMaxMenuValue; }
	[[nodiscard]] int32_t getMinValue() const override { return 0; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return NUMBER; }

private:
	[[nodiscard]] bool isRhythm() const { return extra == deluge::fugue::Extra::ARP_RHYTHM; }
	[[nodiscard]] int32_t rhythmIndex() { return std::clamp<int32_t>(this->getValue(), 0, kMaxPresetArpRhythm); }

	uint8_t voice;
	deluge::fugue::Extra extra;
	char const* columnLabel;
};

/// Randomizer lock on / off
class VoiceLock final : public Toggle {
public:
	VoiceLock(l10n::String newName, uint8_t newVoice) : Toggle(newName, newName), voice(newVoice) {}

	void readCurrentValue() override { this->setValue(fugue.getExtra(voice, deluge::fugue::Extra::LOCK) != 0); }
	void writeCurrentValue() override { fugue.setExtra(voice, deluge::fugue::Extra::LOCK, this->getValue() ? 1 : 0); }
	void getColumnLabel(StringBuf& label) override { label.append("Lock"); }

private:
	uint8_t voice;
};

/// Rolls new offsets for all voices that are on
class Randomize final : public MenuItem {
public:
	using MenuItem::MenuItem;

	MenuItem* selectButtonPress() override {
		fugue.randomize();
		display->displayPopup(l10n::get(l10n::String::STRING_FOR_FUGUE_RANDOMIZED));
		return NO_NAVIGATION;
	}
	bool shouldEnterSubmenu() override { return false; }
};

/// Writes the voices, asking first if that would overwrite notes
class Apply final : public MenuItem {
public:
	using MenuItem::MenuItem;

	MenuItem* selectButtonPress() override {
		if (fugue.apply(false) == deluge::fugue::ApplyResult::NEEDS_CONFIRMATION) {
			if (context_menu::fugueOverwrite.setupAndCheckAvailability()) {
				openUI(&context_menu::fugueOverwrite);
			}
		}
		return NO_NAVIGATION;
	}
	bool shouldEnterSubmenu() override { return false; }
};

} // namespace deluge::gui::menu_item::fugue_menu
