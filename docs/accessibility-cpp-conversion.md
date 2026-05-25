# Accessibility code: C → C++ conversion

A planned mechanical conversion of the accessibility mod files from C to C++. The producer side (game-code `CALL_EVENT` sites) and the event bus stay as-is.

## Why

- The accessibility code lives entirely inside the port layer, which is already C++. C linkage here is incidental, not load-bearing.
- `Tts.cpp` is already C++; only its header carries an `extern "C"` shim for C callers. Once the consumers are C++, that shim does no useful work for them.
- Announcement strings keep getting more compound (`"Music 80"`, future cue/HUD lines). `fmt::format` removes the `snprintf` + sized-buffer ceremony, with no truncation risk.
- The audio-cue work is expected to grow (more cue types, hazards, lock-on). C++ idioms — classes, a cue registry, `std::vector` of cue producers — will scale better than the current C function tables. Converting everything now avoids a piecemeal migration later.
- `SPDLOG_*` (fmt-style placeholders) is more ergonomic than `LUSLOG_*` (printf-style) for the kind of diagnostic logging this code does, and is directly available once the file is C++.

## Scope — files that convert

Rename `.c` → `.cpp`. Headers keep the `.h` extension (project convention; C++ does not care).

- `src/port/mods/Accessibility.{c,h}`
- `src/port/mods/AccessibilityCues.{c,h}`
- `src/port/mods/AccessibilityTrainingMinimal.{c,h}`
- `src/port/mods/ObjectSpawnLog.{c,h}`
- `src/port/mods/accessibility_screens/AccessibilityScreens.h`
- `src/port/mods/accessibility_screens/{MainMenu,SoundMenu,PauseMenu,TitleScreen}.c`

`src/port/accessibility/Tts.{cpp,h}` is already C++ on the implementation side. **Leave `Tts.h`'s `extern "C"` wrapper in place** — defensive, in case future C game-side code wants to speak directly.

## Changes per file

Mechanical pass:

1. **Drop the `extern "C"` block** from each accessibility header (`Accessibility.h`, `AccessibilityCues.h`, `AccessibilityTrainingMinimal.h`, `AccessibilityScreens.h`) along with the `#include <stdbool.h>` that goes with it. `bool` is built-in in C++.
   - **`ObjectSpawnLog.h` is a partial drop, not a full drop.** It declares `ObjectId_GetName` and `EventId_GetName`, which are *defined* in `ObjectIdNames.generated.c` / `EventIdNames.generated.c`. Those generators stay C (out of scope for this pass), so those two declarations must keep C linkage or the converted `ObjectSpawnLog.cpp` will fail to link against the unmangled definitions. Wrap just those two in `extern "C"`; let `ObjectSpawnLog_Init` sit outside. Same pattern as `Tts.h`.
2. **Replace `snprintf` + `char buf[N]` with `fmt::format(...).c_str()`** at every speak site. The canonical case today is `SoundMenu.c:39-47`; expect similar patterns to appear as announcements grow.
   - Add `#include <spdlog/fmt/fmt.h>` — libultraship's established include for `fmt::format` (see `libultraship/src/window/gui/GfxDebuggerWindow.cpp`).
   - Lifetime footgun: keep the call inline, e.g. `Tts_Speak(fmt::format("{}", x).c_str(), interrupt)`. The temporary lives to end-of-full-expression so this is safe. Pulling it into `auto p = fmt::format(...).c_str();` would dangle.
3. **Replace `LUSLOG_*` with `SPDLOG_*`** for any logging calls inside the converted files. Swap `#include "log/luslog.h"` for `#include <spdlog/spdlog.h>`, and convert format strings from printf-style (`%s`, `%d`) to fmt-style (`{}`). `ObjectSpawnLog` has the most of these.
4. **`void func(void)` → `void func()`** in every signature touched. Identical in C++; `()` is the idiom.

Non-changes worth being explicit about:

- **Event handler functions stay as named `static void` functions, not lambdas.** Lambdas are technically possible (`REGISTER_LISTENER` takes a function pointer, captureless lambdas convert), but the named form reads better and is easier to set breakpoints on. Do not refactor to lambdas.
- **The event bus stays as-is.** `src/port/hooks/impl/EventSystem.h` already supports both C and C++ callers, and producers in game-side C files (`src/engine/*`, `src/overlays/*`) keep firing events via `CALL_EVENT` exactly as today.
- **Function names keep the `Accessibility_Foo` pascal-with-underscores style.** No need to introduce a namespace or rename anything in this pass — that's churn without payoff.
- **No `Tts.h` changes** beyond what was already noted.
- **`Engine.cpp`'s calls to `Accessibility_Init()` / `Accessibility_Exit()` are unaffected** — same symbol names, same signatures.
- **`extern s32 D_menu_801B9288;` and similar externs in the per-screen files don't need wrapping.** They declare C-defined file-scope variables; after conversion the declarations get C++ linkage in the consumer, but GCC/Clang/MSVC don't mangle global variable names so the link works on every target this project builds on. Leave them as-is — matches the existing `sfxjukebox.c` precedent of bare `extern` declarations for game-side globals.

## Build system

`CMakeLists.txt`'s `file(GLOB_RECURSE ALL_FILES ...)` block already includes `src/port/*.cpp` recursively, so the renamed files will be picked up automatically (`src/port/accessibility/Tts.cpp` builds today via the same line). No CMake edits needed; CI workflows do not need editing.

## Verification

- Configure + build cleanly on the host platform.
- Launch the game and confirm: screen reader still announces title screen, main menu, sound menu, and pause menu correctly; cursor navigation announces row labels; sound-menu value changes still speak.
- Toggle `gAccessibilityScreenReader` at runtime and confirm announcements stop / resume without restart.
- Enter Training mode with `gAccessibilityAudioCues` and `gAccessibilityTrainingMinimal` on; confirm the positional ring cue still plays and minimal-spawn filtering still works.
- Enable `gObjectSpawnLog`, enter Training, confirm trace lines still emit (after the LUSLOG → SPDLOG conversion they will go through the same spdlog pipeline; threshold is still the `gDeveloperTools.LogLevel` CVar — set to 0 for trace).

## Future work (out of scope for this pass)

- **Overload `Tts_Speak` on `std::string_view`** so call sites can drop the trailing `.c_str()` after `fmt::format`. Touches the transport's public API; deserves its own pass.
- **Class-based cue producers** for `AccessibilityCues`: as more cue types land (hazards, lock-on, enemies), replacing the single-cue C functions with a small class hierarchy or a registry of cue producers will scale better than adding more conditional branches.
- **Namespace the accessibility functions** under e.g. `accessibility::` if the API surface grows enough that the `Accessibility_` prefix starts feeling redundant.

These should not be folded into the mechanical conversion — they are design changes, not renames.
