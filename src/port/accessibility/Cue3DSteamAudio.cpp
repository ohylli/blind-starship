#include "Cue3D.h"

#ifdef HAVE_STEAM_AUDIO

#include <phonon.h>
#include "miniaudio.h"
#include <spdlog/spdlog.h>
#include <atomic>
#include <cmath>
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

namespace {

constexpr int kSampleRate = 48000; // requested device rate (a hint); the real rate is read back at Init
constexpr int kFrameSize = 1024;   // Steam Audio fixed processing block

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

// Fixed pool of sources. Cues are few (today: a ring cue + an enemy cue); 16 gives
// generous headroom for future cue types while keeping the per-block scan trivial.
// A fixed array means slot addresses never move, so a Cue3DSource* handed to a
// caller stays valid for the whole Cue3D lifetime and the lock-free publish below
// is safe.
constexpr int kMaxSources = 16;

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
    double cursor = 0.0; // fractional playback position, frames (audio-thread-only)
    bool loop = false;

    // Cross-thread: game thread writes, audio callback reads. Read once per block.
    std::atomic<float> x{ 0.0f }; // game convention: +x right
    std::atomic<float> y{ 0.0f }; //                  +y up
    std::atomic<float> z{ 1.0f }; //                  +z ahead
    std::atomic<float> gain{ 1.0f };
    std::atomic<float> rate{ 1.0f }; // playback-rate multiplier (pitch); 1.0 = native
    std::atomic<bool> playing{ false }; // game sets on Play/Stop; callback clears at non-loop EOF

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
    } catch (const std::exception& e) {
        // Never let a C++ allocation failure unwind through the extern "C" callers.
        SPDLOG_ERROR("Cue3D: PCM allocation failed ({}); source not created", e.what());
        iplBinauralEffectRelease(&s.effect); // undo the effect created just above
        return nullptr;
    }
    s.frameCount = frames;
    s.cursor = 0.0;
    s.loop = loop;
    s.x.store(0.0f, std::memory_order_relaxed);
    s.y.store(0.0f, std::memory_order_relaxed);
    s.z.store(1.0f, std::memory_order_relaxed); // default "ahead"
    s.gain.store(1.0f, std::memory_order_relaxed);
    s.rate.store(1.0f, std::memory_order_relaxed); // native pitch until SetPitch
    s.playing.store(false, std::memory_order_relaxed); // silent until Cue3D_Play

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

    for (int i = 0; i < kMaxSources; i++) {
        Cue3DSource& s = g_sources[i];
        if (!s.inUse.load(std::memory_order_acquire)) {
            continue;
        }
        if (!s.playing.load(std::memory_order_acquire)) {
            continue;
        }

        // Read the cross-thread params once per block (not per sample). A value one
        // frame stale is inaudible for a cue.
        const float gain = s.gain.load(std::memory_order_relaxed);
        const float rate = s.rate.load(std::memory_order_relaxed);
        const float px = s.x.load(std::memory_order_relaxed);
        const float py = s.y.load(std::memory_order_relaxed);
        const float pz = s.z.load(std::memory_order_relaxed);

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
        const float effGain = gain * distGain;

        // Fill the mono input from this source's PCM, applying gain and advancing
        // the cursor by `rate` (the pitch multiplier) with linear interpolation
        // between bracketing samples — nearest-sample at a fractional rate would
        // add audible zipper/aliasing noise. Loop-wrap (preserving the fractional
        // phase), or zero-pad + stop at the end of a one-shot.
        double cursor = s.cursor;
        bool ended = false;
        for (int n = 0; n < kFrameSize; n++) {
            if (cursor >= s.frameCount) {
                if (s.loop) {
                    cursor -= s.frameCount;
                    if (cursor >= s.frameCount) {
                        cursor = 0.0; // guard a tiny pcm / huge rate overshoot
                    }
                } else {
                    mono[n] = 0.0f;
                    ended = true;
                    continue;
                }
            }
            int i0 = (int) cursor;
            float frac = (float) (cursor - i0);
            float a = s.pcm[i0];
            int i1 = i0 + 1;
            // Next sample: wrap to the start for a loop (seamless), hold for a one-shot.
            float b = (i1 < s.frameCount) ? s.pcm[i1] : (s.loop ? s.pcm[0] : a);
            mono[n] = (a + (b - a) * frac) * effGain;
            cursor += rate;
        }
        s.cursor = ended ? 0.0 : cursor;
        if (ended) {
            // End of a non-looping source: stop it from the audio thread (the cursor
            // was already rewound to 0 above, so a later Cue3D_Play restarts from the
            // beginning; this write is race-free as the cursor is the callback's own).
            // The slot stays inUse.
            s.playing.store(false, std::memory_order_release);
        }

        // Game (+z ahead) -> Steam Audio (-Z forward). THE negation lives here.
        float dx = px;
        float dy = py;
        float dz = -pz;
        float len = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (len < 1e-6f) {
            dx = 0.0f;
            dy = 0.0f;
            dz = -1.0f; // degenerate position -> straight ahead
        } else {
            float inv = 1.0f / len;
            dx *= inv;
            dy *= inv;
            dz *= inv;
        }

        IPLBinauralEffectParams params{};
        params.direction = IPLVector3{ dx, dy, dz };
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
    }

    for (int n = 0; n < kFrameSize; n++) {
        // Trim for headroom, then hard-clamp to [-1, 1] so a rare stack of loud,
        // in-phase sources can never hand the device an out-of-range sample. Both
        // are branch-cheap and allocation-free (this is the audio callback thread).
        float l = g.accL[n] * kOutputTrim;
        float r = g.accR[n] * kOutputTrim;
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
        source->playing.store(true, std::memory_order_release);
    }
}

extern "C" void Cue3D_Stop(Cue3DSource* source) {
    if (source != nullptr) {
        source->playing.store(false, std::memory_order_release);
    }
}

extern "C" void Cue3D_SetGain(Cue3DSource* source, float gain) {
    if (source != nullptr) {
        source->gain.store(gain, std::memory_order_relaxed);
    }
}

extern "C" void Cue3D_SetPitch(Cue3DSource* source, float rate) {
    if (source != nullptr) {
        source->rate.store(rate, std::memory_order_relaxed);
    }
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

#else // HAVE_STEAM_AUDIO not defined — no-op stubs

extern "C" void Cue3D_Init(void) {}
extern "C" void Cue3D_Shutdown(void) {}
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
extern "C" void Cue3D_Stop(Cue3DSource* source) {
    (void) source;
}

#endif // HAVE_STEAM_AUDIO
