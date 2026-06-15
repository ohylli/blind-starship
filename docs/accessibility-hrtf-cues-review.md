# Steam Audio / HRTF (Cue3D) — review findings

## What this document is

A record of a multi-perspective review of the Steam Audio / HRTF "Cue3D"
implementation, asking: **is it in a good state, and is it a good foundation for
future positional-cue work for blind players?**

It exists to be a **basis for later, per-issue discussion and decisions**. Each
issue below is written to stand on its own so a future session can pick one up in
isolation. Severities, framings, and recommendations are **the review team's**,
recorded as they were reached — they are inputs to a decision, not decisions.
Nothing here locks in next steps; the priority tiers are the team's suggested
ordering, not a committed plan.

It deliberately adds **no interpretation beyond the team analysis**. Where experts
disagreed on severity, both framings are kept rather than resolved.

- **Scope reviewed:** `src/port/accessibility/Cue3D.h`,
  `src/port/accessibility/Cue3DSteamAudio.cpp`, `src/port/accessibility/miniaudio_impl.cpp`,
  `src/port/mods/AccessibilityCues.cpp`, the Steam Audio block in `CMakeLists.txt`
  (~lines 346-419), the CI workflows, and the design docs
  (`accessibility-hrtf-cues.md`, `accessibility-cues-tuning.md`, `accessibility-enemy-cue.md`).
- **Review date:** 2026-06-15. Line numbers cited are from that date.
- **Method / participants:** see the appendix.

## Overall verdict (unanimous)

The implementation is in a **good state and is a sound foundation**, in two
distinct halves:

- The **Cue3D seam and the Steam Audio audio core are genuinely good and should be
  protected** (see "Strengths to protect").
- The **consumer layer (`AccessibilityCues.cpp`) and the hardening of the second
  audio device are where the work is** — before the cues are safe as a
  blind-player default, and before a third cue type is added.

Nothing found is a present-day crash or data-corruption bug. The highest-leverage
single observation, which all four reviewers reached independently: **`Cue3D_SetGain`
is the reserved master-volume / pause / ducking lever and nothing currently drives
it** — wiring it touches several issues below at once.

---

## Strengths to protect

The team independently agreed these are load-bearing and correct, and flagged them
as "do not touch":

- **The seam / backend boundary** (`Cue3D.h`, `Cue3DSteamAudio.cpp`): clean C ABI
  with opaque handles; the seam owns the game coordinate convention and the backend
  is the only place that knows Steam Audio's −Z axis (`Cue3DSteamAudio.cpp:244-247`).
  The doc's claim that OpenAL Soft would be a one-file swap was **verified true** —
  callers leak no backend detail.
- **The lock-free real-time design:** the `inUse` release/acquire publish gate, the
  audio-thread-only `cursor`, the fixed 16-source pool giving stable addresses, and
  exceptions caught at the `extern "C"` boundary — all **verified correct**.
- **The per-source held binaural effect:** correct — Steam Audio crossfades HRTF
  taps across the block, which prevents motion clicks.
- **The carry-buffer callback bridge and the linear-interpolation resampler**
  (including click-free loop-seam handling): **verified correct**.
- **The separate OS audio device is the right architecture** (not a stopgap):
  merging the spatializer into libultraship's SDL device would couple the seam to
  ulship internals and lose the swap property. The recommendation is to keep the
  separate-device model and harden it, not to merge it away.
- **Build hygiene already correct:** version pinning + CI cache key, the
  `HAVE_STEAM_AUDIO` gate with no-op stubs, the single-TU miniaudio implementation
  (no duplicate-symbol risk), and the accessibility WAV assets (git-tracked, reach
  the exe directory correctly — verified end-to-end). The **Windows path — the only
  platform that ships today — is solid, cached in CI, and packaged correctly.**

---

## Issues

Each issue has a stable ID (`CUE3D-n`), the team's severity (with any
framing split preserved), the code location, the finding, and the team's
recommendation. The "team priority" field records where the team placed it in their
suggested ordering; it is not a commitment.

