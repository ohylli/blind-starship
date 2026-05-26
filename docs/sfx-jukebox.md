# SFX Jukebox — how the existing mod works

Reference notes for the existing **SFX Jukebox** mod, gathered to bootstrap a future session on making it accessible to blind players. This document describes the mod as it stands today; it does **not** propose any accessibility design — that decision is deferred.

## Which file is actually built

`src/mods/sfxjukebox2.c` is the live version. Confirmation:

- `src/overlays/ovl_menu/fox_option.c:1671` textually pulls it into the Expert Sound translation unit:
  ```c
  #include "../../mods/sfxjukebox2.c"
  ```
- `src/mods/*.c` is **not** in the `file(GLOB_RECURSE ALL_FILES …)` block in `CMakeLists.txt:143-166`. Only `src/audio`, `src/engine`, `src/libultra/...`, `src/overlays`, `src/port`, `src/sys`, and a few specific root files are globbed. Mods reach the linker only when something else `#include`s them.

`src/mods/sfxjukebox.c` is an older orphan: not referenced anywhere, not in the build, and would not compile cleanly as written (the `Jukebox_SelectClamp` definition at line 47 is missing its return type). The two files are otherwise nearly identical; `sfxjukebox2.c` replaced the file-scope `contPress`/`contHold` pointers with locals and switched the display list setup from `SETUPDL_83_POINT` to `SETUPDL_83_OPTIONAL`. Future work should target `sfxjukebox2.c`; the `.c` orphan can be ignored or deleted in passing.

## How the user reaches the jukebox

1. Enable the **SFX Jukebox** checkbox in the dev menu — sets the `gSfxJukebox` CVar (`src/port/ui/ImguiUI.cpp:824`, tooltip: *"Press L in the Expert Sound options to play sound effects from the game"*). The same CVar also unlocks the Expert Sound submenu entry (`fox_option.c:401`).
2. Navigate into the main menu's **Expert Sound** screen.
3. Press the **L trigger**. That flips `showJukebox` at `fox_option.c:1680`. From that frame on, `Jukebox_Update()` takes over the Expert Sound update/draw cycle (`fox_option.c:1683`); the normal Expert Sound logic is skipped while the jukebox is open.

So while the jukebox is up, no other menu code competes for inputs or draw calls — useful as a hook point.

## What the user is editing

A single 32-bit packed SFX ID (`u32 sfx`), assembled by the `SFX_PACK` macro at `include/sfx.h:66`:

```c
SFX_PACK(bank, index, range, importance, flags)
```

The big hex line at the top of the screen (`"SFX ID: %08X"`, `sfxjukebox2.c:203`) is the assembled value. Bit layout (`include/sfx.h:40-68`):

| bits  | field         | notes                                         |
|-------|---------------|-----------------------------------------------|
| 28-31 | bank          | 4-bit, but only 0–4 are defined (`SfxBankId`) |
| 27    | flag bit 27   | "allow duplicate requests" (`SFX_FLAG_27`)    |
| 25-26 | flag bits     | filled from `sfxFlags[6]`, `sfxFlags[7]`      |
| 24    | state flag    | always set by `SFX_PACK`                      |
| 18-23 | flag bits     | filled from `sfxFlags[0..5]`                  |
| 16-17 | range         | 0–3                                           |
| 8-15  | importance    | 0x00–0xFF (priority byte)                     |
| 0-7   | index         | 0..bankSize-1                                 |

Bank enum values (`include/sfx.h:70-76`): `SFX_BANK_PLAYER`, `SFX_BANK_1`, `SFX_BANK_2`, `SFX_BANK_3`, `SFX_BANK_SYSTEM`.

Per-bank index caps from `sfxjukebox2.c:23`:
```c
static u8 bankSizes[] = { 0x33, 0x85, 0x9C, 0x9C, 0x37 };
```

Plus a 3D source position `f32 sfxSource[3]` for playback location (X / Y / Z), so the user can audition a sound from somewhere other than the listener.

## Mode structure

