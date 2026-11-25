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

#include "gui/context_menu/fugue_overwrite.h"
#include "gui/l10n/l10n.h"
#include "hid/display/display.h"
#include "model/fugue/fugue.h"

namespace deluge::gui::context_menu {

FugueOverwrite fugueOverwrite{};

char const* FugueOverwrite::getTitle() {
	using enum l10n::String;
	return l10n::get(STRING_FOR_OVERWRITE_QMARK);
}

std::span<char const*> FugueOverwrite::getOptions() {
	using enum l10n::String;
	if (display->haveOLED()) {
		static char const* options[] = {l10n::get(STRING_FOR_OK)};
		return {options, 1};
	}
	else {
		// Blinks on the 7-segment display, as "OVER"
		static char const* options[] = {l10n::get(STRING_FOR_OVERWRITE)};
		return {options, 1};
	}
}

bool FugueOverwrite::acceptCurrentOption() {
	deluge::fugue::fugue.apply(true);
	return false; // Close this menu either way; apply() has shown what happened
}

} // namespace deluge::gui::context_menu
