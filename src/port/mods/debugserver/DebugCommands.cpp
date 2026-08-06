// Commands for the debug control server: `health` (readiness handshake), `player` and
// `objects` (JSON dumps), the execution controls `pause` / `resume` / `step`, and the
// scenario controls `warp` / `checkpoint`. Multi-frame machinery (step's frame counting,
// warp's staged level transition) runs on game-thread event listeners registered here
// alongside the commands. Handlers write compact JSON into the console output string; the
// server wraps it in its wire envelope, the ImGui console prints it verbatim. JSON
// because the primary consumer is a script — filtering/diffing/watching happens
// client-side (docs/debug-server-plan.md).

#include "DebugCommands.h"
#include "DebugServer.h"

#include "port/CGameCompat.h"
#include "port/PlayerAim.h"
#include "port/hooks/Events.h"
#include "port/mods/ObjectSpawnLog.h"

#include <nlohmann/json.hpp>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

// Bumped by hand whenever the wire protocol or a command's output shape changes
// incompatibly; reported by `health` so clients can bail out early.
static constexpr int kProtocolVersion = 1;

static const char* GameStateName(s32 state) {
    switch (state) {
        case GSTATE_NONE:       return "GSTATE_NONE";
        case GSTATE_INIT:       return "GSTATE_INIT";
        case GSTATE_TITLE:      return "GSTATE_TITLE";
        case GSTATE_MENU:       return "GSTATE_MENU";
        case GSTATE_MAP:        return "GSTATE_MAP";
        case GSTATE_GAME_OVER:  return "GSTATE_GAME_OVER";
        case GSTATE_VS_INIT:    return "GSTATE_VS_INIT";
        case GSTATE_PLAY:       return "GSTATE_PLAY";
        case GSTATE_ENDING:     return "GSTATE_ENDING";
        case GSTATE_BOOT:       return "GSTATE_BOOT";
        case GSTATE_BOOT_WAIT:  return "GSTATE_BOOT_WAIT";
        case GSTATE_SHOW_LOGO:  return "GSTATE_SHOW_LOGO";
        case GSTATE_CHECK_SAVE: return "GSTATE_CHECK_SAVE";
        case GSTATE_LOGO_WAIT:  return "GSTATE_LOGO_WAIT";
        case GSTATE_START:      return "GSTATE_START";
        default:                return "GSTATE_UNKNOWN";
    }
}

static const char* PlayStateName(s32 state) {
    switch (state) {
        case PLAY_STANDBY: return "PLAY_STANDBY";
        case PLAY_INIT:    return "PLAY_INIT";
        case PLAY_UPDATE:  return "PLAY_UPDATE";
        case PLAY_PAUSE:   return "PLAY_PAUSE";
        default:           return "PLAY_UNKNOWN";
    }
}

static const char* PlayerStateName(s32 state) {
    switch (state) {
        case PLAYERSTATE_STANDBY:         return "PLAYERSTATE_STANDBY";
        case PLAYERSTATE_INIT:            return "PLAYERSTATE_INIT";
        case PLAYERSTATE_LEVEL_INTRO:     return "PLAYERSTATE_LEVEL_INTRO";
        case PLAYERSTATE_ACTIVE:          return "PLAYERSTATE_ACTIVE";
        case PLAYERSTATE_DOWN:            return "PLAYERSTATE_DOWN";
        case PLAYERSTATE_U_TURN:          return "PLAYERSTATE_U_TURN";
        case PLAYERSTATE_NEXT:            return "PLAYERSTATE_NEXT";
        case PLAYERSTATE_LEVEL_COMPLETE:  return "PLAYERSTATE_LEVEL_COMPLETE";
        case PLAYERSTATE_ENTER_WARP_ZONE: return "PLAYERSTATE_ENTER_WARP_ZONE";
        case PLAYERSTATE_START_360:       return "PLAYERSTATE_START_360";
        case PLAYERSTATE_GFOX_REPAIR:     return "PLAYERSTATE_GFOX_REPAIR";
        case PLAYERSTATE_ANDROSS_MOUTH:   return "PLAYERSTATE_ANDROSS_MOUTH";
        case PLAYERSTATE_UNK_12:          return "PLAYERSTATE_UNK_12";
        case PLAYERSTATE_VS_STANDBY:      return "PLAYERSTATE_VS_STANDBY";
        default:                          return "PLAYERSTATE_UNKNOWN";
    }
}

static const char* PlayerFormName(s32 form) {
    switch (form) {
        case FORM_ARWING:      return "FORM_ARWING";
        case FORM_LANDMASTER:  return "FORM_LANDMASTER";
        case FORM_BLUE_MARINE: return "FORM_BLUE_MARINE";
        case FORM_ON_FOOT:     return "FORM_ON_FOOT";
        case FORM_UNK_4:       return "FORM_UNK_4";
        case FORM_NONE:        return "FORM_NONE";
        default:               return "FORM_UNKNOWN";
    }
}

static const char* ObjStatusName(u8 status) {
    switch (status) {
        case OBJ_FREE:   return "OBJ_FREE";
        case OBJ_INIT:   return "OBJ_INIT";
        case OBJ_ACTIVE: return "OBJ_ACTIVE";
        case OBJ_DYING:  return "OBJ_DYING";
        default:         return "OBJ_UNKNOWN";
    }
}

static const char* ShotStatusName(u8 status) {
    switch (status) {
        case SHOT_FREE:    return "SHOT_FREE";
        case SHOT_ACTIVE:  return "SHOT_ACTIVE";
        case SHOT_HITMARK: return "SHOT_HITMARK";
        default:           return "SHOT_UNKNOWN";
    }
}

static nlohmann::json Vec3(const Vec3f& v) {
    return nlohmann::json{ { "x", v.x }, { "y", v.y }, { "z", v.z } };
}

// gPlayer is heap-allocated per level and left dangling by Memory_FreeAll between levels,
// so the state check is the real guard; the null check only covers first boot.
static bool InPlayMode() {
    return (gGameState == GSTATE_PLAY) && (gPlayState > PLAY_INIT) && (gPlayer != NULL);
}

