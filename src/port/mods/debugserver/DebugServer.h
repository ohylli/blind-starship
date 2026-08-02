#pragma once

// Debug control server: a dev-only localhost TCP listener that accepts text commands,
// executes them on the game thread via the Ship::Console command registry, and answers
// with one JSON line per request. Off by default; gated by the gDebugServer.Enabled CVar.
// Wire protocol and design record: docs/debug-server-plan.md.

#ifdef __cplusplus
#include <functional>
#include <string>

void DebugServer_Init();      // register CVars; start the listener if enabled
void DebugServer_Exit();      // stop the listener, join threads, fail pending waiters
void DebugServer_FrameTick(); // game thread: runtime start/stop toggle + drain the request queue

// Multi-frame commands (e.g. a frame-step that completes N frames later): a handler calls
// DebugServer_Defer instead of writing its output, and the transport re-runs `poll` every
// frame until it returns true; the output it filled then becomes the ok response. Returns
// false when the command was not invoked through the socket transport (in-game ImGui
// console, Switch builds) — the handler must then answer synchronously.
using DebugServerPollFn = std::function<bool(std::string* output)>;
bool DebugServer_Defer(DebugServerPollFn poll);
#endif
