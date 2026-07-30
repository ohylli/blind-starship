# Plan: debug control server for the running game

Status: proposed (2026-07-29). Design agreed in outline with the maintainer; refined
the same day after a code review of the libultraship console, threading, and
debug-pause machinery, and after a live experiment with the native debugger (see
"Relationship to the native debugger"); no implementation yet. This document is the what-and-why record
and the starting point for refinement — it deliberately stops short of an
implementation specification (no wire protocol, no file layout). Remaining open
questions are collected at the end.

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
  the same handler force-resets `gDebugPause` to 0 every play frame while
  `gLToDebugPause` is 0 (`PortEnhancements.c:181`), so a remote pause must set both
  CVars (or that guard gets reworked); and single-stepping is not CVar-drivable — the
  existing `gLToFrameAdvance` machinery is driven by a physical L-trigger press, and a
  socket client flipping `gDebugPause` off/on cannot guarantee exactly one frame
  elapses in between. Pause/resume work via CVars from day one; `step [n]` needs to be
  a real command with a game-thread frame counter.
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

## Open questions for refinement

- **Transport and framing.** Plain TCP works on all desktop platforms and every
  client; is line-oriented text with a JSON payload per response enough, or is
  length-prefixed framing needed for multi-line dumps? (Named pipes would be
  Windows-only; no reason found yet to prefer them.) On Windows the socket thread
  needs `WSAStartup` and `ws2_32` linkage; a 127.0.0.1-only bind avoids the firewall
  prompt a wildcard bind would trigger. Responses inherently carry up-to-one-frame
  latency (socket thread waits for the game-thread drain).
- **Dump surface.** Which `Player` / object fields go in the first JSON dumps, and do
  dumps live game-side (C, next to the structs) or port-side (C++ via
  `CGameCompat.h`)? What identifies an object across frames for client-side diffing?
- **TTS history query.** Where to record announcements — in the TTS transport
  (`src/port/accessibility/`) as a ring buffer, or as a tap on the speak call sites?
- **Launch story.** Half of this is already solved: `tools/launch.ps1` starts the game
  without stealing focus (`CreateProcess` with `STARTF_USESHOWWINDOW` +
  `SW_SHOWMINNOACTIVE`, which covers the game window and the `AllocConsole` console
  alike), so an agent-driven test run no longer interrupts the developer's screen reader.
  Today it proves the launch worked by enumerating the process's windows and checking who
  holds the foreground — a proxy for "the game is up", since it cannot see past the
  window into the game's own readiness, and its fixed settle delay is a guess.
  **The server should replace that with a real readiness handshake**: a `health` command
  in the phase-1 core (cheap, no play-mode precondition, answers with something like
  version + current game state), and a wait loop in the launcher that polls the port until
  it answers or the process dies. Polling the process as well as the port matters — it
  turns "exited during startup" into an immediate, specific failure instead of a silent
  timeout. The soundz mod (github.com/ahicks92/soundz, `scripts/launch.ps1`) is where the
  focus trick came from and does exactly this handshake against its own dev server; worth
  a look when writing ours. Still open beyond that: skip the ROM picker when `sf64.o2r`
  exists, and start straight into a level via CVar.
- **Naming.** CVar (`gDebugServer`?), module name, default port.