### CUE3D-1 — Backend/device failure leaves the player in silence with no fallback
- **Severity:** HIGH (by consequence). Reviewers were explicit this is HIGH because
  the *outcome* is unacceptable for a primary navigation aid, **not** because it is
  likely — the Windows device path is solid, so the failure is rare today. The
  "no availability-query API" framing (a C-seam nicety) was separately rated
  LOW/MEDIUM; that is a different thing from the player-outcome framing.
- **Team priority:** must-fix before safe default-on (Tier A).
- **Location:** `AccessibilityCues.cpp:210-214`, `:402-406`; `Cue3DSteamAudio.cpp:343-359`.
- **Finding (verified):** With `gAccessibilityCue3D` on (default), a device-open or
  load failure makes `StartRingCue` / `StartEnemyCue` return with **no cue at all**.
  The SF64 fallback path is reachable only when the CVar is *off*, so on the default
  config a failure means total silence with no spoken reason — the rescue path is
  present but deliberately suppressed.
- **Team recommendation (option):** on a NULL source, fall through to the SF64 cue
  path (same always-available device) instead of returning; optionally add a spoken
  PRISM notice. This converts "silent" to "degraded but audible." The reviewer noted
  that this remedy is one branch and drops the player-facing severity to LOW.

### CUE3D-2 — Cue volume is uncontrollable; `Cue3D_SetGain` is wired to nothing
- **Severity:** HIGH (player-facing). The UX reviewer recommended promoting it from
  "parked open question" to must-fix-before-default; DSP, architect, and buildsmith
  all concurred it is the highest-leverage single wire-up in the review.
- **Team priority:** must-fix before safe default-on (Tier A); the wiring's permanent
  home is the Cue abstraction (CUE3D-5).
- **Location:** `Cue3D_SetGain` defined in `Cue3DSteamAudio.cpp:463-467`, called
  nowhere; the "Master Volume" slider is `ImguiUI.cpp:236`; per-frame push points are
  `AccessibilityCues.cpp:202-204` and `:394-396`.
- **Finding (verified):** `gGameMasterVolume` and BGM ducking (`SFX_FLAG_19`) do not
  reach the second OS audio device. The one volume control a blind player can find
  (Master Volume) does nothing to the cues. Cues can be masked by loud music or
  jarringly loud in quiet passages, with no trim available except console CVars.
- **Team recommendation (option):** each tick, drive `Cue3D_SetGain(src,
  gGameMasterVolume × cueVolumeCVar)`, and expose one cue-volume slider in the
  Accessibility menu. UX threshold for acceptable default-on = CUE3D-1 + this. The
  team noted that placing this wiring inside the Cue abstraction (CUE3D-5) makes it
  one site instead of N; without the abstraction it is copied into every cue's
  Refresh path (currently 2, growing).

### CUE3D-3 — No clip guard when summed spatialized sources exceed ±1.0
- **Severity:** Split, converging on "do it now, it's cheap." UX rated it HIGH and
  ranked it #1 by player impact (a clipped cue actively misleads direction). DSP
  rated it HIGH in round 1 then demoted to MEDIUM relative to the player-stranding
  issues (crackle vs. playable/not-playable). Architect held HIGH; buildsmith framed
  it HIGH-for-foundation / not-a-today-defect (currently bounded to 2 loop cues).
- **Team priority:** must-fix before safe default-on (Tier A); backend-local, no
  dependency on other items.
- **Location:** `Cue3DSteamAudio.cpp:271-280`.
- **Finding (verified):** Steam Audio spatializes but does not mix, so summing is done
  by hand with no limiter/clip guard. The ring and enemy cues both fire on Training
  near the aim line; summed peaks exceed ±1.0 → hard clipping at exactly the busy
  moment the player needs a clean cue. The problem grows linearly as cue types are
  added.
