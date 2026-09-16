#include "CueCommon.h"

#include <math.h>

#include "port/CGameCompat.h"
#include "port/accessibility/Cue.h"

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
    const f32 scale = CueCommon_ReadFloat(kCuePitchScaleCVar, kCuePitchScaleDefault, 1.0f); // a divisor
    const f32 range = CueCommon_ReadFloat(kCuePitchRangeOctavesCVar, kCuePitchRangeOctavesDefault, 0.0f);
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

// See the header for the two constraints (lead-in vs the backend's restart fade, buffer
// length vs the fastest interval). With harmonicMix == 0 this reproduces the original
// aim-click math bit for bit.
std::vector<float> CueCommon_GenerateDampedTone(int sampleRate, float leadInSec, float toneSec, float toneHz,
                                                float harmonicMix, float decayPerSec, float amplitude,
                                                float tailSec) {
    constexpr f32 kTwoPi = 6.2831853f;
    int lead = (int) (leadInSec * (f32) sampleRate);
    int frames = (int) (toneSec * (f32) sampleRate);
    if (frames < 2) {
        frames = 2;
    }
    int tail = (int) (tailSec * (f32) sampleRate);
    std::vector<float> pcm((size_t) (lead + frames), 0.0f);
    for (int i = 0; i < frames; i++) {
        f32 t = (f32) i / (f32) sampleRate;
        f32 env = expf(-decayPerSec * t);
        int remaining = frames - 1 - i;
        if (remaining < tail) {
            env *= 0.5f * (1.0f - cosf(0.5f * kTwoPi * (f32) remaining / (f32) tail));
        }
        f32 wave = sinf(kTwoPi * toneHz * t) + harmonicMix * sinf(kTwoPi * 2.0f * toneHz * t);
        pcm[(size_t) (lead + i)] = amplitude * env * wave / (1.0f + harmonicMix);
    }
    return pcm;
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

// See the header for why the guards are spelled !(v >= lo).
float CueCommon_ReadFloat(const char* cvar, float def, float lo, float hi) {
    float v = CVarGetFloat(cvar, def);
    if (!(v >= lo)) {
        v = def; // below the floor, or NaN
    }
    return (v > hi) ? hi : v;
}

float CueCommon_ReadPositiveFloat(const char* cvar, float def, float hi) {
    float v = CVarGetFloat(cvar, def);
    if (!(v > 0.0f)) {
        v = def; // zero, negative, or NaN
    }
    return (v > hi) ? hi : v;
}

float CueCommon_UnityRadius() {
    const float radius = Cue3D_GetUnityGainDistance();
    return (radius > 0.0f) ? radius : 100.0f; // any positive radius pans (and centers) the same
}

void CueCommon_PlaceOnPanArc(CueTarget& target, float pan) {
    const float radius = CueCommon_UnityRadius();
    pan = CueCommon_ClampUnit(pan);
    target.x = radius * pan;
    target.y = 0.0f;
    target.z = radius * sqrtf(1.0f - pan * pan);
}
