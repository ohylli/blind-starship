# Developer menu reference

This documents the **Developer** submenu of the F1 enhancements menu — what each
feature does and how you trigger it, from a user's point of view rather than how
it's implemented. The purpose is to have an inventory of the debugging and
testing capabilities the port already ships, so accessibility work can lean on
them instead of reinventing them.

The menu is built in `DrawDebugMenu()` in `src/port/ui/ImguiUI.cpp`. Every entry
is a libultraship CVar; toggling it in the menu is equivalent to `set <cvar> 1`
in the Console (see below), so any of these can also be flipped from the console
or bound to a key.

A note on triggers: many features are "press L during play" style shortcuts.
Those overload the same physical L-Trigger, so only enable one L-based feature at
a time or they will fight over the button.

## Diagnostic windows

### Log Level (`gDeveloperTools.LogLevel`)
A dropdown (trace, debug, info, warn, error, critical, off; default "debug")
setting the spdlog verbosity threshold for messages printed to the console /
stdout. It does **not** change what gets written to `logs/Starship.log` — the log
file always records everything. Lower levels surface more diagnostic detail;
note that `trace` is filtered out by default even in debug builds, so you must
pick "trace" here to see `LUSLOG_TRACE` / `SPDLOG_TRACE` lines.

### Stats (`gStatsEnabled`)
A tiny always-on-top overlay showing two lines: the OS you're running on, and a
status line with the current frametime in milliseconds and the derived framerate
(e.g. "16.667 ms/frame (60.0 FPS)"). No per-subsystem breakdown or history — just
a minimal FPS/frametime readout.

### Console (`gConsoleEnabled`)
An in-game developer command line with scrollback, a filter box, and live log
display. It is keyboard-driven and is the text-based way to reach every CVar. The
registered commands are exactly libultraship's seven built-ins — Starship adds no
game-specific commands (there is no actor/spawn/teleport command):

- `set <var> <value>` — set a CVar
- `get <var>` — read a CVar
- `help` — list commands
- `clear` — clear the console
- `bind <key> <cmd>` — run a command when a key is pressed
- `bind-toggle <key> <cvar>` — flip a boolean CVar on a key
- `unbind <key>`

`bind-toggle` is the most useful one for hands-free testing: bind a key to any of
the toggles in this menu.

### Gfx Debugger (`gGfxDebuggerEnabled`)
Despite the menu tooltip ("input commands, type help" — copy-pasted from the
Console entry), this is **not** a text prompt. It is a display-list inspector for
graphics developers. Click "Debug" to capture the current frame's Fast3D/F3DEX
display list, which pauses rendering; you then browse the captured commands as an
expandable tree of decoded GBI operations (display-list calls, textures,
matrices, triangles, framebuffer ops), set breakpoints on nodes, and inspect live
render state (the display stack with source file:line, loaded textures with
previews and formats, current RDP colors). "Resume Game" un-pauses. Low-level
rendering tool; little accessibility relevance.

## Level and progression shortcuts

### Level Selector (`gLevelSelector`)
Adds a level-select overlay to the Lylat map screen so you can jump straight to
any level instead of playing through the campaign. On the map, D-Pad Left/Right
cycles the seven mission columns and D-Pad Up/Down changes the branch/difficulty;
the selected planet name is drawn at the bottom. Press **A** to launch the chosen
level directly (bypassing normal path-progress restrictions), and **L** toggles a
"start option" that selects alternate routes for certain planets, shown as "WARP
ZONE", "ANDROSS", or "BETA SB". It also unlocks the Expert-mode options in the
option menu. This is the primary way to get into a specific level quickly for
testing.

### Skip Briefing (`gSkipBriefing`)
When launching a level from the Level Selector, skips the pre-level briefing/intro
so play starts immediately. Tied to the level-selector start flow. (Venom always
skips the briefing regardless.)

### Enable Expert Mode (`gForceExpertMode`)
Fakes a 100%-completion save whenever the option menu is set up: it marks every
planet as played and cleared on both Normal and Expert with both medals granted.
The practical effect is that Expert mode is force-unlocked and the map shows as
fully completed. It writes into the in-memory save each time, so it re-applies
continuously.

### Debug Ending (`gDebugEnding`)
Changes the title-screen transition so advancing past the title (Start/A) takes
you into the ending/credits instead of the main menu. It also sets the conditions
the ending expects (Great Fox intact, Zoness/Katina cleared). Triggered from the
title screen, not mid-game.

### Set / Clear Checkpoint (`gCheckpoint.<level>.*`)
"Set Checkpoint" (a button that only appears while in play) saves your current
progress point in the current level: the path distance (from the player's Z
position), the object-load streaming index, and a "set" flag, all keyed by level.
The next time that level loads, play resumes partway through at that saved path
distance with the matching object-streaming point instead of from the start.
"Clear Checkpoint" removes the saved point so the level starts from the
beginning. Caveat: it restores **position/progress only** — not health, lives,
score, or exact X/Y — and applies mainly to on-rails progression. Useful for
repeatedly testing something that happens deep in a level.

