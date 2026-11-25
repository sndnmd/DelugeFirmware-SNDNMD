/*
 * Copyright © 2018-2023 Synthstrom Audible Limited
 *
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
#include "gui/colour/colour.h"
#include "gui/views/timeline_view.h"

class ClipInstance;
class InstrumentClip;
class Instrument;
class ParamManagerForTimeline;
class Instrument;
class ArpeggiatorSettings;
class NoteRow;
class Kit;
class Action;
class Drum;
class ModelStack;
class ModelStackWithNoteRow;

class ArrangerView final : public TimelineView {
public:
	ArrangerView();
	bool opened() override;
	void focusRegained() override;
	ActionResult padAction(int32_t x, int32_t y, int32_t velocity) override;
	ActionResult handleEditPadAction(int32_t x, int32_t y, int32_t velocity);
	ActionResult handleStatusPadAction(int32_t y, int32_t velocity, UI* ui);
	ActionResult handleAuditionPadAction(int32_t y, int32_t velocity, UI* ui);
	ActionResult buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) override;
	ActionResult verticalEncoderAction(int32_t offset, bool inCardRoutine) override;
	void selectEncoderAction(int8_t offset) override;
	void modEncoderAction(int32_t whichModEncoder, int32_t offset) override;

	void repopulateOutputsOnScreen(bool doRender = true);
	bool renderSidebar(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth],
	                   uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth]) override;
	void drawMuteSquare(int32_t yDisplay, RGB thisImage[]);
	bool renderMainPads(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth],
	                    uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth], bool drawUndefinedArea = true) override;
	bool renderRow(ModelStack* modelStack, int32_t yDisplay, int32_t xScroll, uint32_t xZoom, RGB* thisImage,
	               uint8_t thisOccupancyMask[], int32_t renderWidth);
	void editPadAction(int32_t x, int32_t y, bool on);
	ActionResult horizontalEncoderAction(int32_t offset) override;
	void shiftAutomationHorizontally(int32_t offset, int32_t scroll_amount, Action* action);
	void shiftClipsHorizontally(int32_t offset, int32_t scroll_amount, Action* action);
	uint32_t getMaxLength() override;
	uint32_t getMaxZoom() override;
	void graphicsRoutine() override;
	[[nodiscard]] int32_t getNavSysId() const override { return NAVIGATION_ARRANGEMENT; }
	void navigateThroughPresets(int32_t offset);
	void notifyActiveClipChangedOnOutput(Output* output);
	ActionResult timerCallback() override;
	void reassessWhetherDoingAutoScroll(int32_t pos = -1);
	void autoScrollOnPlaybackEnd();
	bool initiateXScroll(int32_t newScrollPos);
	[[nodiscard]] bool supportsTriplets() const override { return false; }
	bool putDraggedClipInstanceInNewPosition(Output* output);
	void tellMatrixDriverWhichRowsContainSomethingZoomable() override;
	void scrollFinished() override;
	void notifyPlaybackBegun() override;
	uint32_t getGreyedOutRowsNotRepresentingOutput(Output* output) override;
	void playbackEnded() override;
	void clipNeedsReRendering(Clip* clip) override;
	void exitSubModeWithoutAction(UI* ui = nullptr);
	bool transitionToArrangementEditor();
	bool getGreyoutColsAndRows(uint32_t* cols, uint32_t* rows) override;
	void setLedStates();
	ActionResult verticalScrollOneSquare(int32_t direction);
	ActionResult horizontalScrollOneSquare(int32_t direction);

	void renderOLED(deluge::hid::display::oled_canvas::Canvas& canvas) override;

	Output* outputsOnScreen[kDisplayHeight]{};
	int8_t yPressedEffective{};
	int8_t yPressedActual{};
	int8_t xPressed{};
	bool actionOnDepress{};
	uint32_t pressTime{};
	int32_t desiredLength{};
	int32_t originallyPressedClipActualLength{};
	bool pressedHead{};

	int32_t pressedClipInstanceIndex{};
	Output* pressedClipInstanceOutput{};
	int32_t pressedClipInstanceXScrollWhenLastInValidPosition{};
	bool pressedClipInstanceIsInValidPosition{};

	bool blinkOn{};

	bool doingAutoScrollNow;
	bool mustRedrawTickSquares{};

	int32_t autoScrollNumSquaresBehind{};

	int32_t lastInteractedOutputIndex;
	int32_t lastInteractedPos;
	uint8_t lastInteractedSection;
	ClipInstance* lastInteractedClipInstance;

	int32_t lastInteractedArrangementPos;

	int32_t lastTickSquare{};

	int32_t xScrollWhenPlaybackStarted{};

	// Arranger loop lanes (community features "Arranger Loop" and "Arranger Loop 2+FX"). Lane 0 is the first loop,
	// lane 1 the second.
	enum class LoopMarker : uint8_t { NONE, START, END };
	LoopMarker loopMarkerSelected{LoopMarker::NONE}; // The marker that's blinking, waiting to be placed
	int8_t loopMarkerSelectedLane{0};                // And which lane it's on
	bool loopMarkerBlinkOn{};
	int8_t loopLaneHeldX{-1}; // Pad inside a loop region that's being held down. -1 if none
	int8_t loopLaneHeldY{-1};
	int8_t loopLaneHeldLane{0};
	bool loopLaneShiftTap{};                            // Held pad was pressed with SHIFT down
	bool loopLaneHoldUsed{};                            // Held pad got used for PLAY or <>
	bool loopLaneRecouplePending{};                     // <> pressed while holding the second loop
	LoopMarker loopLanePressedMarker{LoopMarker::NONE}; // Marker on the held pad, if any
	int32_t getLoopLaneYDisplay(int32_t lane = 0);
	int32_t getLoopLaneFromYDisplay(int32_t yDisplay);
	bool isHoldingLoopLane();
	void updateLoop2Fx();

	// Fading the second loop's FX settings in or out
	enum class Loop2Fade : uint8_t { NONE, IN, OUT };
	Loop2Fade loop2Fade{Loop2Fade::NONE};
	bool loop2FadeRepeat{};   // Latched fade which starts over each time the loop comes round
	bool loop2FadeFromHold{}; // The pad which the fade was started from is still being held
	uint32_t loop2FadeStartSample{};
	uint32_t loop2FadeLengthSamples{};
	int32_t loop2FadeLastLivePos{};
	void displayLoopLaneName(int32_t lane);

	// ui
	UIType getUIType() override { return UIType::ARRANGER; }
	UIModControllableContext getUIModControllableContext() override { return UIModControllableContext::SONG; }

	Clip* getClipForSelection();

	void requestRendering(UI* ui, uint32_t whichMainRows = 0xFFFFFFFF, uint32_t whichSideRows = 0xFFFFFFFF);

private:
	void changeOutputType(OutputType newOutputType);
	void moveClipToSession();
	void auditionPadAction(bool on, int32_t y, UI* ui);
	void beginAudition(Output* output);
	void endAudition(Output* output, bool evenIfPlaying = false);
	ModelStackWithNoteRow* getNoteRowForAudition(ModelStack* modelStack, Kit* kit);
	Drum* getDrumForAudition(Kit* kit);
	void drawAuditionSquare(int32_t yDisplay, RGB thisImage[]);
	void setNoSubMode();
	void outputActivated(Output* output);
	void outputDeactivated(Output* output);
	void transitionToClipView(ClipInstance* clipInstance);
	void deleteClipInstance(Output* output, int32_t clipInstanceIndex, ClipInstance* clipInstance, Action* action,
	                        bool clearingWholeArrangement = false);
	void clearArrangement();
	void interactWithClipInstance(Output* output, int32_t yDisplay, ClipInstance* clipInstance);
	void rememberInteractionWithClipInstance(int32_t yDisplay, ClipInstance* clipInstance);
	void deleteOutput();
	void auditionEnded();
	void goToSongView();
	void changeOutputToAudio();
	bool renderRowForOutput(ModelStack* modelStack, Output* output, int32_t xScroll, uint32_t xZoom, RGB* image,
	                        uint8_t occupancyMask[], int32_t renderWidth, int32_t ignoreI);
	Instrument* createNewInstrument(OutputType newOutputType, bool* instrumentAlreadyInSong);
	uint32_t doActualRender(int32_t xScroll, uint32_t xZoom, uint32_t whichRows, RGB* image,
	                        uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth], int32_t renderWidth,
	                        int32_t imageWidth);
	void renderDisplay();

	// edit pad actions
	void cloneClipInstanceToWhite(Output* output, int32_t x, int32_t y, int32_t xScroll);
	void createNewClipInstance(Output* output, int32_t x, int32_t y, int32_t squareStart, int32_t squareEnd,
	                           int32_t xScroll);
	ClipInstance* createClipInstance(Output* output, int32_t y, int32_t squareStart);
	Clip* getClipForNewClipInstance(Output* output, Output* lastOutputInteractedWith, ClipInstance* lastClipInstance);
	Clip* getClipFromSection(Output* output);
	void adjustClipInstanceLength(Output* output, int32_t x, int32_t y, int32_t squareStart, int32_t squareEnd);
	void deleteClipInstance(Output* output, ClipInstance* clipInstance);
	void createNewClipForClipInstance(Output* output, ClipInstance* clipInstance);
	void recordEditPadPress(Output* output, ClipInstance* clipInstance, int32_t x, int32_t y, int32_t xScroll);

	// arranger loop lanes
	void renderLoopLane(int32_t lane, int32_t xScroll, uint32_t xZoom, RGB* image, int32_t renderWidth);
	void loopLanePadAction(int32_t lane, int32_t x, int32_t y);
	void loopLanePadReleased();
	void shiftLoop(int32_t lane, int32_t shift);
	void loopLaneStatusPadAction(int32_t lane, int32_t y, UI* ui);
	bool isLoopLanePadDown();
	bool isAnyPadDownOnLoopLane(int32_t lane);
	void toggleLoop2FxLatch();
	void startLoop2Fade(bool fadeOut, int32_t numBars, int32_t lengthTicks, bool latchAndRepeat);
	void loopPointsChanged();
};

extern ArrangerView arrangerView;
