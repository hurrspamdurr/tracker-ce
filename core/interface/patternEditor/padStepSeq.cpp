#include "patternEditor/padStepSeq.h"

#ifndef TRACKER_MINI

#include "core/interfaceDefs.h"
#include "core/interfacePopups.h"
#include "fileManager.h"
#include "mtAudioEngine.h"
#include "mtPadsBacklight.h"
#include "mtSequencer.h"
#include "mtStructs.h"
#include "patternEditor/patternEditor.h"

#include <stdio.h>

// The 12x4 pad grid edits the pattern that is already on screen.
// sequencer.getPatternToUI() is that pattern. Nothing in this file keeps
// a second copy of the sequence (the clipboard below is one step, and
// paste writes it back into the same pattern).
//
// How the two views meet
//   Track   editor cursor track, 0-15. The popup says Track 1..16.
//           Tracks 1-8 are the sample tracks. Tracks 9-16 are the MIDI tracks.
//   Step    page * 32 + row * 8 + column. Step 0 is the first pattern step.
//           Row 0 is pad indexes 0-11, the same order performance mode uses.
//           The popup numbers the step the way the pattern editor does
//           (from 0, from 1, or hex), from Config numbering mode.
//   Page    32 steps. The pad page is the page that contains
//           middleVisibleStep, the step the pattern editor is scrolled to.
//           This follows the editor while no pad is held, stopped or
//           playing. Choosing a page on the pads moves the editor cursor
//           to the same offset on that page, which is what scrolls the
//           pattern view. Steps past the pattern length stay dark.
//   Cursor  actualStep. That pad is bright when it is on the current page.
//           Tapping or holding a step moves the editor cursor there.
//           Several held steps leave the cursor on the last one pressed.
//           While transport is running the playhead pad is bright as well.
//           Notes are the dim pads. One brightness level is shared, so a
//           selected step and the playhead look the same.
//   Note    step.note and step.instrument. A short tap toggles a pitched
//           note. Hold the step and turn for free semitones, 0..127, with
//           no scale lock. The same delta is added to every held step.
//   Volume  step.volume, 0..100. Empty volume starts from the instrument.
//   FX      FX1 on screen is step.fx[1]. FX2 is step.fx[0]. CC and roll
//           write that lane. An empty CC lane becomes CC A. An empty roll
//           lane becomes a roll. The letter of a CC that is already there
//           stays put. The encoder changes the value.
//
// Columns, left to right. The UI index is already corrected by
// PadsImpl::convertToGridKey4x12, which folds the swapped right-edge scan
// pair into the same index the LEDs use. Do not swap columns 10 and 11
// again. If a device shows edit-mode and actions mirrored, change
// columnOf() only.
//   0-7  steps
//   8    page
//   9    track, or the FX lane
//   10   edit mode: note, velocity, CC, roll
//   11   action: mute, copy, paste, clear
//
// Hold a control and turn to scroll its options. The choice is on the
// title bar and in a popup until you let go. Page, track, and edit mode
// apply as the encoder moves. An action applies when you let go, so
// scrolling past Mute does not mute. A short tap still jumps to that
// row's page or edit mode, toggles the FX lane, or runs that action.
//
// Hold one or more step pads and turn to edit those steps in the current
// mode. The popup shows the step and the value. A short tap (released
// before the pad hold time, and without turning) toggles the note.
// These presses do not send MIDI.
//
// State is ordinary BSS. There is no static constructor and no write to
// external RAM from here.

