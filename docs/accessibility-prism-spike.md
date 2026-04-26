# Accessibility Spike: PRISM Integration

This document is the kickoff brief for an experimental integration of [PRISM](https://github.com/ethindp/prism) into Starship as the screen-reader / TTS backend for a planned blind-accessibility mod. It captures the conclusions of a prior design discussion so that a fresh Claude session can begin implementation work without re-deriving the context.

## Goal of the spike

Prove that PRISM can be brought into Starship's CMake build, linked into the executable, and used to announce a single message through the user's active screen reader (or system TTS) when the game starts up. Nothing more.

If the spike succeeds, the larger accessibility mod (HUD event announcements, menu narration, level-state cues) will be built on top of this foundation in subsequent work. If the spike fails — typically because Prism's C++23 requirement clashes with the project's MSVC v143 toolset, or because the platform speech backends fail to initialize — we'll reassess and likely fall back to a per-platform DIY wrapper around the OS-native APIs (NVDA controller client / SAPI on Windows, AVSpeechSynthesizer on macOS, libspeechd on Linux).

## What PRISM is

PRISM ("Platform-agnostic Reader Interface for Speech and Messages") is a C library with `extern "C"` headers that fans out a single `prism_backend_speak(text, interrupt)` call to whichever screen reader or TTS engine is available on the current platform:

- **Windows**: NVDA controller client (when NVDA is running), JAWS, ZDSR, SAPI fallback
- **macOS**: AVSpeechSynthesizer; routes through VoiceOver when active
- **Linux**: speech-dispatcher (D-Bus), Orca

License: MPL-2.0. C++23 internally; the public header is plain C.

## Why PRISM was chosen over alternatives

The user is building this mod primarily for personal use, with a possible later distribution as a fork for other blind users. PRISM's appeal is that it covers all the desktop platforms a forked release might care about with a single API and zero per-call platform branching in the integration code. The cost is that Starship has to absorb PRISM's build complexity (C++23, generated D-Bus / MIDL code, system speechd dependency on Linux). The user accepted that trade given the long-term goal of cross-platform support.

Alternatives considered and rejected for this spike: SRAL (smaller scope, fewer backends), Tolk (Windows-only), and a bespoke per-platform wrapper inside `src/port/accessibility/`. The bespoke wrapper remains the documented fallback if PRISM proves unworkable.

## How PRISM will be brought in

**FetchContent everywhere** — the user chose this over the per-platform vcpkg/FetchContent hybrid for simplicity (single code path, no branching by host OS). PRISM is added via `FetchContent_Declare` in the top-level `CMakeLists.txt`, pinned to a specific commit hash, and made available at configure time.

This mirrors the existing precedent for `dr_libs` in the build. It does mean the first configure on a fresh checkout requires network access, which is consistent with the rest of the project's behavior.

The submodule and vendored options were also viable; they are documented at the end of this file in case FetchContent turns out to have downsides we did not anticipate.

## Where the integration lives in the codebase

A new directory:

```
src/port/accessibility/
  Accessibility.h        // C-callable API consumed by C game code
  Accessibility.cpp      // C++ wrapper around PrismContext
```

For the spike, that's all. The full mod design eventually adds `AccessibilityUI.cpp` (ImGui menubar entry) and `AccessibilityHooks.cpp` (listeners for game events from `src/port/hooks/`), but neither is needed to verify Prism works.

## API shape (spike scope)

`Accessibility.h` exposes the minimum needed:

```c
void Accessibility_Init(void);
void Accessibility_Shutdown(void);
void Accessibility_Speak(const char* text, bool interrupt);
```

Every function is a no-op when `HAVE_PRISM` is not defined (Switch build, or any build where Prism was unavailable at configure time).

## Touch points in existing files

Two changes only:

1. **`CMakeLists.txt`** — add the `FetchContent_Declare(prism ...)` block, gate it on `if(NOT NSWITCH)`, set `target_compile_features(prism PRIVATE cxx_std_23)` to keep C++23 isolated to PRISM's own targets, define `HAVE_PRISM` for the Starship target, and add a POST_BUILD copy step for the Prism shared library. The accessibility wrapper sources also need to be added to the build's source list — note that the existing `list(FILTER ALL_FILES EXCLUDE …)` block in `CMakeLists.txt` only globs specific subdirs under `src/`, so verify `src/port/accessibility/` is actually picked up before assuming it builds.

2. **`src/port/Engine.cpp`** — call `Accessibility_Init()` after the `Ship::Context` is constructed in `GameEngine::GameEngine()`, then call `Accessibility_Speak("Starship accessibility check", false)` so we get an audible startup announcement. Call `Accessibility_Shutdown()` from `GameEngine::Destroy()`. The Engine class is declared at `src/port/Engine.h:25`.

## C++ standard isolation

The project sets `CMAKE_CXX_STANDARD 20` at `CMakeLists.txt:31`. Do **not** bump this. Instead, raise the standard only on PRISM's CMake targets via `target_compile_features(prism PRIVATE cxx_std_23)` after `FetchContent_MakeAvailable(prism)`. The wrapper code in `src/port/accessibility/` stays C++20 and only uses Prism through its C API, so the rest of Starship is unaffected.

The known risk here is that MSVC v143 (pinned in this project for Visual Studio 2022) supports most of C++23 but not all. If Prism uses a feature v143 doesn't have, the spike will fail at compile time inside Prism's source. The fallback is bumping the project's MSVC toolset to v144 (VS 2022 17.10+).

## Spike target platform

**Windows only for the spike.** The user runs Windows; verifying on one platform first keeps the loop tight. Cross-platform expansion happens after Windows works:

- **macOS** — should work without further changes; AVSpeechSynthesizer is built into the OS, no system packages needed. Verify on the existing macos-14 universal-binary CI job and confirm Prism's Objective-C++ files compile for both `arm64` and `x86_64`.
- **Linux** — requires `libspeechd-dev` and `libglib2.0-dev` at build time and a running `speech-dispatcher` daemon at runtime. Add the apt-get line to the Linux CI job. If `pkg_check_modules(SPEECHD speech-dispatcher)` fails on a contributor's box, Prism builds without the Linux backends and the wrapper degrades to no-op — the build does not break.
- **Switch** — explicitly excluded via `if(NOT NSWITCH)` around the FetchContent block. Wrapper compiles to no-ops. No screen reader exists on Switch homebrew anyway.

## Verification criteria for the spike

The spike is successful if, on Windows:

1. A clean configure + build succeeds without errors.
2. `Starship.exe` launches and the user hears the startup announcement, either through their running screen reader (NVDA, JAWS) or through the SAPI default voice.
3. `Accessibility_Shutdown()` does not crash on exit.

That is the entire success bar. The mod design (event listeners, menu narration, voice/rate config, etc.) is out of scope for this spike.

## Out of scope for the spike

Explicitly **not** doing in the spike:

- Listening to game events through `src/port/hooks/`
- Adding an ImGui submenu under `src/port/ui/`
- Adding CVars for enable/disable, voice selection, speech rate
- Calling `Accessibility_Speak` from anywhere other than `GameEngine::GameEngine()`
- Linux or macOS verification — Windows only
- Packaging the result as a release artifact

These all happen in follow-up work once we know PRISM functions inside this build.

## Open risks to watch during implementation

- **MSVC v143 + C++23 compatibility.** If PRISM's source uses C++23 features not in v143, compilation will fail inside `_deps/prism-src/`. Either bump the toolset or, if it's a small number of features, see if PRISM has a configure option to disable them.
- **PRISM CMake invoking MIDL on Windows.** PRISM's Windows backend uses MIDL-generated NVDA controller stubs. MIDL ships with the Windows SDK and is normally on `PATH` inside a VS developer prompt, but a non-developer-prompt configure could fail to find it. Worth knowing if the configure step errors in `idl/` or similar.
- **Shared-vs-static library default.** PRISM defaults to building shared libraries. The POST_BUILD copy step must place `prism.dll` next to `Starship.exe`. If we'd prefer a single-binary distribution later, investigate whether PRISM can be coerced to static; for the spike, shared is fine.

## Reference

- PRISM repository: https://github.com/ethindp/prism
- PRISM public header: https://github.com/ethindp/prism/blob/master/include/prism.h
- This project's CMake top: `CMakeLists.txt`
- This project's existing FetchContent precedent: search for `FetchContent` in `CMakeLists.txt` (used for `dr_libs`)
- Engine entry points: `src/port/Engine.h`, `src/port/Engine.cpp`, `src/port/Game.cpp`
- Event bus (used in later mod work, not in the spike): `src/port/hooks/impl/EventSystem.h`

## Alternatives if FetchContent disappoints

Recorded for the next session in case FetchContent turns out to have offline-build or pinning issues we did not anticipate:

- **Submodule under `libs/prism/`** — same pattern as `libultraship/`, `tools/Torch/`, `tools/asm-differ/`. Deterministic, works offline after initial clone, but requires `git submodule update --init`.
- **vcpkg on Windows + FetchContent on Linux/macOS** — matches the `SDL2_net` pattern in `CMakeLists.txt:370` (vcpkg under MSVC, `find_package` elsewhere). PRISM is in vcpkg as `ethindp-prism`. Cleanest on Windows but relies on PRISM eventually appearing in Linux/macOS package managers.
- **Vendor in-tree** — copy PRISM source into `libs/prism/` and commit it. MPL-2.0 permits this if `LICENSE` and `NOTICE` are preserved. Best for cutting reproducible fork releases later; worst for keeping current with upstream PRISM.
