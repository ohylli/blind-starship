# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is

Starship is a native PC port of *Star Fox 64*, built on top of libultraship. The repository combines decompiled SF64 game code (C) with a C++ "port" layer that integrates the game with libultraship's window, audio, input, and Fast3D rendering. No copyrighted assets ship with the source — the user's own ROM is extracted into `.o2r` archives at first run (or via the `ExtractAssets` cmake target).

This fork's active direction is an exploratory accessibility mod for blind players — see the "Accessibility fork" section below.

## Prerequisites — submodules

`libultraship`, `tools/Torch`, and `tools/asm-differ` are git submodules. Always run `git submodule update --init` after cloning. Without `libultraship` populated, the project will not configure or build.

## Build / run

CMake drives everything. The full sequence (see `docs/BUILDING.md` for per-platform package lists):

```
# Configure
cmake -S . -B build/x64                                 # Linux/macOS Ninja: cmake -H. -Bbuild-cmake -GNinja
# (Windows MSVC: -G "Visual Studio 17 2022" -T v143 -A x64)

# 1. Extract assets from the user's ROM (must place baserom.z64 at repo root)
cmake --build build/x64 --target ExtractAssets          # writes sf64.o2r at repo root + build dir

# 2. Bundle the port's own assets/shaders
cmake --build build/x64 --target GeneratePortO2R        # writes starship.o2r

# 3. Build the executable
cmake --build build/x64
```

`ExtractAssets` and `GeneratePortO2R` shell out to `tools/Torch` (`torch o2r baserom.z64` and `torch pack port starship.o2r o2r`). Torch uses the YAML descriptors in `assets/yaml/{us,jp,eu,cn}/rev{0,1}/` and the rom-hash → config map in `config.yml`. Torch generates code into `src/assets/`, `src/jp/`, `src/eu/`, `src/cn/` — all gitignored.

The build hard-codes `VERSION_US=1`. JP/EU/CN ROMs are only supported as voice-replacement `.o2r` files dropped into `mods/`; they are not selectable build targets.

After building, the executable expects `sf64.o2r`, `starship.o2r`, `config.yml`, `assets/`, and `gamecontrollerdb.txt` next to it. A `POST_BUILD` step copies `config.yml` and `assets/` automatically; the two `.o2r` files come from the targets above.

## Code formatting

`tools/format.py` runs clang-format-14 + clang-tidy-14 over `src*/**/*.c` (excluding `assets/`) and adds final newlines. `tools/check_format.sh` is the CI gate — it runs `format.py -j` and fails if `git status` changes. Style rules are in `.clang-format` (4-space indent, 120 cols, attached braces, left pointer alignment, `SortIncludes: false`). There is no test suite to run; correctness is verified by playing the game.

## Logging

Logging goes through spdlog, configured by libultraship in `libultraship/src/Context.cpp`. Two sinks: stdout (terminal on Linux/macOS; a dedicated console window only in Windows `_DEBUG` builds) and a rotating file sink at `logs/Starship.log` next to the executable (10 MB × 10 files). Async logger, safe to call from any thread. Output pattern is `[date time.ms] [file:line] [level] message`. Levels in increasing severity: trace, debug, info, warn, error, critical. Runtime threshold is `trace` in `_DEBUG` builds and `debug` in release, so `SPDLOG_TRACE` calls are silently dropped in release builds — but note that spdlog also compile-time gates trace/debug via `SPDLOG_ACTIVE_LEVEL`, so trace lines may not even be emitted in a release build regardless of runtime level.

- **C++ (port layer)** — `#include <spdlog/spdlog.h>` and use `SPDLOG_TRACE/DEBUG/INFO/WARN/ERROR/CRITICAL` with fmt-style `{}` placeholders. Examples throughout `src/port/`.
- **C (game/decomp side)** — `#include "log/luslog.h"` and use `LUSLOG_TRACE/DEBUG/INFO/WARN/ERROR/CRITICAL` with printf-style `%s`/`%d`. These funnel into the same spdlog pipeline. The decomp still has many raw `printf` calls that bypass the log file — prefer `LUSLOG_*` for new game-side instrumentation.

Severity convention: info for sparse high-level milestones, debug for diagnostic detail when investigating, trace for firehose per-frame / per-iteration detail meant to stay in the code as opt-in instrumentation.

## Architecture

The codebase has two distinct halves wired together by `src/port/Engine.{h,cpp}`.