- **Team recommendation (option):** output trim (~0.5-0.7) plus a cheap safety clip in
  `ProduceBlock`.

### CUE3D-4 — Hardcoded 48 kHz never reconciled with the actual device rate
- **Severity:** Split by horizon. HIGH as a foundation / cross-machine correctness
  bug (DSP, buildsmith). MEDIUM as a *today* player-impact bug (UX), because Windows
  shared-mode WASAPI generally opens at 48 kHz, so it has not bitten the shipping
  player.
- **Team priority:** Tier A backend-local fix (do in parallel); must-fix before any
  second platform.
- **Location:** `Cue3DSteamAudio.cpp:340` (the `cfg.sampleRate = 48000` hint and the
  missing read-back); propagation points `:133`, `:320`, `:403`, `:394`.
- **Finding (verified):** `cfg.sampleRate` is a hint; the code never reads back
  `g.device.sampleRate`. If the OS negotiates 44.1 kHz, the whole cue stream plays
  ~8.8% slow / pitched down, which biases the Y→pitch elevation cue the player steers
  by.
- **Team recommendation (option):** read back the negotiated rate **and propagate it
  end-to-end** — the HRTF/effect `IPLAudioSettings.samplingRate`, the decoder target,
  and `Cue3D_GetSampleRate` must all follow the actual device rate, including
  rebuilding the HRTF/effects at that rate. DSP was explicit that reading the rate
  back without propagating it just moves the desync; a partial fix leaves it broken.

### CUE3D-5 — Dual-backend boilerplate does not scale; no Cue abstraction
- **Severity:** HIGH.
- **Team priority:** must-fix before cue #3 is added (Tier B). The team identified
  this as the structural prerequisite and the permanent home for CUE3D-1, CUE3D-2,
  and CUE3D-6.
- **Location:** `AccessibilityCues.cpp:164-237` (ring) vs `:362-434` (enemy).
- **Finding (verified by direct diff):** the ring cue and enemy cue are the same
  two-backend state machine copied nearly verbatim — backend enum, Get/Refresh/Start/
  Stop, lazy-load + cache, and the CVar-flip handling. Each new cue is ~80-90 lines,
  ~70 of them mechanical copies. At 5-10 cues this becomes N copies of the switch
  logic, and any per-cue fix (master volume, fallback, ducking) becomes N hand-edits.
- **Team recommendation (option):** introduce a `Cue` abstraction and implement
  CUE3D-1 (fallback), CUE3D-2 (master volume), and CUE3D-6 (handle nulling) inside it
  once, so each future cue inherits them. The team's framing: the abstraction is not
  competing with the player-facing fixes — it is the place those fixes land in one
  spot instead of being multiplied.

### CUE3D-6 — Cached `Cue3DSource*` handles dangle after shutdown (not nulled on Exit)
- **Severity:** HIGH-latent / MEDIUM-today. The team agreed it is benign today and a
  free fix; the HIGH label reflects the asymmetry with the test code and that the fix
  costs nothing once CUE3D-5 exists. If scored strictly on reachable-today, it is
  MEDIUM.
- **Team priority:** fold into the Cue abstraction's Exit (Tier B).
- **Location:** `AccessibilityCues_Exit` (`AccessibilityCues.cpp:501-504`);
  `Cue3D_Shutdown` frees all sources at `Cue3DSteamAudio.cpp:374-385`;
  `SpatialAudioTest.cpp:81-88` already nulls its handle.
- **Finding (verified):** `AccessibilityCues_Exit` stops the cues but never nulls the
  cached `sRing3DSource` / `sEnemy3DSource` pointers, while `Cue3D_Shutdown` frees
  every source. **Safe today only by teardown ordering** (`Engine.cpp:312` shuts down
  in the right order, and single-shot process teardown means listeners don't fire
  post-Exit). It breaks on any future Init/Exit/Init reuse, where listeners would touch
  freed sources. The shipping cue path did not replicate the fix the test code already
  has.
