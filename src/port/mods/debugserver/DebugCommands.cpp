// Game-state commands for the debug control server: `health` (readiness handshake),
// `player` and `objects` (JSON dumps). Handlers write compact JSON into the console
// output string; the server wraps it in its wire envelope, the ImGui console prints it
// verbatim. JSON because the primary consumer is a script — filtering/diffing/watching
// happens client-side (docs/debug-server-plan.md).

#include "DebugCommands.h"

#include "port/CGameCompat.h"
#include "port/mods/ObjectSpawnLog.h"

#include <nlohmann/json.hpp>
#include <cstdlib>
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
    // The guard predicate, pre-evaluated: tells clients whether guarded commands will
    // succeed without having to try one.
    j["playMode"] = InPlayMode();
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

void DebugCommands_Register() {
    auto console = Ship::Context::GetInstance()->GetConsole();
    console->AddCommand("health", { HealthHandler,
                                    "Debug server readiness: protocol version + current game state. Safe anywhere.",
                                    {} });
    console->AddCommand("player", { PlayerHandler,
                                    "Dump player state as JSON. Requires play mode.",
                                    { { "index", Ship::ArgumentType::NUMBER, true } } });
    console->AddCommand("objects", { ObjectsHandler,
                                     "Dump live object arrays as JSON. Requires play mode.",
                                     { { "actors|bosses|items|effects|sprites|scenery|scenery360|shots",
                                         Ship::ArgumentType::TEXT, true } } });
}
