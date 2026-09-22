#include "Cue3D.h"

#ifdef HAVE_STEAM_AUDIO

#include <phonon.h>
#include "miniaudio.h"
#include "signalsmith-stretch.h"
#include <spdlog/spdlog.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <vector>

// Steam Audio backend for the Cue3D seam (Cue3D.h). miniaudio owns the OS playback
// device and pulls audio via a callback; each Cue3D source carries its own decoded
// mono PCM and its own Steam Audio binaural effect (the effect holds per-source
// HRTF-interpolation state). The callback mixes all active sources — Steam Audio
// spatializes but does not mix, so summing is our job — and bridges the callback's
// variable frame count to Steam Audio's fixed processing block with a one-block
// carry buffer.
//
// This file is the ONLY place that knows about Steam Audio's -Z-forward axis: the
// seam takes the game's +z-ahead convention and ProduceBlock negates Z when it
// builds the direction vector. An alternate backend would do its own mapping.
//
// Distance attenuation is the backend's job (per the Cue3D.h contract): ProduceBlock
// feeds the source's distance to Steam Audio's inverse-distance model and folds the
// resulting gain into playback, so a far source renders quieter than a near one. The
// caller pushes raw positions and never models falloff itself.
//
// Front/back exaggeration is likewise backend policy: ProduceBlock muffles (low-pass),
// slightly dips, and amplitude-pulses rear-hemisphere sources (the g_rear* knobs, set
// through Cue3D_SetRearEffect) because the generic HRTF's own front/back rendering is
// too weak to read in play.

namespace {

constexpr int kSampleRate = 48000; // requested device rate (a hint); the real rate is read back at Init
constexpr int kFrameSize = 1024;   // Steam Audio fixed processing block
constexpr float kPi = 3.14159265f;

// Output trim on the summed mix. Steam Audio spatializes but does not mix, so
// ProduceBlock sums up to kMaxSources stereo streams by hand; without headroom the
// summed peaks can exceed ±1.0 and hard-clip exactly when several cues overlap near
// the aim line. The trim pulls the common case back under unity; the safety clip in
// ProduceBlock is the last-resort backstop. Tuned by ear (0.5-0.7 range).
constexpr float kOutputTrim = 0.6f;

// Distance attenuation: cue loudness falls off with distance via Steam Audio's
// inverse-distance model (1/d past kMinDistanceMeters). iplDistanceAttenuationCalculate
// interprets distance in meters, but SF64 world units are huge (rings spawn ~3000
// units, clamp box ±5000), so we scale world units -> meters first. At scale 1000:
// 3000u -> 3m -> gain ~0.33, 5000u -> 5m -> 0.2. Two knobs, tuned by ear:
//   kWorldUnitsPerMeter — overall steepness. Larger = louder/flatter (a given
//     distance maps to fewer "meters", so less falloff); smaller = quieter/steeper.
//   kMinDistanceMeters  — near plateau. A source closer than this gets no
//     attenuation (gain capped at 1.0), so the cue stops getting louder once you're
//     basically on top of it. Larger = wider full-volume bubble.
constexpr float kWorldUnitsPerMeter = 1000.0f;
constexpr float kMinDistanceMeters = 1.0f;

// Rear effect: exaggerated front/back discrimination. The generic HRTF's own
// front/back cue is weak (docs/accessibility-hrtf-cues.md), so a source in the rear
// hemisphere is additionally muffled (one-pole low-pass), slightly dipped in gain,
// and amplitude-pulsed (tremolo) — the first two an exaggeration of the natural head
// shadow, the tremolo a purely conventional but maximally salient "behind you" marker.
// All blend in smoothly with the rear angle and leave the entire front hemisphere
// untouched. Four knobs, live-tunable so they can be compared by ear without a
// rebuild: game thread writes via Cue3D_SetRearEffect (driven by the CVar sliders
// under F1 -> Developer -> Blind Starship — see docs/accessibility-cues-tuning.md),
// the callback reads once per block. Defaults are the seam's CUE3D_REAR_*_DEFAULT.
//   cutoffHz     — low-pass cutoff when the source is dead behind. Lower = duller.
//   gainDip      — fraction of gain removed when dead behind. Kept mild on purpose:
//     volume already encodes distance, so a deep dip would read as "far away", not
//     "behind". 0 = filter-only.
//   tremoloDepth — pulse strength when dead behind. 0 = no tremolo.
//   tremoloHz    — pulse rate.
std::atomic<float> g_rearCutoffHz{ CUE3D_REAR_CUTOFF_HZ_DEFAULT };
std::atomic<float> g_rearGainDip{ CUE3D_REAR_GAIN_DIP_DEFAULT };
std::atomic<float> g_rearTremoloDepth{ CUE3D_REAR_TREMOLO_DEPTH_DEFAULT };
std::atomic<float> g_rearTremoloHz{ CUE3D_REAR_TREMOLO_HZ_DEFAULT };

// How Cue3D_SetPitch is realized (Cue3D_SetPitchStyle) — global across sources like the
// rear knobs. RESAMPLE (rate-change) is the default so a caller that never touches the
// style gets the seam's long-documented behavior; the cue settings layer pushes the
// CVar-selected style every tick.
std::atomic<int> g_pitchStyle{ CUE3D_PITCH_RESAMPLE };

// Hard bounds for the values the game thread pushes at the audio thread. The CVars behind
// them are reachable from the console and a hand-edited config, not just the sliders, so
// the seam — not the UI — is where the DSP's preconditions get enforced.
//
// Rear knobs: a cutoff <= 0 would put the one-pole low-pass outside the unit circle (see
// lpAlpha in ProduceBlock) and diverge to NaN within a single block; a dip or depth above
// 1 inverts and amplifies gain instead of reducing it; a tremolo rate near the sample rate
// breaks the single-subtract phase wrap. tremoloHz's ceiling sits well above the slider's
// 16 Hz so by-ear tuning stays free.
//
// Pitch rate: the playback cursor advances by it every sample, so a non-finite or negative
// rate walks the cursor off pcm[] (an out-of-bounds READ, not just bad audio). The bounds
// are five octaves either way — far wider than the Y->pitch mapping can produce — so they
// only ever catch a corrupt value, never a tuned one.
constexpr float kRearCutoffMinHz = 20.0f;
constexpr float kRearCutoffMaxHz = 20000.0f;
constexpr float kRearTremoloMaxHz = 50.0f;
constexpr float kMinPitchRate = 0.03125f; // -5 octaves
constexpr float kMaxPitchRate = 32.0f;    // +5 octaves

// Interval (pulse cadence) bounds, seconds. 0 is "off" (seamless loop); the ceiling
// keeps intervalCounter's per-block frame math well inside a double's exact range.
constexpr float kMaxIntervalSec = 30.0f;

// Per-source low-pass bounds, Hz. 0 is the documented DISABLE value (see Cue3D_SetLowPass);
// a positive cutoff is clamped to audible range like the rear muffle's cutoff.
constexpr float kLowPassMinHz = 20.0f;
constexpr float kLowPassMaxHz = 20000.0f;

// Start/stop click guard: a per-source gain ramp toward 1 (rendering) or 0 (stopped/ended),
// one-pole with a ~5 ms time constant, multiplied into the mono fill. A source keeps
// rendering while its ramp is above kRampSilenceGate so Stop and one-shot EOF fade out
// instead of clicking; below the gate a non-rendering source is skipped entirely.
constexpr float kRampTimeConstantSec = 0.005f;
constexpr float kRampSilenceGate = 1.0e-4f;

// Pitch-step smoothing: the game thread pushes a new rate at most once per game frame
// (~30 Hz), so a vertically moving target would step pitch in audible jumps. The fill
// loop slews the applied rate toward the target per sample with this time constant —
// long enough to turn ~33 ms steps into a glide, short enough that the elevation
// signal barely lags the target.
constexpr float kRateSmoothTimeConstantSec = 0.020f;

// Tonality limit handed to the spectral shifter (SHIFT style) each block: harmonics below
// this stay phase-coherent when transposed, which is what keeps tonal cues sounding like
// themselves. The library's docs recommend sampleRate/4-ish for musical material; 8 kHz is
// that shape at the 48 kHz device rate and covers every cue sound's harmonic content.
constexpr float kShiftTonalityLimitHz = 8000.0f;

constexpr float kInvSqrt2 = 0.70710678f; // equal-power center gain for CUE3D_MODE_DIRECT

// Non-finite input falls back to the caller's default rather than a bound: NaN has no
// nearest edge, and a value that silently reverts to its documented default is easier to
// recognize by ear than one that jumps to an extreme.
float SanitizeParam(float value, float lo, float hi, float fallback) {
    if (!std::isfinite(value)) {
        return fallback;
    }
    return value < lo ? lo : (value > hi ? hi : value);
}

// Fixed pool of sources. Slots are claimed lazily, one per Cue voice on its first
// sound (plus the test bench's two raw sources), and a claim that finds no free slot
// latches that cue silent for the session (Cue::EnsureVoiceLoaded) — so the pool must
// stay comfortably above the registered voice count, not just above the number of
// cues sounding at once. Today's cues register 13 voices; 32 leaves room for the
// next few families while the per-block scan over idle slots stays trivial. A fixed
// array means slot addresses never move, so a Cue3DSource* handed to a caller stays
// valid for the whole Cue3D lifetime and the lock-free publish below is safe.
constexpr int kMaxSources = 32;

} // namespace

