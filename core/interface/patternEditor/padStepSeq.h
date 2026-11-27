#ifndef PAD_STEP_SEQ_H_
#define PAD_STEP_SEQ_H_

#include <stdint.h>

// Pad step sequencer for the original Tracker and Tracker+.
// Tracker Mini has no pad grid; the calls below are empty there.
//
// The grid reads and writes the pattern the pattern editor is showing.
// There is no second pattern. The rightmost four columns are the
// lane, track, euclid, and edit controls. See padStepSeq.cpp.

#ifndef TRACKER_MINI

bool padStepSeqIsOn();
void padStepSeqToggle();
void padStepSeqPaint();
// Returns true when this mode consumed the pad.
bool padStepSeqOnPad(uint8_t pad, uint8_t state);
// Returns true when this mode consumed the encoder.
bool padStepSeqOnEncoder(int16_t value);
void padStepSeqOnFrame(bool transportRunning, int playhead);
void padStepSeqOnPlay();

#else

inline bool padStepSeqIsOn()
{
    return false;
}
inline void padStepSeqToggle() {}
inline void padStepSeqPaint() {}
inline bool padStepSeqOnPad(uint8_t, uint8_t)
{
    return false;
}
inline bool padStepSeqOnEncoder(int16_t)
{
    return false;
}
inline void padStepSeqOnFrame(bool, int) {}
inline void padStepSeqOnPlay() {}

#endif

#endif