// Per-command precondition for everything that dereferences play-mode state. Returning a
// clean error beats crashing on a dangling gPlayer when invoked from a menu.
static bool RequirePlay(std::string* output) {
    if (InPlayMode()) {
        return true;
    }
    if (output != nullptr) {
        *output += "requires play mode (currently ";
        *output += GameStateName(gGameState);
        *output += ")";
    }
    return false;
}

static bool IsDebugPaused() {
    return CVarGetInteger("gDebugPause", 0) != 0;
}

// Defined in the warp section below; pause/step refuse to fight an in-flight warp, whose
// listener holds the pause off until the arrival completes.
static bool WarpInFlight();

static int32_t HealthHandler(std::shared_ptr<Ship::Console> console, const std::vector<std::string>& args,
                             std::string* output) {
    nlohmann::json j;
    j["server"] = kProtocolVersion;
    j["gameState"] = (s32) gGameState;
    j["gameStateName"] = GameStateName(gGameState);
    j["playState"] = gPlayState;
    j["playStateName"] = PlayStateName(gPlayState);
    j["level"] = (s32) gCurrentLevel;
    j["levelName"] = Starship_LevelName(gCurrentLevel);
    j["levelMode"] = (s32) gLevelMode;
    j["frame"] = gGameFrameCount;
    j["players"] = gCamCount;
    j["paused"] = IsDebugPaused();
    // The guard predicate, pre-evaluated: tells clients whether guarded commands will
    // succeed without having to try one.
    j["playMode"] = InPlayMode();
    if (output != nullptr) {
        *output += j.dump();
    }
    return 0;
}

// --- pause / resume / step ---------------------------------------------------------------
//
// One shared pause state: gDebugPause, the same CVar the in-game L-trigger shortcut and
// the F1 Frame Advance machinery drive (src/port/mods/PortEnhancements.c). Pausing from
// the server and resuming with L in game (or vice versa) therefore compose freely, and
// the cues keep sounding at their frozen positions while paused — the cue listeners run
// on GamePostUpdateEvent, which is not on the cancelled play-update chain.
//
// `step [n]` lets exactly n play frames through: it unpauses and counts PlayUpdateEvents
// that actually ran. Its listener registers at EVENT_PRIORITY_HIGH so it runs after the
// pause handler (EventSystem calls listeners in ascending priority order) and sees the
// final cancelled flag; on the frame the count hits zero it re-sets gDebugPause, which
// takes effect from the next play update — the n-th frame itself still runs.

// Reject step counts that could truncate on the long long -> int narrowing or run the
// game away unpaused for hours; generous enough for any real use (~an hour of play).
static constexpr long long kMaxStepFrames = 100000;

// State of the step in flight. Each `step` request allocates a fresh StepState and its
// deferred poll captures the shared_ptr, so a poll always answers from the request it
// belongs to — even if another frontend arms a new step on the very tick the old one
// finished (a new request replaces sStep, never mutates a predecessor).
struct StepState {
    int requested = 0;
    int remaining = 0;               // > 0 while the step is in flight
    int run = 0;                     // play frames that actually executed
    const char* endedEarly = nullptr; // why the step stopped short, for the response note
};
static std::shared_ptr<StepState> sStep; // game thread only

static bool StepInFlight() {
    return sStep != nullptr && sStep->remaining > 0;
}

static void StepCancel(const char* why) {
    if (StepInFlight()) {
        sStep->remaining = 0;
        sStep->endedEarly = why;
    }
}

static void StepOnPlayUpdate(IEvent* event) {
    if (!StepInFlight()) {
        return;
    }
    if (event->cancelled) {
        return; // something re-paused mid-step (e.g. the L shortcut); hold the count
    }
    sStep->run++;
    if (--sStep->remaining == 0) {
        CVarSetInteger("gDebugPause", 1);
    }
}

// Fires every tick, in play mode or not — the one cleanup path that also covers steps
// started from the ImGui console, which have no deferred poll. Without it, a step that
// outlives its level (death, level clear) would keep its count armed, reject every later
// `step` as "already in progress", and silently re-pause the next level that many frames
// in. The socket poll only observes completion; it never has to enforce it.
static void StepOnGamePostUpdate(IEvent* event) {
    if (StepInFlight() && !InPlayMode()) {
        StepCancel("play mode ended before the step completed");
    }
}

// `frame` is gGameFrameCount, which Play_Main increments *before* the cancellable
// play-update event: it keeps ticking while debug-paused. It timestamps the response but
// must not be used to count elapsed simulation frames — that is the step reply's
// `framesRun`.
static std::string PauseStateJson() {
    nlohmann::json j;
    j["paused"] = IsDebugPaused();
    j["frame"] = gGameFrameCount;
    return j.dump();
}

static int32_t PauseHandler(std::shared_ptr<Ship::Console> console, const std::vector<std::string>& args,
                            std::string* output) {
    if (!RequirePlay(output)) {
        return 1;
    }
    if (WarpInFlight()) {
        if (output != nullptr) {
            *output += "a warp is in progress; it clears the pause until the arrival completes (use warp --paused)";
        }
        return 1;
    }
    StepCancel("cancelled by pause"); // a pause beats an in-flight step: freeze now, don't finish it
    CVarSetInteger("gDebugPause", 1);
    if (output != nullptr) {
        *output += PauseStateJson();
    }
    return 0;
}

// No play-mode guard: resume only clears a flag, and must work anywhere so a pause that
// outlived its level (or was set by mistake) can always be cleared.
static int32_t ResumeHandler(std::shared_ptr<Ship::Console> console, const std::vector<std::string>& args,
                             std::string* output) {
    StepCancel("cancelled by resume"); // resume means run freely, not "finish the step and repause"
    CVarSetInteger("gDebugPause", 0);
    if (output != nullptr) {
        *output += PauseStateJson();
    }
    return 0;
}

