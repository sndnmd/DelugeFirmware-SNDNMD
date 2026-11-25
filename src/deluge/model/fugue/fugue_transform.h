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

// The note maths of the Fugue feature. Deliberately free of any other firmware header, so it can be compiled and
// tested on a desktop machine.

#include <algorithm>
#include <cstdint>

namespace deluge::fugue {

/// One note of a voice. `tag` is the index of the source note it was made from, so the caller can look up velocity,
/// probability etc. once the maths is done.
struct FugueNote {
	int32_t y;
	int32_t pos;
	int32_t length;
	int32_t tag;
};

/// The scale the pitch operations work in. With `active` false everything is in semitones.
struct FugueScale {
	bool active{false};
	int32_t root{0};
	int32_t size{12};
	int8_t notes[12]{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}; // semitones above root, ascending, notes[0] == 0
};

struct FugueTransform {
	bool invert{false};
	int32_t pitchSteps{0}; // scale steps, or semitones when the scale is not active
	int32_t octaves{0};
	int32_t rotateTicks{0}; // applied before tempo, wraps inside the clip length
	int32_t timeMul{1};     // tempo 1/2 -> timeMul 2
	int32_t timeDiv{1};     // tempo 2 -> timeDiv 2
	int32_t gridTicks{0};   // rounding grid used when timeDiv > 1; 0 = round to the tick
};

enum class FugueError : uint8_t { NONE, PITCH_OUT_OF_RANGE, TOO_LONG };

constexpr int32_t kFugueMaxNote = 127;
constexpr int32_t kFugueMaxLength = 1610612736; // == kMaxSequenceLength

inline int32_t fugueFloorDiv(int32_t a, int32_t b) {
	int32_t q = a / b;
	if ((a % b != 0) && ((a < 0) != (b < 0))) {
		q--;
	}
	return q;
}

inline int32_t fugueMod(int32_t a, int32_t b) {
	return a - fugueFloorDiv(a, b) * b;
}

/// Note code -> position on the scale ladder (scale steps counted from the root at octave 0).
inline int32_t fugueNoteToStep(const FugueScale& scale, int32_t y) {
	if (!scale.active) {
		return y;
	}
	int32_t rel = y - scale.root;
	int32_t octave = fugueFloorDiv(rel, 12);
	int32_t interval = rel - octave * 12;
	// Highest degree at or below the interval. In a scale-mode clip every note is on a degree.
	int32_t degree = 0;
	for (int32_t d = 0; d < scale.size; d++) {
		if (scale.notes[d] <= interval) {
			degree = d;
		}
	}
	return octave * scale.size + degree;
}

inline int32_t fugueStepToNote(const FugueScale& scale, int32_t step) {
	if (!scale.active) {
		return step;
	}
	int32_t octave = fugueFloorDiv(step, scale.size);
	int32_t degree = step - octave * scale.size;
	return scale.root + octave * 12 + scale.notes[degree];
}

/// Rounds a / b to the nearest integer, halves up. b > 0, a >= 0.
inline int64_t fugueRoundDiv(int64_t a, int64_t b) {
	return (2 * a + b) / (2 * b);
}

/// Transforms `notes` (numNotes entries, any order) in place and returns how many are left. The result is sorted by
/// pitch, then position, and no two notes of the same pitch overlap.
inline int32_t fugueApplyTransform(FugueNote* notes, int32_t numNotes, int32_t clipLength, const FugueScale& scale,
                                   const FugueTransform& t, int32_t* newClipLength, FugueError* error) {
	*error = FugueError::NONE;
	*newClipLength = clipLength;
	if (clipLength <= 0 || t.timeMul < 1 || t.timeDiv < 1) {
		return 0;
	}

	// --- Pitch: invert, pitch, octave -----------------------------------------------------------------------------
	// The mirror axis is the first note of the clip; the lowest one if several start together.
	int32_t axisStep = 0;
	if (t.invert && numNotes > 0) {
		int32_t first = 0;
		for (int32_t i = 1; i < numNotes; i++) {
			if (notes[i].pos < notes[first].pos || (notes[i].pos == notes[first].pos && notes[i].y < notes[first].y)) {
				first = i;
			}
		}
		axisStep = fugueNoteToStep(scale, notes[first].y);
	}

	int32_t stepsPerOctave = scale.active ? scale.size : 12;
	for (int32_t i = 0; i < numNotes; i++) {
		int32_t step = fugueNoteToStep(scale, notes[i].y);
		if (t.invert) {
			step = 2 * axisStep - step;
		}
		step += t.pitchSteps + t.octaves * stepsPerOctave;
		int32_t y = fugueStepToNote(scale, step);
		if (y < 0 || y > kFugueMaxNote) {
			*error = FugueError::PITCH_OUT_OF_RANGE;
			return 0;
		}
		notes[i].y = y;
	}

	// --- Rotate ---------------------------------------------------------------------------------------------------
	if (t.rotateTicks != 0) {
		for (int32_t i = 0; i < numNotes; i++) {
			int64_t pos = ((int64_t)notes[i].pos + t.rotateTicks) % clipLength;
			notes[i].pos = (int32_t)((pos < 0) ? (pos + clipLength) : pos);
		}
	}

	// --- Tempo ----------------------------------------------------------------------------------------------------
	int64_t length = clipLength;
	if (t.timeMul > 1) {
		length *= t.timeMul;
		if (length > kFugueMaxLength) {
			*error = FugueError::TOO_LONG;
			return 0;
		}
		for (int32_t i = 0; i < numNotes; i++) {
			notes[i].pos *= t.timeMul;
			int64_t noteLength = (int64_t)notes[i].length * t.timeMul;
			notes[i].length = (int32_t)std::min<int64_t>(noteLength, length);
		}
	}
	if (t.timeDiv > 1) {
		int64_t grid = (t.gridTicks > 0) ? t.gridTicks : 1;
		// Clip length and note starts go to the nearest grid step
		length = fugueRoundDiv(length, (int64_t)t.timeDiv * grid) * grid;
		if (length < grid) {
			length = grid;
		}
		for (int32_t i = 0; i < numNotes; i++) {
			int64_t pos = fugueRoundDiv(notes[i].pos, (int64_t)t.timeDiv * grid) * grid;
			notes[i].pos = (int32_t)(pos % length);
			int64_t noteLength = fugueRoundDiv(notes[i].length, t.timeDiv);
			notes[i].length = (int32_t)std::max<int64_t>(noteLength, 1);
		}
	}
	*newClipLength = (int32_t)length;

	// --- Tidy up: sort, drop collisions (first note wins), stop notes running into the next one ---------------------
	// stable_sort keeps the source order for notes landing on the same pitch and position
	std::stable_sort(notes, notes + numNotes, [](const FugueNote& a, const FugueNote& b) {
		if (a.y != b.y) {
			return a.y < b.y;
		}
		return a.pos < b.pos;
	});

	int32_t numOut = 0;
	for (int32_t i = 0; i < numNotes; i++) {
		if (numOut > 0 && notes[numOut - 1].y == notes[i].y && notes[numOut - 1].pos == notes[i].pos) {
			continue;
		}
		notes[numOut++] = notes[i];
	}

	int32_t rowStart = 0;
	while (rowStart < numOut) {
		int32_t rowEnd = rowStart;
		while (rowEnd < numOut && notes[rowEnd].y == notes[rowStart].y) {
			rowEnd++;
		}
		for (int32_t i = rowStart; i < rowEnd; i++) {
			int32_t nextPos =
			    (i + 1 < rowEnd) ? notes[i + 1].pos : (notes[rowStart].pos + (int32_t)length); // last one wraps
			int32_t maxLength = nextPos - notes[i].pos;
			notes[i].length = std::clamp(notes[i].length, (int32_t)1, std::max(maxLength, (int32_t)1));
		}
		rowStart = rowEnd;
	}

	return numOut;
}

} // namespace deluge::fugue
