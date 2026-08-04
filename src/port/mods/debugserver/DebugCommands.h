#pragma once

// Registers the debug server's commands (health, pause/resume/step, player, objects) with
// the Ship::Console registry, plus the game-thread event listeners that drive `step`'s
// frame counting and cleanup. Called from GameEngine::Create on every platform —
// including Switch, where the socket transport is compiled out — so the in-game ImGui
// console can use the commands even when the server is off or absent.

#ifdef __cplusplus
void DebugCommands_Init();
#endif