static int32_t StepHandler(std::shared_ptr<Ship::Console> console, const std::vector<std::string>& args,
                           std::string* output) {
    if (!RequirePlay(output)) {
        return 1;
    }
    // The play-mode guard passes during PLAY_PAUSE (the in-game pause menu, where play
    // updates don't run) — stepping there would just hang until the menu closes.
    if (gPlayState != PLAY_UPDATE) {
        if (output != nullptr) {
            *output += "requires live gameplay (currently ";
            *output += PlayStateName(gPlayState);
            *output += ")";
        }
        return 1;
    }
    if (StepInFlight()) {
        if (output != nullptr) {
            *output += "a step is already in progress";
        }
        return 1;
    }
    if (WarpInFlight()) {
        if (output != nullptr) {
            *output += "a warp is in progress (warp --paused, then step, to count from the arrival)";
        }
        return 1;
    }
    long long n = 1;
    if (args.size() > 1) {
        char* end = nullptr;
        errno = 0;
        n = strtoll(args[1].c_str(), &end, 10);
        // The overflow check matters: unchecked, a huge count either truncates to <= 0 on
        // the int narrowing (arming nothing after gDebugPause was already cleared — a
        // `step` that silently resumes) or saturates into a billions-of-frames run.
        if (end == args[1].c_str() || *end != '\0' || errno == ERANGE || n < 1 || n > kMaxStepFrames) {
            if (output != nullptr) {
                *output += "step count must be an integer between 1 and ";
                *output += std::to_string(kMaxStepFrames);
            }
            return 1;
        }
    }
    auto step = std::make_shared<StepState>();
    step->requested = (int) n;
    step->remaining = (int) n;
    sStep = step; // arms the listener
    CVarSetInteger("gDebugPause", 0);
    // Answer only once the frames have actually elapsed, so a client can `step 30` and
    // immediately dump post-step state. Ends early (with the partial count and a note)
    // when play mode ends mid-step or a pause/resume cancels it — the listeners above
    // clear `remaining` in every such case, so completion is the only condition.
    bool deferred = DebugServer_Defer([step](std::string* out) {
        if (step->remaining > 0) {
            return false;
        }
        if (out != nullptr) {
            nlohmann::json j;
            j["requested"] = step->requested;
            j["framesRun"] = step->run;
            j["paused"] = IsDebugPaused();
            j["frame"] = gGameFrameCount;
            if (step->endedEarly != nullptr) {
                j["note"] = step->endedEarly;
            }
            *out += j.dump();
        }
        return true;
    });
    if (!deferred && output != nullptr) {
        // In-game ImGui console: acknowledge now; the listener still steps and re-pauses.
        nlohmann::json j;
        j["stepping"] = (int) n;
        *output += j.dump();
    }
    return 0;
}

// --- warp / checkpoint -------------------------------------------------------------------
//
// `warp <level> [phase]` enters any level from any post-boot state through the game's own
// transition seam — gNextLevel / gNextLevelPhase / gNextGameState, consumed by
// Game_SetGameState (fox_game.c) — the same route the in-play level transitions and the
// MODS_BOOT_STATE hack use. (The map screen assigns gGameState directly instead; the
// gNextGameState route is a superset of that — it also frees level memory, clears the
// object arrays, and fades audio, which is what makes it safe from any state.) The
// mission briefing belongs to the map screen's flow, so a warp never shows it. Like
// `step`, the transition is driven by an
// unconditional game-thread listener rather than the socket poll, because a warp started
// from the ImGui console has no poll: the listener enforces, the poll only observes.
//
// Stages: WAIT_WARPABLE (post-boot state reached) -> kickoff -> WAIT_STANDBY (the new
// level's PLAY_STANDBY frames — at least three of them, gNextGameStateTimer starts at 3 —
// where --no-intro clears the play-the-intro flag D_ctx_8017782C, taking the same
// no-cutscene path a death restart takes) -> WAIT_PLAY (Play_Init done, first play frame
// reached) -> DONE.
//
// Checkpoints: `checkpoint` captures the (pathProgress, objectLoadIndex, groundSurface)
// tuple the game's own mid-level respawn uses, with the same formula as the F1 "Set
// Checkpoint" button; `warp ... --at <p> --load <n> [--ground <g>]` feeds a captured
// tuple back through DebugServer_GetCheckpointOverride, which Player_Setup (fox_play.c)
// consults instead of the gCheckpoint CVars for that one level start. A checkpoint start
// forces the intro skip: the game's own gate (Player_Setup) skips the cutscene only when
// the restored *object-load index* is nonzero — a capture from the first stretch of a
// level would otherwise still play it, and on Corneria have it overwrite the restored
// ground surface — so the warp sets noIntro itself whenever checkpoint data is given.
// --fresh overrides with the untouched level-start defaults, i.e. suppresses a configured
// gCheckpoint CVar for one start. Naming and storage of captured checkpoints is the
// client's job (tools/debug_client.py, tools/checkpoints.json).
//
// "Advance N frames after arrival, then inspect" is deliberately not a warp option: it is
// `warp <level> --paused` followed by `step N`.

static constexpr int kWarpTimeoutTicks = 1800; // ~60 s of game ticks; covers a slow boot

struct WarpState {
    enum Stage { WAIT_WARPABLE, WAIT_STANDBY, WAIT_PLAY, DONE };
    Stage stage = WAIT_WARPABLE;
    s32 level = 0;
    s32 phase = 0;
    bool noIntro = false;
    bool paused = false;
    bool fresh = false;
    bool haveCheckpoint = false;
    f32 pathProgress = 0.0f;
    s32 objectLoadIndex = 0;
    s32 groundSurface = -1; // -1: keep the level default
    bool completed = false;
    bool introFlagCleared = false;
    bool checkpointConsumed = false;
    int ticks = 0;
    const char* note = nullptr;
};
static std::shared_ptr<WarpState> sWarp; // game thread only

static bool WarpInFlight() {
    return sWarp != nullptr && sWarp->stage != WarpState::DONE;
}

