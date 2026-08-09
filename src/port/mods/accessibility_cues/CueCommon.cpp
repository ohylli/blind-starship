#include "CueCommon.h"

#include <math.h>

#include "port/CGameCompat.h"

void CueCommon_RegisterCVars() {
    CVarRegisterInteger(kAudioCuesEnabledCVar, 1);
    CVarRegisterInteger(kCuePitchForHeightCVar, 1);
    CVarRegisterFloat(kCuePitchScaleCVar, kCuePitchScaleDefault);
    CVarRegisterFloat(kCuePitchRangeOctavesCVar, kCuePitchRangeOctavesDefault);
}

bool CueCommon_IsEnabled() {
    return CVarGetInteger(kAudioCuesEnabledCVar, 1) == 1;
}

// Shared Y->pitch mapping for the HRTF cues: higher source = higher pitch. Two knobs,
// both live via CVars under Developer -> Blind Starship (see
// docs/accessibility-cues-tuning.md): the sensitivity divisor (world height per
// octave) and the max pitch deviation (octaves clamped at the height extremes). The
// whole effect is gated by the gAccessibilityCuePitchForHeight toggle; when off the
// cue keeps native pitch and height is conveyed only by the HRTF elevation.
float CueCommon_ComputeFreqModFromY(float y) {
    if (CVarGetInteger(kCuePitchForHeightCVar, 1) != 1) {
        return 1.0f; // effect off: native pitch, height not conveyed
    }
    // The guards below are written as !(x >= lo) rather than (x < lo) on purpose: every
    // comparison against NaN is false, so the natural spelling would wave a NaN straight
    // through. These CVars are reachable from the console and a hand-edited config, and a
    // NaN here would ride out as a NaN playback rate, which used to be an out-of-bounds
    // read on the audio thread. Cue3D_SetPitch validates too — this just keeps the bad
    // value from ever being built.
    f32 scale = CVarGetFloat(kCuePitchScaleCVar, kCuePitchScaleDefault);
    if (!(scale >= 1.0f)) {
        scale = kCuePitchScaleDefault; // divide-by-tiny, negative, or NaN
    }
    f32 range = CVarGetFloat(kCuePitchRangeOctavesCVar, kCuePitchRangeOctavesDefault);
    if (!(range >= 0.0f)) {
        range = kCuePitchRangeOctavesDefault; // negative or NaN
    }
    f32 octaves = y / scale;
    if (octaves > range) {
        octaves = range;
    } else if (octaves < -range) {
        octaves = -range;
    } else if (!(octaves == octaves)) {
        octaves = 0.0f; // NaN y: neither clamp fires, so pin to native pitch
    }
    return powf(2.0f, octaves);
}

// Object_ClampSfxSource keeps the vector inside the SF64 engine's documented ±5000/±2000
// box — kept on the HRTF path so the by-ear-verified distance/pitch tuning is unchanged;
// whether the 3D backend still wants the clamp is a tuning follow-up. On top of the HRTF
// we drive pitch from the clamped Y: the generic HRTF only renders strong elevation when
// the target is nearly on the aim line (the vertical angle is tiny for most of the
// approach), so the raw-Y pitch supplies a distance-independent "above/below you" signal
// throughout.
void CueCommon_ComputeCueTarget(float dx, float dy, float dz, float outSrc[3], float* outFreq) {
    outSrc[0] = dx;
    outSrc[1] = dy;
    outSrc[2] = dz;
    Object_ClampSfxSource(outSrc);
    *outFreq = CueCommon_ComputeFreqModFromY(outSrc[1]);
}

// The divisors upstream of the normalized signals are engine-owned (pathWidth/pathHeight)
// or guarded CVars, but a zero divisor's ±inf still lands on a sane extreme here instead
// of riding into the pan/pitch math.
float CueCommon_ClampUnit(float v) {
    if (v > 1.0f) {
        return 1.0f;
    }
    if (v < -1.0f) {
        return -1.0f;
    }
    return (v == v) ? v : 0.0f;
}