// Real definition of the seam's opaque handle.
struct Cue3DSource {
    // Audio-thread-only: the callback is the sole accessor once the source is
    // published (inUse == true). effect/pcm/frameCount/loop are immutable after
    // publish and ride the inUse release/acquire pair for visibility, so they need
    // no atomics. cursor is mutated every block, but ONLY by the callback, so it is
    // likewise atomic-free — do NOT write it from the game thread without adding
    // synchronization.
    IPLBinauralEffect effect = nullptr; // one per source — holds interpolation state
    std::vector<float> pcm;             // decoded mono at the device rate (g.sampleRate), owned
    int frameCount = 0;
    double cursor = 0.0;     // fractional playback position, frames (audio-thread-only)
    float lpState = 0.0f;    // rear-muffle one-pole low-pass state (audio-thread-only, like cursor)
    float lpState2 = 0.0f;   // per-source low-pass one-pole state (audio-thread-only, like lpState)
    float tremPhase = 0.0f;  // rear-tremolo LFO phase, radians (audio-thread-only, like cursor)
    float rampGain = 0.0f;   // start/stop click-guard ramp, 0..1 (audio-thread-only, like cursor)
    float smoothRate = 1.0f; // slewed pitch rate the cursor actually advances by (audio-thread-only, like cursor)

    // Spectral pitch shifter for CUE3D_PITCH_SHIFT. Allocated and configured (which is
    // what allocates) in CreateSource on the MAIN thread, freed in Cue3D_Shutdown; the
    // audio thread only calls its allocation-free process()/reset()/setTransposeFactor().
    // Visibility rides the inUse release/acquire pair like effect/pcm.
    signalsmith::stretch::SignalsmithStretch<float>* stretch = nullptr;
    int shiftLatencyFrames = 0;  // shifter input+output latency, frames (immutable after publish)
    int shiftTail = 0;           // shifter tail left to drain after stop/EOF, frames (audio-thread-only)
    bool shiftWasActive = false; // previous block took the shift path (audio-thread-only) — style-flip reset detector
    double intervalCounter =
        0.0;              // wall-clock frames since the last (re)start, for interval cadence (audio-thread-only)
    uint32_t lastGen = 0; // last startGen the callback adopted (audio-thread-only, like cursor)
    bool ended = false;   // one-shot reached EOF; render gate, cleared on gen bump (audio-thread-only, like cursor)
    bool loop = false;

    // Cross-thread: game thread writes, audio callback reads. Read once per block.
    std::atomic<float> x{ 0.0f }; // game convention: +x right
    std::atomic<float> y{ 0.0f }; //                  +y up
    std::atomic<float> z{ 1.0f }; //                  +z ahead
    std::atomic<float> gain{ 1.0f };
    std::atomic<float> rate{ 1.0f };          // playback-rate multiplier (pitch); 1.0 = native
    std::atomic<float> intervalSec{ 0.0f };   // restart cadence, seconds; 0 = seamless loop (looping sources only)
    std::atomic<int> mode{ CUE3D_MODE_HRTF }; // Cue3DMode render selector (HRTF / PAN / DIRECT)
    std::atomic<float> lowPassHz{ 0.0f };     // per-source low-pass cutoff; 0 = off
    // Cue3DSourcePitchStyle: per-source pitch-realization override; GLOBAL follows g_pitchStyle.
    std::atomic<int> pitchStyleOverride{ CUE3D_SOURCE_PITCH_GLOBAL };

    // The stop lever, GAME-THREAD-WRITE-ONLY: Cue3D_Play sets it, Cue3D_Stop clears it,
    // and NOTHING else writes it. The callback only reads it. Split out from the old
    // dual-writer design (CUE3D-13) so a Play landing as a one-shot ends can never be
    // clobbered by the audio thread — one-shot EOF now sets the audio-thread-only
    // `ended` instead of touching this. Restart-from-zero is driven by startGen below.
    std::atomic<bool> playing{ false };

    // Bumped by Cue3D_Play (game thread). The callback restarts the source from sample 0
    // whenever this differs from the audio-thread-only `lastGen`, then adopts it. This is
    // how every Play — including one re-triggering a finished one-shot — rewinds without a
    // flag the two threads both write.
    std::atomic<uint32_t> startGen{ 0 };

    // Audio -> game advisory status, read by Cue3D_IsPlaying. Written by the callback each
    // block (`playing && !ended`) AND eagerly by Cue3D_Play (so a same-tick IsPlaying is
    // not stale-false) — TWO writers on purpose. Acceptable ONLY because it is advisory:
    // a lost update costs ~one audio block (~21 ms) of staleness in voice bookkeeping and
    // never gates playback. No render decision may read this.
    std::atomic<bool> audible{ false };

    // Publish gate: set LAST (release) in CreateSource, read FIRST (acquire) in the
    // callback, so a half-built source is never observed.
    std::atomic<bool> inUse{ false };
};

namespace {

struct SpatialState {
    bool active = false;

    IPLContext ctx = nullptr;
    IPLHRTF hrtf = nullptr;
    IPLAudioBuffer inBuf{};  // mono, kFrameSize — reused per source
    IPLAudioBuffer tmpOut{}; // stereo, kFrameSize — reused per source

    ma_device device{};