// GSTATE_INIT is deliberately not warpable: its Game_Update case performs the base
// bootstrap (lives, team shields, forms, volumes) and clobbers gNextGameState, so a warp
// must wait until it has run and settled into GSTATE_TITLE. The boot states (>=
// GSTATE_BOOT) simply have not reached that bootstrap yet.
static bool WarpableNow() {
    switch (gGameState) {
        case GSTATE_TITLE:
        case GSTATE_MENU:
        case GSTATE_MAP:
        case GSTATE_PLAY:
        case GSTATE_GAME_OVER:
        case GSTATE_ENDING:
            return true;
        default:
            return false;
    }
}

// The level-select mod's overlay function; sets the level's SFX channel allocation, which
// the map screen normally does during the briefing.
extern "C" void Map_LevelStart_AudioSpecSetup(LevelId level);

static void WarpOnGamePostUpdate(IEvent* event) {
    if (!WarpInFlight()) {
        return;
    }
    WarpState& warp = *sWarp;
    if (++warp.ticks > kWarpTimeoutTicks) {
        if (warp.note == nullptr) { // an earlier note is the better diagnostic; completed:false says timeout
            warp.note = "warp timed out before the level came up";
        }
        warp.stage = WarpState::DONE;
        return;
    }
    // Hold the pause off for the whole transition, not just at kickoff: a `pause` command
    // slipping in or the in-game L shortcut would cancel the play updates the arrival
    // needs (see the kickoff comment) and stall the warp to the timeout. --paused still
    // applies at completion.
    if ((warp.stage == WarpState::WAIT_STANDBY || warp.stage == WarpState::WAIT_PLAY) && IsDebugPaused()) {
        CVarSetInteger("gDebugPause", 0);
    }
    switch (warp.stage) {
        case WarpState::WAIT_WARPABLE:
            if (!WarpableNow()) {
                return;
            }
            gNextLevel = warp.level;
            gNextLevelPhase = warp.phase;
            gNextGameState = GSTATE_PLAY;
            Map_LevelStart_AudioSpecSetup((LevelId) warp.level);
            // Always clear the pause for the transition: the level start needs one live
            // play frame — Player_Setup (intro decision, checkpoint restore) runs from
            // the player-state machine inside the cancellable play update, not from
            // Play_Init — so a standing pause would freeze the arrival half-initialized.
            // --paused is applied at completion instead (see WAIT_PLAY).
            CVarSetInteger("gDebugPause", 0);
            warp.stage = WarpState::WAIT_STANDBY;
            return;
        case WarpState::WAIT_STANDBY:
            if (gGameState != GSTATE_PLAY || gCurrentLevel != warp.level) {
                return;
            }
            if (gPlayState == PLAY_STANDBY) {
                if (warp.noIntro) {
                    D_ctx_8017782C = false; // Play_Setup re-set it; Play_Init has not read it yet
                    warp.introFlagCleared = true;
                }
                warp.stage = WarpState::WAIT_PLAY;
            } else {
                // Belt and suspenders: the standby window should be unmissable (see the
                // header comment), but if it ever is, keep going rather than hang.
                if (warp.noIntro) {
                    warp.note = "level start window missed; intro not skipped";
                }
                warp.stage = WarpState::WAIT_PLAY;
            }
            return;
        case WarpState::WAIT_PLAY:
            // "The level is up" means the first play update actually ran: gPlayState
            // reaching PLAY_UPDATE only says Play_Init finished — the player leaves
            // PLAYERSTATE_INIT when Player_Setup ran inside that first update, which is
            // where the intro decision and the checkpoint restore live.
            if (gGameState == GSTATE_PLAY && gCurrentLevel == warp.level && gPlayState > PLAY_INIT &&
                gPlayer != NULL && gPlayer[0].state != PLAYERSTATE_INIT) {
                if (warp.paused) {
                    CVarSetInteger("gDebugPause", 1); // freeze right after the first play frame
                }
                warp.completed = true;
                warp.stage = WarpState::DONE;
            }
            return;
        default:
            return;
    }
}

// Consulted by Player_Setup (fox_play.c) in place of the gCheckpoint CVar read. Active
// only between kickoff and completion of a warp that asked for an override, and only for
// the warp's own level, so every other level start behaves as stock.
extern "C" bool DebugServer_GetCheckpointOverride(int32_t* groundSurface, float* pathProgress,
                                                  int32_t* objectLoadIndex) {
    if (sWarp == nullptr || (sWarp->stage != WarpState::WAIT_STANDBY && sWarp->stage != WarpState::WAIT_PLAY)) {
        return false;
    }
    if (!sWarp->haveCheckpoint && !sWarp->fresh) {
        return false;
    }
    if (sWarp->checkpointConsumed) {
        return false; // one level start only; a second Player_Setup inside the window is stock
    }
    if (gCurrentLevel != sWarp->level) {
        return false;
    }
    if (sWarp->haveCheckpoint) {
        if (sWarp->groundSurface >= 0) {
            *groundSurface = sWarp->groundSurface;
        }
        *pathProgress = sWarp->pathProgress;
        *objectLoadIndex = sWarp->objectLoadIndex;
    } // --fresh: leave the Play_Setup defaults untouched
    sWarp->checkpointConsumed = true;
    return true;
}

// Levels warp accepts, with the client-facing names. LEVEL_UNK_15 (a title-scene stub)
// and LEVEL_VERSUS (needs the whole VS setup path) are deliberately absent.
struct WarpLevelName {
    const char* name;
    s32 level;
};
static const WarpLevelName kWarpLevels[] = {
    { "corneria", LEVEL_CORNERIA },      { "meteo", LEVEL_METEO },     { "sector-x", LEVEL_SECTOR_X },
    { "area-6", LEVEL_AREA_6 },          { "beta-sb", LEVEL_UNK_4 },   { "sector-y", LEVEL_SECTOR_Y },
    { "venom-1", LEVEL_VENOM_1 },        { "solar", LEVEL_SOLAR },     { "zoness", LEVEL_ZONESS },
    { "andross", LEVEL_VENOM_ANDROSS },  { "training", LEVEL_TRAINING }, { "macbeth", LEVEL_MACBETH },
    { "titania", LEVEL_TITANIA },        { "aquas", LEVEL_AQUAS },     { "fortuna", LEVEL_FORTUNA },
    { "katina", LEVEL_KATINA },          { "bolse", LEVEL_BOLSE },     { "sector-z", LEVEL_SECTOR_Z },
    { "venom-2", LEVEL_VENOM_2 },
};

