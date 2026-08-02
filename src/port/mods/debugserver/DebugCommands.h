#pragma once

// Registers the debug server's game-state commands (health, player, objects) with the
// Ship::Console registry. Called from GameEngine::Create on every platform — including
// Switch, where the socket transport is compiled out — so the in-game ImGui console can
// use the commands even when the server is off or absent.

#ifdef __cplusplus
void DebugCommands_Register();
#endif
