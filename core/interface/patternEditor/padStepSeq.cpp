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

// 8 step columns and 4 control columns. One page is 32 steps.
// Four pages cover a 128-step pattern. Everything written here goes
// into sequencer.getPatternToUI(), the pattern on screen.
//
// Step index = page * 32 + row * 8 + column. Row 0 is pad indexes
// 0-11, the same order performance mode uses. The popup numbers a
// step the way the pattern editor does (from 0, from 1, or hex).
//
// The pad page follows middleVisibleStep, the step the editor is
// scrolled to, whenever no pad is held. Choosing a page moves the
// editor cursor to the same offset on that page.
//
// Columns 10 and 11 already come from PadsImpl::convertToGridKey4x12,
// which folds the swapped right-edge scan into the index the LEDs use.
// Do not swap them again. If a device shows FX2 and Page mirrored
// against FX1 and Solo, change columnOf() only.
//
// Right-hand pads, row 0 at the top:
//   Note  Instrument  FX1  FX2     tap selects the lane
//   Track Mute        Solo Page    track and page are hold + encoder
//   Hits  Length      Rot  Reverse euclid rewrites this track's notes
//   <<    >>          x2   Fills
//
// Hold a step and turn to edit the selected lane. A short tap toggles
// the note. Several held steps take the same encoder change.
// These presses do not send MIDI.
//
// Reverse is playback state in the sequencer, not pattern data. Stop
// clears it. Euclid, shift, double, and halve write the current track.

