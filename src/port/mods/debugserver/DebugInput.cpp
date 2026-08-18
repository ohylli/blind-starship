// `input` — synthetic controller input for the debug control server, so a client can fly
// the ship without a human on the stick (the driving use case: setting up reproducible
// obstacle/enemy approaches for audio-cue tuning, composed with `warp --paused` + `step`).
//
// Injection point: the game reads input through a double buffer — Controller_ReadData
// (sys_joybus.c) fills sNextController from libultraship once per engine tick, and the
// *next* tick's Controller_UpdateInput consumes it into gControllerHold/gControllerPress
// (held state, press edges, dead-zoned stick). We merge injected state into
// sNextController on GamePostUpdateEvent, i.e. after ReadData refilled it this tick and
// after this tick's play update ran. Downstream, injected input is indistinguishable from
// a real pad: the game's own code computes edges and the dead zone, and the injection
// works with no physical controller connected (gControllerPlugged is hardcoded on in the
// port). It also bypasses ControlDeck's ImGui input blocking — acceptable, and arguably
// useful, for a dev tool.
//
// Durations are measured in *play* frames, not engine ticks: the countdown listener sits
// on the cancellable PlayUpdateEvent and skips cancelled (debug-paused) frames, so a hold
// armed while paused survives the pause and `input stick -60 0 30` + `step 30` is exactly
// one second of full-left bank. The `primed` flags keep the count exact: a channel only
// counts down once the pad it was merged into has actually been consumed (merge at
// GamePostUpdate of tick N -> consumed by UpdateInput at tick N+1 -> counted at that
// tick's play update), and the frame that hits zero deactivates the channel *before* this
// tick's GamePostUpdate merge, so nothing bleeds into an extra frame.
//
// Presses are different: press edges are recomputed every engine tick, so a button held
// via the pad while debug-paused fires its edge on a paused tick and the stepped frame
// only ever sees "held". `input press` therefore bypasses the pad and forces the press
// (and hold) bits directly into gControllerPress/gControllerHold on the next
// *non-cancelled* play frame, after UpdateInput computed them and before the play body
// reads them (event listeners run before the CALL_CANCELLABLE_EVENT body). Actions gated
// on press-then-hold (e.g. the charge shot) are the composition `input press a` +
// `input hold a <frames>` armed together — a paused/`step` workflow. Free-running, the
// two arming commands land a frame apart and the gap reads as a one-frame release; there
// `hold` alone already produces a clean pad edge, so `press` is only needed while paused.

#include "DebugInput.h"
#include "DebugCommands.h"

#include "port/CGameCompat.h"
#include "port/hooks/Events.h"

extern "C" {
#include <sf64thread.h> // gControllerHold/gControllerPress, sNextController, gControllerLock
}

#include <nlohmann/json.hpp>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <string>
#include <vector>

// Same ceiling as step's kMaxStepFrames, for the same reason: reject counts that could
// truncate on int narrowing or outlive any real test run.
static constexpr long kMaxInputFrames = 100000;

// Controller_AddDeadZone subtracts a 16-unit dead zone and clamps to +/-60 — the range
// gameplay code actually sees (in gControllerPress's stick fields). The command speaks
// post-dead-zone units so "stick -60 0" is a full-left bank; add the bias back here.
static constexpr long kStickMax = 60;
static constexpr int kDeadZone = 16;

struct InputState {
    bool stickActive = false;
    s8 stickRawX = 0; // pad units, dead-zone bias included
    s8 stickRawY = 0;
    int stickX = 0; // game units as given, echoed in status
    int stickY = 0;
    int stickRemaining = -1; // play frames; -1 = until cleared
    bool stickPrimed = false;

    u16 holdButtons = 0;
    int holdRemaining = -1;
    bool holdPrimed = false;

    u16 pressButtons = 0; // fires once, on the next non-cancelled play frame
};
static InputState sInput; // game thread only

struct ButtonName {
    const char* name;
    u16 mask;
};
static const ButtonName kButtons[] = {
    { "a", A_BUTTON },        { "b", B_BUTTON },        { "z", Z_TRIG },          { "start", START_BUTTON },
    { "l", L_TRIG },          { "r", R_TRIG },          { "c-up", U_CBUTTONS },   { "c-down", D_CBUTTONS },
    { "c-left", L_CBUTTONS }, { "c-right", R_CBUTTONS }, { "d-up", U_JPAD },      { "d-down", D_JPAD },
    { "d-left", L_JPAD },     { "d-right", R_JPAD },
};

// Lowercased alphanumerics only: "c-up", "C_UP", and "cup" all normalize to "cup".
static std::string NormalizeButtonName(const std::string& raw) {
    std::string s;
    for (char c : raw) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            s += (char) std::tolower(static_cast<unsigned char>(c));
        }
    }
    return s;
}

static bool ResolveButton(const std::string& token, u16* outMask) {
    std::string wanted = NormalizeButtonName(token);
    if (wanted.empty()) {
        return false;
    }
    for (const auto& entry : kButtons) {
        if (NormalizeButtonName(entry.name) == wanted) {
            *outMask = entry.mask;
            return true;
        }
    }
    return false;
}