namespace
{

constexpr int kGridColumns = 12;
constexpr int kGridRows = 4;
constexpr int kStepColumns = 8;
constexpr int kStepsPerPage = 32;
constexpr int kPageCount = 4;
constexpr int kColPage = 8;
constexpr int kColTrack = 9;
constexpr int kColMode = 10;
constexpr int kColAction = 11;
constexpr int kTrackChoices = kAllTracks + 2;
constexpr int kMaxHeldSteps = 32;

constexpr int kModeNote = 0;
constexpr int kModeVelocity = 1;
constexpr int kModeCc = 2;
constexpr int kModeRoll = 3;
constexpr int kModeCount = 4;

constexpr int kActionMute = 0;
constexpr int kActionCopy = 1;
constexpr int kActionPaste = 2;
constexpr int kActionClear = 3;

constexpr int kHoldNone = 0;
constexpr int kHoldPage = 1;
constexpr int kHoldTrack = 2;
constexpr int kHoldMode = 3;
constexpr int kHoldAction = 4;

using Step = Sequencer::strPattern::strTrack::strStep;
using Fx = Sequencer::strFxConsts;

struct HeldStep
{
    uint8_t pad;
    int16_t step;
    bool sawHold;
};

bool active = false;
uint8_t currentPage = 0;
uint8_t padEditMode = kModeNote;
uint8_t fxLane = 0; // 0 = FX1 (step.fx[1]), 1 = FX2 (step.fx[0])
uint8_t held = kHoldNone;
uint8_t heldPad = 0;
int holdIndex = 0;
bool holdTurned = false;
HeldStep heldSteps[kMaxHeldSteps];
int heldStepCount = 0;
bool stepGestureTurned = false;
int shownPlayhead = -2;
int shownCursor = -2;
bool pendingPaint = false;
bool hasClipboard = false;
Step clipboard;
char statusLine1[24];
char statusLine2[24];
char statusTitle[40];

int columnOf(uint8_t pad)
{
    return int(pad) % kGridColumns;
}

int rowOf(uint8_t pad)
{
    return int(pad) / kGridColumns;
}

const char* modeName(int mode)
{
    switch (mode)
    {
        case kModeVelocity: return "Velocity";
        case kModeCc: return "CC";
        case kModeRoll: return "Roll";
        default: return "Note";
    }
}

const char* actionName(int action)
{
    switch (action)
    {
        case kActionCopy: return "Copy";
        case kActionPaste: return "Paste";
        case kActionClear: return "Clear";
        default: return "Mute";
    }
}

int fxDataSlot()
{
    // The on-screen FX1 column is stored in fx[1]. FX2 is fx[0].
    return fxLane == 0 ? 1 : 0;
}

void syncEditColumn()
{
    if (padEditMode == kModeNote)
        patternEditor.editParam = cPatternEditor::EditNote;
    else if (padEditMode == kModeCc || padEditMode == kModeRoll)
        patternEditor.editParam = fxLane == 0 ? cPatternEditor::EditFx1 : cPatternEditor::EditFx2;
}

Sequencer::strPattern* pattern()
{
    return sequencer.getPatternToUI();
}

int lastStepIndex()
{
    Sequencer::strPattern* pat = pattern();
    if (pat == nullptr)
        return -1;
    return pat->track[0].length;
}

int currentTrack()
{
    int track = patternEditor.trackerPattern.actualTrack;
    if (track < 0)
        track = 0;
    if (track >= kAllTracks)
        track = kAllTracks - 1;
    return track;
}

int currentStepIndex()
{
    int step = patternEditor.trackerPattern.actualStep;
    int last = lastStepIndex();
    if (step < 0)
        step = 0;
    if (last >= 0 && step > last)
        step = last;
    if (step > Sequencer::MAXSTEP)
        step = Sequencer::MAXSTEP;
    return step;
}

Step* stepPtr(int track, int step)
{
    Sequencer::strPattern* pat = pattern();
    if (pat == nullptr || track < 0 || track >= kAllTracks || step < 0 || step > Sequencer::MAXSTEP)
        return nullptr;
    return &pat->track[track].step[step];
}

bool padGestureActive()
{
    return held != kHoldNone || heldStepCount > 0;
}

int editorScrollStep()
{
    int step = patternEditor.trackerPattern.middleVisibleStep;
    if (step < 0)
        step = patternEditor.trackerPattern.actualStep;
    if (step < 0)
        step = 0;
    if (step > Sequencer::MAXSTEP)
        step = Sequencer::MAXSTEP;
    return step;
}

void pullPageFromEditor()
{
    if (padGestureActive())
        return;

    int page = editorScrollStep() / kStepsPerPage;
    if (page < 0)
        page = 0;
    if (page >= kPageCount)
        page = kPageCount - 1;
    currentPage = uint8_t(page);
}

uint8_t instrumentForTrack(int track)
{
    int inst = mtProject.values.lastUsedInstrument;
    if (inst < 0 || inst >= kTotalInstrumentsCount)
        inst = 0;

    if (isSampleTrack(track))
    {
        if (inst >= kSampleInstrumentsCount)
            inst = 0;
    }
    else if (!isMidiInstrument(inst))
    {
        inst = MIDI_INSTRUMENTS_START_IDX;
    }

    return uint8_t(inst);
}

uint8_t noteForNewStep()
{
    uint8_t note = mtProject.values.lastUsedNote;
    if (note == 0 || note > Sequencer::STEP_NOTE_MAX)
        note = uint8_t(Sequencer::STEP_NOTE_DEFAULT);
    return note;
}

void commitPattern()
{
    newFileManager.setActualPatternStructChanged();
    patternEditor.refreshPattern();
    padStepSeqPaint();
}

void selectStep(int step)
{
    if (step < 0)
        step = 0;
    if (step > Sequencer::MAXSTEP)
        step = Sequencer::MAXSTEP;
    int last = lastStepIndex();
    if (last >= 0 && step > last)
        step = last;

    patternEditor.trackerPattern.actualStep = int16_t(step);
    patternEditor.trackerPattern.middleVisibleStep = int16_t(step);
    patternEditor.trackerPattern.selectState = 1;
    patternEditor.trackerPattern.selectColumn = 0;
    patternEditor.focusOnActual();
    syncEditColumn();
}

void scrollEditorToPage(int page)
{
    int cursor = patternEditor.trackerPattern.actualStep;
    int offset = 0;
    if (cursor >= 0)
        offset = cursor % kStepsPerPage;
    int target = page * kStepsPerPage + offset;
    selectStep(target);
    patternEditor.refreshPattern();
}

void showHeldStatus()
{
    statusLine2[0] = 0;

    switch (held)
    {
        case kHoldPage:
            snprintf(statusLine1, sizeof statusLine1, "Page %d", holdIndex + 1);
            break;
        case kHoldTrack:
            if (holdIndex < kAllTracks)
            {
                snprintf(statusLine1, sizeof statusLine1, "Track %d", holdIndex + 1);
                snprintf(statusLine2, sizeof statusLine2, "FX %d", fxLane + 1);
            }
            else
            {
                snprintf(statusLine1, sizeof statusLine1, "FX %d", holdIndex - kAllTracks + 1);
            }
            break;
        case kHoldMode:
            snprintf(statusLine1, sizeof statusLine1, "%s", modeName(holdIndex));
            break;
        case kHoldAction:
            snprintf(statusLine1, sizeof statusLine1, "%s", actionName(holdIndex));
            break;
        default: statusLine1[0] = 0; break;
    }

    if (statusLine1[0] == 0)
        return;

    patternEditor.showPadStepStatus(statusLine1);
    if (statusLine2[0] != 0)
        mtPopups.show(0, statusLine1, statusLine2, 30);
    else
        mtPopups.show(0, statusLine1, 30);
}

void showBriefStatus(const char* text)
{
    snprintf(statusLine1, sizeof statusLine1, "%s", text);
    patternEditor.showPadStepStatus(statusLine1);
    mtPopups.show(0, statusLine1, 2);
}

void applyPage(int index)
{
    if (index < 0)
        index = 0;
    if (index >= kPageCount)
        index = kPageCount - 1;
    currentPage = uint8_t(index);
    scrollEditorToPage(currentPage);
}

void applyMode(int index)
{
    if (index < 0)
        index = 0;
    if (index >= kModeCount)
        index = kModeCount - 1;
    padEditMode = uint8_t(index);
    syncEditColumn();
    patternEditor.refreshPattern();
}

void applyTrackChoice(int index)
{
    if (index < 0)
        index = 0;
    if (index >= kTrackChoices)
        index = kTrackChoices - 1;

    if (index < kAllTracks)
    {
        patternEditor.trackerPattern.actualTrack = int8_t(index);
        patternEditor.focusOnActual();
    }
    else
    {
        fxLane = uint8_t(index - kAllTracks);
        syncEditColumn();
    }
    patternEditor.refreshPattern();
}

void toggleFxLane()
{
    fxLane = fxLane == 0 ? 1 : 0;
    syncEditColumn();
    char text[8];
    snprintf(text, sizeof text, "FX %d", fxLane + 1);
    showBriefStatus(text);
    patternEditor.refreshPattern();
    padStepSeqPaint();
}

void doMute()
{
    int track = currentTrack();
    const char* text = "Muted";

    if (isSampleTrack(track))
    {
        uint8_t& mute = mtProject.values.trackMute[track];
        mute = mute ? 0 : 1;
        engine.muteTrack(uint8_t(track), mute);
        if (!mute)
            text = "Unmuted";
    }
    else
    {
        int midi = track - kSampleTracks;
        if (midi < 0 || midi >= kMaxMidiTracksInTrackers)
            return;
        uint8_t& mute = mtProject.values.midiTrackMute[midi];
        mute = mute ? 0 : 1;
        if (!mute)
            text = "Unmuted";
    }

    patternEditor.setProjectSaveFlags();
    showBriefStatus(text);
    patternEditor.refreshPattern();
    padStepSeqPaint();
}

void doCopy()
{
    Step* step = stepPtr(currentTrack(), currentStepIndex());
    if (step == nullptr)
        return;
    clipboard = *step;
    hasClipboard = true;
    showBriefStatus("Copied");
    padStepSeqPaint();
}

void doPaste()
{
    if (!hasClipboard)
    {
        showBriefStatus("Nothing copied");
        return;
    }

    Step* step = stepPtr(currentTrack(), currentStepIndex());
    if (step == nullptr)
        return;

    newFileManager.storePatternUndoRevision();
    *step = clipboard;
    showBriefStatus("Pasted");
    commitPattern();
}

bool stepIsEmpty(const Step* step)
{
    return step->note < 0 && step->instrument == 0 && step->fx[0].type == 0 &&
           step->fx[1].type == 0 && step->volume < 0 && step->micromove < 0;
}

void doClear()
{
    Step* step = stepPtr(currentTrack(), currentStepIndex());
    if (step == nullptr || stepIsEmpty(step))
    {
        showBriefStatus("Clear");
        return;
    }

    newFileManager.storePatternUndoRevision();
    sequencer.clearStep(step);
    showBriefStatus("Cleared");
    commitPattern();
}

void applyAction(int index)
{
    switch (index)
    {
        case kActionMute: doMute(); break;
        case kActionCopy: doCopy(); break;
        case kActionPaste: doPaste(); break;
        default: doClear(); break;
    }
}

void endHold()
{
    int was = held;
    int index = holdIndex;
    bool turned = holdTurned;
    held = kHoldNone;
    holdTurned = false;

    if (was == kHoldAction)
    {
        applyAction(index);
        return;
    }

    if (was == kHoldTrack && !turned)
    {
        toggleFxLane();
        return;
    }

    mtPopups.hide();
    patternEditor.clearPadStepStatus();
    padStepSeqPaint();
}

void beginPage(uint8_t pad, int row)
{
    held = kHoldPage;
    heldPad = pad;
    holdTurned = false;
    holdIndex = row;
    applyPage(row);
    showHeldStatus();
    padStepSeqPaint();
}

void beginTrack(uint8_t pad)
{
    held = kHoldTrack;
    heldPad = pad;
    holdTurned = false;
    holdIndex = currentTrack();
    showHeldStatus();
    padStepSeqPaint();
}

void beginMode(uint8_t pad, int row)
{
    held = kHoldMode;
    heldPad = pad;
    holdTurned = false;
    holdIndex = row;
    applyMode(row);
    showHeldStatus();
    padStepSeqPaint();
}

void beginAction(uint8_t pad, int row)
{
    held = kHoldAction;
    heldPad = pad;
    holdTurned = false;
    holdIndex = row;
    showHeldStatus();
    padStepSeqPaint();
}

void scrollHold(int16_t value)
{
    int count = 4;
    if (held == kHoldTrack)
        count = kTrackChoices;

    int dir = value > 0 ? 1 : -1;
    int steps = 1;
    if (held == kHoldTrack && (value >= 10 || value <= -10))
        steps = 4;

    holdTurned = true;
    for (int n = 0; n < steps; n++)
    {
        holdIndex += dir;
        if (holdIndex < 0)
            holdIndex = count - 1;
        if (holdIndex >= count)
            holdIndex = 0;
    }

    if (held == kHoldPage)
        applyPage(holdIndex);
    else if (held == kHoldTrack)
        applyTrackChoice(holdIndex);
    else if (held == kHoldMode)
        applyMode(holdIndex);

    showHeldStatus();
    padStepSeqPaint();
}

void toggleNote(int step)
{
    Step* st = stepPtr(currentTrack(), step);
    if (st == nullptr)
        return;

    newFileManager.storePatternUndoRevision();
    if (st->note >= 0)
    {
        st->note = Sequencer::STEP_NOTE_EMPTY;
        st->instrument = 0;
    }
    else
    {
        uint8_t note = noteForNewStep();
        uint8_t inst = instrumentForTrack(currentTrack());
        st->note = int8_t(note);
        st->instrument = inst;
        mtProject.values.lastUsedNote = note;
        mtProject.values.lastUsedInstrument = inst;
    }
    commitPattern();
}

uint8_t fxTypeForCcSlot(int slot)
{
    if (slot < 0)
        slot = 0;
    if (slot > 25)
        slot = 25;
    if (slot <= 4)
        return uint8_t(Fx::FX_TYPE_SEND_CC_A + slot);
    if (slot == 5)
        return uint8_t(Fx::FX_TYPE_SEND_CC_F);
    return uint8_t(Fx::FX_TYPE_SEND_CC_G + (slot - 6));
}

char ccLetter(int slot)
{
    if (slot < 0)
        slot = 0;
    if (slot > 25)
        slot = 25;
    return char('A' + slot);
}

bool sameStep(const Step& a, const Step& b)
{
    return a.note == b.note && a.instrument == b.instrument && a.volume == b.volume &&
           a.micromove == b.micromove && a.fx[0].type == b.fx[0].type &&
           a.fx[0].value == b.fx[0].value && a.fx[1].type == b.fx[1].type &&
           a.fx[1].value == b.fx[1].value;
}

void adjustNote(Step* st, int delta)
{
    // Semitones, not scale degrees. Held steps keep the intervals between them.
    int note;
    bool placeInstrument = false;
    if (st->note >= 0)
        note = int(st->note) + delta;
    else
    {
        note = int(noteForNewStep()) + delta;
        placeInstrument = true;
    }

    if (note < Sequencer::STEP_NOTE_MIN)
        note = Sequencer::STEP_NOTE_MIN;
    if (note > Sequencer::STEP_NOTE_MAX)
        note = Sequencer::STEP_NOTE_MAX;

    st->note = int8_t(note);
    if (placeInstrument)
        st->instrument = instrumentForTrack(currentTrack());
}

void adjustVelocity(Step* st, int delta)
{
    int volume = st->volume;
    if (volume < 0)
        volume = st->note >= 0 ? sequencer.getInstrumentVolume(st->instrument) : 50;

    volume += delta;
    if (volume < 0)
        volume = 0;
    if (volume > 100)
        volume = 100;
    st->volume = int8_t(volume);
}

void adjustCc(Step* st, int delta)
{
    Step::strFx& fx = st->fx[fxDataSlot()];
    if (Fx::ccSlotFromFx(fx.type) < 0)
    {
        fx.type = fxTypeForCcSlot(0);
        fx.value = 64;
    }
    else if (fx.value > 127)
    {
        fx.value = 127;
    }

    int value = int(fx.value) + delta;
    if (value < 0)
        value = 0;
    if (value > 127)
        value = 127;
    fx.value = uint8_t(value);
}

void adjustRoll(Step* st, int delta)
{
    Step::strFx& fx = st->fx[fxDataSlot()];
    if (fx.type != Fx::FX_TYPE_ROLL)
    {
        fx.type = Fx::FX_TYPE_ROLL;
        int fallback = sequencer.getFxDefault(Fx::FX_TYPE_ROLL);
        if (fallback < 0)
            fallback = 0;
        if (fallback > 255)
            fallback = 255;
        fx.value = uint8_t(fallback);
    }

    int value = int(fx.value) + delta;
    int minValue = sequencer.getFxMin(Fx::FX_TYPE_ROLL);
    int maxValue = sequencer.getFxMax(Fx::FX_TYPE_ROLL);
    if (value < minValue)
        value = minValue;
    if (value > maxValue)
        value = maxValue;
    if (value < 0)
        value = 0;
    if (value > 255)
        value = 255;
    fx.value = uint8_t(value);
}

void adjustCopy(Step* st, int delta)
{
    if (padEditMode == kModeNote)
        adjustNote(st, delta);
    else if (padEditMode == kModeVelocity)
        adjustVelocity(st, delta);
    else if (padEditMode == kModeCc)
        adjustCc(st, delta);
    else if (padEditMode == kModeRoll)
        adjustRoll(st, delta);
}

int findHeldPad(uint8_t pad)
{
    for (int i = 0; i < heldStepCount; i++)
    {
        if (heldSteps[i].pad == pad)
            return i;
    }
    return -1;
}

bool isHeldStep(int step)
{
    for (int i = 0; i < heldStepCount; i++)
    {
        if (heldSteps[i].step == step)
            return true;
    }
    return false;
}

void formatStepNumber(int stepIndex, char* buf, size_t size)
{
    if (mtConfig.general.numberingMode == NumberingMode::kHexadecimal)
        snprintf(buf, size, "%02X", stepIndex & 0xFF);
    else if (mtConfig.general.numberingMode == NumberingMode::kDecimalWithStepsFrom0)
        snprintf(buf, size, "%d", stepIndex);
    else
        snprintf(buf, size, "%d", stepIndex + 1);
}

int stepValueKey(const Step* st)
{
    const Step::strFx& fx = st->fx[fxDataSlot()];
    switch (padEditMode)
    {
        case kModeVelocity: return st->volume;
        case kModeCc:
        {
            int slot = Fx::ccSlotFromFx(fx.type);
            if (slot < 0)
                return -1;
            return slot * 128 + int(fx.value);
        }
        case kModeRoll:
            if (fx.type != Fx::FX_TYPE_ROLL)
                return -1;
            return fx.value;
        default: return st->note;
    }
}

void formatValue(const Step* st, char* buf, size_t size)
{
    const Step::strFx& fx = st->fx[fxDataSlot()];
    switch (padEditMode)
    {
        case kModeVelocity:
            if (st->volume >= 0)
                snprintf(buf, size, "Vel %d", int(st->volume));
            else
                snprintf(buf, size, "Vel off");
            break;
        case kModeCc:
        {
            int slot = Fx::ccSlotFromFx(fx.type);
            if (slot >= 0)
                snprintf(buf, size, "CC %c %u", ccLetter(slot), unsigned(fx.value));
            else
                snprintf(buf, size, "CC off");
            break;
        }
        case kModeRoll:
            if (fx.type == Fx::FX_TYPE_ROLL)
                snprintf(buf, size, "Roll %u", unsigned(fx.value));
            else
                snprintf(buf, size, "Roll off");
            break;
        default:
            if (st->note >= 0)
                snprintf(buf, size, "%s", mtNotes[st->note]);
            else
                snprintf(buf, size, "Off");
            break;
    }
}

void showHeldStepStatus()
{
    if (heldStepCount <= 0)
        return;

    if (heldStepCount == 1)
    {
        char number[8];
        formatStepNumber(heldSteps[0].step, number, sizeof number);
        snprintf(statusLine1, sizeof statusLine1, "Step %s", number);
    }
    else
    {
        snprintf(statusLine1, sizeof statusLine1, "%d steps", heldStepCount);
    }

    Step* first = stepPtr(currentTrack(), heldSteps[0].step);
    if (first == nullptr)
        snprintf(statusLine2, sizeof statusLine2, "-");
    else
        formatValue(first, statusLine2, sizeof statusLine2);

    if (heldStepCount > 1 && first != nullptr)
    {
        int key = stepValueKey(first);
        for (int i = 1; i < heldStepCount; i++)
        {
            Step* other = stepPtr(currentTrack(), heldSteps[i].step);
            if (other == nullptr || stepValueKey(other) != key)
            {
                snprintf(statusLine2, sizeof statusLine2, "Mixed");
                break;
            }
        }
    }

    snprintf(statusTitle, sizeof statusTitle, "%s  %s", statusLine1, statusLine2);
    patternEditor.showPadStepStatus(statusTitle);
    mtPopups.show(0, statusLine1, statusLine2, 30);
}

void rememberEditedNote()
{
    if (padEditMode != kModeNote || heldStepCount <= 0)
        return;

    Step* st = stepPtr(currentTrack(), heldSteps[heldStepCount - 1].step);
    if (st == nullptr || st->note < 0)
        return;

    mtProject.values.lastUsedNote = uint8_t(st->note);
    mtProject.values.lastUsedInstrument = st->instrument;
}

void adjustHeldSteps(int delta)
{
    int track = currentTrack();
    bool any = false;

    for (int i = 0; i < heldStepCount; i++)
    {
        Step* st = stepPtr(track, heldSteps[i].step);
        if (st == nullptr)
            continue;
        Step copy = *st;
        adjustCopy(&copy, delta);
        if (!sameStep(copy, *st))
            any = true;
    }

    if (!any)
    {
        showHeldStepStatus();
        return;
    }

    newFileManager.storePatternUndoRevision();
    for (int i = 0; i < heldStepCount; i++)
    {
        Step* st = stepPtr(track, heldSteps[i].step);
        if (st != nullptr)
            adjustCopy(st, delta);
    }
    rememberEditedNote();
    showHeldStepStatus();
    commitPattern();
}

void onStepPress(uint8_t pad, int step)
{
    int last = lastStepIndex();
    if (last < 0 || step < 0 || step > last)
        return;
    if (findHeldPad(pad) >= 0 || heldStepCount >= kMaxHeldSteps)
        return;

    heldSteps[heldStepCount].pad = pad;
    heldSteps[heldStepCount].step = int16_t(step);
    heldSteps[heldStepCount].sawHold = false;
    heldStepCount++;

    selectStep(step);
    patternEditor.refreshPattern();
    showHeldStepStatus();
    padStepSeqPaint();
}

void onStepHold(uint8_t pad)
{
    int index = findHeldPad(pad);
    if (index < 0)
        return;
    heldSteps[index].sawHold = true;
    showHeldStepStatus();
}

void onStepRelease(uint8_t pad)
{
    int index = findHeldPad(pad);
    if (index < 0)
        return;

    bool tap = !stepGestureTurned && !heldSteps[index].sawHold;
    int step = heldSteps[index].step;
    for (int j = index + 1; j < heldStepCount; j++)
        heldSteps[j - 1] = heldSteps[j];
    heldStepCount--;

    if (tap)
        toggleNote(step);

    if (heldStepCount == 0)
    {
        stepGestureTurned = false;
        mtPopups.hide();
        patternEditor.clearPadStepStatus();
        if (!tap)
            padStepSeqPaint();
    }
    else
    {
        showHeldStepStatus();
        padStepSeqPaint();
    }
}

} // namespace