- **Team recommendation (option):** null the cached handles in `_Exit` (one line per
  cue); it disappears into the abstraction's Exit.

### CUE3D-7 — Linux/macOS: copied `libphonon.so` / `.dylib` will not load at runtime (no rpath)
- **Severity:** Downgraded HIGH→MEDIUM during the debate. MEDIUM as a *consequence
  today* (no non-Windows artifact ships; release jobs are `if: false`), but a real
  foundation trap (the recommendation to fix now was held).
- **Team priority:** foundation hardening, before a second platform (Tier C); fix
  recommended now because it is nearly free.
- **Location:** `CMakeLists.txt` POST_BUILD copy `:414-418`; verified repo-wide that
  no rpath is set anywhere except the unused iOS toolchain.
- **Finding (HIGH-confidence, partly suspected):** the build succeeds and the shared
  library sits next to the exe, but with no rpath, `dlopen` still fails with a
  "library not found" error that points nowhere near the cause. Windows is immune
  (it searches the exe dir). This bites the first developer who builds on Linux/mac to
  add a cue — the stated future work.
- **Team recommendation (option):** inside the existing non-Switch block, set
  `INSTALL_RPATH`/`BUILD_RPATH` to `@loader_path` on APPLE and `$ORIGIN` on UNIX (two
  lines, no Windows behavior change).

### CUE3D-8 — `miniaudio.h` pulled from a mutable upstream tag with no verification
- **Severity:** MEDIUM (verified). The one supply-chain item the build reviewer would
  actually block on.
- **Team priority:** foundation hardening (Tier C).
- **Location:** `CMakeLists.txt:387-389`.
- **Finding (verified):** the header is downloaded at configure time from a mutable
  git tag straight into the implementation TU. A force-moved tag would inject
  unverified code into the binary.
- **Team recommendation (option):** vendor the header (commit the 0.11.21
  `miniaudio.h` — public-domain, single file), which also fixes offline configure; or,
  if not vendoring, add `EXPECTED_HASH SHA256`. The Steam Audio zip (a versioned
  GitHub release asset) is lower risk — `EXPECTED_HASH` there was rated hygiene, not a
  blocker. The `if(NOT EXISTS phonon.h)` cache guard could be poisoned by a partial
  extract; tightening it to check `phonon.lib` instead of the header was noted as a
  when-convenient hardening.

### CUE3D-9 — No CI exercises Steam Audio on Linux/macOS end-to-end
- **Severity:** MEDIUM (verified).
- **Team priority:** foundation hardening (Tier C); this is why CUE3D-7 is invisible
  to CI.
- **Location:** `.github/workflows/{linux,mac}.yml` (PR jobs compile but never copy the
  runtime lib or launch); release jobs `if: false`.
- **Finding (verified):** only Windows is fully validated end-to-end. The PR Linux/mac
  jobs compile the code but never copy the runtime library or launch, so the rpath gap
  (CUE3D-7) and any Linux device-coexistence problem stay hidden.
- **Team recommendation (option):** none locked; the team also flagged confirming Linux
  shared-mode device coexistence between miniaudio's backend and libultraship's SDL
  device (possible default-sink contention; currently untested).

### CUE3D-10 — Optional Steam Audio DLLs dropped from the Windows package
- **Severity:** MEDIUM (verified), flagged as a conscious drop rather than an oversight.
- **Team priority:** informational.
- **Location:** the extracted Steam Audio tree ships `GPUUtilities.dll` +
  `TrueAudioNext.dll` alongside `phonon.dll`; CI packages only `phonon.dll`.
- **Finding (verified):** almost certainly fine — the CPU binaural path does not need
  the GPU/TrueAudio helpers — but recorded so the decision is explicit.
- **Team recommendation (option):** none; noted for awareness.