// Lowercased alphanumerics only, with a leading "level" dropped: "Sector-X", "sector_x",
// and "LEVEL_SECTOR_X" all normalize to "sectorx".
static std::string NormalizeLevelName(const std::string& raw) {
    std::string s;
    for (char c : raw) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            s += (char) std::tolower(static_cast<unsigned char>(c));
        }
    }
    if (s.rfind("level", 0) == 0) {
        s.erase(0, 5);
    }
    return s;
}

static bool ResolveWarpLevel(const std::string& token, s32* outLevel) {
    char* end = nullptr;
    long v = strtol(token.c_str(), &end, 10);
    if (end != token.c_str() && *end == '\0') {
        for (const auto& entry : kWarpLevels) {
            if (entry.level == (s32) v) {
                *outLevel = entry.level;
                return true;
            }
        }
        return false;
    }
    std::string wanted = NormalizeLevelName(token);
    if (wanted.empty()) {
        return false;
    }
    for (const auto& entry : kWarpLevels) {
        if (NormalizeLevelName(entry.name) == wanted) {
            *outLevel = entry.level;
            return true;
        }
    }
    return false;
}

static std::string WarpLevelNameList() {
    std::string s;
    for (const auto& entry : kWarpLevels) {
        if (!s.empty()) {
            s += ", ";
        }
        s += entry.name;
    }
    return s;
}

static bool ParseWarpLong(const std::vector<std::string>& args, size_t* i, long min, long max, long* out,
                          std::string* output) {
    if (*i + 1 >= args.size()) {
        if (output != nullptr) {
            *output += args[*i] + " needs a value";
        }
        return false;
    }
    const std::string& value = args[++*i];
    char* end = nullptr;
    errno = 0;
    long v = strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' || errno == ERANGE || v < min || v > max) {
        if (output != nullptr) {
            *output += args[*i - 1] + " value out of range: " + value;
        }
        return false;
    }
    *out = v;
    return true;
}

static nlohmann::json WarpResultJson(const WarpState& warp) {
    nlohmann::json j;
    j["completed"] = warp.completed;
    j["level"] = warp.level;
    j["levelName"] = Starship_LevelName(warp.level);
    j["phase"] = (s32) gLevelPhase;
    j["paused"] = IsDebugPaused();
    j["introSkipped"] = warp.introFlagCleared;
    if (warp.haveCheckpoint || warp.fresh) {
        j["checkpointApplied"] = warp.checkpointConsumed;
    }
    j["gameStateName"] = GameStateName(gGameState); // mostly for the timeout case
    j["frame"] = gGameFrameCount;
    if (warp.note != nullptr) {
        j["note"] = warp.note;
    }
    return j;
}

static int32_t WarpHandler(std::shared_ptr<Ship::Console> console, const std::vector<std::string>& args,
                           std::string* output) {
    if (gVersusMode) {
        if (output != nullptr) {
            *output += "warp is not available in VS mode";
        }
        return 1;
    }
    if (WarpInFlight()) {
        if (output != nullptr) {
            *output += "a warp is already in progress";
        }
        return 1;
    }

    auto warp = std::make_shared<WarpState>();
    long ground = -1;
    bool haveAt = false;
    bool haveLoad = false;
    bool haveGround = false;
    int positional = 0;
    for (size_t i = 1; i < args.size(); i++) {
        const std::string& a = args[i];
        if (a == "--no-intro") {
            warp->noIntro = true;
        } else if (a == "--paused") {
            warp->paused = true;
        } else if (a == "--fresh") {
            warp->fresh = true;
        } else if (a == "--at") {
            if (i + 1 >= args.size()) {
                if (output != nullptr) {
                    *output += "--at needs a value";
                }
                return 1;
            }
            const std::string& value = args[++i];
            char* end = nullptr;
            errno = 0;
            float v = strtof(value.c_str(), &end);
            // isfinite: strtof accepts "inf" without ERANGE, and an infinite path
            // progress poisons the path/camera math with NaN from the first frame.
            if (end == value.c_str() || *end != '\0' || errno == ERANGE || !std::isfinite(v) || !(v >= 0.0f)) {
                if (output != nullptr) {
                    *output += "--at value must be a non-negative number: " + value;
                }
                return 1;
            }
            warp->pathProgress = v;
            haveAt = true;
        } else if (a == "--load") {
            long v = 0;
            if (!ParseWarpLong(args, &i, 0, 10000, &v, output)) {
                return 1;
            }
            warp->objectLoadIndex = (s32) v;
            haveLoad = true;
        } else if (a == "--ground") {
            if (!ParseWarpLong(args, &i, 0, 100, &ground, output)) {
                return 1;
            }
            haveGround = true;
        } else if (a.rfind("--", 0) == 0) {
            if (output != nullptr) {
                *output += "unknown warp option: " + a;
            }
            return 1;
        } else if (positional == 0) {
            if (!ResolveWarpLevel(a, &warp->level)) {
                if (output != nullptr) {
                    *output += "unknown level: " + a + " (one of: " + WarpLevelNameList() + ")";
                }
                return 1;
            }
            positional++;
        } else if (positional == 1) {
            char* end = nullptr;
            errno = 0;
            long v = strtol(a.c_str(), &end, 10);
            if (end == a.c_str() || *end != '\0' || errno == ERANGE || v < 0 || v > 2) {
                if (output != nullptr) {
                    *output += "phase must be 0..2: " + a;
                }
                return 1;
            }
            warp->phase = (s32) v;
            positional++;
        } else {
            if (output != nullptr) {
                *output += "unexpected argument: " + a;
            }
            return 1;
        }
    }
    if (positional == 0) {
        if (output != nullptr) {
            *output += "usage: warp <level> [phase] [--no-intro] [--paused] [--fresh] [--at <p> --load <n> "
                       "[--ground <g>]]; levels: " +
                       WarpLevelNameList();
        }
        return 1;
    }
    if (haveAt != haveLoad) {
        if (output != nullptr) {
            *output += "--at and --load must be given together (both come from a `checkpoint` capture)";
        }
        return 1;
    }
    // The override only applies alongside checkpoint data; a lone --ground would parse
    // fine and then be silently dropped.
    if (haveGround && !haveAt) {
        if (output != nullptr) {
            *output += "--ground requires --at and --load (all three come from a `checkpoint` capture)";
        }
        return 1;
    }
    warp->haveCheckpoint = haveAt;
    warp->groundSurface = (s32) ground;
    if (warp->fresh && warp->haveCheckpoint) {
        if (output != nullptr) {
            *output += "--fresh and --at/--load are mutually exclusive";
        }
        return 1;
    }
    // A checkpoint start always skips the intro — see the section comment: the game's own
    // gate would still play it (and clobber the restored Corneria ground surface) when
    // the restored object-load index is 0.
    if (warp->haveCheckpoint) {
        warp->noIntro = true;
    }

    StepCancel("cancelled by warp"); // the level the step counted is going away
    sWarp = warp;                    // arms the listener

    bool deferred = DebugServer_Defer([warp](std::string* out) {
        if (warp->stage != WarpState::DONE) {
            return false;
        }
        if (out != nullptr) {
            *out += WarpResultJson(*warp).dump();
        }
        return true;
    });
    if (!deferred && output != nullptr) {
        // In-game ImGui console: acknowledge now; the listener still runs the warp.
        nlohmann::json j;
        j["warping"] = Starship_LevelName(warp->level);
        *output += j.dump();
    }
    return 0;
}