**Game side (C, decompiled)** — directly mirrors the SF64 source layout:
- `src/engine/` — main game loop, players, enemies, demos, HUD, RCP/display-list setup (`fox_*.c`).
- `src/overlays/ovl_i{1..6}`, `ovl_menu`, `ovl_ending`, `ovl_unused` — per-level / per-screen overlays. Overlay calls are dispatched via the `OverlayCalls` enum in `include/global.h`.
- `src/audio/` — N64 audio thread, sequence player, synthesis.
- `src/sys/` — math, matrix, memory, save, lib stubs.
- `src/libultra/`, `src/libc_*.c` — minimal stubs for the parts of libultra/libc the decomp still references.
- `include/` — game headers (`global.h` is the umbrella include; `sf64*.h` are the major subsystems).

**Port side (C++)** — lives entirely under `src/port/`:
- `Engine.{h,cpp}`, `Game.cpp` — `GameEngine` is the singleton bridge. It creates the libultraship `Ship::Context`, mounts `sf64.o2r` + `starship.o2r` + any `.otr/.o2r/.zip` in `mods/`, hosts the audio thread, and feeds gfx commands through Fast3D. `Game.cpp` owns `main()` / `SDL_main()` and the per-frame loop.
- `resource/{type,importers,loaders}/` — custom resource types (Animation, ColPoly, Hitbox, Limb, Message, ObjectInit, Script, Skeleton, Vec3{f,s}Array, EnvSettings, GenericArray, audio Drum/Envelope/Instrument/Loop/Sample/SoundFont/Book/AudioTable). Importers register with libultraship at startup; access at runtime goes through `LOAD_ASSET(path)` / `ResourceGetDataByName` macros in `Engine.h`.
- `interpolation/FrameInterpolation.{h,cpp}` — records matrix ops each game frame and interpolates them across rendered frames so the 30 fps game logic can render at higher refresh rates.
- `audio/`, `patches/DisplayListPatch.*`, `extractor/GameExtractor.*` — audio bridge, runtime DL fixes, and the in-app ROM picker / hash validator.
- `ui/` — ImGui menubar (`ImguiUI`), resolution editor, shared widgets.
- `notification/` — toast notifications.
- `hooks/` — small in-process event bus. `impl/EventSystem.{h,cpp}` defines a C-friendly `EventSystem_RegisterEvent / RegisterListener / CallEvent` API; events are declared in `hooks/list/*.h` via the `DEFINE_EVENT(Name, fields...)` macro and are fired with `CALL_EVENT(...)` / `CALL_CANCELLABLE_EVENT(...)`. `port/mods/PortEnhancements.{c,h}` registers listeners for cheats, debug controls, and HUD enhancements driven by libultraship CVars (`CVarGetInteger("g...", default)`).
- `mods/` (top-level `src/mods/`) — additional gameplay mods (boss-killer, FPS counter, level select, SFX jukebox, object-RAM viewer, spawner). Compiled into the main exe but driven by CVars.

**Vendored components**:
- `libultraship/` (submodule) — N64 OS/RCP emulation, Fast3D, audio, windowing, ImGui hosting. Linked as a CMake subdir.
- `tools/Torch/` (submodule) — the asset extractor. Built once via `ExternalProject_Add` and reused for both extraction targets.
- `dr_libs` is fetched via `FetchContent`; `sse2neon.h` is downloaded at configure time.

## Accessibility fork

This fork is exploring an accessibility mod for blind players (the maintainer is blind/low-vision).

