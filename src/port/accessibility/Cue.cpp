#include "Cue.h"

#include <libultraship.h>
#include <spdlog/spdlog.h>
#include <cmath>
#include <memory>

namespace {

// Preview length in game ticks (Tick runs once per 30 fps game tick): ~3 s — long
// enough to judge timbre and level, short enough to browse a list of cues by ear.
constexpr double kPreviewDurationTicks = 90.0;

// Hard bound on one cue's voice pool. The backend has 16 source slots shared by ALL cues;
// half for a single cue is already generous.
constexpr int kMaxVoicesPerCue = 8;

// Per-voice-slot identity pitch, multiplied onto the target pitch pushed to that voice.
// The tuning knob for making simultaneous copies of the same loop distinguishable by ear.
// All unity for now: pitch already carries the elevation signal (the consumer's Y->pitch
// mapping), so an identity detune would read as a false above/below — if spatial
// separation alone proves insufficient, prefer per-voice timbre (WAV variants) over
// values here. See docs/accessibility-cues-tuning.md.
constexpr float kVoiceIdentityPitch[kMaxVoicesPerCue] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };

std::vector<std::unique_ptr<Cue>>& OwnedCues() {
    static std::vector<std::unique_ptr<Cue>> cues;
    return cues;
}

std::vector<Cue*>& AllCues() {
    static std::vector<Cue*> cues;
    return cues;
}

} // namespace

Cue::Cue(const char* id, const char* name, const char* description, const char* wavPath, int maxVoices)
    : mId(id), mName(name), mDescription(description), mWavPath(wavPath),
      mVolumeCVar(std::string("gAccessibilityCueVolume.") + id) {
    CVarRegisterFloat(mVolumeCVar.c_str(), 1.0f);
    if (maxVoices < 1) {
        maxVoices = 1;
    } else if (maxVoices > kMaxVoicesPerCue) {
        maxVoices = kMaxVoicesPerCue;
    }
    mVoices.resize(maxVoices);
}

bool Cue::EnsureVoiceLoaded(Voice& voice) {
    if (voice.source != nullptr) {
        return true;
    }
    if (mLoadFailed) {
        return false;
    }
    // The callers sit on an event chain that starts in plain C (CALL_EVENT in fox_game.c),
    // so a C++ exception (the path std::string can throw bad_alloc) must not unwind past
    // here — same boundary rule the Cue3D backend applies at its extern "C" surface.
    try {
        Cue3D_Init(); // idempotent; opens the device + HRTF on first use only
        std::string path = Ship::Context::GetPathRelativeToAppDirectory(mWavPath.c_str());
        // Each voice is its own backend source (own decode of the same file); the pools
        // are small enough that sharing PCM across voices hasn't been worth a seam change.
        voice.source = Cue3D_Load(path.c_str(), true); // loop: continuous while the voice has a target
        if (voice.source == nullptr) {
            mLoadFailed = true;
            SPDLOG_WARN("Cue '{}' unavailable (failed to load '{}')", mId, path);
        }
    } catch (const std::exception& e) {
        mLoadFailed = true;
        SPDLOG_ERROR("Cue '{}' load threw: {}", mId, e.what());
    }
    return voice.source != nullptr;
}

void Cue::PushGain() {
    // The game's master volume deliberately scales the cues too: the one volume control a
    // blind player already knows about must not silently skip the second audio device
    // (review CUE3D-2). The cue master and per-cue trims layer on top for cue-only balance.
    float gain = CVarGetFloat("gGameMasterVolume", 1.0f) * CVarGetFloat(kCueMasterVolumeCVar, 1.0f) *
                 CVarGetFloat(mVolumeCVar.c_str(), 1.0f);
    if (mPreviewing) {
        // A preview is a single voice at reference loudness — no multi-voice trim, so the
        // slider maps 1:1 to what the player hears.
        Cue3D_SetGain(mVoices[0].source, gain);
        return;
    }
    int playing = 0;
    for (const Voice& voice : mVoices) {
        if (voice.playing) {
            playing++;
        }
    }
    if (playing == 0) {
        return;
    }
    // Headroom for simultaneous copies of the same loop: equal-power 1/sqrt(N), the
    // standard cheap guard against N near-coherent sources summing hot. A tuning knob —
    // if two voices by ear feel too quiet relative to one, soften or drop it.
    float trim = (playing > 1) ? 1.0f / sqrtf((float) playing) : 1.0f;
    for (const Voice& voice : mVoices) {
        if (voice.playing) {
            Cue3D_SetGain(voice.source, gain * trim);
        }
    }
}

