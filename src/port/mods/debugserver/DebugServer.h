#pragma once

// Debug control server: a dev-only localhost TCP listener that accepts text commands,
// executes them on the game thread via the Ship::Console command registry, and answers
// with one JSON line per request. Off by default; gated by the gDebugServer.Enabled CVar.
// Wire protocol and design record: docs/debug-server-plan.md.

#ifdef __cplusplus
void DebugServer_Init();      // register CVars + commands; start the listener if enabled
void DebugServer_Exit();      // stop the listener, join threads, fail pending waiters
void DebugServer_FrameTick(); // game thread: runtime start/stop toggle + drain the request queue
#endif