static int32_t CheckpointHandler(std::shared_ptr<Ship::Console> console, const std::vector<std::string>& args,
                                 std::string* output) {
    if (!RequirePlay(output)) {
        return 1;
    }
    if (gVersusMode) {
        if (output != nullptr) {
            *output += "checkpoint capture is not available in VS mode";
        }
        return 1;
    }
    // The saved-progress mechanism is on-rails only (Play_Init fast-forwards the object
    // load index along the corridor path); all-range mid-level state is a different
    // machine (gAllRangeCheckpoint) not covered here.
    if (gLevelMode != LEVELMODE_ON_RAILS) {
        if (output != nullptr) {
            *output += "checkpoints only exist on on-rails levels (current mode is all-range)";
        }
        return 1;
    }
    // During intros/U-turns/level complete the position is a cutscene position; a
    // checkpoint captured there would respawn somewhere the player never flew.
    if (gPlayer[0].state != PLAYERSTATE_ACTIVE) {
        if (output != nullptr) {
            *output += "capture requires normal flight (currently ";
            *output += PlayerStateName(gPlayer[0].state);
            *output += ")";
        }
        return 1;
    }
    // Same formula as the F1 "Set Checkpoint" button: respawn ~250 units before the
    // player's current position along the path — clamped to the level start, so a capture
    // taken in the first 250 path units still replays (warp --at rejects negatives).
    f32 pathProgress = (-gPlayer[0].pos.z) - 250.0f;
    if (pathProgress < 0.0f) {
        pathProgress = 0.0f;
    }
    nlohmann::json j;
    j["level"] = (s32) gCurrentLevel;
    j["levelName"] = Starship_LevelName(gCurrentLevel);
    // The phase distinguishes the warp-zone alternate routes (Meteo, Sector X): they are
    // on-rails with their own object tables, so a capture there must replay into the same
    // phase. Stored by the client, replayed as warp's positional phase argument.
    j["phase"] = (s32) gLevelPhase;
    j["pathProgress"] = pathProgress;
    j["objectLoadIndex"] = (s32) gObjectLoadIndex;
    j["groundSurface"] = (s32) gGroundSurface;
    j["frame"] = gGameFrameCount;
    if (output != nullptr) {
        *output += j.dump();
    }
    return 0;
}

static nlohmann::json DumpPlayer(const Player& p) {
    nlohmann::json j;
    j["num"] = p.num;
    j["state"] = (s32) p.state;
    j["stateName"] = PlayerStateName(p.state);
    j["form"] = (s32) p.form;
    j["formName"] = PlayerFormName(p.form);
    j["pos"] = Vec3(p.pos); // pos.z is progress along the level path, not world Z
    j["trueZpos"] = p.trueZpos;
    j["rot"] = Vec3(p.rot);
    // `aimYaw`/`aimPitch` are the flight direction — derivation, live verification, and
    // the faceYaw draw convention are documented on the shared helpers in
    // port/PlayerAim.h. They only hold for the forms that fly by the Arwing composition
    // (Arwing, Blue Marine); the Landmaster and on-foot Fox compose velocity differently,
    // so those forms report null rather than a number that is not the flight direction.
    j["heading"] = { { "yRot", p.yRot_114 },
                     { "xRot", p.xRot_120 },
                     { "yRotVel", p.yRotVel_11C },
                     { "aerobaticPitch", p.aerobaticPitch },
                     { "somersault", (bool) p.somersault } };
    if (Player_AimAnglesValid(p)) {
        j["heading"]["aimYaw"] = Player_AimYaw(p);
        j["heading"]["aimPitch"] = Player_AimPitch(p);
        j["heading"]["faceYaw"] = Player_FaceYaw(p);
    } else {
        j["heading"]["aimYaw"] = nullptr;
        j["heading"]["aimPitch"] = nullptr;
        j["heading"]["faceYaw"] = nullptr;
    }
    j["vel"] = Vec3(p.vel);
    j["baseSpeed"] = p.baseSpeed;
    j["boostSpeed"] = p.boostSpeed;
    j["shields"] = p.shields;
    j["damage"] = p.damage;
    j["boost"] = { { "meter", p.boostMeter }, { "active", (bool) p.boostActive }, //
                   { "cooldown", (bool) p.boostCooldown } };
    j["wings"] = { { "left", p.arwing.leftWingState }, { "right", p.arwing.rightWingState } };
    j["cam"] = { { "eye", Vec3(p.cam.eye) }, { "at", Vec3(p.cam.at) }, //
                 { "yaw", p.camYaw }, { "pitch", p.camPitch } };
    j["mercyTimer"] = p.mercyTimer;
    j["hitTimer"] = p.hitTimer;
    j["grounded"] = (bool) p.grounded;
    j["csState"] = p.csState;
    return j;
}