void Cue::StopVoice(Voice& voice) {
    Cue3D_Stop(voice.source);
    voice.playing = false;
    voice.hasKey = false;
    voice.refreshed = false;
}

void Cue::SetTarget(float x, float y, float z, float pitch) {
    Voice& voice = mVoices[0];
    voice.x = x;
    voice.y = y;
    voice.z = z;
    voice.pitch = pitch;
    if (!voice.playing) {
        return; // remembered for the next Start(); a preview keeps its fixed position
    }
    Cue3D_SetPosition(voice.source, x, y, z);
    Cue3D_SetPitch(voice.source, pitch * kVoiceIdentityPitch[0]);
    PushGain(); // volume CVars are runtime-editable; one relaxed store per tick is cheap
}

void Cue::Start() {
    Voice& voice = mVoices[0];
    if (mPreviewing || voice.playing) {
        return; // already sounding, or a preview owns the cue right now
    }
    if (!EnsureVoiceLoaded(voice)) {
        return;
    }
    // Position before Play so the first audible block is already where the target is.
    Cue3D_SetPosition(voice.source, voice.x, voice.y, voice.z);
    Cue3D_SetPitch(voice.source, voice.pitch * kVoiceIdentityPitch[0]);
    voice.playing = true; // before PushGain so the headroom trim counts this voice
    PushGain();
    Cue3D_Play(voice.source);
}

void Cue::Stop() {
    Voice& voice = mVoices[0];
    if (mPreviewing || !voice.playing) {
        return; // idle: nothing to do; previewing: the UI owns the cue, don't cut it
    }
    StopVoice(voice);
}

void Cue::TargetVoice(uint64_t key, float x, float y, float z, float pitch) {
    if (mPreviewing) {
        return; // the UI owns the cue; the driver re-acquires on the tick after expiry
    }
    Voice* voice = nullptr;
    for (Voice& candidate : mVoices) {
        if (candidate.hasKey && candidate.key == key) {
            voice = &candidate;
            break;
        }
    }
    if (voice == nullptr) {
        voice = AcquireVoice(key);
    }
    if (voice == nullptr || !EnsureVoiceLoaded(*voice)) {
        return;
    }
    voice->x = x;
    voice->y = y;
    voice->z = z;
    voice->pitch = pitch;
    voice->refreshed = true;
    Cue3D_SetPosition(voice->source, x, y, z);
    Cue3D_SetPitch(voice->source, pitch * kVoiceIdentityPitch[voice - mVoices.data()]);
    bool starting = !voice->playing;
    voice->playing = true; // before PushGain so the headroom trim counts this voice
    PushGain();
    if (starting) {
        Cue3D_Play(voice->source);
    }
}

Cue::Voice* Cue::AcquireVoice(uint64_t key) {
    for (Voice& voice : mVoices) {
        if (!voice.playing) {
            voice.key = key;
            voice.hasKey = true;
            return &voice;
        }
    }
    // No silent voice: steal one whose key wasn't refreshed this tick — it's on its way
    // out at the next CueRegistry_Tick anyway. This is what makes a changed target set
    // work whether the registry tick runs before or after the driving listener: the
    // departed target's voice is reusable immediately, not only after the reap.
    for (Voice& voice : mVoices) {
        if (voice.hasKey && !voice.refreshed) {
            voice.key = key;
            return &voice;
        }
    }
    SPDLOG_TRACE("Cue '{}': no free voice for key {:#x} (pool of {})", mId, key, mVoices.size());
    return nullptr;
}

void Cue::StopAllVoices() {
    if (mPreviewing) {
        return; // gameplay voices are already silent; don't disturb the preview
    }
    for (Voice& voice : mVoices) {
        if (voice.playing) {
            StopVoice(voice);
        }
    }
}