    // Actual device sample rate, read back from miniaudio after ma_device_init (the
    // requested rate is only a hint). Everything that bakes in a rate follows this —
    // the HRTF, the per-source binaural effects, the decoder target, and
    // Cue3D_GetSampleRate — so a device the OS opens at 44.1 kHz stays in tune rather
    // than playing ~8.8% slow. kSampleRate until Init reconciles it.
    int sampleRate = kSampleRate;

    // Planar stereo accumulator: sources are summed here before interleaving.
    float accL[kFrameSize] = { 0 };
    float accR[kFrameSize] = { 0 };

    // Scratch mono block for CUE3D_PITCH_SHIFT: the fill loop renders native-rate PCM
    // here and the spectral shifter produces the spatializer's mono input from it.
    // Reused per source, audio-thread-only, like inBuf.
    float shiftIn[kFrameSize] = { 0 };

    // Interleaved stereo carry buffer: one processed block, drained across however
    // many frames the device callback asks for. Audio-thread-only.
    float block[kFrameSize * 2] = { 0 };
    int blockAvail = 0; // frames remaining in `block`
    int blockPos = 0;   // read cursor (frames) into `block`
};

SpatialState g;
Cue3DSource g_sources[kMaxSources];

// Build a per-source source from already-decoded mono PCM. Main thread only:
// allocates, decodes, and creates the Steam Audio effect, then publishes the slot
// with a single release store so the callback never sees it half-built.
Cue3DSource* CreateSource(const float* monoPcm, int frames, bool loop) {
    if (!g.active || monoPcm == nullptr || frames <= 0) {
        return nullptr;
    }

    int slot = -1;
    for (int i = 0; i < kMaxSources; i++) {
        if (!g_sources[i].inUse.load(std::memory_order_acquire)) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        SPDLOG_WARN("Cue3D: no free source slot ({} in use); load ignored", kMaxSources);
        return nullptr;
    }
    Cue3DSource& s = g_sources[slot];

    // Match the effect to the negotiated device rate (set at Init). Sources are only
    // ever created after Init, so g.sampleRate is already reconciled here.
    IPLAudioSettings audioSettings{};
    audioSettings.samplingRate = g.sampleRate;
    audioSettings.frameSize = kFrameSize;
    IPLBinauralEffectSettings effectSettings{};
    effectSettings.hrtf = g.hrtf;
    if (iplBinauralEffectCreate(g.ctx, &audioSettings, &effectSettings, &s.effect) != IPL_STATUS_SUCCESS) {
        SPDLOG_ERROR("Cue3D: iplBinauralEffectCreate failed; source not created");
        return nullptr;
    }

    try {
        s.pcm.assign(monoPcm, monoPcm + frames);
        // Pitch shifter for CUE3D_PITCH_SHIFT. Allocated once per slot and reconfigured
        // on reuse (configure is where SignalsmithStretch allocates — done HERE, on the
        // main thread, so the audio thread's process() never does). presetDefault is the
        // library's full-quality setting: ~0.12 s analysis block, which is also where the
        // shift path's latency comes from (see shiftLatencyFrames / the drain logic).
        if (s.stretch == nullptr) {
            s.stretch = new signalsmith::stretch::SignalsmithStretch<float>();
        }
        s.stretch->presetDefault(1, (float) g.sampleRate);
        s.shiftLatencyFrames = s.stretch->inputLatency() + s.stretch->outputLatency();
    } catch (const std::exception& e) {
        // Never let a C++ allocation failure unwind through the extern "C" callers.
        SPDLOG_ERROR("Cue3D: source allocation failed ({}); source not created", e.what());
        delete s.stretch; // possibly half-configured; drop it entirely
        s.stretch = nullptr;
        iplBinauralEffectRelease(&s.effect); // undo the effect created just above
        return nullptr;
    }
    s.frameCount = frames;
    s.cursor = 0.0;
    s.lpState = 0.0f;
    s.lpState2 = 0.0f;
    s.tremPhase = 0.0f;
    s.rampGain = 0.0f;
    s.smoothRate = 1.0f;
    s.shiftTail = 0;
    s.shiftWasActive = false; // forces a shifter reset on the slot's first shifted block
    s.intervalCounter = 0.0;
    s.lastGen = 0;
    s.ended = false;
    s.loop = loop;
    s.x.store(0.0f, std::memory_order_relaxed);
    s.y.store(0.0f, std::memory_order_relaxed);
    s.z.store(1.0f, std::memory_order_relaxed); // default "ahead"
    s.gain.store(1.0f, std::memory_order_relaxed);
    s.rate.store(1.0f, std::memory_order_relaxed);        // native pitch until SetPitch
    s.intervalSec.store(0.0f, std::memory_order_relaxed); // seamless loop until SetInterval
    s.mode.store(CUE3D_MODE_HRTF, std::memory_order_relaxed);
    s.lowPassHz.store(0.0f, std::memory_order_relaxed); // no per-source muffle until SetLowPass
    s.pitchStyleOverride.store(CUE3D_SOURCE_PITCH_GLOBAL, std::memory_order_relaxed);
    s.playing.store(false, std::memory_order_relaxed); // silent until Cue3D_Play
    // lastGen (0 above) matches this fresh startGen, so the first block does not spuriously
    // "restart" — the first real restart comes from Cue3D_Play bumping startGen to 1.
    s.startGen.store(0, std::memory_order_relaxed);
    s.audible.store(false, std::memory_order_relaxed);

    // Publish. Everything above must be visible before inUse flips true.
    s.inUse.store(true, std::memory_order_release);
    return &s;
}

// Produce one kFrameSize block: mix every active source through its binaural
// effect at its current position, then interleave into the carry buffer.
void ProduceBlock() {
    std::memset(g.accL, 0, sizeof(g.accL));
    std::memset(g.accR, 0, sizeof(g.accR));

    float* mono = g.inBuf.data[0];
    const int pitchStyle = g_pitchStyle.load(std::memory_order_relaxed);

    for (int i = 0; i < kMaxSources; i++) {
        Cue3DSource& s = g_sources[i];
        if (!s.inUse.load(std::memory_order_acquire)) {
            continue;
        }
        // The stop lever (game-thread-write-only) and the restart generation (bumped by
        // Cue3D_Play). Read once per block, both with acquire so a freshly published slot
        // and a fresh Play are seen coherently.
        const bool playing = s.playing.load(std::memory_order_acquire);
        const uint32_t gen = s.startGen.load(std::memory_order_acquire);

        // Adopt a new Play: rewind to sample 0, clear the one-shot EOF latch, reset the
        // interval cadence, and ramp in from silence. This is the ONLY writer of these
        // audio-thread-only fields on a restart, so a Play that lands as a one-shot ends
        // can never be lost (CUE3D-13): the callback simply sees the new gen next block.
        if (gen != s.lastGen) {
            s.lastGen = gen;
            s.cursor = 0.0;
            s.ended = false;
            s.intervalCounter = 0.0;
            s.rampGain = 0.0f;
            // Snap the rate slew to the freshly set target: a restart ramps in from
            // silence anyway, and gliding out of the previous pitch would smear the
            // first pulse of a retargeted voice. The atomic only ever holds sanitized
            // values (SetPitch validates on store), so the snap needs no re-check.
            s.smoothRate = s.rate.load(std::memory_order_relaxed);
        }

        // Render while the source wants to sound; keep rendering a stopped or finished
        // source until its ramp has faded to silence (below), so Stop / one-shot EOF do
        // not click. `audible` is the advisory status handed back to Cue3D_IsPlaying.
        const bool shouldRender = playing && !s.ended;
        s.audible.store(shouldRender, std::memory_order_relaxed);
        // SHIFT style routes the fill through the spectral shifter below. A source keeps
        // rendering past stop/EOF while the shifter still holds signal (shiftTail, in
        // frames) — the skip gate honours it so a one-shot's last ~0.1 s is not cut off.
        // A style flip mid-drain zeroes the tail; RESAMPLE has nothing left to drain and
        // the stale count must not keep the source alive forever.
        const int styleOverride = s.pitchStyleOverride.load(std::memory_order_relaxed);
        const int effectiveStyle = styleOverride == CUE3D_SOURCE_PITCH_RESAMPLE ? CUE3D_PITCH_RESAMPLE
                                   : styleOverride == CUE3D_SOURCE_PITCH_SHIFT  ? CUE3D_PITCH_SHIFT
                                                                                : pitchStyle;
        const bool shiftPath = effectiveStyle == CUE3D_PITCH_SHIFT && s.stretch != nullptr;
        if (!shiftPath) {
            s.shiftTail = 0;
        }
        if (!shouldRender && s.rampGain <= kRampSilenceGate && s.shiftTail <= 0) {
            continue;
        }

        // Read the cross-thread params once per block (not per sample). A value one
        // frame stale is inaudible for a cue.
        const float gain = s.gain.load(std::memory_order_relaxed);
        const float rate = s.rate.load(std::memory_order_relaxed);
        const float px = s.x.load(std::memory_order_relaxed);
        const float py = s.y.load(std::memory_order_relaxed);
        const float pz = s.z.load(std::memory_order_relaxed);
        int mode = s.mode.load(std::memory_order_relaxed);
        // Coerce an out-of-range mode to the HRTF default. Reachable: the bench pushes
        // mode = (Cue3DMode) CVarGetInteger(...), so a console-set CVar past the 0..2 combo
        // lands here. Without this the fill-gain switch (defaults to HRTF) and the render
        // switch below (defaults to DIRECT) would disagree on which branch a stray value takes.
        if (mode < CUE3D_MODE_HRTF || mode > CUE3D_MODE_DIRECT) {
            mode = CUE3D_MODE_HRTF;
        }
        const float intervalSec = s.intervalSec.load(std::memory_order_relaxed);
        const float lowPassHz = s.lowPassHz.load(std::memory_order_relaxed);

        // Normalized direction in the GAME frame (+z ahead), needed twice: the rear
        // muffle keys off the forward component here, and the binaural effect below
        // wants the same vector remapped to Steam Audio's frame.
        float dx = px;
        float dy = py;
        float dz = pz;
        float len = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (len < 1e-6f) {
            dx = 0.0f;
            dy = 0.0f;
            dz = 1.0f; // degenerate position -> straight ahead
        } else {
            float inv = 1.0f / len;
            dx *= inv;
            dy *= inv;
            dz *= inv;
        }
        // Rear amount: 0 across the entire front hemisphere (dz >= 0), ramping to 1
        // dead behind. Continuous through the side positions, so an enemy crossing
        // the shoulder has no audible seam.
        const float rearAmount = dz < 0.0f ? -dz : 0.0f;

        // Distance attenuation: fold Steam Audio's inverse-distance gain into the
        // source's flat gain. The coordinate frame is irrelevant here (only the
        // distance magnitude matters), so we feed the raw position with no Z
        // negation. For the inverse-distance type this is pure math (no callback /
        // simulation), so calling it once per block in the audio callback is cheap
        // and thread-safe. `gain` (Cue3D_SetGain) is a distance-independent
        // multiplier layered on top of this falloff.
        IPLDistanceAttenuationModel distModel{};
        distModel.type = IPL_DISTANCEATTENUATIONTYPE_INVERSEDISTANCE;
        distModel.minDistance = kMinDistanceMeters;
        const float invScale = 1.0f / kWorldUnitsPerMeter;
        IPLVector3 srcPos{ px * invScale, py * invScale, pz * invScale };
        IPLVector3 listener{ 0.0f, 0.0f, 0.0f };
        const float distGain = iplDistanceAttenuationCalculate(g.ctx, srcPos, listener, &distModel);
        // Fill gain by render mode. The rear gain dip and distance attenuation are folded
        // in HERE, at the PCM fill, separately from the rear muffle/tremolo loop below — so
        // "skip the rear effect" in PAN/DIRECT means dropping the dip term from this gain,
        // not only skipping that loop. DIRECT additionally drops distance attenuation.
        const float rearGainDip = g_rearGainDip.load(std::memory_order_relaxed);
        float effGain;
        if (mode == CUE3D_MODE_DIRECT) {
            effGain = gain; // dead center: no distance falloff, no rear dip
        } else if (mode == CUE3D_MODE_PAN) {
            effGain = gain * distGain; // distance stays; rear dip does not
        } else {
            effGain = gain * distGain * (1.0f - rearGainDip * rearAmount); // HRTF: today's path
        }

        // Interval cadence: a LOOPING source with intervalSec > 0 restarts start-to-start
        // every intervalSec wall-clock seconds instead of wrapping seamlessly. Counted in
        // output frames (not scaled by pitch), so the cadence is pitch-independent.
        const bool intervalActive = s.loop && intervalSec > 0.0f;
        // Floor the threshold at one frame. A pathological sub-sample interval (reachable
        // only via a hand-set CVar/console, never the slider) would otherwise make the
        // single per-sample subtract below net-accumulate — counter += 1, then -= thresh
        // with thresh < 1 leaves it climbing every sample. At >= 1 the counter stays bounded
        // (it restarts every sample, garbage but harmless) instead of drifting for hours.
        double intervalThresh = (double) intervalSec * (double) g.sampleRate;
        if (intervalThresh < 1.0) {
            intervalThresh = 1.0;
        }

        // Start/stop ramp: smooth `rampGain` toward 1 while rendering, 0 while stopping,
        // ~5 ms time constant (per output sample, wall-clock — pitch does not change it).
        // A restart (gen bump above, or interval expiry below) drops it to 0 to ramp in.
        const float rampTarget = shouldRender ? 1.0f : 0.0f;
        float rampAlpha = 1.0f - std::exp(-1.0f / (kRampTimeConstantSec * (float) g.sampleRate));
        rampAlpha = rampAlpha < 0.0f ? 0.0f : (rampAlpha > 1.0f ? 1.0f : rampAlpha);

        // Rate slew (see kRateSmoothTimeConstantSec): the cursor advances by smoothRate,
        // eased toward the block's target rate per sample. The slewed value is a convex
        // mix of seam-sanitized rates, so it inherits their positive/finite bounds and
        // the cursor-validity argument below is unchanged.
        float rateAlpha = 1.0f - std::exp(-1.0f / (kRateSmoothTimeConstantSec * (float) g.sampleRate));
        rateAlpha = rateAlpha < 0.0f ? 0.0f : (rateAlpha > 1.0f ? 1.0f : rateAlpha);

        // Fill a mono block from this source's PCM, applying the fill gain and the ramp.
        // RESAMPLE style renders straight into the spatializer's mono input, advancing
        // the cursor by the slewed rate (pitch) and interpolating between bracketing
        // samples (4-point Catmull-Rom below); SHIFT style renders at native rate (cursor
        // advances by exactly 1) into the scratch block, and the spectral shifter after
        // this loop produces the mono input from it. Either way: loop-wrap (seamless,
        // preserving the fractional phase) when no interval is set; otherwise play once
        // and pad the gap with silence until the interval restarts it; or zero-pad +
        // latch `ended` at a one-shot's end.
        float* fill = shiftPath ? g.shiftIn : mono;
        const bool wrapsSeamlessly = s.loop && !intervalActive;
        double cursor = s.cursor;
        double intervalCounter = s.intervalCounter;
        float rampGain = s.rampGain;
        float smoothRate = s.smoothRate;
        bool ended = s.ended;
        for (int n = 0; n < kFrameSize; n++) {
            // Interval restart, before sampling so the restarted cursor is used this frame.
            // Rewound preserving the overshoot so the cadence does not drift.
            if (intervalActive) {
                intervalCounter += 1.0;
                if (intervalCounter >= intervalThresh) {
                    intervalCounter -= intervalThresh;
                    cursor = 0.0;
                    rampGain = 0.0f; // ramp each pulse in from silence
                }
            }

            rampGain += (rampTarget - rampGain) * rampAlpha;

            // "Not a usable index" rather than "past the end": that covers the normal
            // end-of-buffer case AND the two ways a corrupt rate can wreck the cursor —
            // negative (walks off the front) and NaN. NaN is the reason for the negated
            // spelling: every comparison against NaN is false, so `cursor >= frameCount`
            // would MISS it, and (int) NaN is undefined behavior that yields INT_MIN on
            // x86 — an out-of-bounds read of pcm[] gigabytes below the buffer.
            // Cue3D_SetPitch now rejects such rates, so this is unreachable in practice;
            // it is nonetheless the bounds check standing in front of the indexing, and a
            // bounds check does not get to assume its input was validated elsewhere.
            if (!(cursor >= 0.0 && cursor < s.frameCount)) {
                if (wrapsSeamlessly) {
                    cursor -= s.frameCount;
                    if (!(cursor >= 0.0 && cursor < s.frameCount)) {
                        cursor = 0.0; // tiny pcm / huge rate overshoot, or a non-finite cursor
                    }
                } else {
                    // One-shot past its end, or an interval source waiting out the gap
                    // between pulses: emit silence. NOTE the ramp does NOT round off this
                    // edge — the output here is a hard 0 (there is no signal left to fade),
                    // so a one-shot / pulse whose PCM does not end near zero steps straight to
                    // silence and can click. Author such sounds enveloped to zero (the bench's
                    // blip is); the ramp only smooths a Stop taken while the cursor is still
                    // inside the buffer, plus every (re)start. A one-shot latches `ended`; an
                    // interval source keeps rendering and waits for the counter above to
                    // restart it. The cursor is left past the end — the gen bump or interval
                    // restart rewinds it.
                    fill[n] = 0.0f;
                    if (!s.loop) {
                        ended = true;
                    }
                    continue;
                }
            }
            if (shiftPath) {
                // Native-rate read: the pitch multiplier is realized by the spectral
                // shifter after this loop, so no interpolation and no rate slew here.
                // (int) truncation is exact — the cursor is integral on this path —
                // except for one sample right after a style flip, where dropping the
                // resample path's stale fraction once is inaudible.
                fill[n] = s.pcm[(int) cursor] * effGain * rampGain;
                cursor += 1.0;
                continue;
            }
            smoothRate += (rate - smoothRate) * rateAlpha;

            // 4-point Catmull-Rom around the cursor — linear interpolation at a fractional
            // rate leaves audible grit (no anti-alias rolloff pitching up, dulling pitching
            // down). x1 is the sample at/behind the cursor, x0 its predecessor, x2/x3 the
            // two ahead. Neighbors wrap for a seamless loop — each index lands at most one
            // step out of range, so a single conditional wrap each (chained for x3) covers
            // any frameCount >= 1 — and hold at the buffer edges otherwise (one-shot, or an
            // interval source whose single playthrough must not wrap).
            int i1 = (int) cursor;
            float frac = (float) (cursor - i1);
            float x1 = s.pcm[i1];
            float x0 = (i1 > 0) ? s.pcm[i1 - 1] : (wrapsSeamlessly ? s.pcm[s.frameCount - 1] : x1);
            float x2, x3;
            if (wrapsSeamlessly) {
                int i2 = (i1 + 1 < s.frameCount) ? i1 + 1 : 0;
                int i3 = (i2 + 1 < s.frameCount) ? i2 + 1 : 0;
                x2 = s.pcm[i2];
                x3 = s.pcm[i3];
            } else {
                x2 = (i1 + 1 < s.frameCount) ? s.pcm[i1 + 1] : x1;
                x3 = (i1 + 2 < s.frameCount) ? s.pcm[i1 + 2] : x2;
            }
            float c1 = 0.5f * (x2 - x0);
            float c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
            float c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
            fill[n] = (((c3 * frac + c2) * frac + c1) * frac + x1) * effGain * rampGain;
            cursor += smoothRate;
        }
        s.cursor = cursor;
        s.intervalCounter = intervalCounter;
        s.rampGain = rampGain;
        // On the shift path the slew never ran; pin it to the block target so a flip
        // back to RESAMPLE starts at the current pitch instead of gliding out of a
        // stale one.
        s.smoothRate = shiftPath ? rate : smoothRate;
        // One-shot EOF latches `ended` (audio-thread-only) — it never touches `playing`,
        // so a concurrent Cue3D_Play cannot be lost. The next gen bump clears it and
        // rewinds the cursor.
        s.ended = ended;

        // SHIFT style: realize the pitch multiplier by running the native-rate scratch
        // block through the spectral shifter into the spatializer's mono input (1:1
        // frame count, so pitch moves but duration/cadence do not). Everything after
        // this point — per-source low-pass, rear effect, HRTF/pan/direct — is common to
        // both styles and operates on the shifter's OUTPUT.
        if (shiftPath) {
            if (!s.shiftWasActive) {
                // First shifted block (style flip, or a reused slot's first render):
                // drop whatever a previous sound or style left inside the analysis
                // window. reset() is allocation-free, so it is safe here.
                s.stretch->reset();
            }
            // Applied per block (~21 ms); the shifter's own spectral hop smooths the
            // steps, so no per-sample slew is needed on this path.
            s.stretch->setTransposeFactor(rate, kShiftTonalityLimitHz / (float) g.sampleRate);
            float* shiftInCh[1] = { g.shiftIn };
            float* shiftOutCh[1] = { mono };
            s.stretch->process(shiftInCh, kFrameSize, shiftOutCh, kFrameSize);
            // Latency drain bookkeeping for the skip gate above: while rendering, the
            // shifter always holds ~latency frames of signal; once stopped/ended, count
            // that tail down block by block as silence flushes it out.
            s.shiftTail =
                shouldRender ? s.shiftLatencyFrames : (s.shiftTail > kFrameSize ? s.shiftTail - kFrameSize : 0);
        }
        s.shiftWasActive = shiftPath;

        // Per-source low-pass "muffle" (Cue3D_SetLowPass), in series BEFORE the rear muffle
        // and active in EVERY render mode — so it must run even when the rear block below
        // (HRTF only) is skipped. Own one-pole state (lpState2); same alpha-clamp
        // discipline as the rear filter. cutoff 0 = off: skipped, state left untouched.
        if (lowPassHz > 0.0f) {
            float lpAlpha2 = 1.0f - std::exp(-2.0f * kPi * lowPassHz / (float) g.sampleRate);
            lpAlpha2 = lpAlpha2 < 0.0f ? 0.0f : (lpAlpha2 > 1.0f ? 1.0f : lpAlpha2);
            float lp2 = s.lpState2;
            for (int n = 0; n < kFrameSize; n++) {
                lp2 += (mono[n] - lp2) * lpAlpha2;
                mono[n] = lp2;
            }
            s.lpState2 = lp2;
        }

        if (mode == CUE3D_MODE_HRTF) {
            // Rear muffle + tremolo (knobs live via Cue3D_SetRearEffect; read once per block
            // like the position): a one-pole low-pass, dry/wet-mixed by rearAmount, takes the
            // highs off a source behind the listener; an amplitude pulse scaled by the same
            // rearAmount then chops it. Filter state and LFO phase advance every block — even
            // fully in front, where both effects are identity — so a front->rear transition
            // has no step and the pulse stays continuous. lp is updated from the dry sample
            // before that sample is mixed.
            const float rearCutoffHz = g_rearCutoffHz.load(std::memory_order_relaxed);
            const float tremDepth = g_rearTremoloDepth.load(std::memory_order_relaxed) * rearAmount;
            const float tremStep = 2.0f * kPi * g_rearTremoloHz.load(std::memory_order_relaxed) / (float) g.sampleRate;
            // Clamped independently of the seam's sanitizing: the filter is only stable for
            // alpha in [0, 1], and that invariant is cheap enough to assert right where it is
            // relied upon rather than trust across a thread boundary.
            float lpAlpha = 1.0f - std::exp(-2.0f * kPi * rearCutoffHz / (float) g.sampleRate);
            lpAlpha = lpAlpha < 0.0f ? 0.0f : (lpAlpha > 1.0f ? 1.0f : lpAlpha);
            float lp = s.lpState;
            float phase = s.tremPhase;
            for (int n = 0; n < kFrameSize; n++) {
                lp += (mono[n] - lp) * lpAlpha;
                mono[n] += (lp - mono[n]) * rearAmount;
                mono[n] *= 1.0f - tremDepth * (0.5f + 0.5f * std::sin(phase));
                phase += tremStep;
                if (phase > 2.0f * kPi) {
                    phase -= 2.0f * kPi;
                }
            }
            s.lpState = lp;
            s.tremPhase = phase;

            IPLBinauralEffectParams params{};
            // Game (+z ahead) -> Steam Audio (-Z forward). THE negation lives here; the
            // vector was normalized in the game frame above.
            params.direction = IPLVector3{ dx, dy, -dz };
            params.interpolation = IPL_HRTFINTERPOLATION_BILINEAR;
            params.spatialBlend = 1.0f;
            params.hrtf = g.hrtf;
            params.peakDelays = nullptr;
            iplBinauralEffectApply(s.effect, &params, &g.inBuf, &g.tmpOut);

            // Sum into the accumulator (Steam Audio spatializes but does not mix).
            const float* L = g.tmpOut.data[0];
            const float* R = g.tmpOut.data[1];
            for (int n = 0; n < kFrameSize; n++) {
                g.accL[n] += L[n];
                g.accR[n] += R[n];
            }
        } else if (mode == CUE3D_MODE_PAN) {
            // Constant-power stereo pan from the horizontal direction only — front/back
            // and elevation collapse (inherent to a stereo pan). No HRTF, no rear effect;
            // distance attenuation is already folded into effGain. p is the sine of the
            // azimuth (0 when the horizontal direction is degenerate, e.g. straight above).
            const float horiz = std::sqrt(dx * dx + dz * dz);
            const float p = horiz > 1e-6f ? dx / horiz : 0.0f;
            const float angle = (p + 1.0f) * (kPi * 0.25f);
            const float lg = std::cos(angle);
            const float rg = std::sin(angle);
            for (int n = 0; n < kFrameSize; n++) {
                g.accL[n] += mono[n] * lg;
                g.accR[n] += mono[n] * rg;
            }
        } else { // CUE3D_MODE_DIRECT
            // Dead center, equal power into both channels. No HRTF, no rear effect, and
            // distance attenuation was already dropped from effGain above.
            for (int n = 0; n < kFrameSize; n++) {
                const float v = mono[n] * kInvSqrt2;
                g.accL[n] += v;
                g.accR[n] += v;
            }
        }
    }

    for (int n = 0; n < kFrameSize; n++) {
        // Trim for headroom, then hard-clamp to [-1, 1] so a rare stack of loud,
        // in-phase sources can never hand the device an out-of-range sample. Both
        // are branch-cheap and allocation-free (this is the audio callback thread).
        // Non-finite samples render as silence: a comparison against NaN is always
        // false, so a bare min/max clamp would pass NaN straight through to the
        // device (silence or a full-scale blast, driver's choice). This is the last
        // line of defence in front of the user's headphones — it does not get to
        // have a hole in it, however sanitized the DSP inputs upstream are.
        float l = g.accL[n] * kOutputTrim;
        float r = g.accR[n] * kOutputTrim;
        l = std::isfinite(l) ? l : 0.0f;
        r = std::isfinite(r) ? r : 0.0f;
        g.block[2 * n] = l < -1.0f ? -1.0f : (l > 1.0f ? 1.0f : l);
        g.block[2 * n + 1] = r < -1.0f ? -1.0f : (r > 1.0f ? 1.0f : r);
    }
    g.blockAvail = kFrameSize;
    g.blockPos = 0;
}

void DataCallback(ma_device* device, void* output, const void* input, ma_uint32 frameCount) {
    (void) device;
    (void) input;
    float* out = (float*) output; // interleaved stereo
    ma_uint32 written = 0;
    while (written < frameCount) {
        if (g.blockAvail == 0) {
            ProduceBlock();
        }
        ma_uint32 n = frameCount - written;
        if (n > (ma_uint32) g.blockAvail) {
            n = (ma_uint32) g.blockAvail;
        }
        std::memcpy(out + (size_t) written * 2, g.block + (size_t) g.blockPos * 2, (size_t) n * 2 * sizeof(float));
        written += n;
        g.blockPos += (int) n;
        g.blockAvail -= (int) n;
    }
}

} // namespace