## In-play debug controls

These act during active gameplay (`GSTATE_PLAY`). Except where noted they check
that you're actually in a level.

### Jump To Map (`gDebugJumpToMap`) — Z + R + C-Up
Hold Z-Trigger + R-Trigger and press C-Up to immediately abort the current level
and return to the Lylat map. Resets the level-clear screen and star count. Works
only during gameplay.

### L To Warp Zone (`gDebugWarpZone`) — L
Press L to force the Arwing into the warp-zone entry state with the warp SFX.
Only actually does something on Sector X and Meteo (the levels that have warp
zones); pressing it elsewhere does nothing.

### L to Level Complete (`gDebugLevelComplete`) — L
Press L to instantly finish the current level (mission accomplished, player put
into the level-complete state). On Fortuna it instead sets "mission complete" and
fast-forwards the all-range event timer to trigger that level's ending. Gameplay
only.

### L to All-Range mode (`gDebugJumpToAllRange`) — L
Press L to switch the current on-rails stage into all-range (360°) mode. Only
acts when you're not already in all-range. Note: this handler doesn't gate on
play state the way the others do, so it can fire whenever the button is read.

### Disable Collision (`gDebugNoCollision`)
Pins the player's mercy-invincibility timer high every frame, so the vehicle
takes no damage. It is effectively "no damage" / invincibility rather than true
no-clip — you don't pass through geometry, you just aren't hurt by it.

### Speed Control (`gDebugSpeedControl`) — D-Pad
D-Pad Left/Right decreases/increases the Arwing's base forward speed in steps of
50. D-Pad Down is a freeze toggle: first press saves the current speed and sets
it to 0 (stops forward movement); pressing it again restores the saved speed.
Changes persist until you adjust them again.

### Debug Pause (`gLToDebugPause`) and Frame Advance (`gLToFrameAdvance`) — L
With Debug Pause on, press L to freeze the game simulation. Enabling the Frame
Advance sub-option (only visible when Debug Pause is on) changes L, while paused,
to advance exactly one frame and re-pause. Turning Debug Pause off force-clears
any active pause, and the pause state is reset on startup so you never boot into
a stuck pause.

## Object spawning

### Spawner Mod (`gSpawnerMod`)
Spawns game objects on demand during play — meant as a debugging/documentation
aid. It runs only in-game and draws a HUD: current spawn category and object ID
at bottom-left, live counts of active scenery/sprites/actors/bosses/items/effects
at bottom-right. Controls: **C-Right** cycles the eight categories (Scenery,
Scenery360, Sprite, Actor, Boss, Item, Effect, Event); **D-Pad Left/Right**
changes the object ID (auto-repeats when held); the **analog stick** aims a
reticle for the spawn position; **L** spawns the selected object there; **D-Pad
Up** clears all spawned objects; **D-Pad Down** freezes/unfreezes the Arwing's
forward speed. It also keeps the player buffed each frame (max lasers, 9 bombs, 9
lives, refreshed invincibility). **Warning:** spawning an object whose assets
aren't loaded in memory will likely crash the game.

## Rendering / modding toggles

### Disable Starfield interpolation (`gDisableStarsInterpolation`)
A performance toggle. The port interpolates object positions between the 30 fps
game frames to render smoothly at higher refresh rates; this excludes the many
individual background stars from that interpolation, skipping the per-star math at
the cost of slightly less smooth stars. Intended to help on slower CPUs.

### Disable Gamma Boost (`gGraphics.GammaMode`) — needs reload
The original N64 applied a gamma boost that brightens the picture; the port
reproduces it via sRGB output. Enabling this option disables that boost so the
game renders without the artificial brightening. "Useful for modders" because it
lets them see textures and colors at their true, un-boosted values. Applied at
startup, hence "needs reload."

## Relevance to accessibility work

The features most likely to be useful when building and testing accessibility
mods (screen reader, audio cues):

- **Level Selector + Skip Briefing** — jump straight into the specific level and
  scenario you're testing a cue or announcement against, without replaying the
  campaign.
- **Set Checkpoint** — re-enter a level at the exact spot where the thing you're
  testing happens.
- **Disable Collision** and **Speed Control** (including its freeze) — survive
  long enough to hear a cue, or hold position and stop the ship to isolate a
  single audio cue without dying or being carried past it.
- **Spawner Mod** — place a specific enemy or item on demand as a controlled cue
  target (this is already how enemy/ring cues get tuned).
- **Console `bind` / `bind-toggle`** — bind a key to any accessibility CVar
  (`gAccessibilityScreenReader`, `gAccessibilityAudioCues`, etc.) for hands-free A/B
  testing, and `set`/`get` to inspect them.
- **Log Level = trace** — surface the `SPDLOG_TRACE` instrumentation used by cue
  tuning and the object-spawn log.
- **L to All-Range / L to Level Complete / Jump To Map** — move quickly between
  gameplay phases to check announcements at each transition.
