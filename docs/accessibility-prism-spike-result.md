# PRISM Spike — Outcome

Companion to `accessibility-prism-spike.md`. Records what shipped and the surprises that bit during integration so a future session doesn't have to rediscover them.

## Status

**Working on Windows.** Startup announcement plays through NVDA when running, Microsoft OneCore (SAPI fallback) when not. Clean exit, no crash. 

## What's in the build

- `src/port/accessibility/Accessibility.h` — three C-callable entry points (`Init`, `Speak`, `Shutdown`).
- `src/port/accessibility/Accessibility.cpp` — wraps `PrismContext*` + `PrismBackend*` lifecycle. `#ifdef HAVE_PRISM` real path, `#else` no-op stubs.
- `src/port/Engine.cpp` — `Accessibility_Init()` + `Accessibility_Speak("Starship accessibility check", false)` at the end of `GameEngine::GameEngine()` (after `PortEnhancements_Init()`); `Accessibility_Shutdown()` at top of `GameEngine::Destroy()`.
- `CMakeLists.txt` — `ExternalProject_Add(PrismExternal …)` block after `add_subdirectory(libultraship)`. POST_BUILD copies `prism.dll` next to `Starship.exe`.

## Surprises (the ones that matter)

1. **PRISM target name collides with libultraship's `prism`.** libultraship pulls `KiritoDv/prism-processor` (a Fast3D shader template processor, completely unrelated) and exposes it as a CMake target named `prism`. ethindp PRISM's CMake target is also `prism`, and it owns the `<prism/...>` include namespace. FetchContent put both in the same CMake graph and broke libultraship's compilation. **Fix:** build PRISM via `ExternalProject_Add` (separate CMake invocation) and link by absolute path. Do not use FetchContent for PRISM.

2. **`prism_registry_acquire_best` already initializes the backend.** The upstream README and the WebFetch summary suggest you must call `prism_backend_initialize(backend)` after acquiring; in practice that returns `Already initialized`. The wrapper just acquires-and-uses. If a future PRISM update changes this, the symptom will be either a missing-init failure on speak, or the `Already initialized` line resurfacing.

3. **C++23 toolchain floor.** PRISM uses `<expected>` and `<stdalign.h>`. On Windows that needs MSVC 14.38+ (Visual Studio 2022 17.8+). Older 17.x toolsets fail inside `_deps/.../prism-src/`. Project's effective minimum is now 17.8.

4. **Pre-existing tinyxml2 dynamic-build quirk** (not caused by this spike). Some libultraship transitive FetchContent fetches `leethomason/tinyxml2` and builds it as a SHARED library; a `pkgRedirects/tinyxml2-config.cmake` entry then hijacks `find_package(tinyxml2)` so libultraship picks up the dynamic build instead of vcpkg's static one. Result: `Starship.exe` imports `tinyxml2.dll` at runtime. **Workaround in CMakeLists.txt:** POST_BUILD copy of `$<TARGET_FILE:tinyxml2>` next to `Starship.exe` when the target is a SHARED_LIBRARY. **Real fix is upstream:** find which dep declares it (none in our top-level CMakeLists or libultraship's `windows.cmake`/`common.cmake` reference it directly — a transitive somewhere) and stop the FetchContent or force `BUILD_SHARED_LIBS=OFF` for it.

## Diagnostic tip

Logger isn't fully wired until late in `GameEngine::GameEngine()`. If you ever see "no Accessibility log line at all" rather than a real error, calls have been moved too early in the constructor. Keep them after `PortEnhancements_Init()`.

The wrapper logs one of:

- `Accessibility: PRISM backend '<name>' ready` — happy path.
- `Accessibility: prism_init returned NULL; TTS disabled`
- `Accessibility: no PRISM backend available; TTS disabled`
- `Accessibility: prism_backend_speak failed (<error>)`

Logs land in `build/Release/logs/Starship.log`.

## Out of scope (still)

Event listeners (`src/port/hooks/`), ImGui menubar entry, CVars (enable),
per-screen narration calls, macOS/Linux verification (not high priority), release packaging. These
are the follow-up mod work. The spike's API surface
(`Accessibility_{Init,Speak,Shutdown}`) is stable enough to layer them on.