extern "C" void Cue3D_Init(void) {
    if (g.active) {
        return;
    }

    IPLContextSettings ctxSettings{};
    ctxSettings.version = STEAMAUDIO_VERSION;
    if (iplContextCreate(&ctxSettings, &g.ctx) != IPL_STATUS_SUCCESS) {
        SPDLOG_ERROR("Cue3D: iplContextCreate failed; 3D audio disabled");
        return;
    }

    // Open the device BEFORE building the HRTF: cfg.sampleRate is only a hint, so the
    // real rate isn't known until miniaudio negotiates one, and the HRTF (and every
    // per-source effect) has to be baked at that rate. ma_device_init does not run the
    // callback — ma_device_start (below) does — so it is safe to create the Steam Audio
    // objects the callback depends on in the window between init and start.
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate = kSampleRate;
    cfg.periodSizeInFrames = kFrameSize;
    cfg.dataCallback = DataCallback;
    if (ma_device_init(nullptr, &cfg, &g.device) != MA_SUCCESS) {
        SPDLOG_ERROR("Cue3D: ma_device_init failed; 3D audio disabled");
        iplContextRelease(&g.ctx);
        return;
    }

    // Read back what the OS actually opened; everything rate-dependent follows this.
    g.sampleRate = g.device.sampleRate > 0 ? (int) g.device.sampleRate : kSampleRate;

    IPLAudioSettings audioSettings{};
    audioSettings.samplingRate = g.sampleRate;
    audioSettings.frameSize = kFrameSize;

    IPLHRTFSettings hrtfSettings{};
    hrtfSettings.type = IPL_HRTFTYPE_DEFAULT;
    hrtfSettings.volume = 1.0f;
    hrtfSettings.normType = IPL_HRTFNORMTYPE_NONE;
    if (iplHRTFCreate(g.ctx, &audioSettings, &hrtfSettings, &g.hrtf) != IPL_STATUS_SUCCESS) {
        SPDLOG_ERROR("Cue3D: iplHRTFCreate failed; 3D audio disabled");
        ma_device_uninit(&g.device);
        iplContextRelease(&g.ctx);
        return;
    }

    // Reused per source by the callback; effects are per-source, created on load.
    iplAudioBufferAllocate(g.ctx, 1, kFrameSize, &g.inBuf);
    iplAudioBufferAllocate(g.ctx, 2, kFrameSize, &g.tmpOut);

    if (ma_device_start(&g.device) != MA_SUCCESS) {
        SPDLOG_ERROR("Cue3D: ma_device_start failed; 3D audio disabled");
        ma_device_uninit(&g.device);
        iplAudioBufferFree(g.ctx, &g.tmpOut);
        iplAudioBufferFree(g.ctx, &g.inBuf);
        iplHRTFRelease(&g.hrtf);
        iplContextRelease(&g.ctx);
        return;
    }

    g.active = true;
    SPDLOG_INFO("Cue3D: Steam Audio {} backend ready @ {} Hz", STEAMAUDIO_VERSION, g.sampleRate);
}

