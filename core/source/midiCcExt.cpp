#include "midiCcExt.h"

#include "fileTransfer.h"
#include "mtStructs.h"

#include <cr_section_macros.h>
#include <stdio.h>
#include <string.h>

// Not in strMtProject: that object sits in internal RAM, and the project file
// image must stay the stock size (see mtFileManagerOldVersionRecovery.cpp).
__NOINIT(EXTERNAL_RAM)
static uint8_t gMidiCcExt[kMidiInstrumentsCount][kMidiCcExtCount];

namespace
{
struct MidiCcExtBoot
{
    MidiCcExtBoot() { resetMidiCcExt(); }
} midiCcExtBoot;
}

static const uint8_t kDefaultCc[kMidiCcSlotCount] = {
    1,  5,  7,  11, 64, 128, 74, 71, 73, 72, 91, 93, 10,
    2,  16, 17, 18, 19, 20,  21, 22, 23, 24, 25, 26, 27
};

static const char* const kSlotLetter[kMidiCcSlotCount] = {
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
    "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"
};

static const char* const kColumnTitle[kMidiCcSlotCount] = {
    "CC A", "CC B", "CC C", "CC D", "CC E", "CC F", "CC G", "CC H", "CC I",
    "CC J", "CC K", "CC L", "CC M", "CC N", "CC O", "CC P", "CC Q", "CC R",
    "AS S", "AS T", "AS U", "AS V", "TE W", "TE X", "TE Y", "TE Z"
};

// Novation DrumStation manual p.42. Index 0 is CC 20. 808 is 20..75, 909 is 76..119.
static const char* const kDrumRole[100] = {
    "BDcut", "BDpan", "BDdst", "BDtun", "BDton", "BDdcy", "SDcut", "SDpan", "SDdst", "SDtun",
    "SDton", "SDsnp", "LTcut", "LTpan", "LTdst", "LTtun", "LTdcy", "MTcut", "MTpan", "MTdst",
    "MTtun", "MTdcy", "HTcut", "HTpan", "HTdst", "HTtun", "HTdcy", "RSpan", "RStun", "CPpan",
    "CPtun", "CBpan", "CBdst", "CBtun", "CHpan", "CHtun", "CHdcy", "OHpan", "OHtun", "OHdcy",
    "CYpan", "CYton", "CYdcy", "LCpan", "LCdst", "LCtun", "MCpan", "MCdst", "MCtun", "HCpan",
    "HCdst", "HCtun", "MApan", "MAtun", "CLpan", "CLtun", "BDtun", "BDatk", "BDdcy", "SDtun",
    "SDton", "SDsnp", "LTcut", "LTpan", "LTdst", "LTtun", "LTdcy", "MTcut", "MTpan", "MTdst",
    "MTtun", "MTdcy", "HTcut", "HTpan", "HTdst", "HTtun", "HTdcy", "RSpan", "RStun", "CPpan",
    "CPtun", "CHdst", "CHtun", "CHdcy", "OHtun", "BDcut", "BDpan", "BDdst", "SDcut", "SDpan",
    "SDdst", "CHpan", "OHpan", "OHdcy", "CYpan", "CYtun", "CYdcy", "RDpan", "RDtun", "RDdcy"
};

static const char* drumGmRole(int value)
{
    switch (value)
    {
        case 0: return "Bank";
        case 1: return "ModWh";
        case 2: return "Brth";
        case 4: return "Foot";
        case 5: return "Glide";
        case 6: return "Data";
        case 7: return "ChVol";
        case 8: return "Balnc";
        case 10: return "Pan";
        case 11: return "Exp";
        case 120: return "SOff";
        case 121: return "Rst";
        case 122: return "Locl";
        case 123: return "NOff";
        default: return nullptr;
    }
}