static std::string ButtonNameList() {
    std::string s;
    for (const auto& entry : kButtons) {
        if (!s.empty()) {
            s += ", ";
        }
        s += entry.name;
    }
    return s;
}

static nlohmann::json ButtonNamesJson(u16 mask) {
    nlohmann::json names = nlohmann::json::array();
    for (const auto& entry : kButtons) {
        if (mask & entry.mask) {
            names.push_back(entry.name);
        }
    }
    return names;
}

static bool ParseLongToken(const std::string& token, long min, long max, long* out) {
    char* end = nullptr;
    errno = 0;
    long v = strtol(token.c_str(), &end, 10);
    if (end == token.c_str() || *end != '\0' || errno == ERANGE || v < min || v > max) {
        return false;
    }
    *out = v;
    return true;
}

static bool AnyInjection() {
    return sInput.stickActive || sInput.holdButtons != 0 || sInput.pressButtons != 0;
}

static nlohmann::json InputStateJson() {
    nlohmann::json j;
    if (sInput.stickActive) {
        nlohmann::json stick;
        stick["x"] = sInput.stickX;
        stick["y"] = sInput.stickY;
        if (sInput.stickRemaining >= 0) {
            stick["framesRemaining"] = sInput.stickRemaining;
        }
        j["stick"] = stick;
    } else {
        j["stick"] = nullptr;
    }
    if (sInput.holdButtons != 0) {
        nlohmann::json hold;
        hold["buttons"] = ButtonNamesJson(sInput.holdButtons);
        if (sInput.holdRemaining >= 0) {
            hold["framesRemaining"] = sInput.holdRemaining;
        }
        j["hold"] = hold;
    } else {
        j["hold"] = nullptr;
    }
    j["press"] = ButtonNamesJson(sInput.pressButtons);
    j["frame"] = gGameFrameCount;
    return j;
}

// Merges the injection into the pad the *next* tick's Controller_UpdateInput consumes.
// Runs every tick, paused or not, so held state persists through a debug pause.
static void InputOnGamePostUpdate(IEvent* event) {
    if (!AnyInjection()) {
        return;
    }
    // Play mode ended (death, level clear, warp kickoff): drop everything, exactly like
    // step's cleanup. A stale stick hold would otherwise steer the next menu — the menus
    // read the same gControllerPress this injection ultimately feeds.
    if (!DebugCommands_InPlayMode()) {
        sInput = InputState{};
        return;
    }
    // Stand down without disarming — and keep the countdown honest (an unconsumed pad
    // must not count) — whenever the game expects the pad untouched: transition frames,
    // where Controller_ReadData zeroed the pad instead of reading it (gControllerLock),
    // and the in-game pause menu, where an injected stick would steer the menu and a
    // re-merged button could never form the press edge the unpause check needs.
    if (gControllerLock != 0 || gPlayState != PLAY_UPDATE) {
        sInput.stickPrimed = false;
        sInput.holdPrimed = false;
        return;
    }
    OSContPad* pad = &sNextController[gMainController];
    if (sInput.holdButtons != 0) {
        pad->button |= sInput.holdButtons;
        sInput.holdPrimed = true;
    }
    if (sInput.stickActive) {
        pad->stick_x = sInput.stickRawX;
        pad->stick_y = sInput.stickRawY;
        sInput.stickPrimed = true;
    }
}

// Registered at EVENT_PRIORITY_HIGH like step's listener, for the same reason: it must
// run after the NORMAL-priority pause handler to see the final cancelled flag.
static void InputOnPlayUpdate(IEvent* event) {
    if (event->cancelled) {
        return; // debug-paused frame: nothing was consumed, presses stay armed
    }
    // Same stand-down as the merge: while gControllerLock holds input across a
    // transition the game expects zeroed input, so a press stays armed instead of firing.
    if (sInput.pressButtons != 0 && gControllerLock == 0) {
        gControllerPress[gMainController].button |= sInput.pressButtons;
        gControllerHold[gMainController].button |= sInput.pressButtons;
        sInput.pressButtons = 0;
    }
    if (sInput.stickActive && sInput.stickPrimed && sInput.stickRemaining > 0 && --sInput.stickRemaining == 0) {
        sInput.stickActive = false;
        sInput.stickPrimed = false;
    }
    if (sInput.holdButtons != 0 && sInput.holdPrimed && sInput.holdRemaining > 0 && --sInput.holdRemaining == 0) {
        sInput.holdButtons = 0;
        sInput.holdPrimed = false;
    }
}

