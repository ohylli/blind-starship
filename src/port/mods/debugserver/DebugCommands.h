#pragma once

// Registers the debug server's game-state commands (health, player, objects) with the
// Ship::Console registry. Registered unconditionally so the in-game ImGui console can use
// them even when the socket server is off.

#ifdef __cplusplus
void DebugCommands_Register();
#endif
