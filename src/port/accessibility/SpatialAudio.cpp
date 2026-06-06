#include "SpatialAudio.h"

#ifdef HAVE_STEAM_AUDIO

#include <phonon.h>
#include "miniaudio.h"
#include <spdlog/spdlog.h>
#include <cmath>
#include <cstring>

// End-to-end smoke test for the Steam Audio + miniaudio pipeline. A single mono
// tone is synthesized procedurally and run through Steam Audio's binaural HRTF
// effect with a source direction that slowly orbits the listener in the
// horizontal plane. miniaudio owns the OS playback device and pulls audio via a
// callback; we bridge the callback's (variable) frame count to Steam Audio's
// fixed frame size with a one-block carry buffer — the same block-size matching
// the real cue engine will need.

namespace {

constexpr int kSampleRate = 48000;
constexpr int kFrameSize = 1024;          // Steam Audio fixed processing block
constexpr double kTwoPi = 6.283185307179586;
constexpr double kToneHz = 440.0;         // the orbiting tone's pitch
constexpr double kOrbitHz = 0.25;         // one full circle around the head every 4 s
constexpr float kToneGain = 0.2f;

struct SpatialState {
    bool active = false;

    IPLContext ctx = nullptr;
    IPLHRTF hrtf = nullptr;
    IPLBinauralEffect effect = nullptr;
    IPLAudioBuffer inBuf{};               // mono, kFrameSize
    IPLAudioBuffer outBuf{};              // stereo, kFrameSize

    ma_device device{};

    double tonePhase = 0.0;               // continuous across blocks (avoids clicks)
    double toneInc = 0.0;                 // radians/sample
    float orbit = 0.0f;                   // current source angle, radians
    float orbitInc = 0.0f;                // radians/sample