void Cue::StartPreview() {
    Voice& voice = mVoices[0];
    if (!EnsureVoiceLoaded(voice)) {
        return;
    }
    // One preview at a time: browsing the cue list should never stack sounds.
    for (Cue* other : AllCues()) {
        if (other != this) {
            other->StopPreview();
        }
    }
    // Gameplay is interrupted, all voices; its listener re-targets after expiry.
    StopAllVoices();
    // Straight ahead at the backend's unity-gain plateau: with the distance term at 1.0,
    // the preview's loudness IS the player's volume setting. (Non-zero whenever
    // EnsureVoiceLoaded succeeded, since that implies a live backend.)
    Cue3D_SetPosition(voice.source, 0.0f, 0.0f, Cue3D_GetUnityGainDistance());
    Cue3D_SetPitch(voice.source, 1.0f);
    mPreviewing = true; // before PushGain so it takes the untrimmed preview path
    PushGain();
    Cue3D_Play(voice.source);
    mPreviewTicksLeft = kPreviewDurationTicks;
}

void Cue::StopPreview() {
    if (!mPreviewing) {
        return;
    }
    Cue3D_Stop(mVoices[0].source);
    mPreviewing = false;
}

void Cue::Tick() {
    if (mPreviewing) {
        // Gameplay pushes gain every SetTarget/TargetVoice; a preview has no target, so
        // re-read the volume CVars here so it tracks ANY volume slider (incl. game
        // master) live.
        PushGain();
        mPreviewTicksLeft -= 1.0;
        if (mPreviewTicksLeft <= 0.0) {
            StopPreview();
        }
        return;
    }
    bool reaped = false;
    for (Voice& voice : mVoices) {
        if (voice.playing && voice.hasKey && !voice.refreshed) {
            StopVoice(voice);
            reaped = true;
        }
        voice.refreshed = false;
    }
    if (reaped) {
        PushGain(); // the survivors get back the headroom the reaped voice was using
    }
}

Cue* CueRegistry_Register(const char* id, const char* name, const char* description, const char* wavPath,
                          int maxVoices) {
    CVarRegisterFloat(kCueMasterVolumeCVar, 1.0f); // idempotent; first Register wins
    CVarRegisterFloat(kCueRearCutoffCVar, CUE3D_REAR_CUTOFF_HZ_DEFAULT);
    CVarRegisterFloat(kCueRearGainDipCVar, CUE3D_REAR_GAIN_DIP_DEFAULT);
    CVarRegisterFloat(kCueRearTremoloDepthCVar, CUE3D_REAR_TREMOLO_DEPTH_DEFAULT);
    CVarRegisterFloat(kCueRearTremoloHzCVar, CUE3D_REAR_TREMOLO_HZ_DEFAULT);
    OwnedCues().emplace_back(new Cue(id, name, description, wavPath, maxVoices));
    Cue* cue = OwnedCues().back().get();
    AllCues().push_back(cue);
    return cue;
}

const std::vector<Cue*>& CueRegistry_All() {
    return AllCues();
}

void CueRegistry_Tick() {
    // Re-read the rear-effect knobs and push them to the backend, for the same reason
    // PushGain re-reads the volume CVars: they are runtime-editable from the settings
    // sliders AND from the console, and a pull-per-tick is the only wiring that honours
    // both. Four gets and four relaxed stores per 30 fps tick, so cheaper than PushGain.
    Cue3D_SetRearEffect(CVarGetFloat(kCueRearCutoffCVar, CUE3D_REAR_CUTOFF_HZ_DEFAULT),
                        CVarGetFloat(kCueRearGainDipCVar, CUE3D_REAR_GAIN_DIP_DEFAULT),
                        CVarGetFloat(kCueRearTremoloDepthCVar, CUE3D_REAR_TREMOLO_DEPTH_DEFAULT),
                        CVarGetFloat(kCueRearTremoloHzCVar, CUE3D_REAR_TREMOLO_HZ_DEFAULT));
    for (Cue* cue : AllCues()) {
        cue->Tick();
    }
}

void CueRegistry_UnloadAll() {
    for (Cue* cue : AllCues()) {
        cue->StopPreview();
        cue->StopAllVoices();
        for (Cue::Voice& voice : cue->mVoices) {
            // Cue3D_Shutdown (the caller's next step) frees the backend slots; dropping
            // the handle here is what keeps a post-shutdown listener tick harmless.
            voice = Cue::Voice{};
        }
        cue->mLoadFailed = false;
    }
}