### CUE3D-11 — Y→pitch elevation layered on top of HRTF: keep, but listening-test before locking
- **Severity:** MEDIUM. UX raised the conflicting-signal concern; DSP adjudicated it as
  coherent enough to keep.
- **Team priority:** tuning / future (Tier P3); needs a blind-player listening test
  before it is locked as the default.
- **Location:** `AccessibilityCues_ComputeFreqModFromY` (`AccessibilityCues.cpp:116-124`);
  applied via `Cue3D_SetPitch`.
- **Finding (DSP position):** the synthetic pitch cue and HRTF elevation are **not
  contradictory** — generic `IPL_HRTFTYPE_DEFAULT` elevation is genuinely weak, so for
  most of the approach pitch is the only elevation signal (no conflict); they coexist
  only near the aim line, and both rise with height (additive redundancy, a legitimate
  sensory-substitution design). Caveats: the ±1-octave range is aggressive and the
  resampling bends the loop's timbre enough to muddy "is this the same cue?"; and the
  SOFA-HRTF future path requires a user-supplied file, so the pitch layer is the
  default for everyone and **cannot be assumed temporary**.
- **Team recommendation (option):** keep the layer; consider narrowing the pitch range;
  blind-player listening test before locking as default.

### CUE3D-12 — Distance model uses full 3D distance, fighting the pitch-elevation cue
- **Severity:** LOW/tuning.
- **Team priority:** tuning / future (Tier P3). DSP would address this before touching
  the pitch layer (CUE3D-11).
- **Location:** `Cue3DSteamAudio.cpp:197-204` (inverse-distance over the full 3D
  vector).
- **Finding (verified):** the pure inverse-distance model includes the vertical
  component, so a high target reads as genuinely farther / quieter — which fights the
  "higher pitch = higher" cue. The SF64 path de-emphasised vertical separation
  (`y/2.5`).
- **Team recommendation (option):** use horizontal distance (or de-weight the vertical
  component) before the distance calc.

### CUE3D-13 — `s.playing` two-writer pattern is dead code today, latent for one-shot cues
- **Severity:** LOW today (dead code), becomes a real lost-update/resurrection bug the
  moment the first one-shot cue is added. The team confirmed it is **not** a data race
  (every access is an atomic store) — architect's "two writers" concern and DSP's
  "atomics are correct" are the same finding from two angles.
- **Team priority:** tuning / future (Tier P3); must be fixed before the first one-shot
  cue.
- **Location:** audio-thread write `Cue3DSteamAudio.cpp:241`; game-thread writes `:453`
  / `:459`; the `ended` branch `:220-223`; both shipping cues load `loop=true`
  (`AccessibilityCues.cpp:173`, `:371`).
- **Finding (verified):** because both shipping cues loop, the non-loop EOF branch that
  self-clears `playing` from the audio thread never executes, so the lost-update path
  (audio thread clears `playing=false` at EOF in the same window the game thread sets
  `playing=true`) is currently unreachable.
- **Team recommendation (option):** before adding a one-shot cue, have the game thread
  own all `playing` transitions (don't self-clear from the audio thread; e.g. let the
  cursor sit at EOF, or use a separate audio-thread-only "finished" flag the game
  thread reads).

### CUE3D-14 — Per-block position read is logically torn (not a data race)
- **Severity:** MEDIUM-trending-LOW. Inaudible for smooth motion.
- **Team priority:** deferrable (Tier C/P3); lowest priority.
- **Location:** position published as 3 relaxed stores (`Cue3DSteamAudio.cpp:481-483`),
  read as independent relaxed loads (`:186-188`).
- **Finding (verified):** race-free but without snapshot consistency, so an abrupt
  enemy target switch can produce a one-block wrong-direction blip (new x / old z). For
  smooth motion it is inaudible.
- **Team recommendation (option):** a seqlock or packed publish if it ever matters;
  noted as not a foundation blocker.