extern "C" void Cue3D_Shutdown(void) {
    if (!g.active) {
        return;
    }
    // Uninit stops/joins the callback first, so the teardown below has no concurrent
    // reader of the sources or Steam Audio objects.
    ma_device_uninit(&g.device);
    g.active = false;

    for (int i = 0; i < kMaxSources; i++) {
        Cue3DSource& s = g_sources[i];
        // The pitch shifter outlives inUse=false only until here (slots are reusable;
        // the heap allocation is not kept across an Init/Shutdown cycle).
        delete s.stretch;
        s.stretch = nullptr;
        if (s.inUse.load(std::memory_order_relaxed)) {
            iplBinauralEffectRelease(&s.effect);
            s.pcm.clear();
            s.pcm.shrink_to_fit();
            s.frameCount = 0;
            s.cursor = 0.0;
            s.playing.store(false, std::memory_order_relaxed);
            s.inUse.store(false, std::memory_order_relaxed); // reusable on a later Init
        }
    }

    iplAudioBufferFree(g.ctx, &g.tmpOut);
    iplAudioBufferFree(g.ctx, &g.inBuf);
    iplHRTFRelease(&g.hrtf);
    iplContextRelease(&g.ctx);
}

extern "C" bool Cue3D_IsActive(void) {
    return g.active;
}

