# Cutscene Audio Descriptions — Feasibility Notes

## Status

**Preliminary. No code, no commitments.** Discussion-only feasibility pass on 2026-04-30. The conclusions below are leanings, not locked-in choices — they are written down so a future session can pick up without re-deriving them, not so we feel obligated to follow them. Expect details to change as implementation surfaces things this discussion missed.

## The idea

For blind players, narrate what's happening visually during in-engine cutscenes. The author writes a timestamped script per cutscene; the existing TTS transport (`src/port/accessibility/Tts.{h,cpp}`) speaks each line as its moment arrives. Source content for an AD script can come from a sighted collaborator or from an AI describer working off a video capture (Gemini was mentioned as a candidate) — or some mix.

## Why it fits this engine

A few properties make this cleaner than it would be in most games:

- **All SF64 cutscenes are in-engine, not pre-rendered video.** Title intro, mission intros/outros, the ending, and the inter-mission radio sequences run as game overlays (e.g. `src/overlays/ovl_ending`). The port has direct visibility into when each starts and ends — there's no opaque video decoder to sync against.
- **Game logic is locked to a fixed 30 fps timestep.** A frame counter from cutscene start is deterministic and survives pause, slowdown, and debug fast-forward with no extra work. Wall-clock time would be the harder path; we get to skip it.
- **Plumbing already exists.** `src/port/hooks/` (event bus) plus the PRISM-backed TTS transport already cover the full path: fire an event when a cutscene starts → consumer mod listens, frame-counts, calls `Tts_Speak(...)`. Same recipe as the title-screen / menu announcements that just shipped.

## Architecture sketch (tentative)

1. **Game side:** at each cutscene's overlay entry point, fire a `CutsceneStarted(id)` event (and `CutsceneEnded` / `CutsceneSkipped` on exit). Same `DEFINE_EVENT` / `CALL_EVENT` mechanism used by `MenuEvent.h`. New events probably live in a new `hooks/list/CutsceneEvent.h`.
2. **Consumer mod** (likely a sibling of `src/port/mods/Accessibility.{c,h}`, name TBD): on `CutsceneStarted`, load the script for that ID, start a per-frame tick that fires each line via `Tts_Speak(...)` as its trigger frame is reached. Cancel pending lines on `CutsceneEnded` / `CutsceneSkipped`.
3. **Non-interrupting playback** — the TTS module already supports this. If a new line tries to fire while the previous is still speaking, drop or queue (default lean: drop). Authors keep individual lines short to minimise overruns.
4. **Gating** on `gAccessibilityScreenReader` happens at fire time (matching existing announcements), so the CVar can be toggled at runtime without restart.

## Script format (tentative)

One YAML file per cutscene, filename = cutscene ID, bundled into `starship.o2r` via a Torch importer.

```yaml
# Optional: where the timestamps were transcribed from. Ignored by the importer.
source: https://youtu.be/...
# Optional: subtracted from every "at" value before frame conversion.
# Use it when transcribing from a longer recording where the cutscene starts
# partway through (e.g. a YouTube compilation video).
offset: 1:00
lines:
  - at: 1:05         # mm:ss[.f] form
    text: "Fox stands on the tarmac, gazing skyward."
  - at: 1:12.5       # decimal seconds also accepted (bare 12.5 too)
    text: "An Arwing descends from the clouds."
```

### Importer responsibilities

