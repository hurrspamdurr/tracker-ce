#ifndef MIDI_CC_EXT_H_
#define MIDI_CC_EXT_H_

#include <stddef.h>
#include <stdint.h>

// MIDI CC slots A..Z. A..F stay in the 16-byte instrument record inside
// project.mt. G..Z live in external RAM and in project.otcc beside project.mt.
// The on-disk project struct is left at its stock size: growing it has frozen
// the original Tracker when opening sample playback, the sample editor, and
// song mode.

constexpr int kMidiCcSlotCount = 26;
constexpr int kMidiCcRecordCount = 6;
constexpr int kMidiCcExtCount = kMidiCcSlotCount - kMidiCcRecordCount;
constexpr int kMidiCcValueCount = 131;
constexpr int kMidiCcPageCount = 4;

// "OTCC", version, 16*20 payload, checksum.
constexpr int kMidiCcSideSize = 4 + 1 + 16 * kMidiCcExtCount + 1;

enum class MidiCcLegend : uint8_t
{
    Drum,
    AStation,
    Tetra
};

void resetMidiCcExt();
uint8_t defaultMidiCcValue(int slot);
uint8_t midiCcSlotValue(int instrumentIndex, int slot);
void setMidiCcSlotValue(int instrumentIndex, int slot, uint8_t value);
void loadMidiCcExtBeside(const char* projectFilePath);
bool saveMidiCcExtBeside(const char* projectFilePath);

MidiCcLegend midiCcLegendForSlot(int slot);
const char* midiCcSlotLetter(int slot);
const char* midiCcColumnTitle(int slot);
const char* const* midiCcValueLabels(MidiCcLegend legend);

// Page 0 is Velocity, Offset, CC A..F. Later pages are eight CC columns,
// except the last page which is CC W..Z.
int midiCcPageColumns(int page);
int midiCcColumnSlot(int page, int column);

#endif