static int32_t PlayerHandler(std::shared_ptr<Ship::Console> console, const std::vector<std::string>& args,
                             std::string* output) {
    if (!RequirePlay(output)) {
        return 1;
    }
    int first = 0;
    int last = gCamCount - 1;
    if (args.size() > 1) {
        char* end = nullptr;
        long index = strtol(args[1].c_str(), &end, 10);
        if (end == args[1].c_str() || *end != '\0' || index < 0 || index >= gCamCount) {
            if (output != nullptr) {
                *output += "player index must be 0.." + std::to_string(gCamCount - 1);
            }
            return 1;
        }
        first = last = (int) index;
    }
    nlohmann::json j;
    j["frame"] = gGameFrameCount;
    auto players = nlohmann::json::array();
    for (int i = first; i <= last; i++) {
        players.push_back(DumpPlayer(gPlayer[i]));
    }
    j["players"] = std::move(players);
    if (output != nullptr) {
        *output += j.dump();
    }
    return 0;
}

// Identity for client-side diffing is (array, index, id, eventType-for-actors) — slots
// are reused, so a FREE->INIT transition on the same index means a new entity.
static nlohmann::json ObjectCommon(int index, const Object& obj) {
    nlohmann::json j;
    j["index"] = index;
    j["id"] = obj.id;
    j["name"] = ObjectId_GetName(obj.id);
    j["status"] = obj.status;
    j["statusName"] = ObjStatusName(obj.status);
    j["pos"] = Vec3(obj.pos);
    j["rot"] = Vec3(obj.rot);
    return j;
}

static nlohmann::json DumpActors() {
    auto arr = nlohmann::json::array();
    for (int i = 0; i < ARRAY_COUNT(gActors); i++) {
        const Actor& actor = gActors[i];
        if (actor.obj.status == OBJ_FREE) {
            continue;
        }
        auto j = ObjectCommon(i, actor.obj);
        if (actor.obj.id == OBJ_ACTOR_EVENT) {
            j["eventType"] = actor.eventType;
            j["eventName"] = EventId_GetName(actor.eventType);
        }
        j["state"] = actor.state;
        j["health"] = actor.health;
        j["aiType"] = actor.aiType;
        j["aiIndex"] = actor.aiIndex;
        j["vel"] = Vec3(actor.vel);
        j["scale"] = actor.scale;
        arr.push_back(std::move(j));
    }
    return arr;
}

static nlohmann::json DumpBosses() {
    auto arr = nlohmann::json::array();
    for (int i = 0; i < ARRAY_COUNT(gBosses); i++) {
        const Boss& boss = gBosses[i];
        if (boss.obj.status == OBJ_FREE) {
            continue;
        }
        auto j = ObjectCommon(i, boss.obj);
        j["state"] = boss.state;
        j["health"] = boss.health;
        j["vel"] = Vec3(boss.vel);
        arr.push_back(std::move(j));
    }
    return arr;
}

static nlohmann::json DumpItems() {
    auto arr = nlohmann::json::array();
    for (int i = 0; i < ARRAY_COUNT(gItems); i++) {
        const Item& item = gItems[i];
        if (item.obj.status == OBJ_FREE) {
            continue;
        }
        auto j = ObjectCommon(i, item.obj);
        j["state"] = item.state;
        j["collected"] = item.collected;
        arr.push_back(std::move(j));
    }
    return arr;
}

static nlohmann::json DumpEffects() {
    auto arr = nlohmann::json::array();
    for (int i = 0; i < ARRAY_COUNT(gEffects); i++) {
        const Effect& effect = gEffects[i];
        if (effect.obj.status == OBJ_FREE) {
            continue;
        }
        auto j = ObjectCommon(i, effect.obj);
        j["state"] = effect.state;
        j["vel"] = Vec3(effect.vel);
        j["scale1"] = effect.scale1;
        j["scale2"] = effect.scale2;
        arr.push_back(std::move(j));
    }
    return arr;
}

static nlohmann::json DumpSprites() {
    auto arr = nlohmann::json::array();
    for (int i = 0; i < ARRAY_COUNT(gSprites); i++) {
        const Sprite& sprite = gSprites[i];
        if (sprite.obj.status == OBJ_FREE) {
            continue;
        }
        auto j = ObjectCommon(i, sprite.obj);
        j["sceneryId"] = sprite.sceneryId;
        j["destroy"] = sprite.destroy;
        j["toLeft"] = sprite.toLeft;
        arr.push_back(std::move(j));
    }
    return arr;
}

static nlohmann::json DumpScenery() {
    auto arr = nlohmann::json::array();
    for (int i = 0; i < ARRAY_COUNT(gScenery); i++) {
        const Scenery& scenery = gScenery[i];
        if (scenery.obj.status == OBJ_FREE) {
            continue;
        }
        auto j = ObjectCommon(i, scenery.obj);
        j["state"] = scenery.state;
        j["vel"] = Vec3(scenery.vel);
        arr.push_back(std::move(j));
    }
    return arr;
}

