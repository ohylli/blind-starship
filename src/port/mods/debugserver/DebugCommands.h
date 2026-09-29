#pragma once

// Registers the debug server's commands (health, pause/resume/step, warp, checkpoint,
// player, objects, input, screenshot) with the Ship::Console registry, plus the game-thread event listeners
// that drive `step`'s frame counting and `warp`'s staged level transition. Called from
// GameEngine::Create on every platform — including Switch, where the socket transport is
// compiled out — so the in-game ImGui console can use the commands even when the server
// is off or absent.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
#include <string>

void DebugCommands_Init();

// Shared play-mode preconditions, used by the commands here and by DebugInput.cpp:
// InPlayMode says gameplay state (gPlayer and friends) is safe to touch; RequirePlay
// additionally writes the standard error message when it is not; RequireLiveGameplay
// further rejects PLAY_PAUSE (the in-game pause menu), where play frames don't run.
bool DebugCommands_InPlayMode();
bool DebugCommands_RequirePlay(std::string* output);
bool DebugCommands_RequireLiveGameplay(std::string* output);

extern "C" {
#endif

// One-shot checkpoint override for the `warp` command, consulted by Player_Setup
// (fox_play.c) *instead of* the gCheckpoint CVars. Returns true — filling the
// saved-progress globals — only while a warp with explicit checkpoint data (or --fresh,
// which "overrides" with the untouched level-start defaults) is in flight for the level
// being initialized; a plain warp returns false and the CVar checkpoint applies as it
// would on any other level start.
bool DebugServer_GetCheckpointOverride(int32_t* groundSurface, float* pathProgress, int32_t* objectLoadIndex);

#ifdef __cplusplus
}
#endif