// Collects button names, plus an optional single trailing integer as the frame count.
static bool ParseButtonArgs(const std::vector<std::string>& args, size_t first, bool allowFrames, u16* outMask,
                            long* outFrames, std::string* output) {
    u16 mask = 0;
    long frames = -1;
    for (size_t i = first; i < args.size(); i++) {
        u16 buttonMask;
        if (ResolveButton(args[i], &buttonMask)) {
            mask |= buttonMask;
            continue;
        }
        if (allowFrames && i + 1 == args.size() && ParseLongToken(args[i], 1, kMaxInputFrames, &frames)) {
            break;
        }
        // Route numeric tokens to a frame-count diagnosis — "unknown button: 0" would
        // send the author hunting for a button-name problem.
        char* end = nullptr;
        (void) strtol(args[i].c_str(), &end, 10);
        if (end != args[i].c_str() && *end == '\0') {
            if (output != nullptr) {
                if (!allowFrames) {
                    *output += "press takes no frame count (one-frame tap; use: input press <button...>)";
                } else if (i + 1 != args.size()) {
                    *output += "frame count must be the last argument";
                } else {
                    *output += "frame count must be an integer between 1 and " + std::to_string(kMaxInputFrames);
                }
            }
            return false;
        }
        if (output != nullptr) {
            *output += "unknown button: " + args[i] + " (valid: " + ButtonNameList() + ")";
        }
        return false;
    }
    if (mask == 0) {
        if (output != nullptr) {
            *output += "at least one button required (valid: " + ButtonNameList() + ")";
        }
        return false;
    }
    *outMask = mask;
    *outFrames = frames;
    return true;
}

static int32_t InputHandler(std::shared_ptr<Ship::Console> console, const std::vector<std::string>& args,
                            std::string* output) {
    // Status and clear work from anywhere: clear only drops state, and both must be
    // usable after play mode ended out from under an armed injection.
    if (args.size() < 2 || args[1] == "status") {
        if (output != nullptr) {
            *output += InputStateJson().dump();
        }
        return 0;
    }
    const std::string& sub = args[1];
    if (sub == "clear") {
        sInput = InputState{};
        if (output != nullptr) {
            *output += InputStateJson().dump();
        }
        return 0;
    }
    // Reject a mistyped subcommand before the play gates — that diagnosis needs no game
    // state.
    if (sub != "stick" && sub != "hold" && sub != "press") {
        if (output != nullptr) {
            *output += "unknown subcommand: " + sub + " (stick, hold, press, clear, status)";
        }
        return 1;
    }
    // Same live-gameplay gate as step: during PLAY_PAUSE (the in-game pause menu) play
    // frames don't run, and an armed stick would steer the menu instead of the ship.
    if (!DebugCommands_RequireLiveGameplay(output)) {
        return 1;
    }
    if (sub == "stick") {
        if (args.size() < 4 || args.size() > 5) {
            if (output != nullptr) {
                *output += "usage: input stick <x> <y> [frames], x/y in -60..60 (60 = full deflection)";
            }
            return 1;
        }
        long x, y;
        if (!ParseLongToken(args[2], -kStickMax, kStickMax, &x) ||
            !ParseLongToken(args[3], -kStickMax, kStickMax, &y)) {
            if (output != nullptr) {
                *output += "stick values must be integers in -60..60";
            }
            return 1;
        }
        long frames = -1;
        if (args.size() == 5 && !ParseLongToken(args[4], 1, kMaxInputFrames, &frames)) {
            if (output != nullptr) {
                *output += "frame count must be an integer between 1 and " + std::to_string(kMaxInputFrames);
            }
            return 1;
        }
        sInput.stickActive = true;
        sInput.stickX = (int) x;
        sInput.stickY = (int) y;
        sInput.stickRawX = (s8) (x == 0 ? 0 : (x > 0 ? x + kDeadZone : x - kDeadZone));
        sInput.stickRawY = (s8) (y == 0 ? 0 : (y > 0 ? y + kDeadZone : y - kDeadZone));
        sInput.stickRemaining = (int) frames;
        sInput.stickPrimed = false;
    } else if (sub == "hold") {
        u16 mask;
        long frames;
        if (!ParseButtonArgs(args, 2, true, &mask, &frames, output)) {
            return 1;
        }
        sInput.holdButtons = mask;
        sInput.holdRemaining = (int) frames;
        sInput.holdPrimed = false;
    } else { // press
        u16 mask;
        long frames;
        if (!ParseButtonArgs(args, 2, false, &mask, &frames, output)) {
            return 1;
        }
        // Accumulate: a second press armed in the same paused stretch must not silently
        // drop the first — both fire on the next play frame. `input clear` disarms.
        sInput.pressButtons |= mask;
    }
    if (output != nullptr) {
        *output += InputStateJson().dump();
    }
    return 0;
}

void DebugInput_Register() {
    REGISTER_LISTENER(PlayUpdateEvent, InputOnPlayUpdate, EVENT_PRIORITY_HIGH);
    REGISTER_LISTENER(GamePostUpdateEvent, InputOnGamePostUpdate, EVENT_PRIORITY_NORMAL);

    auto console = Ship::Context::GetInstance()->GetConsole();
    console->AddCommand("input", { InputHandler,
                                   "Inject controller input: input stick <x> <y> [frames] | hold <button...> "
                                   "[frames] | press <button...> | clear | status. Durations are play frames.",
                                   { { "stick|hold|press|clear|status", Ship::ArgumentType::TEXT, true } } });
}
