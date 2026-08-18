---
name: game-testing
description: Launch Starship for a test run and drive it through the debug server - check status, pause/step, warp to a level or checkpoint, save checkpoints, set CVars. Use whenever a change needs to be verified in the running game.
---

# Launching and driving the game for testing

Two tools, both run from the repo root:

- `tools/launch.ps1` — launches the game **minimized and without stealing focus** (deliberate: a normal launch would interrupt the user's screen reader mid-sentence — never bypass this with `Start-Process`), waits for the debug server's readiness handshake, and can warp on launch.
- `tools/debug_client.py` — stdlib Python client for the debug server (localhost TCP, one command in, one JSON line out).

## Prerequisites

- A built exe at `build/x64/{Debug|Release}/Starship.exe`.
- The debug server on: CVar `gDebugServer.Enabled` = 1 (default off; port `gDebugServer.Port`, default 7764) in `starship.cfg.json` next to the exe. If the launch script reports the server did not answer, that's the first thing to check. Edit the config only while the game is **not** running — the game rewrites it on exit.
- Only one instance at a time; the launch script refuses to start a second (shared config + log).

## Launch

```powershell
powershell -ExecutionPolicy Bypass -File tools\launch.ps1 [-Config Release] `
    [-Level corneria | -Checkpoint <id>] [-NoIntro] [-Paused]
```

Defaults to the Debug build. With `-Level`/`-Checkpoint`, exit code 0 means the game is actually sitting in that level (the warp blocks server-side). Without them it just waits for the health handshake.

Because the game comes up minimized, nothing is visible on screen — don't try to screenshot it; verify through the debug server and the log (`build/x64/<Config>/logs/Starship.log`).

## Debug client

```
python tools/debug_client.py [--port N] [--raw] <command ...>
```

Client flags must come **before** the command. `--wait <s>` polls until the server answers `health` (readiness check); `--repl` reads one command per stdin line for batching.

Commands (same set works in the in-game ImGui console):

- `health` — readiness + game state; works from any state.
- `player`, `objects [groups...]` — JSON state dumps, only valid in play mode.
- `cues` — accessibility cue state (voices + per-cue policy decisions); works anywhere.
- `pause`, `resume`, `step [n]` — debug pause, shared with the in-game L-trigger machinery. Typical inspection loop: `warp <level> --paused`, then `step N`, then query.
- `warp <level> [--no-intro] [--paused]` — enter a level from any post-boot state (never shows the briefing). An invalid level name errors with the full list of valid names — use that to discover them.
- `checkpoint-save <id> [--desc TEXT] [--force]` — capture the current flight position into `tools/checkpoints.json` (gitignored). Replay with `warp --checkpoint <id>` (the checkpoint stores its level, so no level argument; combines with `--paused`/`--no-intro`). Manage with `checkpoint-list` / `checkpoint-delete <id>`.
- `input stick <x> <y> [frames]` / `input hold <button...> [frames]` / `input press <button...>` / `input clear` / `input status` — synthetic controller input, only armable during live gameplay. Stick is post-dead-zone units (-60..60, 60 = full deflection); durations are play frames, so holds compose exactly with `step` and survive a pause (`input stick -60 0 30` then `step 30` = 30 frames of full-left bank). `press` is a one-frame tap that fires on the next stepped/live play frame; press-then-hold actions (charge shot) are `press a` + `hold a N` armed together (a paused-workflow composition; free-running, `hold` alone gives the clean press edge). Armed injection stands down, countdown frozen, while the game's own pause menu is open. Injection auto-clears when play mode ends. Button names: a, b, z, start, l, r, c-up/down/left/right, d-up/down/left/right.
- `set <name> <value>`, `get <name>` — any CVar, live.

Exit codes are trustworthy for scripting: 0 success (a warp that answered but didn't complete exits 1), 1 command error, 2 could not connect.

## Quirks

- `set` infers the CVar type from the value text: anything not starting with a digit — **including negative numbers like -1** — becomes a *string* CVar.
- `get` on a nonexistent CVar reports status ok with "Could not find variable" text; test the text, not the status.
- There is no quit command. Stop the game with `Stop-Process -Name Starship`. CVar changes made during the run are lost (config only saves on clean exit) — usually what you want for a test run.

Deep detail (wire protocol, design record): `docs/debug-server-plan.md`.