extern "C" int Cue3D_GetSampleRate(void) {
    return g.sampleRate;
}

extern "C" float Cue3D_GetUnityGainDistance(void) {
    // The inverse-distance model's near plateau: closer than kMinDistanceMeters the
    // gain is capped at 1.0 (see ProduceBlock's distance attenuation).
    return kMinDistanceMeters * kWorldUnitsPerMeter;
}

extern "C" Cue3DSource* Cue3D_Load(const char* path, bool loop) {
    if (!g.active || path == nullptr) {
        return nullptr;
    }

    // Force mono downmix + resample to the negotiated device rate + float samples.
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 1, g.sampleRate);
    ma_decoder decoder;
    if (ma_decoder_init_file(path, &cfg, &decoder) != MA_SUCCESS) {
        SPDLOG_ERROR("Cue3D: failed to open '{}'", path);
        return nullptr;
    }

    std::vector<float> pcm;
    try {
        ma_uint64 total = 0;
        if (ma_decoder_get_length_in_pcm_frames(&decoder, &total) == MA_SUCCESS && total > 0) {
            pcm.resize((size_t) total);
            ma_uint64 read = 0;
            ma_decoder_read_pcm_frames(&decoder, pcm.data(), total, &read);
            pcm.resize((size_t) read); // trim to what actually decoded
        } else {
            // Length unknown (some streamed formats): read until EOF.
            float chunk[kFrameSize];
            for (;;) {
                ma_uint64 read = 0;
                ma_result r = ma_decoder_read_pcm_frames(&decoder, chunk, kFrameSize, &read);
                if (read > 0) {
                    pcm.insert(pcm.end(), chunk, chunk + read);
                }
                if (r != MA_SUCCESS || read < (ma_uint64) kFrameSize) {
                    break;
                }
            }
        }
    } catch (const std::exception& e) {
        // Keep allocation failures from unwinding through the extern "C" boundary.
        SPDLOG_ERROR("Cue3D: allocation failed decoding '{}' ({})", path, e.what());
        ma_decoder_uninit(&decoder);
        return nullptr;
    }
    ma_decoder_uninit(&decoder);

    if (pcm.empty()) {
        SPDLOG_ERROR("Cue3D: '{}' decoded to 0 frames", path);
        return nullptr;
    }
    return CreateSource(pcm.data(), (int) pcm.size(), loop);
}

