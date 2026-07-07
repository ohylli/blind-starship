#include "Cue.h"

#include <libultraship.h>
#include <spdlog/spdlog.h>
#include <memory>

namespace {

// Preview length in game ticks (TickPreviews runs once per 30 fps game tick): ~3 s — long
// enough to judge timbre and level, short enough to browse a list of cues by ear.
constexpr double kPreviewDurationTicks = 90.0;

std::vector<std::unique_ptr<Cue>>& OwnedCues() {
    static std::vector<std::unique_ptr<Cue>> cues;
    return cues;
}

std::vector<Cue*>& AllCues() {
    static std::vector<Cue*> cues;
    return cues;
}

} // namespace

Cue::Cue(const char* id, const char* name, const char* description, const char* wavPath)
    : mId(id), mName(name), mDescription(description), mWavPath(wavPath),
      mVolumeCVar(std::string("gAccessibilityCueVolume.") + id) {
    CVarRegisterFloat(mVolumeCVar.c_str(), 1.0f);
}

bool Cue::EnsureLoaded() {
    if (mSource != nullptr) {
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
        mSource = Cue3D_Load(path.c_str(), true); // loop: continuous while the cue has a target
        if (mSource == nullptr) {
            mLoadFailed = true;
            SPDLOG_WARN("Cue '{}' unavailable (failed to load '{}')", mId, path);
        }
    } catch (const std::exception& e) {
        mLoadFailed = true;
        SPDLOG_ERROR("Cue '{}' load threw: {}", mId, e.what());
    }
    return mSource != nullptr;
}

void Cue::PushGain() {
    if (mSource == nullptr) {
        return;
    }
    // The game's master volume deliberately scales the cues too: the one volume control a
    // blind player already knows about must not silently skip the second audio device
    // (review CUE3D-2). The cue master and per-cue trims layer on top for cue-only balance.
    float gain = CVarGetFloat("gGameMasterVolume", 1.0f) * CVarGetFloat(kCueMasterVolumeCVar, 1.0f) *
                 CVarGetFloat(mVolumeCVar.c_str(), 1.0f);
    Cue3D_SetGain(mSource, gain);
}

void Cue::SetTarget(float x, float y, float z, float pitch) {
    mTargetX = x;
    mTargetY = y;
    mTargetZ = z;
    mTargetPitch = pitch;
    if (mState != State::Playing) {
        return; // remembered for the next Start(); a preview keeps its fixed position
    }
    Cue3D_SetPosition(mSource, x, y, z);
    Cue3D_SetPitch(mSource, pitch);
    PushGain(); // volume CVars are runtime-editable; one relaxed store per tick is cheap
}

void Cue::Start() {
    if (mState != State::Idle) {
        return; // already sounding, or a preview owns the source right now
    }
    if (!EnsureLoaded()) {
        return;
    }
    // Position before Play so the first audible block is already where the target is.
    Cue3D_SetPosition(mSource, mTargetX, mTargetY, mTargetZ);
    Cue3D_SetPitch(mSource, mTargetPitch);
    PushGain();
    Cue3D_Play(mSource);
    mState = State::Playing;
}

void Cue::Stop() {
    if (mState != State::Playing) {
        return; // idle: nothing to do; previewing: the UI owns the source, don't cut it
    }
    Cue3D_Stop(mSource);
    mState = State::Idle;
}

void Cue::StartPreview() {
    if (!EnsureLoaded()) {
        return;
    }
    // One preview at a time: browsing the cue list should never stack sounds.
    for (Cue* other : AllCues()) {
        if (other != this) {
            other->StopPreview();
        }
    }
    if (mState == State::Playing) {
        Cue3D_Stop(mSource); // gameplay is interrupted; its listener re-Starts after expiry
    }
    // Straight ahead at the backend's unity-gain plateau: with the distance term at 1.0,
    // the preview's loudness IS the player's volume setting. (Non-zero whenever
    // EnsureLoaded succeeded, since that implies a live backend.)
    Cue3D_SetPosition(mSource, 0.0f, 0.0f, Cue3D_GetUnityGainDistance());
    Cue3D_SetPitch(mSource, 1.0f);
    PushGain();
    Cue3D_Play(mSource);
    mState = State::Previewing;
    mPreviewTicksLeft = kPreviewDurationTicks;
}

void Cue::StopPreview() {
    if (mState != State::Previewing) {
        return;
    }
    Cue3D_Stop(mSource);
    mState = State::Idle;
}

Cue* CueRegistry_Register(const char* id, const char* name, const char* description, const char* wavPath) {
    CVarRegisterFloat(kCueMasterVolumeCVar, 1.0f); // idempotent; first Register wins
    OwnedCues().emplace_back(new Cue(id, name, description, wavPath));
    Cue* cue = OwnedCues().back().get();
    AllCues().push_back(cue);
    return cue;
}

const std::vector<Cue*>& CueRegistry_All() {
    return AllCues();
}

void CueRegistry_TickPreviews() {
    for (Cue* cue : AllCues()) {
        if (cue->mState != Cue::State::Previewing) {
            continue;
        }
        // Gameplay pushes gain every SetTarget; a preview has no SetTarget, so re-read the
        // volume CVars here so a preview tracks ANY volume slider (incl. game master) live.
        cue->PushGain();
        cue->mPreviewTicksLeft -= 1.0;
        if (cue->mPreviewTicksLeft <= 0.0) {
            cue->StopPreview();
        }
    }
}

void CueRegistry_UnloadAll() {
    for (Cue* cue : AllCues()) {
        cue->StopPreview();
        cue->Stop();
        // Cue3D_Shutdown (the caller's next step) frees the backend slots; dropping the
        // handle here is what keeps a post-shutdown listener tick harmless.
        cue->mSource = nullptr;
        cue->mLoadFailed = false;
    }
}