static nlohmann::json DumpScenery360() {
    auto arr = nlohmann::json::array();
    // MEM_ARRAY_ALLOCATE(gScenery360, 200) in fox_play.c — heap array, no ARRAY_COUNT.
    for (int i = 0; i < 200; i++) {
        const Scenery360& scenery = gScenery360[i];
        if (scenery.obj.status == OBJ_FREE) {
            continue;
        }
        auto j = ObjectCommon(i, scenery.obj);
        j["pathIndex"] = scenery.pathIndex;
        arr.push_back(std::move(j));
    }
    return arr;
}

// PlayerShot's obj.id is a PlayerShotId and obj.status a PlayerShotStatus, so no
// ObjectId_GetName here.
static nlohmann::json DumpShots() {
    auto arr = nlohmann::json::array();
    for (int i = 0; i < ARRAY_COUNT(gPlayerShots); i++) {
        const PlayerShot& shot = gPlayerShots[i];
        if (shot.obj.status == SHOT_FREE) {
            continue;
        }
        nlohmann::json j;
        j["index"] = i;
        j["id"] = shot.obj.id;
        j["status"] = shot.obj.status;
        j["statusName"] = ShotStatusName(shot.obj.status);
        j["pos"] = Vec3(shot.obj.pos);
        j["rot"] = Vec3(shot.obj.rot);
        j["vel"] = Vec3(shot.vel);
        j["scale"] = shot.scale;
        j["timer"] = shot.timer;
        j["sourceId"] = shot.sourceId;
        arr.push_back(std::move(j));
    }
    return arr;
}

static const char* const kObjectArrayNames[] = {
    "actors", "bosses", "items", "effects", "sprites", "scenery", "scenery360", "shots",
};

static bool DumpObjectArray(const std::string& name, nlohmann::json& arrays) {
    if (name == "actors") {
        arrays["actors"] = DumpActors();
    } else if (name == "bosses") {
        arrays["bosses"] = DumpBosses();
    } else if (name == "items") {
        arrays["items"] = DumpItems();
    } else if (name == "effects") {
        arrays["effects"] = DumpEffects();
    } else if (name == "sprites") {
        arrays["sprites"] = DumpSprites();
    } else if (name == "scenery") {
        arrays["scenery"] = DumpScenery();
    } else if (name == "scenery360") {
        // Only allocated in all-range levels (fox_play.c), and left dangling after one —
        // the play-mode guard does not cover this, so it gets its own precondition.
        if (gLevelMode == LEVELMODE_ALL_RANGE && gScenery360 != NULL) {
            arrays["scenery360"] = DumpScenery360();
        } else {
            arrays["scenery360"] = { { "note", "only available in all-range mode" } };
        }
    } else if (name == "shots") {
        arrays["shots"] = DumpShots();
    } else {
        return false;
    }
    return true;
}

static int32_t ObjectsHandler(std::shared_ptr<Ship::Console> console, const std::vector<std::string>& args,
                              std::string* output) {
    if (!RequirePlay(output)) {
        return 1;
    }
    nlohmann::json j;
    j["frame"] = gGameFrameCount;
    j["level"] = (s32) gCurrentLevel;
    j["levelName"] = Starship_LevelName(gCurrentLevel);
    auto arrays = nlohmann::json::object();
    if (args.size() > 1) {
        for (size_t i = 1; i < args.size(); i++) {
            if (!DumpObjectArray(args[i], arrays)) {
                if (output != nullptr) {
                    *output += "unknown object array: " + args[i];
                }
                return 1;
            }
        }
    } else {
        for (const char* name : kObjectArrayNames) {
            DumpObjectArray(name, arrays);
        }
    }
    j["arrays"] = std::move(arrays);
    if (output != nullptr) {
        *output += j.dump();
    }
    return 0;
}

void DebugCommands_Init() {
    // Runs after PortEnhancements_Init (GameEngine::Create), so the event IDs exist and
    // HIGH sorts the step listener after the NORMAL pause handler on the same event.
    REGISTER_LISTENER(PlayUpdateEvent, StepOnPlayUpdate, EVENT_PRIORITY_HIGH);
    REGISTER_LISTENER(GamePostUpdateEvent, StepOnGamePostUpdate, EVENT_PRIORITY_NORMAL);
    REGISTER_LISTENER(GamePostUpdateEvent, WarpOnGamePostUpdate, EVENT_PRIORITY_NORMAL);

    auto console = Ship::Context::GetInstance()->GetConsole();
    console->AddCommand("health", { HealthHandler,
                                    "Debug server readiness: protocol version + current game state. Safe anywhere.",
                                    {} });
    console->AddCommand("pause", { PauseHandler,
                                   "Debug-pause the game (shared with the L-trigger pause). Requires play mode.",
                                   {} });
    console->AddCommand("resume", { ResumeHandler,
                                    "Clear the debug pause. Safe anywhere.",
                                    {} });
    console->AddCommand("step", { StepHandler,
                                  "Run exactly n play frames (default 1), then re-pause. Requires live gameplay.",
                                  { { "n", Ship::ArgumentType::NUMBER, true } } });
    console->AddCommand("warp", { WarpHandler,
                                  "Enter a level from anywhere: warp <level> [phase] [--no-intro] [--paused] "
                                  "[--fresh] [--at <p> --load <n> [--ground <g>]].",
                                  { { "level", Ship::ArgumentType::TEXT, false },
                                    { "phase", Ship::ArgumentType::NUMBER, true } } });
    console->AddCommand("checkpoint", { CheckpointHandler,
                                        "Capture the current on-rails position as warp checkpoint data. "
                                        "Requires normal flight.",
                                        {} });
    console->AddCommand("player", { PlayerHandler,
                                    "Dump player state as JSON. Requires play mode.",
                                    { { "index", Ship::ArgumentType::NUMBER, true } } });
    console->AddCommand("objects", { ObjectsHandler,
                                     "Dump live object arrays as JSON. Requires play mode.",
                                     { { "actors|bosses|items|effects|sprites|scenery|scenery360|shots",
                                         Ship::ArgumentType::TEXT, true } } });
}