There are two top-level edit modes, tracked by `editMode` (`sfxjukebox2.c:26`) and toggled with D-pad **up/down** (line 145-150, modulo 2):

- **Mode 0** — SFX-ID editing (`Jukebox_UpdateSfx`, line 99).
- **Mode 1** — Source-position editing (`Jukebox_UpdateSource`, line 71).

A `"V"` caret is drawn one row above the currently selected column in the active mode (lines 219-226). That caret is the only on-screen focus indicator.

## Mode 0 — SFX-ID columns

D-pad **left/right** moves between 13 column positions (`sfxEditMode`, 0–12, wrapping). The screen-order from `sfxModeX[]` at line 137:

| `sfxEditMode` | header | field          | controls                                                                   |
|--------------:|--------|----------------|----------------------------------------------------------------------------|
| 9             | **B**  | bank           | any C-button cycles (wrap 0..4)                                            |
| 10            | **ID** | index (hex)    | C-up/down ±1 with auto-repeat after 15 frames; C-left/right ±0x10; clamped |
| 11            | **X**  | bit 27 toggle  | any C-button toggles 0/1                                                   |
| 12            | **R**  | range          | any C-button cycles (wrap 0..3)                                            |
| —             | **S**  | state          | always 1, no navigation index (drawn but not editable)                     |
| 0-7           | **FLAG** digits | `sfxFlags[i]` | any C-button toggles 0/1 (one column per digit)                       |
| 8             | **IMP**| importance (hex) | same as ID — ±1 / ±0x10, clamped 0..0xFF                                 |

The flag-bit packing at line 132 is non-obvious:
```c
for (i = 0; i < 8; i++) {
    flags |= sfxFlags[i] << (i + (i > 5));
}
```
This shifts `sfxFlags[0..5]` into bits 18-23 and `sfxFlags[6..7]` into bits 25-26, intentionally skipping bit 24 (the state flag).

There is a static string table at `sfxjukebox2.c:22` that is **never drawn**:
```c
static char* flagNames[] = { "F18", "F19", "F20", "F21", "F22", "F23", "STT", "F25", "F26", "SFX" };
```
The names correspond to the flag-bit defines in `include/sfx.h:40-47` (`SFX_FLAG_18` through `SFX_FLAG_23`, plus `SFX_FLAG_27`). Useful raw material if labels are ever needed.

## Mode 1 — source-position columns

D-pad **left/right** moves between three columns: X, Y, Z (`srcEditMode`, 0–2, wrapping at line 75-80). Each axis is in world units:

- C-up/down → ±100
- C-left/right → ±10
- All four with hold-to-repeat after 15 frames (line 82-96).

Displayed as three `%5.0f` numbers near the bottom (lines 212-215) with `X` / `Y` / `Z` labels under them.

## Global controls (active in both modes)

- **A** — Stop any BGM/fanfare, then `AUDIO_PLAY_SFX(sfx, sfxSource, 4)` (line 168). Sets `sMusicPlaying = true`.
- **B** — Two behaviors (line 172-188):
  - If nothing is currently playing: leave the jukebox and bounce back to the main menu (plays `NA_SE_ARWING_CANCEL`, starts `NA_BGM_SELECT`, sets `sMenuEntryState = 1000` and `sMainMenuFromCancel = 1`).
  - If something is playing: kill that SFX with `Audio_KillSfxBySource(sfxSource)` plus any BGM, and play `NA_SE_CANCEL`. Stay on the jukebox.
- **Z trigger** — cycles `sSpectrumAnalyzerMode` 0→1→2→0 (line 191-196). The visualizer itself is drawn by the parent Expert Sound screen, not by this mod.
- **Implicit click on change** — whenever the assembled `sfx` value differs from the previous frame, `NA_SE_CURSOR` plays at the default source (line 160-163). So today there is already an audible "something changed" cue, but it is the same click regardless of which field changed or in which direction.

## What is drawn on screen

In three lines, all from `sfxjukebox2.c:200-218`:

1. `SFX ID: XXXXXXXX` — the assembled hex value (line 203).
2. Column header `"B ID  X R S    FLAG   IMP"` (line 205), and beneath it a value row with `<bank> <id> <flag> <range> 1 <8 flag digits> <imp>` (line 206-208).
3. Three `%5.0f` numbers for X / Y / Z source coordinates (lines 212-215), with `X` / `Y` / `Z` labels underneath at fixed x-positions 45, 135, 225 (lines 216-218).

Plus the `"V"` caret one row above the currently selected column.

## File-scope state in `sfxjukebox2.c`

For reference when designing reactive layers:

```c
static s32 showJukebox = 0;                       // toggled by L trigger in fox_option.c
static u32 prevSfx     = NA_SE_NONE;              // last frame's assembled sfx (for change detection)
static u32 sfx         = NA_SE_NONE;              // current assembled sfx, the playback ID
static s32 sfxId       = 0;                       // index within bank, 0..bankSizes[bank]-1
static u32 sfxBank     = SFX_BANK_PLAYER;
static u32 sfxRange    = 0;
static s32 sfxImport   = 0;
static int holdTimer   = 0;                       // shared auto-repeat counter
static u8  sfxFlag     = 0;                       // the bit-27 toggle ("allow duplicates")
static u8  sfxFlags[8] = { 0 };                   // the 8 FLAG-row digits
static u32 sfxEditMode = 9;                       // 0..12, current column in mode 0
static u32 srcEditMode = 0;                       // 0..2,  current axis in mode 1
static u32 editMode    = 0;                       // 0 = sfx-id, 1 = source
static f32 sfxSource[3] = {0};                    // X/Y/Z playback position
```

Two unused statics also live there: `s32 sfxImport` shares its name with the field but `s32 srcVec[3]` is declared (line 27) and never referenced.

## Integration points relevant to a future accessibility pass

These are factual hooks that exist today; how to use them is left for the future session:

- **Single hook site for activation/deactivation.** `fox_option.c:1678-1686` is the only place that decides whether `Jukebox_Update()` runs. The `showJukebox` transition is the natural "screen entered / exited" boundary.
- **Single-cursor model.** Everything the user touches inside the jukebox is one of: a column index in mode 0 (13 positions), a column index in mode 1 (3 positions), the global mode toggle, or one of the global buttons (A/B/Z). No nested submenus.
- **Existing implicit feedback.** The `prevSfx != sfx` check at line 160 already fires `NA_SE_CURSOR` whenever the assembled ID changes. A more informative layer can either replace that, hook in alongside it, or use it as a value-changed signal.
- **All input comes through `gControllerPress`/`gControllerHold`.** No external dispatch — each press maps directly to one of the assignments in `Jukebox_UpdateSfx` or `Jukebox_UpdateSource`. Adding events at the assignment sites is straightforward if event-driven announcements are chosen.
- **Audio source vector is the same one the cues system uses.** `sfxSource` is fed into `Audio_PlaySfx` / `AUDIO_PLAY_SFX` exactly like `gDefaultSfxSource` and the cue-system source. Spatial playback already exists; no new DSP would be needed to position auditions or feedback tones.
- **Bank/index labels exist as data.** `bankSizes[]` (line 23) gives index caps, `flagNames[]` (line 22) gives flag-bit names, and `include/sfx.h:78-508` is a hand-maintained map of every named `NA_SE_*` constant to its packed value — useful for any "what did I just select?" lookup. (The named constants are sparse: most `bank/index` pairs are nameless.)

## Pointers for the next session

- Read `src/mods/sfxjukebox2.c` end-to-end (≈230 lines).
- Cross-reference `include/sfx.h` for the bit layout and the catalogue of named SFX.
- The activation/deactivation boundary lives in `fox_option.c:1678-1686`, not in the mod itself.
- For comparable already-accessible screens (event-driven announcements over the in-process bus), see `src/port/mods/accessibility_screens/` and the event surface in `src/port/hooks/list/MenuEvent.h`.
- For the spatial-audio side, see `src/port/mods/AccessibilityCues.{cpp,h}` and `docs/accessibility-cues-tuning.md`.