    // Interleaved stereo carry buffer: one processed Steam Audio block, drained
    // across however many frames the device callback asks for.
    float block[kFrameSize * 2] = { 0 };
    int blockAvail = 0;                   // frames remaining in `block`
    int blockPos = 0;                     // read cursor (frames) into `block`
};

SpatialState g;

// Produce one kFrameSize block: synthesize the tone, spatialize it at the
// current orbit angle, interleave into the carry buffer.
void ProduceBlock() {
    float* mono = g.inBuf.data[0];
    for (int i = 0; i < kFrameSize; i++) {
        mono[i] = kToneGain * (float) sin(g.tonePhase);
        g.tonePhase += g.toneInc;
        if (g.tonePhase >= kTwoPi) {
            g.tonePhase -= kTwoPi;
        }
    }

    // Advance the orbit once per block; bilinear HRTF interpolation smooths the
    // per-block direction steps. Angle 0 = front (-Z), +90 deg = right (+X).
    g.orbit += g.orbitInc * kFrameSize;
    if (g.orbit >= (float) kTwoPi) {
        g.orbit -= (float) kTwoPi;
    }

    IPLBinauralEffectParams params{};
    params.direction = IPLVector3{ sinf(g.orbit), 0.0f, -cosf(g.orbit) };
    params.interpolation = IPL_HRTFINTERPOLATION_BILINEAR;
    params.spatialBlend = 1.0f;
    params.hrtf = g.hrtf;
    params.peakDelays = nullptr;

    iplBinauralEffectApply(g.effect, &params, &g.inBuf, &g.outBuf);
    iplAudioBufferInterleave(g.ctx, &g.outBuf, g.block);
    g.blockAvail = kFrameSize;
    g.blockPos = 0;
}

void DataCallback(ma_device* device, void* output, const void* input, ma_uint32 frameCount) {
    (void) device;
    (void) input;
    float* out = (float*) output;     // interleaved stereo
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

extern "C" void SpatialAudio_Init(void) {
    if (g.active) {
        return;
    }

    IPLContextSettings ctxSettings{};
    ctxSettings.version = STEAMAUDIO_VERSION;
    if (iplContextCreate(&ctxSettings, &g.ctx) != IPL_STATUS_SUCCESS) {
        SPDLOG_ERROR("SpatialAudio: iplContextCreate failed; 3D audio disabled");
        return;
    }

    IPLAudioSettings audioSettings{};
    audioSettings.samplingRate = kSampleRate;
    audioSettings.frameSize = kFrameSize;

    IPLHRTFSettings hrtfSettings{};
    hrtfSettings.type = IPL_HRTFTYPE_DEFAULT;
    hrtfSettings.volume = 1.0f;
    hrtfSettings.normType = IPL_HRTFNORMTYPE_NONE;
    if (iplHRTFCreate(g.ctx, &audioSettings, &hrtfSettings, &g.hrtf) != IPL_STATUS_SUCCESS) {
        SPDLOG_ERROR("SpatialAudio: iplHRTFCreate failed; 3D audio disabled");
        iplContextRelease(&g.ctx);
        return;
    }

    IPLBinauralEffectSettings effectSettings{};
    effectSettings.hrtf = g.hrtf;
    if (iplBinauralEffectCreate(g.ctx, &audioSettings, &effectSettings, &g.effect) != IPL_STATUS_SUCCESS) {
        SPDLOG_ERROR("SpatialAudio: iplBinauralEffectCreate failed; 3D audio disabled");
        iplHRTFRelease(&g.hrtf);
        iplContextRelease(&g.ctx);
        return;
    }

    iplAudioBufferAllocate(g.ctx, 1, kFrameSize, &g.inBuf);
    iplAudioBufferAllocate(g.ctx, 2, kFrameSize, &g.outBuf);

    g.tonePhase = 0.0;
    g.toneInc = kTwoPi * kToneHz / kSampleRate;
    g.orbit = 0.0f;
    g.orbitInc = (float) (kTwoPi * kOrbitHz / kSampleRate);

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate = kSampleRate;
    cfg.periodSizeInFrames = kFrameSize;
    cfg.dataCallback = DataCallback;
    if (ma_device_init(nullptr, &cfg, &g.device) != MA_SUCCESS) {
        SPDLOG_ERROR("SpatialAudio: ma_device_init failed; 3D audio disabled");
        iplAudioBufferFree(g.ctx, &g.outBuf);
        iplAudioBufferFree(g.ctx, &g.inBuf);
        iplBinauralEffectRelease(&g.effect);
        iplHRTFRelease(&g.hrtf);
        iplContextRelease(&g.ctx);
        return;
    }
    if (ma_device_start(&g.device) != MA_SUCCESS) {
        SPDLOG_ERROR("SpatialAudio: ma_device_start failed; 3D audio disabled");
        ma_device_uninit(&g.device);
        iplAudioBufferFree(g.ctx, &g.outBuf);
        iplAudioBufferFree(g.ctx, &g.inBuf);
        iplBinauralEffectRelease(&g.effect);
        iplHRTFRelease(&g.hrtf);
        iplContextRelease(&g.ctx);
        return;
    }

    g.active = true;
    SPDLOG_INFO("SpatialAudio: Steam Audio {} test tone started (orbiting HRTF source)", STEAMAUDIO_VERSION);
}

extern "C" void SpatialAudio_Shutdown(void) {
    if (!g.active) {
        return;
    }
    // Uninit stops the callback before we free the Steam Audio objects it reads.
    ma_device_uninit(&g.device);
    iplAudioBufferFree(g.ctx, &g.outBuf);
    iplAudioBufferFree(g.ctx, &g.inBuf);
    iplBinauralEffectRelease(&g.effect);
    iplHRTFRelease(&g.hrtf);
    iplContextRelease(&g.ctx);
    g.active = false;
}

#else // HAVE_STEAM_AUDIO not defined — no-op stubs

extern "C" void SpatialAudio_Init(void) {}
extern "C" void SpatialAudio_Shutdown(void) {}

#endif // HAVE_STEAM_AUDIO