static const char* tetraRole(int value)
{
    switch (value)
    {
        case 14: return "TMpo";
        case 15: return "TMsig";
        case 20: return "O1frq";
        case 21: return "O1fin";
        case 22: return "O1shp";
        case 23: return "O1gld";
        case 24: return "O2frq";
        case 25: return "O2fin";
        case 26: return "O2shp";
        case 27: return "O2gld";
        case 28: return "OSmix";
        case 29: return "NOlvl";
        case 30: return "S1lvl";
        case 31: return "S2lvl";
        case 52: return "FBvol";
        case 53: return "FBgan";
        case 75: return "VCsus";
        case 76: return "VCrel";
        case 77: return "E3sus";
        case 78: return "E3rel";
        case 85: return "E3dst";
        case 86: return "E3amt";
        case 87: return "E3vel";
        case 88: return "E3dly";
        case 89: return "E3atk";
        case 90: return "E3dcy";
        case 102: return "FLfrq";
        case 103: return "FLres";
        case 104: return "FLkey";
        case 105: return "FLaud";
        case 106: return "FLamt";
        case 107: return "FLvel";
        case 108: return "FLdly";
        case 109: return "FLatk";
        case 110: return "FLdcy";
        case 111: return "FLsus";
        case 112: return "FLrel";
        case 113: return "VClvl";
        case 114: return "VCpan";
        case 115: return "VCamt";
        case 116: return "VCvel";
        case 117: return "VCdly";
        case 118: return "VCatk";
        case 119: return "VCdcy";
        default: return nullptr;
    }
}

static const char* aStationRole(int value)
{
    switch (value)
    {
        case 1: return "MDwhl";
        case 2: return "BRth";
        case 3: return "ARpat";
        case 5: return "PRtim";
        case 6: return "NRval";
        case 7: return "VOlum";
        case 8: return "PRgld";
        case 9: return "ARrat";
        case 10: return "PNpos";
        case 12: return "PNrat";
        case 13: return "PNsyn";
        case 14: return "VOwid";
        case 15: return "VOsib";
        case 16: return "MDdst";
        case 17: return "DScmp";
        case 18: return "MDdly";
        case 19: return "DLtim";
        case 20: return "DLsyn";
        case 21: return "DLfbk";
        case 22: return "DLwid";
        case 23: return "DLrat";
        case 24: return "MDrev";
        case 25: return "RVdcy";
        case 26: return "MDcho";
        case 27: return "CHrat";
        case 28: return "CHsyn";
        case 29: return "CHfbk";
        case 30: return "CHdep";
        case 31: return "CHctr";
        case 32: return "BAnk";
        case 33: return "EQlvl";
        case 34: return "EQfrq";
        case 35: return "EQrat";
        case 36: return "EQsyn";
        case 37: return "EQdep";
        case 40: return "O1sem";
        case 41: return "O1cnt";
        case 42: return "O1bnd";
        case 43: return "O1lfo";
        case 44: return "O1env";
        case 45: return "O1pw";
        case 46: return "O1lpw";
        case 47: return "O1epw";
        case 48: return "O2sem";
        case 49: return "O2cnt";
        case 50: return "O2bnd";
        case 51: return "O2lfo";
        case 52: return "O2env";
        case 53: return "O2pw";
        case 54: return "O2lpw";
        case 55: return "O2epw";
        case 56: return "O3sem";
        case 57: return "O3cnt";
        case 58: return "O3bnd";
        case 59: return "O3lfo";
        case 60: return "O3env";
        case 61: return "O3pw";
        case 62: return "O3lpw";
        case 63: return "O3epw";
        case 64: return "SUst";
        case 65: return "ENmod";
        case 67: return "UNtyp";
        case 68: return "UNdet";
        case 69: return "OSran";
        case 70: return "OSwav";
        case 71: return "OSoct";
        case 72: return "O1lvl";
        case 73: return "O2lvl";
        case 74: return "O3lvl";
        case 75: return "NOlvl";
        case 76: return "RGlvl";
        case 77: return "EXin";
        case 78: return "LFwav";
        case 79: return "LFkey";
        case 80: return "L1spd";
        case 81: return "L1syn";
        case 82: return "L1dly";
        case 83: return "L2spd";
        case 84: return "L2syn";
        case 85: return "L2dly";
        case 87: return "ARsyn";
        case 88: return "ARgat";
        case 89: return "ARctl";
        case 91: return "RVlvl";
        case 92: return "DLlvl";
        case 93: return "CHlvl";
        case 94: return "PNdep";
        case 95: return "VObal";
        case 98: return "NRnum";
        case 102: return "FLlfo";
        case 103: return "FLqnm";
        case 104: return "FLod";
        case 105: return "FLfrq";
        case 106: return "FLres";
        case 107: return "FLmen";
        case 108: return "AMatk";
        case 109: return "AMdcy";
        case 110: return "AMsus";
        case 111: return "AMrel";
        case 112: return "AMvel";
        case 114: return "MEatk";
        case 115: return "MEdcy";
        case 116: return "MEsus";
        case 117: return "MErel";
        case 118: return "MEvel";
        case 119: return "OUtlv";
        case 120: return "ANoff";
        case 121: return "RSt";
        case 122: return "LOcl";
        case 123:
        case 124:
        case 125:
        case 126:
        case 127: return "Noff";
        default: return nullptr;
    }
}