extern "C" Cue3DSource* Cue3D_LoadPcm(const float* monoPcm, int frames, bool loop) {
    return CreateSource(monoPcm, frames, loop);
}

extern "C" void Cue3D_Play(Cue3DSource* source) {
    if (source != nullptr) {
        // Eager advisory status so a same-tick Cue3D_IsPlaying does not read a stale false
        // before the callback runs (the callback will keep it current thereafter).
        source->audible.store(true, std::memory_order_relaxed);
        // Set the stop lever, then bump the restart generation. The callback sees the new
        // generation and restarts from sample 0 (see ProduceBlock); this is the only path
        // that rewinds, so a re-trigger of a finished one-shot is never swallowed.
        source->playing.store(true, std::memory_order_release);
        source->startGen.fetch_add(1, std::memory_order_release);
    }
}

extern "C" void Cue3D_Stop(Cue3DSource* source) {
    if (source != nullptr) {
        source->playing.store(false, std::memory_order_release);
    }
}

extern "C" bool Cue3D_IsPlaying(Cue3DSource* source) {
    // Advisory: reads the flag the callback publishes each block (and Cue3D_Play sets
    // eagerly). May lag reality by ~one audio block; never use it as a fence.
    return source != nullptr && source->audible.load(std::memory_order_relaxed);
}

extern "C" void Cue3D_SetInterval(Cue3DSource* source, float seconds) {
    if (source != nullptr) {
        // Sanitize like Cue3D_SetPitch: non-finite -> 0 (off), else clamp to [0, 30] s.
        // 0 is a valid value (seamless loop), so it needs no special-casing here.
        source->intervalSec.store(SanitizeParam(seconds, 0.0f, kMaxIntervalSec, 0.0f), std::memory_order_relaxed);
    }
}

extern "C" void Cue3D_SetMode(Cue3DSource* source, Cue3DMode mode) {
    if (source != nullptr) {
        source->mode.store((int) mode, std::memory_order_relaxed);
    }
}