**Current status:**
- Screen-reader announcement layer via PRISM (https://github.com/ethindp/prism), toggled by the `gAccessibilityScreenReader` CVar
- Title screen — entry announcement
- Main menu — entry announcement + cursor navigation
- Sound menu — entry announcement + cursor navigation + value changes
- Positional audio cue guiding players to the next training ring, toggled by the `gAccessibilityAudioCues` CVar — scoped to Training mode for this first test. Lives at `src/port/mods/AccessibilityCues.{c,h}`; the engine handles pan (from X) and volume (from distance) and the mod drives pitch from altitude relative to the Arwing. Tuning knobs documented in `docs/accessibility-cues-tuning.md`.

Future work: more positional audio cues for gameplay (hazards, lock-on, enemies, etc.) across the on-rails levels, leveraging the existing 3D-positional audio system rather than building new DSP (see `docs/audio-system.md` for the port's audio system, and `docs/game-world.md` for how the game models environment, enemies, and items — the producer side of cue work).

**Three-layer split.**
- **TTS transport** at `src/port/accessibility/` wraps PRISM and knows nothing about Star Fox. PRISM init/shutdown lives here because it belongs at process start/end.
- **Consumer mod entry point** at `src/port/mods/Accessibility.{c,h}` mirrors the `PortEnhancements` pattern — owns the screen-reader CVar and dispatches to per-screen registrars. Knows nothing about PRISM.
- **Per-screen listeners** at `src/port/mods/accessibility_screens/` — one file per screen, each registering its own event listeners and owning its announcement strings. New screens get a new file here plus one call from `Accessibility_Init`; the entry point and transport do not change.

Future accessibility features (audio cues) should be separate consumer mods over the existing audio system, not extensions of the TTS layer.

**PRISM integration caveat.** PRISM is built via `ExternalProject_Add`, not `FetchContent`. Its CMake target is named `prism`, which collides with libultraship's unrelated `prism` target (the Fast3D shader template processor `KiritoDv/prism-processor`). FetchContent puts both in the same CMake graph and breaks libultraship's compilation. See `docs/accessibility-prism-spike-result.md` for the full set of integration surprises (toolchain floor, backend init quirk, the tinyxml2 dynamic-build issue triggered alongside).

**Event-driven announcements.** Screen-reader announcements are wired through the in-process event bus (`src/port/hooks/`), not by polling game state from the consumer mod. Game code fires a semantic event at the precise transition (`CALL_EVENT(...)`); a per-screen listener decides what to speak. The motivation is *not* that game state is unreachable — despite the `s` prefix, most `s*` file-scope variables in the decomp actually have external linkage, and `src/mods/sfxjukebox.c` shows the established pattern of adding ad-hoc `extern` declarations to read them from a port-level mod. Events win because transitions are *semantic*: the producer already knows the exact moment a screen becomes "ready," and a single state value often covers several distinct entry paths that all want the same announcement. Pushing that knowledge into the producer keeps the consumer free of per-frame edge detection and per-screen state-machine logic. Menu/screen events live in `src/port/hooks/list/MenuEvent.h`; new domains should get their own `hooks/list/*.h`. Game-side and port-side callers should `#include "port/hooks/Events.h"` (the umbrella) rather than the per-domain list header.

**Adding a new announcement.** Follow the pattern in any existing file under `src/port/mods/accessibility_screens/`: define the event(s) in the appropriate `hooks/list/*.h`, register them in `PortEnhancements_Register()`, fire them from the precise transition point in game code, and add a new `<Screen>.c` with a `Register` function called from `Accessibility_Init`. Listeners early-return on `!Accessibility_IsScreenReaderEnabled()` so the CVar can be toggled at runtime without restart. No new CVars needed per screen.

## Things to know before editing

- **`include/global.h` is the umbrella include for the C game code.** Most game `.c` files only `#include "global.h"`. New subsystem headers should plug in there or in one of the existing `sf64*.h` files.
- **Asset references in C code use string paths** (e.g. `"objects/foo/bar"`) and are resolved at runtime by `LOAD_ASSET`. The `gAlternateAssetsEnabled` / `Tab` toggle and the `mods/` directory rely on this — don't bypass it with raw pointers unless the data genuinely lives in the executable.
- **Generated code is gitignored.** `src/assets/*`, `src/jp/`, `src/eu/`, `src/cn/`, and `properties.h` (from `properties.h.in`) are produced by Torch / cmake. Don't hand-edit; change the YAML or template.
- **Per-version compile of the C code is not supported** — `VERSION_US=1` is fixed in `CMakeLists.txt`. JP/EU/CN data lives in separate `mods/sf64{jp,eu,cn}.o2r` archives loaded at runtime for voice replacement only.
- **Several files are deliberately excluded from the build** (see the `list(FILTER ALL_FILES EXCLUDE …)` block in `CMakeLists.txt`): `fox_colheaders.c`, `fox_edata_info.c`, `fox_rcp_setup.c`, `fox_load_inits.c`, `fox_end2_data.c`, `sys_timer.c`, `sys_fault.c`, `mods/object_ram.c`, and any `*.inc.c`. These are included from other TUs or are decomp-only artifacts. If you add a new top-level source under `src/`, make sure the glob in `CMakeLists.txt` actually picks it up (only specific subdirs are globbed).
- **CI** lives in `.github/workflows/{main,linux,mac,windows,switch}.yml`. `main.yml` produces `starship.o2r` once on Linux and reuses it across platform jobs.