static const char* roleFor(MidiCcLegend legend, int value)
{
    if (legend == MidiCcLegend::Tetra)
        return tetraRole(value);
    if (legend == MidiCcLegend::AStation)
        return aStationRole(value);
    if (value >= 20 && value <= 119)
        return kDrumRole[value - 20];
    return drumGmRole(value);
}

static char gLabelText[3][kMidiCcValueCount][16];
static const char* gLabelPtr[3][kMidiCcValueCount];
static bool gLabelsReady = false;

static void ensureLabels()
{
    if (gLabelsReady)
        return;

    const MidiCcLegend legends[3] = {
        MidiCcLegend::Drum, MidiCcLegend::AStation, MidiCcLegend::Tetra
    };
    for (int legendIndex = 0; legendIndex < 3; legendIndex++)
    {
        for (int value = 0; value < kMidiCcValueCount; value++)
        {
            char* out = gLabelText[legendIndex][value];
            if (value == 128)
                snprintf(out, 16, "Prg Chg");
            else if (value == 129)
                snprintf(out, 16, "Chan AT");
            else if (value == 130)
                snprintf(out, 16, "Poly AT");
            else
            {
                const char* role = roleFor(legends[legendIndex], value);
                if (role)
                    snprintf(out, 16, "%d %s", value, role);
                else
                    snprintf(out, 16, "%d", value);
            }
            gLabelPtr[legendIndex][value] = out;
        }
    }
    gLabelsReady = true;
}

static bool sidePath(char* out, size_t cap, const char* projectFilePath)
{
    if (!projectFilePath || !out || cap < 13)
        return false;
    const size_t len = strlen(projectFilePath);
    const char* tail = "project.mt";
    const size_t tailLen = 10;
    if (len < tailLen || strcmp(projectFilePath + len - tailLen, tail) != 0)
        return false;
    if (len - tailLen + 12 + 1 > cap)
        return false;
    memcpy(out, projectFilePath, len - tailLen);
    memcpy(out + len - tailLen, "project.otcc", 13);
    return true;
}

static uint8_t payloadSum(const uint8_t* payload)
{
    uint8_t sum = 0;
    for (int i = 0; i < kMidiInstrumentsCount * kMidiCcExtCount; i++)
        sum = uint8_t(sum + payload[i]);
    return sum;
}

uint8_t defaultMidiCcValue(int slot)
{
    if (slot < 0 || slot >= kMidiCcSlotCount)
        return 0;
    return kDefaultCc[slot];
}

void resetMidiCcExt()
{
    for (int instrument = 0; instrument < kMidiInstrumentsCount; instrument++)
    {
        for (int slot = 0; slot < kMidiCcExtCount; slot++)
            gMidiCcExt[instrument][slot] = kDefaultCc[kMidiCcRecordCount + slot];
    }
}