extern "C" void Cue3D_SetLowPass(Cue3DSource* source, float cutoffHz) {
    if (source == nullptr) {
        return;
    }
    // Order matters and is NOT SanitizeParam: 0 is the documented DISABLE value, so a
    // non-finite or non-positive cutoff must store 0 (off) — never clamp UP to the 20 Hz
    // floor, which would turn "off" into near-total muffle. Only a positive finite cutoff
    // is clamped to the audible band.
    float value;
    if (!std::isfinite(cutoffHz) || cutoffHz <= 0.0f) {
        value = 0.0f; // off
    } else {
        value = cutoffHz < kLowPassMinHz ? kLowPassMinHz : (cutoffHz > kLowPassMaxHz ? kLowPassMaxHz : cutoffHz);
    }
    source->lowPassHz.store(value, std::memory_order_relaxed);
}

extern "C" void Cue3D_SetGain(Cue3DSource* source, float gain) {
    // Sanitized like every other setter: the Cue layer's gain is a product of unchecked
    // CVars (and now a per-voice level that can be exactly 0), and a non-finite value
    // would poison the per-source low-pass state on the audio thread for as long as the
    // source stays loaded. The ceiling only bounds a runaway boost; the output trim and
    // clip in ProduceBlock still do the real limiting.
    constexpr float kMaxGain = 16.0f;
    if (source != nullptr) {
        source->gain.store(SanitizeParam(gain, 0.0f, kMaxGain, 1.0f), std::memory_order_relaxed);
    }
}

extern "C" void Cue3D_SetPitch(Cue3DSource* source, float rate) {
    if (source != nullptr) {
        // Same contract as Cue3D_SetRearEffect: the seam sanitizes so the audio callback
        // can trust the value. The cursor advances by `rate` each sample, so a NaN rate
        // makes the cursor NaN and a negative one walks it off the front of the buffer —
        // both out-of-bounds reads of pcm[] on the audio thread. Non-finite falls back to
        // native pitch; the bounds are far wider than the Y->pitch mapping can produce
        // (a couple of octaves either way), so real tuning is untouched.
        source->rate.store(SanitizeParam(rate, kMinPitchRate, kMaxPitchRate, 1.0f), std::memory_order_relaxed);
    }
}

extern "C" void Cue3D_SetPitchStyle(Cue3DPitchStyle style) {
    // Coerce like ProduceBlock's mode read: the CVar behind this is console-reachable,
    // so any value that is not exactly SHIFT falls back to the RESAMPLE default.
    g_pitchStyle.store(style == CUE3D_PITCH_SHIFT ? CUE3D_PITCH_SHIFT : CUE3D_PITCH_RESAMPLE,
                       std::memory_order_relaxed);
}

extern "C" void Cue3D_SetSourcePitchStyle(Cue3DSource* source, Cue3DSourcePitchStyle style) {
    if (source == nullptr) {
        return;
    }
    // Coerce like Cue3D_SetPitchStyle: anything that is not exactly RESAMPLE or SHIFT
    // falls back to the follow-global default.
    int value = (style == CUE3D_SOURCE_PITCH_RESAMPLE || style == CUE3D_SOURCE_PITCH_SHIFT)
                    ? (int) style
                    : (int) CUE3D_SOURCE_PITCH_GLOBAL;
    source->pitchStyleOverride.store(value, std::memory_order_relaxed);
}

extern "C" void Cue3D_SetPosition(Cue3DSource* source, float x, float y, float z) {
    if (source == nullptr) {
        return;
    }
    // Game convention (+x right, +y up, +z ahead); the backend negates Z when it
    // builds the Steam Audio direction (see ProduceBlock).
    source->x.store(x, std::memory_order_relaxed);
    source->y.store(y, std::memory_order_relaxed);
    source->z.store(z, std::memory_order_relaxed);
}

extern "C" void Cue3D_SetRearEffect(float cutoffHz, float gainDip, float tremoloDepth, float tremoloHz) {
    // Sanitize here, on the game thread, so the audio callback can trust these values:
    // it runs the filter per sample and has no cheap way to recover once its state is NaN.
    g_rearCutoffHz.store(SanitizeParam(cutoffHz, kRearCutoffMinHz, kRearCutoffMaxHz, CUE3D_REAR_CUTOFF_HZ_DEFAULT),
                         std::memory_order_relaxed);
    g_rearGainDip.store(SanitizeParam(gainDip, 0.0f, 1.0f, CUE3D_REAR_GAIN_DIP_DEFAULT), std::memory_order_relaxed);
    g_rearTremoloDepth.store(SanitizeParam(tremoloDepth, 0.0f, 1.0f, CUE3D_REAR_TREMOLO_DEPTH_DEFAULT),
                             std::memory_order_relaxed);
    g_rearTremoloHz.store(SanitizeParam(tremoloHz, 0.0f, kRearTremoloMaxHz, CUE3D_REAR_TREMOLO_HZ_DEFAULT),
                          std::memory_order_relaxed);
}

#else // HAVE_STEAM_AUDIO not defined — no-op stubs

extern "C" void Cue3D_Init(void) {
}
extern "C" void Cue3D_Shutdown(void) {
}
extern "C" bool Cue3D_IsActive(void) {
    return false;
}
extern "C" int Cue3D_GetSampleRate(void) {
    return 0;
}
extern "C" float Cue3D_GetUnityGainDistance(void) {
    return 0.0f;
}
extern "C" Cue3DSource* Cue3D_Load(const char* path, bool loop) {
    (void) path;
    (void) loop;
    return nullptr;
}
extern "C" Cue3DSource* Cue3D_LoadPcm(const float* monoPcm, int frames, bool loop) {
    (void) monoPcm;
    (void) frames;
    (void) loop;
    return nullptr;
}
extern "C" void Cue3D_Play(Cue3DSource* source) {
    (void) source;
}
extern "C" void Cue3D_SetPosition(Cue3DSource* source, float x, float y, float z) {
    (void) source;
    (void) x;
    (void) y;
    (void) z;
}
extern "C" void Cue3D_SetGain(Cue3DSource* source, float gain) {
    (void) source;
    (void) gain;
}
extern "C" void Cue3D_SetPitch(Cue3DSource* source, float rate) {
    (void) source;
    (void) rate;
}
extern "C" void Cue3D_SetPitchStyle(Cue3DPitchStyle style) {
    (void) style;
}
extern "C" void Cue3D_SetSourcePitchStyle(Cue3DSource* source, Cue3DSourcePitchStyle style) {
    (void) source;
    (void) style;
}
extern "C" void Cue3D_Stop(Cue3DSource* source) {
    (void) source;
}
extern "C" bool Cue3D_IsPlaying(Cue3DSource* source) {
    (void) source;
    return false;
}
extern "C" void Cue3D_SetInterval(Cue3DSource* source, float seconds) {
    (void) source;
    (void) seconds;
}
extern "C" void Cue3D_SetMode(Cue3DSource* source, Cue3DMode mode) {
    (void) source;
    (void) mode;
}
extern "C" void Cue3D_SetLowPass(Cue3DSource* source, float cutoffHz) {
    (void) source;
    (void) cutoffHz;
}
extern "C" void Cue3D_SetRearEffect(float cutoffHz, float gainDip, float tremoloDepth, float tremoloHz) {
    (void) cutoffHz;
    (void) gainDip;
    (void) tremoloDepth;
    (void) tremoloHz;
}

#endif // HAVE_STEAM_AUDIO