namespace
{

constexpr int kGridColumns = 12;
constexpr int kGridRows = 4;
constexpr int kStepColumns = 8;
constexpr int kStepsPerPage = 32;
constexpr int kPageCount = 4;
constexpr int kMaxHeldSteps = 32;
constexpr int kReverseLoopMax = 16;

constexpr int kLaneNote = 0;
constexpr int kLaneInstrument = 1;
constexpr int kLaneFx1 = 2;
constexpr int kLaneFx2 = 3;

constexpr int kCtrlNone = 0;
constexpr int kCtrlNote = 1;
constexpr int kCtrlInstrument = 2;
constexpr int kCtrlFx1 = 3;
constexpr int kCtrlFx2 = 4;
constexpr int kCtrlTrack = 5;
constexpr int kCtrlMute = 6;
constexpr int kCtrlSolo = 7;
constexpr int kCtrlPage = 8;
constexpr int kCtrlHits = 9;
constexpr int kCtrlLength = 10;
constexpr int kCtrlRotate = 11;
constexpr int kCtrlReverse = 12;
constexpr int kCtrlShiftLeft = 13;
constexpr int kCtrlShiftRight = 14;
constexpr int kCtrlDouble = 15;
constexpr int kCtrlFills = 16;

constexpr int kFillVelocity = 0;
constexpr int kFillChance = 1;
constexpr int kFillRatchet = 2;
constexpr int kFillCount = 3;

using Step = Sequencer::strPattern::strTrack::strStep;
using Fx = Sequencer::strFxConsts;

struct HeldStep
{
    uint8_t pad;
    int16_t step;
    bool sawHold;
};

struct Euclid
{
    uint8_t hits;
    uint8_t length;
    uint8_t rotate;
    uint8_t ready;
};

bool active = false;
uint8_t currentPage = 0;
uint8_t padLane = kLaneNote;
uint8_t heldControl = kCtrlNone;
uint8_t heldPad = 0;
bool holdTurned = false;
HeldStep heldSteps[kMaxHeldSteps];
int heldStepCount = 0;
bool stepGestureTurned = false;
int shownPlayhead = -2;
int shownCursor = -2;
uint8_t shownReverse = 0;
bool pendingPaint = false;
int fillChoice = kFillVelocity;
Euclid euclid[kAllTracks];
uint8_t reverseLoops[kAllTracks];
bool soloLatched = false;
int soloTrack = -1;
uint8_t savedSampleMute[kSampleTracks];
uint8_t savedMidiMute[kMaxMidiTracksInTrackers];
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

int controlAt(int row, int col)
{
    static const int kMap[4][4] = {
      { kCtrlNote, kCtrlInstrument, kCtrlFx1, kCtrlFx2 },
      { kCtrlTrack, kCtrlMute, kCtrlSolo, kCtrlPage },
      { kCtrlHits, kCtrlLength, kCtrlRotate, kCtrlReverse },
      { kCtrlShiftLeft, kCtrlShiftRight, kCtrlDouble, kCtrlFills },
    };

    if (row < 0 || row >= kGridRows || col < kStepColumns || col >= kGridColumns)
        return kCtrlNone;
    return kMap[row][col - kStepColumns];
}

const char* laneName(int lane)
{
    switch (lane)
    {
        case kLaneInstrument: return "Instrument";
        case kLaneFx1: return "FX 1";
        case kLaneFx2: return "FX 2";
        default: return "Note";
    }
}

const char* fillName(int choice)
{
    switch (choice)
    {
        case kFillChance: return "Chance";
        case kFillRatchet: return "Ratchet";
        default: return "Velocity";
    }
}

int fxSlotForLane(int lane)
{
    // The on-screen FX1 column is stored in fx[1]. FX2 is fx[0].
    return lane == kLaneFx2 ? 0 : 1;
}

void syncEditColumn()
{
    switch (padLane)
    {
        case kLaneInstrument: patternEditor.editParam = cPatternEditor::EditInstr; break;
        case kLaneFx1: patternEditor.editParam = cPatternEditor::EditFx1; break;
        case kLaneFx2: patternEditor.editParam = cPatternEditor::EditFx2; break;
        default: patternEditor.editParam = cPatternEditor::EditNote; break;
    }
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

int patternStepCount()
{
    int last = lastStepIndex();
    if (last < 0)
        return 0;
    return last + 1;
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

Step* stepPtr(int track, int step)
{
    Sequencer::strPattern* pat = pattern();
    if (pat == nullptr || track < 0 || track >= kAllTracks || step < 0 || step > Sequencer::MAXSTEP)
        return nullptr;
    return &pat->track[track].step[step];
}

bool padGestureActive()
{
    return heldControl != kCtrlNone || heldStepCount > 0;
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

int trackPlayhead()
{
    int pos = sequencer.trackActualPos(uint8_t(currentTrack()));
    if (pos < 0)
        pos = 0;
    if (pos > Sequencer::MAXSTEP)
        pos = Sequencer::MAXSTEP;
    return pos;
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

int instrumentMin(int track)
{
    return isSampleTrack(track) ? 0 : int(MIDI_INSTRUMENTS_START_IDX);
}

int instrumentMax(int track)
{
    if (isSampleTrack(track))
        return int(kSampleInstrumentsCount) - 1;
    return int(MIDI_INSTRUMENTS_START_IDX) + int(kMidiInstrumentsCount) - 1;
}

bool trackIsMuted(int track)
{
    if (isSampleTrack(track))
        return mtProject.values.trackMute[track] != 0;
    int midi = track - kSampleTracks;
    if (midi < 0 || midi >= kMaxMidiTracksInTrackers)
        return false;
    return mtProject.values.midiTrackMute[midi] != 0;
}

void applySampleMutes()
{
    for (int track = 0; track < kSampleTracks; track++)
        engine.muteTrack(uint8_t(track), mtProject.values.trackMute[track]);
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
    selectStep(page * kStepsPerPage + offset);
    patternEditor.refreshPattern();
}

void showLines(const char* line1, const char* line2, uint8_t seconds)
{
    snprintf(statusLine1, sizeof statusLine1, "%s", line1);
    if (line2 != nullptr && line2[0] != 0)
    {
        snprintf(statusLine2, sizeof statusLine2, "%s", line2);
        snprintf(statusTitle, sizeof statusTitle, "%s  %s", statusLine1, statusLine2);
        patternEditor.showPadStepStatus(statusTitle);
        mtPopups.show(0, statusLine1, statusLine2, seconds);
    }
    else
    {
        statusLine2[0] = 0;
        patternEditor.showPadStepStatus(statusLine1);
        mtPopups.show(0, statusLine1, seconds);
    }
}

void showBrief(const char* text)
{
    showLines(text, nullptr, 2);
}

void hideStatus()
{
    mtPopups.hide();
    patternEditor.clearPadStepStatus();
}

void showHeldStatus()
{
    char line1[24];
    char line2[24];
    line1[0] = 0;
    line2[0] = 0;
    int track = currentTrack();

    switch (heldControl)
    {
        case kCtrlTrack:
            snprintf(line1, sizeof line1, "Track %d", track + 1);
            break;
        case kCtrlPage:
            snprintf(line1, sizeof line1, "Page %d", int(currentPage) + 1);
            break;
        case kCtrlHits:
        case kCtrlLength:
        case kCtrlRotate:
            snprintf(line1, sizeof line1, "Euc %d/%d", int(euclid[track].hits), int(euclid[track].length));
            snprintf(line2, sizeof line2, "Rot %d", int(euclid[track].rotate));
            break;
        case kCtrlReverse:
            if (reverseLoops[track] == 0)
                snprintf(line1, sizeof line1, "Reverse inf");
            else
                snprintf(line1, sizeof line1, "Reverse %d", int(reverseLoops[track]));
            break;
        case kCtrlDouble: snprintf(line1, sizeof line1, "Double / halve"); break;
        case kCtrlFills: snprintf(line1, sizeof line1, "%s", fillName(fillChoice)); break;
        default: break;
    }

    if (line1[0] != 0)
        showLines(line1, line2, 30);
}

void setTrack(int track)
{
    if (track < 0)
        track = 0;
    if (track >= kAllTracks)
        track = kAllTracks - 1;
    patternEditor.trackerPattern.actualTrack = int8_t(track);
    patternEditor.focusOnActual();
    patternEditor.refreshPattern();
}

void applyPage(int page)
{
    if (page < 0)
        page = 0;
    if (page >= kPageCount)
        page = kPageCount - 1;
    currentPage = uint8_t(page);
    scrollEditorToPage(currentPage);
}

void selectLane(int lane)
{
    if (lane < kLaneNote)
        lane = kLaneNote;
    if (lane > kLaneFx2)
        lane = kLaneFx2;
    padLane = uint8_t(lane);
    syncEditColumn();
    patternEditor.refreshPattern();
    showBrief(laneName(padLane));
    padStepSeqPaint();
}

void toggleMute()
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
    showBrief(text);
    patternEditor.refreshPattern();
    padStepSeqPaint();
}

void rememberMutes()
{
    for (int track = 0; track < kSampleTracks; track++)
        savedSampleMute[track] = mtProject.values.trackMute[track];
    for (int track = 0; track < kMaxMidiTracksInTrackers; track++)
        savedMidiMute[track] = mtProject.values.midiTrackMute[track];
}

void restoreMutes()
{
    for (int track = 0; track < kSampleTracks; track++)
        mtProject.values.trackMute[track] = savedSampleMute[track];
    for (int track = 0; track < kMaxMidiTracksInTrackers; track++)
        mtProject.values.midiTrackMute[track] = savedMidiMute[track];
    applySampleMutes();
}

void soloOnly(int track)
{
    for (int index = 0; index < kSampleTracks; index++)
        mtProject.values.trackMute[index] = index == track ? 0 : 1;
    for (int index = 0; index < kMaxMidiTracksInTrackers; index++)
    {
        int full = index + kSampleTracks;
        mtProject.values.midiTrackMute[index] = full == track ? 0 : 1;
    }
    applySampleMutes();
}

void toggleSolo()
{
    int track = currentTrack();
    if (soloLatched && soloTrack == track)
    {
        restoreMutes();
        soloLatched = false;
        soloTrack = -1;
        showBrief("Solo off");
    }
    else
    {
        if (!soloLatched)
            rememberMutes();
        soloLatched = true;
        soloTrack = track;
        soloOnly(track);
        showBrief("Solo");
    }

    patternEditor.setProjectSaveFlags();
    patternEditor.refreshPattern();
    padStepSeqPaint();
}

int euclidMaxN()
{
    int steps = patternStepCount();
    if (steps < 1)
        steps = 1;
    if (steps > 32)
        steps = 32;
    return steps;
}

void ensureEuclid(int track)
{
    if (euclid[track].ready)
        return;

    int n = patternStepCount();
    if (n > 16)
        n = 16;
    if (n > euclidMaxN())
        n = euclidMaxN();
    if (n < 1)
        n = 1;

    euclid[track].length = uint8_t(n);
    euclid[track].hits = uint8_t(n < 4 ? n : 4);
    euclid[track].rotate = 0;
    euclid[track].ready = 1;
}

void clampEuclid(int track)
{
    int nMax = euclidMaxN();
    int n = euclid[track].length;
    if (n < 1)
        n = 1;
    if (n > nMax)
        n = nMax;
    int hits = euclid[track].hits;
    if (hits < 0)
        hits = 0;
    if (hits > n)
        hits = n;
    int rot = euclid[track].rotate;
    if (n <= 1)
        rot = 0;
    else
        rot = rot % n;
    euclid[track].length = uint8_t(n);
    euclid[track].hits = uint8_t(hits);
    euclid[track].rotate = uint8_t(rot);
}

void makeEuclid(int hits, int length, int rotate, uint8_t* out)
{
    for (int step = 0; step < length; step++)
        out[step] = 0;
    if (hits <= 0 || length <= 0)
        return;
    if (hits > length)
        hits = length;

    int bucket = 0;
    for (int step = 0; step < length; step++)
    {
        bucket += hits;
        if (bucket >= length)
        {
            bucket -= length;
            out[step] = 1;
        }
    }

    if (rotate == 0 || length <= 1)
        return;

    int shift = rotate % length;
    uint8_t temp[32];
    for (int step = 0; step < length; step++)
        temp[step] = out[step];
    for (int step = 0; step < length; step++)
        out[step] = temp[(step - shift + length) % length];
}

void rewriteEuclid(int track)
{
    ensureEuclid(track);
    clampEuclid(track);

    int steps = patternStepCount();
    if (steps <= 0)
        return;

    int n = euclid[track].length;
    uint8_t hit[32];
    makeEuclid(euclid[track].hits, n, euclid[track].rotate, hit);

    uint8_t note = noteForNewStep();
    uint8_t inst = instrumentForTrack(track);

    newFileManager.storePatternUndoRevision();
    for (int step = 0; step < steps; step++)
    {
        Step* target = stepPtr(track, step);
        if (target == nullptr)
            continue;
        if (hit[step % n])
        {
            target->note = int8_t(note);
            target->instrument = inst;
        }
        else
        {
            target->note = Sequencer::STEP_NOTE_EMPTY;
            target->instrument = 0;
        }
    }

    mtProject.values.lastUsedNote = note;
    mtProject.values.lastUsedInstrument = inst;
    commitPattern();
}

void nudgeEuclid(int track, int field, int dir)
{
    ensureEuclid(track);
    clampEuclid(track);

    if (field == kCtrlHits)
    {
        int hits = int(euclid[track].hits) + dir;
        if (hits < 0)
            hits = 0;
        if (hits > int(euclid[track].length))
            hits = int(euclid[track].length);
        euclid[track].hits = uint8_t(hits);
    }
    else if (field == kCtrlLength)
    {
        int n = int(euclid[track].length) + dir;
        int nMax = euclidMaxN();
        if (n < 1)
            n = 1;
        if (n > nMax)
            n = nMax;
        euclid[track].length = uint8_t(n);
        clampEuclid(track);
    }
    else
    {
        int n = euclid[track].length;
        int rot = int(euclid[track].rotate) + dir;
        if (n <= 1)
            rot = 0;
        else
        {
            rot %= n;
            if (rot < 0)
                rot += n;
        }
        euclid[track].rotate = uint8_t(rot);
    }

    rewriteEuclid(track);
    showHeldStatus();
}

void toggleReverse()
{
    int track = currentTrack();
    bool enabled = sequencer.padTrackReverseEnabled(uint8_t(track));
    sequencer.setPadTrackReverse(uint8_t(track), !enabled, reverseLoops[track]);
    if (!enabled)
    {
        if (reverseLoops[track] == 0)
            showBrief("Reverse inf");
        else
        {
            char text[16];
            snprintf(text, sizeof text, "Reverse %d", int(reverseLoops[track]));
            showBrief(text);
        }
    }
    else
    {
        showBrief("Forward");
    }
    padStepSeqPaint();
}

void nudgeReverseLoops(int dir)
{
    int track = currentTrack();
    int count = reverseLoops[track];
    // 0 is infinite, then 1..kReverseLoopMax.
    if (dir > 0)
        count = count >= kReverseLoopMax ? 0 : count + 1;
    else
        count = count <= 0 ? kReverseLoopMax : count - 1;
    reverseLoops[track] = uint8_t(count);

    if (sequencer.padTrackReverseEnabled(uint8_t(track)))
        sequencer.setPadTrackReverse(uint8_t(track), true, reverseLoops[track]);
    showHeldStatus();
}

void rotateTrack(int dir)
{
    int track = currentTrack();
    int last = lastStepIndex();
    if (last < 1)
        return;

    Sequencer::strPattern* pat = pattern();
    if (pat == nullptr)
        return;

    Step* steps = pat->track[track].step;
    newFileManager.storePatternUndoRevision();
    if (dir > 0)
    {
        Step wrapped = steps[last];
        for (int step = last; step > 0; step--)
            steps[step] = steps[step - 1];
        steps[0] = wrapped;
        showBrief("Shift right");
    }
    else
    {
        Step wrapped = steps[0];
        for (int step = 0; step < last; step++)
            steps[step] = steps[step + 1];
        steps[last] = wrapped;
        showBrief("Shift left");
    }
    commitPattern();
}

void doubleOrHalve(int dir)
{
    int track = currentTrack();
    int count = patternStepCount();
    int half = count / 2;
    if (half < 1)
    {
        showBrief("Too short");
        return;
    }

    Sequencer::strPattern* pat = pattern();
    if (pat == nullptr)
        return;

    Step* steps = pat->track[track].step;
    newFileManager.storePatternUndoRevision();
    if (dir > 0)
    {
        for (int step = 0; step < half; step++)
            steps[half + step] = steps[step];
        showBrief("Doubled");
    }
    else
    {
        for (int step = half; step < count; step++)
            sequencer.clearStep(&steps[step]);
        showBrief("Halved");
    }
    commitPattern();
}

void applyFills()
{
    int track = currentTrack();
    int last = lastStepIndex();
    if (last < 0)
        return;

    int targets[128];
    int count = 0;
    if (heldStepCount > 0)
    {
        for (int index = 0; index < heldStepCount && count < 128; index++)
        {
            int step = heldSteps[index].step;
            if (step >= 0 && step <= last)
                targets[count++] = step;
        }
    }
    else
    {
        for (int step = 0; step <= last && count < 128; step++)
            targets[count++] = step;
    }

    if (count == 0)
        return;

    int slot = fxSlotForLane(padLane);
    newFileManager.storePatternUndoRevision();

    for (int index = 0; index < count; index++)
    {
        Step* step = stepPtr(track, targets[index]);
        if (step == nullptr)
            continue;

        if (fillChoice == kFillVelocity)
        {
            step->volume = int8_t(utilMathRandom::random(0, 101));
        }
        else if (fillChoice == kFillChance)
        {
            step->fx[slot].type = Fx::FX_TYPE_CHANCE;
            step->fx[slot].value = uint8_t(utilMathRandom::random(1, Fx::CHANCE_PROB_MAX + 1));
        }
        else
        {
            int minValue = sequencer.getFxMin(Fx::FX_TYPE_ROLL);
            int maxValue = sequencer.getFxMax(Fx::FX_TYPE_ROLL);
            if (minValue < 1)
                minValue = 1;
            if (maxValue < minValue)
                maxValue = minValue;
            step->fx[slot].type = Fx::FX_TYPE_ROLL;
            step->fx[slot].value = uint8_t(utilMathRandom::random(minValue, maxValue + 1));
        }
    }

    showBrief(fillName(fillChoice));
    commitPattern();
}

void endControlHold()
{
    int control = heldControl;
    bool turned = holdTurned;
    heldControl = kCtrlNone;
    holdTurned = false;

    if (control == kCtrlFills)
    {
        applyFills();
        return;
    }

    if (control == kCtrlReverse && !turned)
    {
        toggleReverse();
        return;
    }

    hideStatus();
    padStepSeqPaint();
}

void beginControl(uint8_t pad, int control)
{
    heldControl = uint8_t(control);
    heldPad = pad;
    holdTurned = false;
    if (control == kCtrlHits || control == kCtrlLength || control == kCtrlRotate)
        ensureEuclid(currentTrack());
    showHeldStatus();
    padStepSeqPaint();
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

void adjustInstrument(Step* st, int delta)
{
    int track = currentTrack();
    int minInst = instrumentMin(track);
    int maxInst = instrumentMax(track);
    int inst = st->instrument;

    if (st->note < 0)
    {
        st->note = int8_t(noteForNewStep());
        inst = instrumentForTrack(track);
    }
    if (inst < minInst || inst > maxInst)
        inst = instrumentForTrack(track);

    inst += delta;
    if (inst < minInst)
        inst = minInst;
    if (inst > maxInst)
        inst = maxInst;
    st->instrument = uint8_t(inst);
}

void adjustFx(Step* st, int slot, int delta)
{
    Step::strFx& fx = st->fx[slot];
    if (fx.type == 0)
    {
        int type = interfaceGlobals.fxNameToId(mtProject.values.lastUsedFx);
        if (type <= 0)
            type = Fx::FX_TYPE_VOLUME;
        fx.type = uint8_t(type);
        int fallback = sequencer.getFxDefault(fx.type);
        if (fallback < 0)
            fallback = 0;
        if (fallback > 255)
            fallback = 255;
        fx.value = uint8_t(fallback);
    }

    int value = int(fx.value) + delta;
    int minValue = sequencer.getFxMin(fx.type);
    int maxValue = sequencer.getFxMax(fx.type);
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
    if (padLane == kLaneInstrument)
        adjustInstrument(st, delta);
    else if (padLane == kLaneFx1)
        adjustFx(st, 1, delta);
    else if (padLane == kLaneFx2)
        adjustFx(st, 0, delta);
    else
        adjustNote(st, delta);
}

int findHeldPad(uint8_t pad)
{
    for (int index = 0; index < heldStepCount; index++)
    {
        if (heldSteps[index].pad == pad)
            return index;
    }
    return -1;
}

bool isHeldStep(int step)
{
    for (int index = 0; index < heldStepCount; index++)
    {
        if (heldSteps[index].step == step)
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
    if (padLane == kLaneInstrument)
        return st->note < 0 ? -1 : int(st->instrument);
    if (padLane == kLaneFx1 || padLane == kLaneFx2)
    {
        const Step::strFx& fx = st->fx[fxSlotForLane(padLane)];
        if (fx.type == 0)
            return -1;
        return int(fx.type) * 256 + int(fx.value);
    }
    return st->note;
}

void formatValue(const Step* st, char* buf, size_t size)
{
    int track = currentTrack();
    if (padLane == kLaneInstrument)
    {
        if (st->note < 0)
            snprintf(buf, size, "Off");
        else if (isSampleTrack(track))
            snprintf(buf, size, "Inst %d", int(st->instrument) + 1);
        else
            snprintf(buf, size, "MIDI %d", int(st->instrument) - int(MIDI_INSTRUMENTS_START_IDX) + 1);
        return;
    }

    if (padLane == kLaneFx1 || padLane == kLaneFx2)
    {
        const Step::strFx& fx = st->fx[fxSlotForLane(padLane)];
        if (fx.type == 0)
            snprintf(buf, size, "FX off");
        else
            snprintf(buf, size, "FX %u", unsigned(fx.value));
        return;
    }

    if (st->note >= 0)
        snprintf(buf, size, "%s", mtNotes[st->note]);
    else
        snprintf(buf, size, "Off");
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
        for (int index = 1; index < heldStepCount; index++)
        {
            Step* other = stepPtr(currentTrack(), heldSteps[index].step);
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
    if (heldStepCount <= 0)
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

    for (int index = 0; index < heldStepCount; index++)
    {
        Step* st = stepPtr(track, heldSteps[index].step);
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
    for (int index = 0; index < heldStepCount; index++)
    {
        Step* st = stepPtr(track, heldSteps[index].step);
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

void onStepRelease(uint8_t pad)
{
    int index = findHeldPad(pad);
    if (index < 0)
        return;

    bool tap = !stepGestureTurned && !heldSteps[index].sawHold;
    int step = heldSteps[index].step;
    for (int next = index + 1; next < heldStepCount; next++)
        heldSteps[next - 1] = heldSteps[next];
    heldStepCount--;

    if (tap)
        toggleNote(step);

    if (heldStepCount == 0)
    {
        stepGestureTurned = false;
        if (heldControl == kCtrlNone)
            hideStatus();
        if (!tap)
            padStepSeqPaint();
    }
    else
    {
        showHeldStepStatus();
        padStepSeqPaint();
    }
}

void onControlPress(uint8_t pad, int control)
{
    switch (control)
    {
        case kCtrlNote: selectLane(kLaneNote); break;
        case kCtrlInstrument: selectLane(kLaneInstrument); break;
        case kCtrlFx1: selectLane(kLaneFx1); break;
        case kCtrlFx2: selectLane(kLaneFx2); break;
        case kCtrlMute: toggleMute(); break;
        case kCtrlSolo: toggleSolo(); break;
        case kCtrlShiftLeft: rotateTrack(-1); break;
        case kCtrlShiftRight: rotateTrack(1); break;
        default: beginControl(pad, control); break;
    }
}

int encoderDir(int16_t value)
{
    return value > 0 ? 1 : -1;
}

void onControlEncoder(int16_t value)
{
    int dir = encoderDir(value);
    holdTurned = true;

    switch (heldControl)
    {
        case kCtrlTrack:
        {
            int jump = (value >= 10 || value <= -10) ? 4 : 1;
            int track = currentTrack() + dir * jump;
            if (track < 0)
                track = kAllTracks - 1;
            if (track >= kAllTracks)
                track = 0;
            setTrack(track);
            showHeldStatus();
            padStepSeqPaint();
            break;
        }
        case kCtrlPage:
        {
            int page = int(currentPage) + dir;
            if (page < 0)
                page = kPageCount - 1;
            if (page >= kPageCount)
                page = 0;
            applyPage(page);
            showHeldStatus();
            padStepSeqPaint();
            break;
        }
        case kCtrlHits:
        case kCtrlLength:
        case kCtrlRotate: nudgeEuclid(currentTrack(), heldControl, dir); break;
        case kCtrlReverse: nudgeReverseLoops(dir); break;
        case kCtrlDouble: doubleOrHalve(dir); break;
        case kCtrlFills:
            fillChoice += dir;
            if (fillChoice < 0)
                fillChoice = kFillCount - 1;
            if (fillChoice >= kFillCount)
                fillChoice = 0;
            showHeldStatus();
            break;
        default: break;
    }
}

bool laneLit(const Step* st)
{
    if (st == nullptr)
        return false;
    if (padLane == kLaneFx1)
        return st->fx[1].type != 0;
    if (padLane == kLaneFx2)
        return st->fx[0].type != 0;
    return st->note >= 0;
}

} // namespace

bool padStepSeqIsOn()
{
    return active;
}

void padStepSeqToggle()
{
    if (heldControl != kCtrlNone)
    {
        heldControl = kCtrlNone;
        holdTurned = false;
        mtPopups.hide();
    }

    heldStepCount = 0;
    stepGestureTurned = false;
    active = !active;
    pendingPaint = false;
    shownPlayhead = -2;
    shownCursor = -2;
    shownReverse = 0;

    if (active)
    {
        pullPageFromEditor();
        syncEditColumn();
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
    const int playhead = trackPlayhead();
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
            bool head = showHead && step == playhead;
            bool selected = step == cursor || isHeldStep(step);
            if (head || selected)
                padsBacklight.setFrontLayer(1, front, uint8_t(pad));
            else if (laneLit(st))
                padsBacklight.setBackLayer(1, back, uint8_t(pad));
        }

        for (int col = kStepColumns; col < kGridColumns; col++)
        {
            int pad = row * kGridColumns + col;
            int control = controlAt(row, col);
            bool bright = false;

            if (control == kCtrlNote)
                bright = padLane == kLaneNote;
            else if (control == kCtrlInstrument)
                bright = padLane == kLaneInstrument;
            else if (control == kCtrlFx1)
                bright = padLane == kLaneFx1;
            else if (control == kCtrlFx2)
                bright = padLane == kLaneFx2;
            else if (control == kCtrlMute)
                bright = trackIsMuted(track);
            else if (control == kCtrlSolo)
                bright = soloLatched && soloTrack == track;
            else if (control == kCtrlReverse)
                bright = sequencer.padTrackReverseEnabled(uint8_t(track));
            else
                bright = heldControl == control;

            if (bright)
                padsBacklight.setFrontLayer(1, front, uint8_t(pad));
            else
                padsBacklight.setBackLayer(1, back, uint8_t(pad));
        }
    }

    shownPlayhead = playhead;
    shownCursor = cursor;
    shownReverse = sequencer.padTrackReverseEnabled(uint8_t(track)) ? 1 : 0;
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

    int col = columnOf(pad);
    int row = rowOf(pad);

    if (state == buttonRelease)
    {
        if (heldControl != kCtrlNone && pad == heldPad)
            endControlHold();
        else if (col < kStepColumns)
            onStepRelease(pad);
        return true;
    }

    if (col < kStepColumns)
    {
        if (heldControl != kCtrlNone && heldControl != kCtrlFills)
            return true;
        onStepPress(pad, int(currentPage) * kStepsPerPage + row * kStepColumns + col);
        return true;
    }

    int control = controlAt(row, col);
    bool lane = control == kCtrlNote || control == kCtrlInstrument || control == kCtrlFx1 ||
                control == kCtrlFx2;
    if (!lane && heldStepCount > 0 && control != kCtrlFills)
        return true;
    if (heldControl != kCtrlNone)
        return true;

    onControlPress(pad, control);
    return true;
}

bool padStepSeqOnEncoder(int16_t value)
{
    if (!active || patternEditor.fillState != 0)
        return false;

    if (heldControl != kCtrlNone)
    {
        if (value != 0)
            onControlEncoder(value);
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

void padStepSeqOnFrame(bool, int)
{
    if (!active)
        return;

    int head = trackPlayhead();
    int cursor = patternEditor.trackerPattern.actualStep;
    uint8_t reverseNow = sequencer.padTrackReverseEnabled(uint8_t(currentTrack())) ? 1 : 0;
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

    if (pageNow != int(currentPage) || head != shownPlayhead || cursor != shownCursor ||
        reverseNow != shownReverse || pendingPaint)
        padStepSeqPaint();
}

void padStepSeqOnPlay()
{
    if (!active)
        return;
    padStepSeqPaint();
}

#endif