uint8_t midiCcSlotValue(int instrumentIndex, int slot)
{
    if (instrumentIndex < 0 || instrumentIndex >= kMidiInstrumentsCount || slot < 0 ||
        slot >= kMidiCcSlotCount)
        return 0;
    if (slot < kMidiCcRecordCount)
        return mtProject.values.midiInstrument[instrumentIndex].ccOut[slot];
    return gMidiCcExt[instrumentIndex][slot - kMidiCcRecordCount];
}

void setMidiCcSlotValue(int instrumentIndex, int slot, uint8_t value)
{
    if (instrumentIndex < 0 || instrumentIndex >= kMidiInstrumentsCount || slot < 0 ||
        slot >= kMidiCcSlotCount)
        return;
    if (slot < kMidiCcRecordCount)
        mtProject.values.midiInstrument[instrumentIndex].ccOut[slot] = value;
    else
        gMidiCcExt[instrumentIndex][slot - kMidiCcRecordCount] = value;
}

void loadMidiCcExtBeside(const char* projectFilePath)
{
    resetMidiCcExt();

    char path[255];
    if (!sidePath(path, sizeof(path), projectFilePath))
        return;

    uint8_t file[kMidiCcSideSize];
    uint8_t status =
      fileTransfer.loadFileToMemory(path, file, sizeof(file), fileWholeOnce);
    if (status != fileTransferEnd)
        return;
    if (file[0] != 'O' || file[1] != 'T' || file[2] != 'C' || file[3] != 'C' || file[4] != 1)
        return;
    if (payloadSum(file + 5) != file[5 + kMidiInstrumentsCount * kMidiCcExtCount])
        return;

    for (int instrument = 0; instrument < kMidiInstrumentsCount; instrument++)
    {
        for (int slot = 0; slot < kMidiCcExtCount; slot++)
        {
            const uint8_t value = file[5 + instrument * kMidiCcExtCount + slot];
            gMidiCcExt[instrument][slot] =
              value > 130 ? kDefaultCc[kMidiCcRecordCount + slot] : value;
        }
    }
}

bool saveMidiCcExtBeside(const char* projectFilePath)
{
    char path[255];
    if (!sidePath(path, sizeof(path), projectFilePath))
        return false;

    uint8_t file[kMidiCcSideSize];
    file[0] = 'O';
    file[1] = 'T';
    file[2] = 'C';
    file[3] = 'C';
    file[4] = 1;
    for (int instrument = 0; instrument < kMidiInstrumentsCount; instrument++)
        memcpy(file + 5 + instrument * kMidiCcExtCount,
               gMidiCcExt[instrument],
               kMidiCcExtCount);
    file[5 + kMidiInstrumentsCount * kMidiCcExtCount] = payloadSum(file + 5);

    return fileTransfer.saveMemoryToFile(file, path, sizeof(file)) == fileTransferEnd;
}

MidiCcLegend midiCcLegendForSlot(int slot)
{
    if (slot >= 22)
        return MidiCcLegend::Tetra;
    if (slot >= 18)
        return MidiCcLegend::AStation;
    return MidiCcLegend::Drum;
}

const char* midiCcSlotLetter(int slot)
{
    if (slot < 0 || slot >= kMidiCcSlotCount)
        return "";
    return kSlotLetter[slot];
}

const char* midiCcColumnTitle(int slot)
{
    if (slot < 0 || slot >= kMidiCcSlotCount)
        return "";
    return kColumnTitle[slot];
}

const char* const* midiCcValueLabels(MidiCcLegend legend)
{
    ensureLabels();
    if (legend == MidiCcLegend::Tetra)
        return gLabelPtr[2];
    if (legend == MidiCcLegend::AStation)
        return gLabelPtr[1];
    return gLabelPtr[0];
}

int midiCcPageColumns(int page)
{
    if (page <= 0 || page == 1 || page == 2)
        return 8;
    return 4;
}

int midiCcColumnSlot(int page, int column)
{
    if (column < 0 || column >= midiCcPageColumns(page))
        return -100;
    if (page <= 0)
    {
        if (column == 0)
            return -2;
        if (column == 1)
            return -1;
        return column - 2;
    }
    if (page == 1)
        return 6 + column;
    if (page == 2)
        return 14 + column;
    return 22 + column;
}
