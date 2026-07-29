# Plan: debug control server for the running game

Status: proposed (2026-07-29). Design agreed in outline with the maintainer; no
implementation yet. This document is the what-and-why record and the starting point for
refinement — it deliberately stops short of an implementation specification (no wire
protocol, no file layout). Open questions for the next session are collected at the end.

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
  its ImGui window are created unconditionally (`Context.cpp:306`, `Gui.cpp:81`). The
  server is "just" a second frontend on infrastructure that is already wired in — and
  CVar get/set alone already controls every mod, cheat, and accessibility toggle.
- **Debug pause is the ideal inspection partner.** `gDebugPause` works by cancelling
  the play-update event (`src/port/mods/PortEnhancements.c:184`), so a "paused" game
  still runs its render loop, ImGui, CVar system, and event bus. The server therefore
  stays responsive while the simulation is frozen: pause, inspect at leisure,
  single-step with the existing `gLToFrameAdvance` machinery, resume — all drivable
  remotely via CVars from day one.
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

1. **Core**: socket listener + game-thread queue/drain + bridge to `Console::Run`; CVar
   get/set thereby free. First new commands: JSON dumps of player state and the
   object/actor lists (a programmatic sibling of the object-RAM viewer,
   `src/mods/object_ram.c`). JSON output because the primary consumer is a script, not
   an eyeball — composition (filtering, diffing, watching) happens client-side.
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
repeatable, doesn't suspend the process); cdb is the occasional scalpel for
"who mutates this?" mysteries.

## Non-goals

- Not a user-facing or shipped feature; dev tooling only, off by default.
- No remote access and no authentication story — loopback bind only, on the same
  machine as an already-trusted process.
- Not a general modding API; no stability promises for command names or output shapes.

## Open questions for refinement

- **Drain point.** Candidates: `GameEngine::StartFrame()` (`src/port/Engine.cpp:326`,
  runs every frame including menus — the ImGuiMenu accessibility module already ticks
  from there) vs. an event-bus listener (`PlayUpdateEvent` fires only in play mode;
  `DisplayPostUpdateEvent` only on play-draw frames). Likely answer: the engine tick,
  for menu/pause coverage — verify command execution is safe there, or drain in two
  places with a "requires play mode" flag per command.
- **Transport and framing.** Plain TCP works on all desktop platforms and every
  client; is line-oriented text with a JSON payload per response enough, or is
  length-prefixed framing needed for multi-line dumps? (Named pipes would be
  Windows-only; no reason found yet to prefer them.)
- **Console bridge details.** Does `Console::Run` have any hidden main-thread or
  ImGui-context assumptions? (`ConsoleWindow::Dispatch` runs it from the render/UI
  path today.)
- **Dump surface.** Which `Player` / object fields go in the first JSON dumps, and do
  dumps live game-side (C, next to the structs) or port-side (C++ via
  `CGameCompat.h`)? What identifies an object across frames for client-side diffing?
- **TTS history query.** Where to record announcements — in the TTS transport
  (`src/port/accessibility/`) as a ring buffer, or as a tap on the speak call sites?
- **Launch story.** Anything needed for the agent to launch the game headless-ish
  (skip the ROM picker when `sf64.o2r` exists, start straight into a level via CVar)?
- **Naming.** CVar (`gDebugServer`?), module name, default port.