bool padStepSeqIsOn()
{
    return active;
}

void padStepSeqToggle()
{
    if (held != kHoldNone)
    {
        held = kHoldNone;
        holdTurned = false;
        mtPopups.hide();
    }

    heldStepCount = 0;
    stepGestureTurned = false;
    active = !active;
    pendingPaint = false;
    shownPlayhead = -2;
    shownCursor = -2;

    if (active)
    {
        pullPageFromEditor();
        mtPopups.show(0, "Pad steps on");
        padStepSeqPaint();
    }
    else
    {
        patternEditor.clearPadStepStatus();
        patternEditor.lightUpPadBoard();
        mtPopups.show(0, "Pad steps off");
    }
}

void padStepSeqPaint()
{
    if (!active)
        return;

    if (patternEditor.selectedPlace >= 0 || patternEditor.fillState != 0 ||
        patternEditor.keyboardManager.getState())
    {
        pendingPaint = true;
        return;
    }

    pendingPaint = false;
    pullPageFromEditor();

    const uint8_t back = mtConfig.values.padsLightBack;
    const uint8_t front = mtConfig.values.padsLightFront;
    const int last = lastStepIndex();
    const int origin = int(currentPage) * kStepsPerPage;
    const int playhead = patternEditor.trackerPattern.playheadPosition;
    const int cursor = patternEditor.trackerPattern.actualStep;
    const bool showHead = !sequencer.isStop();
    const int track = currentTrack();

    padsBacklight.clearGlobalPads(1, 1, 0);

    for (int row = 0; row < kGridRows; row++)
    {
        for (int col = 0; col < kStepColumns; col++)
        {
            int step = origin + row * kStepColumns + col;
            int pad = row * kGridColumns + col;
            if (step > last)
                continue;

            Step* st = stepPtr(track, step);
            bool hasNote = st != nullptr && st->note >= 0;
            bool head = showHead && step == playhead;
            bool selected = step == cursor || isHeldStep(step);
            if (head || selected)
                padsBacklight.setFrontLayer(1, front, uint8_t(pad));
            else if (hasNote)
                padsBacklight.setBackLayer(1, back, uint8_t(pad));
        }

        int pagePad = row * kGridColumns + kColPage;
        if (row == int(currentPage))
            padsBacklight.setFrontLayer(1, front, uint8_t(pagePad));
        else
            padsBacklight.setBackLayer(1, back, uint8_t(pagePad));

        int trackPad = row * kGridColumns + kColTrack;
        if (held == kHoldTrack)
            padsBacklight.setFrontLayer(1, front, uint8_t(trackPad));
        else
            padsBacklight.setBackLayer(1, back, uint8_t(trackPad));

        int modePad = row * kGridColumns + kColMode;
        if (row == int(padEditMode))
            padsBacklight.setFrontLayer(1, front, uint8_t(modePad));
        else
            padsBacklight.setBackLayer(1, back, uint8_t(modePad));

        int actionPad = row * kGridColumns + kColAction;
        if (held == kHoldAction && row == holdIndex)
            padsBacklight.setFrontLayer(1, front, uint8_t(actionPad));
        else
            padsBacklight.setBackLayer(1, back, uint8_t(actionPad));
    }

    shownPlayhead = playhead;
    shownCursor = cursor;
}