- Accept both `12.5` (decimal seconds) and `mm:ss.f` (subtitle-style) for `at:`.
- Subtract `offset` from each `at:`. **Error** on a negative result (likely a typo).
- Convert to absolute frame numbers: `round((at - offset) * 30)`. **Error** on duplicate frames after rounding, or on non-monotonic ordering.
- Optional **warning** when the gap to the next line is too short for an average TTS speech rate to fit the current line — advisory, not blocking.
- Emit a compact binary blob the runtime reads via `LOAD_ASSET("accessibility/cutscenes/<id>")`. (Concrete on-disk shape is not yet decided — pick whatever's convenient when we get there.)

### Why this shape / why this pipeline

- **yaml-cpp is linked into Torch but NOT into the runtime.** Verified during this discussion: the runtime's vcpkg payload (root `CMakeLists.txt`) is `zlib bzip2 libzip libpng sdl2 glew glfw3 nlohmann-json tinyxml2 spdlog libogg libvorbis` — JSON and XML are available, YAML is not. The `YAML_CPP_STATIC_DEFINE` line near the top of the root `CMakeLists.txt` looks like an inert leftover. So if we want YAML for authoring (we do — friendliest format for non-technical contributors), the build *must* transform it. Convenient, because Torch already speaks YAML fluently.
- **Author timestamps, not frames.** Authors are watching a video and reading a clock; `1:23.5` matches their cognition, `frame: 2505` does not.
- **Conversion at build time, not runtime.** Frame rate is fixed at 30 fps and the math is trivial, so deferring buys nothing and introduces float drift across longer cutscenes. Frame-counting also pauses naturally when game logic pauses; second-counting at runtime would need extra plumbing.
- **Bundling via `starship.o2r` (vs. a raw file in `assets/`).** Matches the asset-system pattern, gives `mods/`-style override for free (community translations, alternate verbosity tracks), and keeps the runtime YAML-free.

## Open questions / things to verify before / during implementation

- **Cutscene entry-point inventory.** Are all cutscenes we care about reached through a single instrumentable path, or do title intro / mission intros / mission outros / inter-mission radio / Andross intro / ending each take a different path? Each unique entry site needs its own `CutsceneStarted` fire. **Action:** grep `ovl_ending`, mission overlay entry points, and the radio/comms sequencer to enumerate them when implementation starts.
- **PRISM behaviour under game pause.** If the game is paused, should TTS continue, pause, or stop? The frame counter naturally pauses with game logic — but PRISM's own playback does not, and it may need an explicit `Tts_Stop` (which doesn't exist yet) on pause.
- **Skip / cancel paths.** What inputs/states cause a cutscene to end early? Each one needs to produce `CutsceneEnded` / `CutsceneSkipped` so pending lines don't leak into the next screen.
- **Torch importer effort.** Haven't checked whether Torch has a simple-data-blob template we can mimic, or whether each importer is bespoke. Ideally we copy an existing simple importer rather than write one from scratch.
- **Drop vs. queue policy for overrun.** Default lean is "drop the new line" but the right call depends on what real scripts look like. Decide once we have a script in hand.
- **Mods / override behavior end-to-end.** Verify the `starship.o2r` → `mods/*.o2r` override flow actually works for these resources, since the long-term value (community translations, alternate verbosity) depends on it.
- **`Tts_Stop` / cancel API.** Current TTS transport has `Tts_Init`, `Tts_Shutdown`, `Tts_Speak`, `Tts_IsAvailable`. We probably need a way to cancel speech on cutscene skip — extend the transport when we hit it.

## What we explicitly did NOT decide

- The on-disk binary format produced by the importer.
- Per-line metadata (priority, category, "skippable" flag, language code).
- Whether multiple description tracks per cutscene (e.g. terse vs. verbose) is a v1 feature or future work.
- Localization model — one YAML per cutscene per language vs. a single YAML with multiple language sections.
- Naming and exact file location of the consumer mod.
- Whether dev builds should support hot-reloading raw YAML for faster authoring iteration (probably nice-to-have, not v1).

## Cross-references

- `docs/accessibility-prism-spike-result.md` — TTS transport architecture, the PRISM integration quirks that shaped the current two-layer split.
- `CLAUDE.md` § "Accessibility fork" — overall project direction and the announcement-via-event-bus recipe this feature follows.
- `src/port/hooks/list/MenuEvent.h` — closest existing precedent for the event shape we'd add for cutscenes.