### CUE3D-15 — No attack/release ramp on Play/Stop (clicks on target switch)
- **Severity:** MEDIUM (UX, from the listener's seat) / LOW-improvement (DSP). A click
  fatigues but does not mislead direction.
- **Team priority:** tuning / future (Tier P3).
- **Location:** `Cue3D_Play` / `Cue3D_Stop` (`Cue3DSteamAudio.cpp:451-461`).
- **Finding (verified/suspected):** cutting a loop cue mid-waveform clicks; if
  `FindClosestEnemyAhead` jitters between targets frame-to-frame, clicks could be near
  constant. (Whether target selection jitters was raised as an open question — see
  appendix cross-flags.)
- **Team recommendation (option):** short attack/release ramp on Play/Stop.

### CUE3D-16 — Three uncoordinated audio streams (game / cues / TTS), no ducking
- **Severity:** MEDIUM (suspected; not measured by ear).
- **Team priority:** tuning / future (Tier P3); cross-cuts CUE3D-2 (the gain lever is
  the prerequisite for any ducking).
- **Location:** conceptual — the cues' second device vs. the game audio vs. PRISM/Tolk.
- **Finding (suspected):** looping binaural cues can mask PRISM score / training / menu
  speech — the thing a blind player most needs to hear. No shared ducking or priority
  exists across the three streams.
- **Team recommendation (option):** duck cues while TTS speaks once the gain lever
  (CUE3D-2) exists.

---

## The team's suggested prioritization (recorded, not committed)

The four reviewers converged on this ordering. It is captured as their conclusion to
inform discussion, not as an accepted plan.

- **Tier A — before the cues are a safe default-on feature** (small, mostly
  backend-local, parallelizable): CUE3D-1, CUE3D-2, CUE3D-3, CUE3D-4.
- **Tier B — before cue #3 is added:** CUE3D-5 (the structural prerequisite and home
  for CUE3D-1, CUE3D-2, CUE3D-6), CUE3D-6.
- **Tier C — foundation hardening, before a second platform / release widening:**
  CUE3D-7, CUE3D-8, CUE3D-9 (and CUE3D-10 informational).
- **Tier P3 — tuning / future, none block the default:** CUE3D-11, CUE3D-12, CUE3D-13,
  CUE3D-14, CUE3D-15, CUE3D-16.

Cross-cutting note the team emphasised: **wiring the unused `Cue3D_SetGain` (CUE3D-2)
is the single highest-leverage change** — it is the lever for master volume, the
pause-silence path, and future TTS ducking (CUE3D-16) at once.

---

## Appendix — review method and participants

The review used four independent expert perspectives, each reading the
implementation first-hand, followed by a debate round in which they challenged each
other's severities and adjudicated cross-lane questions, then converged.

- **DSP / real-time audio:** audio callback, lock-free atomics, resampler, HRTF use,
  distance model, sample-rate handling, clipping.
- **Architecture:** the Cue3D seam as a foundation, extensibility to many cues, the
  duplicated dual-backend state machines, lifecycle/ownership.
- **Build / portability:** configure-time downloads, supply chain, cross-platform
  runtime loading, CI packaging.
- **Accessibility / UX:** the blind-player experience, the master-volume / ducking gap,
  HRTF-vs-pitch elevation coherence, behavior on device failure, the control surface.

Cross-lane questions that were raised and resolved during the debate (recorded so the
reasoning is not lost): the `s.playing` two-writer question (CUE3D-13, resolved: dead
code today, atomics correct); the 48 kHz / second-device interaction (CUE3D-4, resolved:
they compound, fix must propagate the real rate); the Y→pitch-on-HRTF coherence question
(CUE3D-11, resolved: additive redundancy, keep but listening-test); the
fix-sequencing question (resolved into the tiers above); the rpath severity (CUE3D-7,
resolved: HIGH→MEDIUM, fix now); and whether to keep the separate audio device (resolved:
keep it, harden it). One open question left for a future session: how jittery enemy target
selection is in practice, which affects CUE3D-13 and CUE3D-15.