bool padStepSeqOnPad(uint8_t pad, uint8_t state)
{
    if (!active)
        return false;
    if (pad >= 48)
        return true;

    if (state == buttonHold)
    {
        onStepHold(pad);
        return true;
    }

    if (state != buttonPress && state != buttonRelease)
        return true;

    if (state == buttonRelease)
    {
        if (held != kHoldNone && pad == heldPad)
            endHold();
        else
            onStepRelease(pad);
        return true;
    }

    int col = columnOf(pad);
    int row = rowOf(pad);

    if (col < kStepColumns)
    {
        if (held != kHoldNone)
            return true;
        onStepPress(pad, int(currentPage) * kStepsPerPage + row * kStepColumns + col);
        return true;
    }

    if (heldStepCount > 0 || held != kHoldNone)
        return true;

    if (col == kColPage)
        beginPage(pad, row);
    else if (col == kColTrack)
        beginTrack(pad);
    else if (col == kColMode)
        beginMode(pad, row);
    else
        beginAction(pad, row);

    return true;
}

bool padStepSeqOnEncoder(int16_t value)
{
    if (!active || patternEditor.fillState != 0)
        return false;

    if (held != kHoldNone)
    {
        if (value != 0)
            scrollHold(value);
        return true;
    }

    if (heldStepCount > 0)
    {
        if (value != 0)
        {
            stepGestureTurned = true;
            adjustHeldSteps(value);
        }
        return true;
    }

    return false;
}

void padStepSeqOnFrame(bool, int playhead)
{
    if (!active)
        return;

    int cursor = patternEditor.trackerPattern.actualStep;
    int pageNow = currentPage;
    if (!padGestureActive())
    {
        int page = editorScrollStep() / kStepsPerPage;
        if (page < 0)
            page = 0;
        if (page >= kPageCount)
            page = kPageCount - 1;
        pageNow = page;
    }

    if (pageNow != int(currentPage) || playhead != shownPlayhead || cursor != shownCursor ||
        pendingPaint)
        padStepSeqPaint();
}

void padStepSeqOnPlay()
{
    if (!active)
        return;
    padStepSeqPaint();
}

#endif
