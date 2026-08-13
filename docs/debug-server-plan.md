# Plan: debug control server for the running game

Status: phase 1 (Core) implemented (2026-07-31); phase 2 in progress — pause/resume/step
implemented (2026-08-04), warp + checkpoints implemented (2026-08-06), cue-state query
(`cues`) implemented (2026-08-08), see "Resolved by
implementation" near the end for the decisions that closed most open questions. Design agreed in
outline with the maintainer 2026-07-29; refined the same day after a code review of
the libultraship console, threading, and debug-pause machinery, and after a live
experiment with the native debugger (see "Relationship to the native debugger").
This document remains the what-and-why record; the implementation lives in
`src/port/mods/debugserver/` and `tools/debug_client.py`.

## Problem

There is currently no way to interact with the running game programmatically. The
debugging loop for gameplay-facing work (audio cues, announcements) is: add `SPDLOG_*`
instrumentation, rebuild, play, listen, read `logs/Starship.log`, repeat. That loop has
two structural problems:

- **It is write-only and compile-time.** Changing *what* is reported means a rebuild and
  a game restart, which is especially costly during by-ear tuning sessions where the
  interesting state exists only mid-flight.
- **It shuts out an AI coding agent.** An agent assisting with this codebase can launch
  the game and tail the log, but cannot ask the live game anything ("what enemies does
  the cue see right now, and at what pan?"), cannot flip state to reproduce a scenario,
  and cannot verify speech output programmatically. For a blind maintainer + agent team,
  "the agent can interrogate the running game" substitutes for a lot of visual
  inspection that neither party can do.

## Decision

Build a **debug control server**: a dev-only port mod that listens on a localhost
socket, accepts text commands, executes them **on the game thread**, and returns their
output. Commands are routed through libultraship's existing `Ship::Console` command
registry, so every capability added for the server is automatically also available to a
human in the in-game ImGui console window (and vice versa). Capabilities grow as an
incremental command vocabulary; embedded scripting (Lua) is explicitly deferred — see
"Deferred" below.

Rationale for this shape:

- **The command registry already exists.** `Ship::Console`
  (`libultraship/src/debug/Console.h:31`) is a string-command registry with a
  `Run(command, &output)` entry point; port code can register commands with
  `AddCommand`. `set`/`get` for any CVar are already registered
  (`libultraship/src/window/gui/ConsoleWindow.cpp:303-308`), and both the console and
  its ImGui window are created unconditionally (`Context.cpp:306`, `Gui.cpp:81` —
  `AddGuiWindow` calls `Init()` on the window, so registration does not depend on the
  window ever being opened). The server is "just" a second frontend on infrastructure
  that is already wired in — and CVar get/set alone already controls every mod, cheat,
  and accessibility toggle.
- **The server owns its dispatch; `Console::Run` is one backend, not the funnel.** The
  registry (the shared command *vocabulary*) is the reuse target, but the server should
  dispatch via `HasCommand` + `GetCommand(...).Handler(...)` with its own parsing and
  status reporting rather than routing every request through `Console::Run`. Three
  concrete reasons, found by code review: `Run` splits on spaces with no quoting
  (`Console.cpp:30`), so any argument containing a space is unrepresentable through it;
  `Run` logs every command *with its full output* at INFO (`Console.cpp:45`), which at
  our default log level would pour entire JSON dumps into `logs/Starship.log` for a
  polling client; and `Run`'s return value is ambiguous (0 means both "command not
  found" and "success", and handler conventions vary), so the wire protocol must carry
  a server-determined status anyway. Commands registered by the server remain fully
  usable from the in-game ImGui console; the reverse (server-issued commands appearing
  in the console window's history) is not a goal — that history is `ConsoleWindow`'s
  private log.
- **Debug pause is the ideal inspection partner.** `gDebugPause` works by cancelling
  the play-update event (`src/port/mods/PortEnhancements.c:184`), so a "paused" game
  still runs its render loop, ImGui, CVar system, and event bus. The server therefore
  stays responsive while the simulation is frozen. Two caveats found by code review:
  the same handler force-reset `gDebugPause` to 0 every play frame while
  `gLToDebugPause` was 0, so a remote pause had to set both CVars or the guard had to be
  reworked (it was — see the phase 2 notes below); and single-stepping is not CVar-drivable — the
  existing `gLToFrameAdvance` machinery is driven by a physical L-trigger press, and a
  socket client flipping `gDebugPause` off/on cannot guarantee exactly one frame
  elapses in between. Pause/resume work via CVars from day one; `step [n]` needs to be
  a real command with a game-thread frame counter. The transport supports this since
  phase 1: a handler calls `DebugServer_Defer(poll)` and the poll is re-run every
  frame until it reports done, so a request can stay open across N frames without
  blocking the game thread (falls back to a synchronous acknowledgment when invoked
  from the ImGui console).
- **The threading model is already solved in this codebase.** Game state must only be
  touched from the game thread. The socket thread only ferries strings; commands are
  queued and drained once per frame from a game-thread hook, the same
  queue-drain-on-tick shape the hooks/event system already uses everywhere.
- **It fits the mod pattern.** A self-contained `src/port/mods/` module, gated behind a
  CVar (off by default), bound to 127.0.0.1 only — same lifecycle as every other port
  enhancement.

## What it enables (usage scenarios)

- **Cue debugging** (the driving use case): debug-pause mid-flight, dump the actor list
  with positions and the cue layer's current targets/pan/pitch, and compare against
  what the cue math *should* have chosen — instead of inferring state from behavior by
  ear.
- **Speech verification**: query the last N TTS announcements, turning "does it speak
  the right thing at the right moment?" into a scriptable assertion.
- **Scenario setup**: warp to a level, set CVars, teleport/heal the player, spawn a
  test object — reproduce a bug or a tuning scenario in seconds instead of replaying
  to it.
- **Semi-automated tests** (later): warp + frame-step + injected input + state dump =
  scripted scenario checks for a codebase that otherwise has no test suite ("load
  Training, fly 300 frames, assert the ring streak announced").
- **Agent as eyes** (later): a screenshot-to-PNG command would let the agent describe
  actual on-screen state when audio and expectation disagree.

## Capability phases (scope outline, not a spec)

1. **Core**: socket listener + game-thread queue/drain + server-owned dispatch over the
   `Ship::Console` registry (see rationale above); CVar get/set thereby free. First new
   commands: JSON dumps of player state and the object/actor lists (a programmatic
   sibling of the object-RAM viewer, `src/mods/object_ram.c`). JSON output because the
   primary consumer is a script, not an eyeball — composition (filtering, diffing,
   watching) happens client-side. **Play-mode guard from day one**: these first dumps
   already dereference play-mode state, so every command that touches it needs a
   "requires play mode" precondition checked at dispatch (returning a clean error when
   invoked from a menu or the title screen) — this is a per-command property, not a
   drain-location concern.
2. **Control & accessibility queries**: pause/step/resume convenience commands (sugar
   over the existing CVars), warp (the level-select mod shows how), recent-TTS-lines
   query, current cue-state query.
3. **Input injection & screenshots**: scripted controller input ("hold A for 30
   frames") and framebuffer capture — the ingredients for scripted scenario tests.
   Deliberately last: highest effort/uncertainty, and only valuable once 1–2 are in
   daily use.

## Deferred: embedded Lua scripting

Considered and consciously postponed, not rejected. Lua would add a `lua <chunk>`
command executing arbitrary scripts in-process against hand-written bindings
(`player()`, `objects()`, …). Its unique wins over fixed commands are per-frame
scripted hooks ("print the cue target every frame it changes" — live, recompile-free
instrumentation) and low-latency mutation loops; its costs are permanent binding
maintenance and ~2–3× the initial effort. Two facts shrink its value today: JSON dumps
give the same compositional power client-side for read paths, and cdb covers ad-hoc
reads by symbol with zero bindings. **Trigger to revisit**: if we find ourselves adding
a new one-off dump/poke command every session, or repeatedly wanting per-frame watch
expressions without rebuilds. Nothing in the server design forecloses it — Lua would be
one more registered command reusing the same transport and game-thread execution.

## Relationship to the native debugger

cdb (installed; invoked as `cdbX64`, the Store-package alias) complements rather than
competes: with the MSVC PDB it can read any global by name with zero game modification,
and it has the one capability the server cannot replicate — hardware data breakpoints
("break when anything writes this variable"). The server is the everyday tool (fast,
repeatable, semantic); cdb is the occasional scalpel for "who mutates this?" mysteries.

**Verified against a running game (2026-07-29).** A four-check experiment attached to
the Debug build at the title screen and confirmed the assumptions above:

- **Agent-drivable batch attach.** `cdbX64 -p <pid> -c "ld Starship; <cmds>; qd"` runs
  from a non-interactive shell with no stdin, and `qd` detaches leaving the game alive
  and running — `gGameFrameCount` read 890 on one attach and 1233 on the next.
- **Decomp C globals resolve with full type info.** `?? Starship!gLevelMode` printed
  `LevelMode LEVELMODE_ON_RAILS (0n0)` — the enum by name, not a bare integer — and the
  C++ expression evaluator traverses struct pointers (`?? Starship!gPlayer->pos`).
- **Hardware write breakpoints work as advertised.** `ba w4 Starship!gGameFrameCount`
  hit on the next frame, and with `.lines -e` the stack came back source-annotated all
  the way down (`Title_Main` at `fox_title.c:238` → `Game_Update` → `push_frame`).
- **The "suspends the process" cost is negligible.** A full attach + symbol load + read
  + detach measured **0.4 s** against the 243 MB PDB. Non-invasive `-pv` (read-only, no
  breakpoints) measured slightly *slower*, so there is no reason to prefer it.

That last result corrects the original framing: process suspension is *not* why cdb
stays the occasional tool. The real limits are that each query is a fresh process spawn
with no session continuity, nothing can be run on the game thread, and the answers are
raw memory rather than semantic ("which enemies is the cue voicing right now?") — which
is exactly the gap the server fills. Two practical notes for scripting it: the first
attach of a session stalls briefly validating the default `srv*` symbol path, and every
run ends with NatVis-unload noise plus a benign checksum warning that a wrapper should
filter. A caveat for the interesting reads: `gPlayer` is null outside play mode, so
useful dereferences need the game already in a level — the same precondition the
server's play-mode guard encodes.

## Non-goals

- Not a user-facing or shipped feature; dev tooling only, off by default.
- No remote access and no authentication story — loopback bind only, on the same
  machine as an already-trusted process.
- Not a general modding API; no stability promises for command names or output shapes.

## Resolved by code review (2026-07-29)

- **Drain point: the engine tick.** `GameEngine::StartFrame()` (`src/port/Engine.cpp`)
  is the right place — it runs every loop iteration including menus and pause, and the
  ImGuiMenu accessibility module already ticks from there. Threading is even simpler
  than assumed: `push_frame()` in `src/port/Game.cpp` shows a single main-loop thread
  running game logic, `StartFrame`, and ImGui rendering in sequence, so a drain there
  executes commands on the very thread that runs `ConsoleWindow::Dispatch` today.
  Play-mode safety is a per-command precondition (see phase 1), not a reason to drain
  in two places.
- **Console bridge.** `Console::Run` has no main-thread or ImGui-context assumptions —
  it is a pure map lookup + handler call, and the built-in handlers (`set`/`get`/
  `help`) touch only CVars and the command map. Its limitations (space-splitting, INFO
  logging of full output, ambiguous return codes) are what motivate the server-owned
  dispatch layer in the Decision section.

## Resolved by implementation (2026-07-31)

Phase 1 (Core) is implemented: `src/port/mods/debugserver/DebugServer.{h,cpp}`
(transport, threading, dispatch) + `DebugCommands.{h,cpp}` (`health`, `player`,
`objects`), ticked from `GameEngine::StartFrame` and torn down first in
`GameEngine::Destroy`. Client: `tools/debug_client.py` (stdlib Python; `--wait`
polls readiness, `--repl` for batch use). Decisions that closed the open questions:

- **Transport and framing: plain TCP, line-oriented both ways.** Raw sockets
  (Winsock behind a small `#ifdef`, `ws2_32` linked on Windows; no-op stubs on
  Switch), bound to 127.0.0.1 only. Request: one text line; whitespace-split with
  double-quote grouping (the quoting `Console::Run` lacks). Response: exactly one
  JSON object per line — `{"status":"ok","output":"..."}` or
  `{"status":"error","error":"..."}`. JSON string escaping keeps any dump on one
  line, so no length-prefixed framing is needed. Dump commands put compact JSON *as
  a string* in `output`, keeping the envelope uniform for every registry command
  (`set`/`get`/`help` included); clients parse twice. One outstanding request per
  connection; multiple connections fine.
- **Dump surface: port-side C++ via `CGameCompat.h`.** `player` serializes the
  useful `Player` fields (pos + `trueZpos`, rot + `heading`, vel, speeds, shields,
  state/form, boost, wings, camera); `objects` covers the eight object arrays with the common
  `Object` header plus per-type extras. Identity for client-side diffing is
  (array, index, id, `eventType` for actors) — the same tuple the enemy-cue voice
  keys use; a FREE→INIT transition on a slot means a new entity. One trap found
  during implementation: `gScenery360` is only allocated in all-range levels and
  left dangling afterwards, so that one array carries its own precondition on top
  of the play-mode guard.
- **Launch story: done as designed.** `health` reports protocol version, game/play
  state, level, frame, and the pre-evaluated play-mode predicate;
  `tools/launch.ps1` polls it (and the process — dead-during-startup fails
  immediately and specifically) instead of sleeping a fixed settle delay, falling
  back to the old behavior with a warning when the server is off.
- **Naming.** `gDebugServer.Enabled` + `gDebugServer.Port` (default 7764) — dotted
  because the CVar config file nests on `.`, so a scalar `gDebugServer` could not
  coexist with `gDebugServer.Port`. Runtime-toggleable: the frame tick notices the
  CVar and starts/stops the listener without a restart.

## Resolved by implementation, phase 2: pause / resume / step (2026-08-04)

The design changed from the original sketch ("sugar over the existing CVars") after a
conceptual review with the maintainer: instead of a second, server-owned pause mechanism,
there is **one shared pause state — `gDebugPause`** — driven equally by the in-game
L-trigger shortcut and by the server's `pause`/`resume`/`step` commands. You can pause
from the server and resume with L in game, or vice versa; `health` reports the one true
state in its `paused` field. Two findings shaped this:

- **The only game-side change needed was the guard rework.** `OnPlayUpdateEvent`
  (`PortEnhancements.c`) used to force-clear `gDebugPause` every play frame while the
  L-shortcut CVar (`gLToDebugPause`) was off, which would have instantly undone a server
  pause. Its intent — "don't leave the game stuck paused when the user turns the shortcut
  off" — is preserved by clearing only on the on→off *transition*. Everything else about
  the existing pause (L toggle, frame advance, the F1 checkboxes) is untouched.
- **The cues keep sounding while paused**, which is a feature, not a bug: the cue
  listeners run on `GamePostUpdateEvent` (`fox_game.c`), which fires unconditionally each
  tick and is not on the cancelled play-update chain. So the maintainer can pause, hear
  the frozen cue soundscape, and interrogate it at the same time — the exact
  pause-and-inspect workflow the cue-state query (next phase 2 slice) is for.

`step [n]` (default 1) is a real command with a game-thread frame counter, as anticipated:
it clears `gDebugPause` and counts play-update events that actually ran via a listener
registered at `EVENT_PRIORITY_HIGH` — the event system calls listeners in ascending
priority order, so it runs after the NORMAL pause handler and sees the final cancelled
flag; when the count hits zero it re-sets `gDebugPause`, which takes effect from the next
play frame (the n-th frame itself runs). The response is deferred via `DebugServer_Defer`
until the frames have elapsed, so `step 30` followed immediately by `player` sees
post-step state; from the ImGui console it answers synchronously with a fire-and-forget
acknowledgment. Guards and edge cases: `step` requires `PLAY_UPDATE` (the play-mode guard
alone would let it hang inside the pause menu, `PLAY_PAUSE`, where play updates never
run); a second `step` while one is in flight is an error; `pause` and `resume` cancel an
in-flight step (freeze now / run freely beat "finish the count"); if play mode ends
mid-step (level complete, death) the response completes early with the partial
`framesRun` and a note. An L-press mid-step (shortcut on) pauses and *holds* the count —
the listener skips cancelled frames — and a second press lets the step finish. `pause`
requires play mode; `resume` works anywhere so a stale pause can always be cleared.

Post-review hardening (same day): the "play mode ended → abort the step" cleanup runs on
an unconditional per-tick `GamePostUpdateEvent` listener rather than inside the socket
poll, because the poll only exists on the socket path — a step started from the ImGui
console would otherwise leak its count when the level ended early, rejecting every later
`step` and silently re-pausing the next level that many frames in. Each `step` request
now owns its state (a `shared_ptr` the poll captures), so a request always answers with
its own numbers even if another frontend arms a new step on the tick the old one
finished, and a cancelled step says why in its `note`. The count is parsed with overflow
checking and capped (see `kMaxStepFrames`) — unchecked, a huge value either truncated to
a no-op *after* clearing `gDebugPause` (a `step` that silently resumes) or saturated into
a billions-of-frames run. One field to know about: `frame` in the responses is
`gGameFrameCount`, which is incremented before the cancellable play update and therefore
keeps advancing while debug-paused — it timestamps the response; only `framesRun` counts
elapsed simulation frames.

## Resolved by implementation, phase 2: warp / checkpoint (2026-08-06)

`warp <level> [phase] [--no-intro] [--paused] [--fresh] [--at <p> --load <n> [--ground
<g>]]` enters any level from any post-boot state, and `checkpoint` captures the current
on-rails position as reusable warp data. Verified live: warp from mid-boot, from the
title, and between levels; intro skip; paused arrival + `step`; mid-level checkpoint
round trip. Design decisions:

- **The game's own transition seam, not a bespoke one.** Setting `gNextLevel` /
  `gNextLevelPhase` / `gNextGameState = GSTATE_PLAY` is exactly how `Game_SetGameState`
  (fox_game.c) is driven by the in-play transitions (Venom → Andross) and the
  `MODS_BOOT_STATE` boot-to-level hack — memory freed, object arrays cleared,
  `Play_Setup` run. (The map screen takes a shortcut and assigns `gGameState` directly;
  the `gNextGameState` route is a superset of that, and its cleanup is what makes the
  warp safe from any state.) The warp adds only the map path's
  `Map_LevelStart_AudioSpecSetup`. The mission briefing belongs to the map screen's
  flow, so a warp never shows it — no option needed, and independent of `gSkipBriefing`.
- **Listener-driven stages, poll observes** — the same lesson `step`'s hardening taught:
  a warp from the ImGui console has no socket poll, so an unconditional
  `GamePostUpdateEvent` listener walks WAIT_WARPABLE (post-boot state reached; a warp
  issued mid-boot waits server-side, which is what lets `launch.ps1 -Level` fire right
  after the health handshake) → kickoff → WAIT_STANDBY → WAIT_PLAY → DONE, with a
  ~60 s timeout answering `completed: false` plus a note. GSTATE_INIT is deliberately
  not warpable: its Game_Update case performs the base bootstrap and clobbers
  `gNextGameState`.
- **The intro decision is one flag with a guaranteed window.** `Player_Setup` plays the
  level intro cutscene only when `D_ctx_8017782C` ("play the intro") is set and no saved
  progress is pending; `--no-intro` clears it during the new level's `PLAY_STANDBY`
  frames (at least three: `gNextGameStateTimer` starts at 3), taking the same
  no-cutscene path a death restart takes. A checkpoint start forces the same skip: the
  game's own gate keys on the restored *object-load index* being nonzero, so a capture
  from the first stretch of a level would otherwise still play the cutscene (and on
  Corneria have it overwrite the restored ground surface).
- **Player init lives in the first play frame, not in Play_Init** — the trap of this
  slice. `Player_Setup` (intro decision, checkpoint restore, player state) runs from the
  player-state machine *inside the cancellable play update*, so pausing at kickoff would
  freeze the arrival half-initialized. The warp therefore holds `gDebugPause` clear for
  the whole transition (a pause or L press landing mid-warp would stall it to the
  timeout; `pause` and `step` commands are rejected while a warp is in flight),
  completes only when the player has left `PLAYERSTATE_INIT`, and
  applies `--paused` at that moment: "arrive paused" means exactly one simulated frame,
  the same thing `step 1` means. "Advance N after arrival" is deliberately not a warp
  option — it is `warp --paused` followed by `step N`.
- **Checkpoints are captured live, resolved by the client, and injected server-side.**
  `checkpoint` returns the (pathProgress, objectLoadIndex, groundSurface) tuple the
  game's own respawn uses (same formula as the F1 "Set Checkpoint" button, clamped to
  the level start so a capture in the first 250 path units still replays; on-rails
  only, and only in normal flight so a cutscene position cannot be captured), plus the
  level phase — the warp-zone alternate routes (Meteo, Sector X) are on-rails phase 1
  with their own object tables, so a capture there must replay into the same phase.
  Names live entirely in `tools/checkpoints.json`, managed by `debug_client.py
  checkpoint-save / checkpoint-list / checkpoint-delete`; `warp --checkpoint <id>`
  expands client-side into the stored level, phase, and `--at/--load/--ground` (the
  entry stores its level, so no level argument). The
  server stays stateless: explicit values arrive via `DebugServer_GetCheckpointOverride`,
  which `Player_Setup` consults *instead of* the `gCheckpoint` CVars for that one level
  start (`--fresh` overrides with the untouched defaults, suppressing a configured CVar
  checkpoint for one start). The JSON gives tests a shared vocabulary ("test in meteo at
  big-asteroids"); it is gitignored for now — commit it later if the spot list proves
  worth sharing.
- **Two pre-existing checkpoint-persistence bugs found and fixed while wiring this.**
  The F1 "Set Checkpoint" button wrote the ground surface to the `gSavedPathProgress`
  key (immediately overwritten by the real progress; `gSavedGroundSurface` never saved),
  and the `gCheckpoint.%d.*` numeric keys made the nested config JSON an array, which
  libultraship's CVar loader silently drops on load (`ConsoleVariable.cpp`) — so F1
  checkpoints never survived a restart. Keys are now `gCheckpoint.Level%d.*` and the
  ground surface saves under its own name; old array entries in existing configs remain
  ignored, as they always were.
- **`launch.ps1 -Level <name> [-Checkpoint <id>] [-NoIntro] [-Paused]`** shells out to
  `debug_client.py warp` after the health handshake; the client blocks until the level
  is actually up and folds a server-side warp timeout (`completed: false` inside a
  status-ok envelope) into exit 1, so exit 0 means "sitting in the level".

## Resolved by implementation, phase 2: cue-state query (2026-08-08)

`cues` dumps the accessibility audio-cue system as JSON: backend availability, the
effective tuning CVars, and per cue the registry identity plus two merged layers — the
*what* (per-voice position/pitch/interval/gain from a new value-copy `Cue::Snapshot()`
accessor; the Cue3D seam stays push-only and is never read) and the *why* (a `policy`
section per gameplay cue from a last-tick mirror in `accessibility_cues/` (one per cue file), where each
listener now records what it decided on every exit path: gate terms when it stopped the
cue, and targets — for the enemy cue the decoded slot/objId/eventType identity, distance,
and the voice key — when it drove it). A client joins `voices[].key` against
`policy.targets[].voiceKey`, and slot/eventType against the `objects actors` dump. No
play-mode guard: everything is a value copy (the policy mirror deliberately stores
scalars, never entity pointers), so the command is also useful at the title screen and in
PLAY_PAUSE; per-section `frame` stamps plus a `fresh` flag tell a client whether a
section was written this tick. While debug-paused the listeners keep running on the
uncancelled GamePostUpdateEvent, so the dump reports the live frozen soundscape — the
pause-and-inspect workflow this command was the point of.

Additions since: the obstacle-ahead cue (2026-08-11) reports its policy the same way —
gates, scan counters (`active`/`obstacles`/`boxes` plus `onCourse`), the effective
warn-distance/margin knobs, and when active the winning hitbox record (source array +
slot/objId/record, `gapZ` to the near face, per-axis `clear`/`delta`/`half`) and the
pushed `intervalSec`. Purely additive to the wire shape, so no protocol bump.

## Open questions for refinement

- **TTS history query** (phase 2). Where to record announcements — in the TTS
  transport (`src/port/accessibility/`) as a ring buffer, or as a tap on the speak
  call sites?
- **Launch conveniences.** Skip the ROM picker when `sf64.o2r` exists. (Starting
  straight into a level is now covered by `launch.ps1 -Level`.)
