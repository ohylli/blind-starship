#pragma once

// Registers the debug server's `input` command (synthetic controller input: stick and
// button holds measured in play frames, one-frame button presses, clear/status) plus the
// game-thread listeners that apply the injection to the game's pad buffers each tick.
// Called from DebugCommands_Init so the command is equally available from the socket and
// the in-game ImGui console. Injection design and rationale: docs/debug-server-plan.md.

void DebugInput_Register();
