# Cue3D extensions — implementation plan

Status: **planned, not implemented** (2026-07-12). This document is the contract for a
future implementation session (expected to coordinate subagents). It records the agreed
scope, the API design, the per-package work breakdown, and the verification story.
Written against the code as of commit `47d7b2d9`.

## Goal

Extend the accessibility audio system beyond "HRTF-positioned looping cue" so future
gameplay features can use:

1. **One-shot sounds** — play a sound once (notifications, warnings), reliably
   re-triggerable. Blocked today by a known race (review CUE3D-13, documented in
   `Cue.h`'s registration comment).
2. **Pulsed playback** — a per-source repeat interval so a cue can tick like a geiger
   counter, with the rate as the signal (targeting assistance, distance-as-pulse-rate,
   "repeat this warning every N seconds").
3. **Render modes** — per-source choice of HRTF (today's path), clean constant-power
   **stereo pan** (dry "interface channel" sounds, e.g. wall-to-your-left), or
   **direct/center** (non-positional notifications).
4. **Per-source low-pass filter** — a settable "muffle" (future meaning: occluded
   target). The DSP already exists as the rear-muffle; this generalizes it.
5. **Synthesized tones as first-class cue sounds** — the seam already has
   `Cue3D_LoadPcm`; the Cue layer gets a generator option so a cue need not ship a WAV.
6. **Click-free starts/stops** — a short gain ramp in the backend (fixes review
   CUE3D-15's click concern), needed anyway once one-shots and interval restarts make
   start/stop frequent.
7. **A live Cue3D test bench** in the F1 developer menu — every capability exercisable
   by ear, no restart, built in the same session as the capabilities.

Pitch is already supported (`Cue3D_SetPitch`) and needs no system work.

### Explicitly out of scope (agreed 2026-07-12)

- Cue-vs-cue priority/ducking and mixing-headroom retuning (`kOutputTrim`), and
  TTS-over-cue ducking (CUE3D-16). Known pressure, deferred.
- A cue-vocabulary document (which acoustic dimension means what). Sound design is
  still being explored with player feedback; do not add one yet.
- Code-level restrictions keeping deliberate pulse rates away from the rear-tremolo
  rate. The overlap is a design consideration only — do **not** clamp for it.
- Gameplay features using the new capabilities. They come in later sessions, verified
  one at a time.
- Shared-PCM loading across voices (each voice keeps its own decoded copy; fine at
  current pool sizes).

## Current state (orientation for the implementing session)

Three layers, all under `src/port/`:

- **Seam**: `accessibility/Cue3D.h` — backend-agnostic C API. Read its header comment
  first; it owns the coordinate convention (+x right, +y up, +z ahead) and the
  threading contract (game thread pushes params; audio callback reads once per block).
- **Backend**: `accessibility/Cue3DSteamAudio.cpp` — Steam Audio HRTF hosted by
  miniaudio on a second OS audio device. `ProduceBlock()` is the per-block DSP loop:
  per-source PCM fill (with pitch/linear interpolation), distance attenuation,
  rear-hemisphere muffle/dip/tremolo, binaural effect, mix, trim, clamp. Fixed pool of
  16 `Cue3DSource` slots; `#else` branch has no-op stubs for non-`HAVE_STEAM_AUDIO`
  builds — **every new seam function needs a stub there too**.
- **Cue layer**: `accessibility/Cue.{h,cpp}` — game-agnostic cue definitions with voice
  pools, volume CVars, preview, keyed refresh-or-stop targeting. Consumer:
  `mods/AccessibilityCues.cpp` (two `CueRegistry_Register` calls, ~line 395). Settings
  UI iterates `CueRegistry_All()` for the volume sliders.
- **Existing smoke test**: `accessibility/SpatialAudioTest.{h,cpp}`, started from
  `Accessibility_Init` (`mods/Accessibility.cpp`) when the `gAccessibilitySpatialTest`
  CVar is set — restart required, a wart the bench removes. F1 checkbox in
  `ui/ImguiUI.cpp` ~line 1046, inside Developer → Blind Starship, next to the
  rear-effect sliders.

## Design

### D1. Seam API additions (`Cue3D.h`)

New declarations (with the usual doc comments; stubs in the backend's `#else` branch):

```c
// Advisory playback status: true while the source is audibly rendering. For looping
// sources this mirrors Play/Stop; for one-shots it goes false when the sound finishes.
// May lag reality by one audio block (~21 ms) — treat as advisory, not a fence.
bool Cue3D_IsPlaying(Cue3DSource* source);

// Restart cadence for a LOOPING source, in wall-clock seconds (independent of pitch).
// 0 (default) = seamless loop, exactly today's behavior. > 0 = the sound restarts from
// its beginning every `seconds`, measured start-to-start: silence pads the gap when the
// interval exceeds the sound's length; a shorter interval truncates and restarts.
// Sample-accurate (the audio callback owns the timing). Cheap; push it every tick like
// position — a live rate change takes effect immediately. Ignored on one-shot sources.
void Cue3D_SetInterval(Cue3DSource* source, float seconds);

// How the source is rendered into stereo. Position (Cue3D_SetPosition) stays the input
// in every mode; the mode selects the renderer:
//   CUE3D_MODE_HRTF   — binaural HRTF + distance attenuation + rear effect (default).
//   CUE3D_MODE_PAN    — constant-power stereo pan from the horizontal direction;
//                       distance attenuation applies; rear effect and HRTF do not.
//                       Front/back and elevation collapse (inherent to stereo pan).
//   CUE3D_MODE_DIRECT — dead center, no spatialization, no distance attenuation, no
//                       rear effect; gain/pitch/interval/low-pass still apply.
typedef enum Cue3DMode { CUE3D_MODE_HRTF = 0, CUE3D_MODE_PAN, CUE3D_MODE_DIRECT } Cue3DMode;
void Cue3D_SetMode(Cue3DSource* source, Cue3DMode mode);

// Per-source low-pass "muffle", independent of (in series with) the rear muffle.
// cutoffHz <= 0 disables it (the default). Applies in every render mode.
void Cue3D_SetLowPass(Cue3DSource* source, float cutoffHz);
```

Header-comment updates: the "backend owes the player" paragraph should mention that
PAN/DIRECT are deliberately *dry* renderings an alternate backend must also honor
(trivially — they're backend-neutral math), so the one-file-swap property is intact.

`Cue3D_Play`'s doc comment also changes: with the gen counter (D2), **every Play
restarts the sound from its beginning** — including a looping source that was
Stopped (previously resumed mid-loop) and a Play issued while already playing
(previously a no-op). This is a deliberate, accepted behavior change: for short cue
loops a rewind is inaudible-to-preferable, and it is exactly what one-shot re-trigger
needs. The current "Begin (or resume) playback" wording must be replaced, not left
to drift from the implementation. (Cue-layer impact: an enemy-cue voice reacquired
after a reap now restarts at sample 0 instead of resuming; a re-pressed preview
restarts its loop. Both are fine — but they are *known* deltas, so the verification
pass's "sounds unchanged" bar means "no new artifacts", not bit-identical phase.)

### D2. Backend changes (`Cue3DSteamAudio.cpp`)

**One-shot re-trigger race (CUE3D-13).** Root cause: `playing` is written by both
threads (game on Play/Stop, callback clearing at one-shot EOF), so a Play landing as
the callback finishes the previous playthrough can be overwritten. Fix by splitting
ownership so no flag *that playback correctness depends on* has two writers (the
advisory `audible` below deliberately keeps two writers — see its bullet):

- Game → audio: `playing` (the stop lever, game-thread writes only) and a new
  `std::atomic<uint32_t> startGen`, incremented by `Cue3D_Play`.
- Audio-thread-only locals on the source (like `cursor`): `lastGen`, `ended`.
  When the callback sees `startGen != lastGen` it rewinds the cursor, clears `ended`,
  adopts the gen. A one-shot EOF sets `ended = true` locally — it never touches
  `playing`. Render condition: `playing && !ended` (plus the ramp tail, below).
- Audio → game: `std::atomic<bool> audible`, written by the callback each block
  (`playing && !ended`), read by `Cue3D_IsPlaying`. `Cue3D_Play` also eagerly stores
  `audible = true` so a same-tick IsPlaying doesn't see a stale false. Note this flag
  therefore *does* have two writers — that is acceptable here and only here, because
  it is advisory: a lost update costs one block (~21 ms) of staleness, worst case a
  finished voice looks busy (or a busy voice reusable), which for voice bookkeeping
  is benign. Correctness of playback control never depends on `audible`. (Reviewer in
  package D: don't flag this as an ownership violation; the rule protects flags the
  render decision reads, and `audible` is not one.)

**Start/stop ramps (CUE3D-15 clicks).** A per-source, audio-thread-only ramp gain
smoothed toward `shouldRender ? 1 : 0` with a ~5 ms time constant, multiplied into the
mono fill. Consequence for the block loop: a stopped source must keep rendering until
its ramp reaches silence — the current early `continue` on `!playing` becomes
"skip only when ramp is fully silent too". Restarts (gen bump, interval expiry) ramp in
from zero. Seamless loop wrap is untouched (no ramp). A truncating interval restart may
still click if the blip is hot at the cut point; acceptable — author blips shorter than
the fastest interval (document in the bench tooltip, not code).

**Interval.** `std::atomic<float> intervalSec` (game-writable), plus audio-thread-only
frames-since-start counter. Wall-clock frames, so pitch does not change the cadence.
Each block: if interval > 0 and the counter passes `interval * sampleRate`, restart
(cursor 0, ramp-in, counter rewound preserving overshoot so the cadence doesn't drift).
Past the PCM's end with interval pending, emit silence (the gap). Sanitize in the
setter like `Cue3D_SetPitch` does: non-finite → 0 (off), clamp to [0, 30] seconds.

**Render modes.** `std::atomic<int> mode` read once per block. The block loop already
computes the normalized game-frame direction before the DSP; branch after the PCM fill:

- HRTF: exactly today's path (rear effect + `iplBinauralEffectApply`).
- PAN: skip rear effect and the binaural call. Pan position `p = dx / sqrt(dx² + dz²)`
  (sin of azimuth; 0 when the horizontal direction is degenerate, e.g. straight
  above). Constant-power: `angle = (p + 1) * π/4`, `L = cos(angle)`, `R = sin(angle)`,
  applied to the mono fill straight into the accumulators. Distance attenuation stays.
- DIRECT: skip rear effect, binaural, *and* distance attenuation; mix mono to both
  channels at equal power (`1/√2` each).

Footgun: "skip rear effect" is **two** code sites, not one. The rear *gain dip* is
folded into `effGain` at the PCM fill (`1.0f - rearGainDip * rearAmount`, currently
~line 305), separate from the muffle/tremolo loop; `distGain` lives in the same
expression. So PAN must drop the dip term from the fill's gain (else it keeps a
phantom rear volume dip), and DIRECT must drop both the dip term and `distGain` —
skipping only the filter/tremolo block is not enough.

**Per-source low-pass.** `std::atomic<float> lowPassHz` + its own one-pole state
(`lpState2`), run in series with (before or after — implementer's pick, keep it
consistent) the rear muffle, active in all modes when cutoff > 0. Same alpha-clamp
discipline as the existing filter. Sanitize in the setter, **in this order** (the
order matters: 0 is the documented *disable* value, so it must never be clamped up
to 20 Hz — that would turn "off" into near-total muffle): non-finite **or** <= 0 →
store 0 (off); otherwise (positive finite) clamp to [20, 20000].

`CreateSource` resets every new field; `Cue3D_Shutdown` needs no change beyond that.

### D3. Cue layer changes (`Cue.{h,cpp}`)

**Registration options struct** — the signature is at its limit; move to a spec before
adding options:

```cpp
struct CueSpec {
    const char* wavPath = nullptr;  // app-relative; exactly one of wavPath/generator set
    std::function<std::vector<float>(int sampleRate)> generator; // synthesized alternative
    int maxVoices = 1;              // clamped to [1, 8] as today
    bool loop = true;               // false = one-shot cue, driven via PlayOnce
    Cue3DMode mode = CUE3D_MODE_HRTF; // pushed to each voice's source at load
    bool hiddenFromSettings = false;  // bench cues stay out of the volume-slider list
};
Cue* CueRegistry_Register(const char* id, const char* name, const char* description,
                          const CueSpec& spec);
```

Replace the old signature outright and migrate the two call sites in
`AccessibilityCues.cpp` (`sRingCue`, `sEnemyCue` — both become
`{ .wavPath = ..., .maxVoices = ... }`). `EnsureVoiceLoaded` picks `Cue3D_Load` vs
generate-then-`Cue3D_LoadPcm` (generator runs once per voice at `Cue3D_GetSampleRate()`;
the existing try/catch boundary and failure latch cover both). The settings UI's
volume-slider loop (`ImguiUI.cpp`, generated from `CueRegistry_All()`) skips hidden
cues — expose the flag via a small `Cue::HiddenFromSettings()` accessor.

**Dynamic per-voice params struct** — same principle for the drive path:

```cpp
struct CueTarget {
    float x = 0.0f, y = 0.0f, z = 1.0f;
    float pitch = 1.0f;
    float intervalSec = 0.0f; // 0 = seamless loop
    float lowPassHz = 0.0f;   // 0 = off
};
void SetTarget(const CueTarget& t);
void TargetVoice(uint64_t key, const CueTarget& t);
```

Keep the existing four-float overloads as thin conveniences delegating with defaults,
so the ring/enemy drivers don't change. The voice struct stores the extra fields and
pushes them alongside position/pitch.

**One-shot API.** `void PlayOnce(const CueTarget& t)` on cues registered with
`loop = false`: reclaim any voice whose one-shot has finished
(`voice.playing && !Cue3D_IsPlaying(voice.source)` → mark idle), acquire a free voice
(un-keyed — `hasKey` stays false so the refresh-or-stop reap never touches it), set
params, `Cue3D_Play`. If the pool is exhausted mid-flight, steal the oldest playing
one-shot (they're fire-and-forget; a re-trigger beats a drop). "Oldest" needs an
ordering stamp — no per-voice start order exists today; add a per-Cue monotonic
counter stamped onto the voice at each PlayOnce (not wall-clock time). `Cue::Tick`
also refreshes one-shot `voice.playing` from `Cue3D_IsPlaying` so `PushGain`'s 1/√N
headroom count converges after sounds end. Looping cues keep today's bookkeeping
untouched — do not route their state through `IsPlaying`.

Guards, matching every other gameplay entry point: `PlayOnce` no-ops while
`mPreviewing` (the UI owns the cue; same rule as SetTarget/TargetVoice). Mixed use
is a caller bug, not UB: `Start`/`TargetVoice` on a `loop = false` cue and
`PlayOnce` on a looping cue both no-op with a one-time warn log.

**Preview of a one-shot cue.** The preview window stays ~3 s; a one-shot sound is
re-triggered about once per second within it so the player hears a few reps instead of
one blip followed by silence. Looping cues preview exactly as today.

### D4. Test bench (new: `accessibility/CueBench.{h,cpp}` + menu wiring)

Replaces `SpatialAudioTest.{h,cpp}` (delete it, its `gAccessibilitySpatialTest` CVar,
its F1 checkbox, and its calls in `Accessibility.cpp`) — the orbit becomes one bench
control, and the restart-required wart goes away. Structure mirrors the old file:
`CueBench_Init()` called from `Accessibility_Init` registers a `GamePostUpdateEvent`
listener unconditionally (cheap early-return while the bench is off — the pattern every
accessibility listener uses), `CueBench_Shutdown()` from `Accessibility_Exit` drops
handles before `Cue3D_Shutdown`. All controls are CVars in a new
F1 → Developer → Blind Starship → **"Cue3D test bench"** submenu (`ImguiUI.cpp`, next
to the rear-effect sliders; `UIWidgets` checkboxes/sliders/combos plus a button, all
self-voiced by the existing narrator). Everything takes effect live.

The bench drives the **raw seam** for the continuous source (that's the layer under
test) and a **hidden one-shot `Cue`** (via `CueSpec::hiddenFromSettings`) for the
`PlayOnce` path, so the Cue layer's new code gets bench coverage too. Register the
hidden cue with `CueSpec::generator` (the bench's sine blip), not a WAV — that gives
the generator path Cue-layer coverage, which the "Synthesized tone" checkbox (raw
seam only) does not.

Source lifetime constraint: the seam has **no per-source free** — a slot is only
reclaimed by `Cue3D_Shutdown`, and adding a `Cue3D_Free` would mean synchronizing
against a callback mid-block in the PCM, which this plan deliberately avoids. So the
bench must NOT recreate sources on toggles. It lazily creates **both** continuous
sources once (one from the WAV via `Cue3D_Load`, one from the generated tone via
`Cue3D_LoadPcm`) and the "Synthesized tone" checkbox switches which of the two is
playing. Fixed cost: 2 slots for the bench's continuous pair + 1 for the hidden cue,
alongside gameplay's up-to-9 — comfortably inside the pool of 16.

Controls, each mapping to a capability and an ear-verifiable pass condition:

| Control | Exercises | Pass by ear |
|---|---|---|
| "Test bench active" checkbox | seam lifecycle, no-restart | sound appears/disappears immediately |
| "Synthesized tone" checkbox | `Cue3D_LoadPcm` + generator path | sound swaps WAV ↔ generated tone (two pre-created sources; the toggle switches which plays — see the lifetime note above) |
| "Orbit" checkbox (else fixed ahead) | old smoke test, kept | 4 s front→right→behind→left sweep |
| Render mode combo (HRTF/Pan/Direct) | `Cue3D_SetMode` | orbit collapses to pure L–R sweep in Pan; centered and constant in Direct |
| Pitch slider (0.5–2.0) | existing `SetPitch` | pitch tracks slider |
| Interval slider (0–2 s, 0 = loop) | `Cue3D_SetInterval` | smooth geiger rate tracking the slider, no stutter or drift |
| Low-pass slider (off–8 kHz) | `Cue3D_SetLowPass` | muffle tracks slider in every mode |
| "Play one-shot" button | `PlayOnce`, gen counter | every press sounds, including rapid presses |
| "Rapid re-trigger" checkbox | CUE3D-13 fix + ramps under stress | re-fires every few ticks; no swallowed plays, no clicks |
| "Start/stop stress" checkbox | ramps | toggles the continuous source every few ticks; no clicks |

The synthesized tone lives in the bench file (a short enveloped sine blip generated at
`Cue3D_GetSampleRate()`), not in any API.

### D5. Documentation updates (same session, after code lands)

- `docs/accessibility-hrtf-cues.md` — mark CUE3D-13 and the click question resolved;
  add a short "capabilities" paragraph (one-shots, interval, modes, low-pass,
  generator) pointing at `Cue3D.h`/`Cue.h` rather than duplicating them; note the
  bench replacing the smoke test.
- `AGENTS.md` accessibility section — update the cue-layer bullet and the smoke-test
  mention (one or two lines; keep it high-level, what + where).
- `docs/steam-audio-handoff.md` references the old smoke test ("Step 2") — leave the
  historical doc alone, but the bench's file comment should note it superseded
  `SpatialAudioTest`.
- No cue-vocabulary doc (out of scope, above).

## Work packages for the implementing session

Suggested subagent decomposition — packages own disjoint files, so B1/B2 can run in
parallel; C depends on both:

- **A (main session, first): freeze the contract.** Write the new `Cue3D.h`
  declarations + doc comments per D1, and the `CueSpec`/`CueTarget` shapes per D3
  (header-level). Small, sequential, and everything else codes against it.
- **B1: backend.** `Cue3DSteamAudio.cpp` per D2 — gen-counter fix, `audible`,
  ramps, interval, modes, low-pass, `#else` stubs, `CreateSource` resets. The one
  package with real DSP/threading subtlety; give it the strongest review.
- **B2: Cue layer.** `Cue.{h,cpp}` per D3 + the two-call-site migration in
  `AccessibilityCues.cpp` + the hidden-cue filter in `ImguiUI.cpp`'s volume list.
- **C: bench.** `CueBench.{h,cpp}`, menu submenu in `ImguiUI.cpp`, `Accessibility.cpp`
  wiring, deletion of `SpatialAudioTest.*` and its CVar/checkbox.
- **D: review + docs.** Adversarial review of B1 especially (thread ownership of every
  new field: exactly one writer, or atomic with a stated staleness budget), then D5.

Practical notes for that session: build with
`cmake --build build/x64` (MSVC generator already configured); there is no test suite —
compile-clean plus the bench is the bar. Match `.clang-format` (4-space, 120 cols,
attached braces); `tools/format.py` needs clang-format-14 which may not be on this
Windows box — hand-match style if so. `CueBench.cpp` needs the
`port/CGameCompat.h`-before-`port/hooks/Events.h` include order (see the comment at the
top of `SpatialAudioTest.cpp`). New files under `src/port/accessibility/` are already
covered by the CMake glob (SpatialAudioTest was picked up without a CMake edit).

## Verification (the user's by-ear pass, after the session)

1. **Regressions first** — ring cue and enemy cue in Training sound unchanged
   (direction, distance falloff, Y→pitch, rear muffle/dip/tremolo sliders still live,
   volume sliders + previews still work, pause still silences). Two known, accepted
   deltas (see the `Cue3D_Play` note in D1): a restarting voice begins at the sound's
   start instead of resuming mid-loop, and every start/stop now has a ~5 ms ramp.
   "Unchanged" means no *new* artifacts — no clicks, no missing cues, no level shifts.
2. **Bench walk-through** — each control per the D4 table, in HRTF mode first, then
   the mode combo, then the stress toggles last (rapid re-trigger, start/stop stress:
   listen for swallowed plays and clicks).
3. Only after both pass: start building gameplay features on the new capabilities,
   one at a time, verifying as we go.
