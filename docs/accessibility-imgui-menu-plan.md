# Plan: making the F1 settings menu screen-reader accessible

Status: planned, not yet implemented. This document is self-contained input for the
implementation session.

## Decision

Self-voice the **existing** ImGui menu (keyboard navigation + PRISM speech) instead of
building a parallel accessible settings UI. Rationale:

- All menu content is port-owned. Every menu and item is drawn by
  `src/port/ui/ImguiUI.cpp` (`DrawGameMenu` :564, `DrawSettingsMenu` :233 — which also
  draws the top-level Graphics menu at :369, `DrawEnhancementsMenu` :591,
  `DrawCheatsMenu` :727, `DrawDebugMenu` :784, all called from
  `GameMenuBar::DrawElement` :916). libultraship only hosts the generic ImGui context
  and the F1 binding.
- Nearly all ~70 leaf items flow through a small set of port-owned widget helpers in
  `src/port/ui/UIWidgets.{h,cpp}`. Each helper receives its label and knows its value,
  so speech can be added in one file and every current and future menu item gets it for
  free. (This sidesteps the label-association problem that blocks generic ImGui
  screen-reader bridges — see ocornut/imgui#8022; upstream ImGui has no accessibility
  support and no shipped AccessKit bridge as of mid-2026.)
- A parallel UI would drift from the real menu and still couldn't reach the
  libultraship-owned windows (Controller Mapping etc.) anyway.

Decisions made with the user:
- Keyboard navigation is tied to the existing screen-reader toggle
  (`gAccessibilityScreenReader`, read via `Accessibility_IsScreenReaderEnabled()`).
  No new CVar.
- The keyboard-input-leak problem is handled **port-side** (no libultraship patch) —
  see "Input blocking" below.
- Deferred out of scope: the surround-sound speaker-position canvas
  (`DrawSpeakerPositionEditor`, `ImguiUI.cpp:122`) and the Resolution Editor window
  (`src/port/ui/ResolutionEditor.{h,cpp}`).

## Key facts established by investigation

- **ImGui version**: 1.91.9b-docking, fetched by libultraship via FetchContent
  (`libultraship/cmake/dependencies/common.cmake`). Full keyboard-nav support exists
  upstream; it is simply never enabled.
- **`ImGuiConfigFlags_NavEnableKeyboard` is set nowhere** in port or libultraship.
  Gamepad nav exists behind the `gControlNav` CVar and is applied only while the menu
  is visible (`libultraship/src/window/gui/Gui.cpp` ~:146, :324, :546). Port code can
  set/clear the keyboard flag itself via `ImGui::GetIO().ConfigFlags` — the context is
  a process-global, no libultraship change needed.
- **F1 binding** is fixed in libultraship: `TOGGLE_BTN = ImGuiKey_F1` (`Gui.cpp:56`),
  handled in `Gui::DrawMenu` (~:534-551). F1 toggles the menu **bar**; Escape/gamepad
  Back toggle a separate full-screen "menu" concept that Starship doesn't use.
- **Input path**: game reads pads in `Controller_ReadData` (`src/sys/sys_joybus.c:84`)
  → `osContGetReadData` (libultraship `src/public/libultra/os.cpp:41`) →
  `LUS::ControlDeck::WriteToOSContPad` (`libultraship/src/controller/controldeck/ControlDeck.cpp:146`).
- **Input blocking**: `WriteToOSContPad` returns early when `AllGameInputBlocked()`,
  and libultraship exposes the public API `ControlDeck::BlockGameInput(int32_t blockId)`
  / `UnblockGameInput(blockId)` (`ControlDeck.cpp:101-107`). Calling this while the
  menu is open blocks keyboard *and* gamepad game input with no libultraship change.
  (The narrower `KeyboardGameInputBlocked()` gap — it only blocks while a widget is
  *active*, not while the nav cursor moves — becomes irrelevant with this approach.)
- **TTS**: `Tts_Speak(const char* text, bool interrupt)` in
  `src/port/accessibility/Tts.h`; safe to call anytime, no-ops without a backend.
- **Per-frame hook point**: `GameEngine::StartFrame()` (`src/port/Engine.cpp:326`)
  runs every game frame and already does keyboard handling; `Accessibility_Init()` is
  called from `GameEngine::Create()` (`Engine.cpp:309`).
- **Menu visibility query**: `Ship::Context::GetInstance()->GetWindow()->GetGui()->GetMenuOrMenubarVisible()`.

## Implementation steps

### Phase 0 — spike: confirm keyboard nav works (do this first)

Add `ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;` (anywhere after
context creation, e.g. `GameUI::SetupGuiElements`), build, press F1, and verify arrow
keys / Tab move the highlight through the menu bar and into submenus.

The one real risk in this plan: **where keyboard focus lands when F1 opens the menu**.
The menu bar lives inside libultraship's dockspace host window. If nav focus doesn't
reach the menu bar automatically, fix from port code with `imgui_internal.h`
(`ImGui::FocusWindow(...)` / `SetNavWindow`) on the open transition — check how the
gamepad-nav path behaves (`Gui.cpp:546-550`) for reference. Resolve this before
building anything else.

ImGui nav model to know: arrows move between items; Enter/Space activates; on sliders,
activate first, then arrows adjust the value, Enter/Escape releases. Menu-bar
horizontal arrow wrap only landed in ImGui 1.92.6, so wrapping may be absent — cosmetic.

**Result (verified 2026-07-05, sightless via log):** The risk is real and now resolved.
Enabling the keyboard flag alone was **not** enough — focus stayed on the main nav layer
of the dockspace host window `"Main - Deck"` with `NavId == 0`, and arrows did nothing.
Menu-bar items live on the separate `ImGuiNavLayer_Menu`, which a normal ImGui app only
enters via the Alt key. **The fix, required, is to drop focus into the menu layer on the
open transition**, replicating ImGui's own Alt-toggle (imgui.cpp `NavUpdateWindowing`
"apply_toggle_layer"): with the host window current inside `BeginMenuBar()`,
`ClearActiveID()` → `FocusWindow(host)` → `g.NavLayer = ImGuiNavLayer_Menu` →
`NavInitWindow(host, true)` (or `SetNavID(host->NavLastIds[Menu], …)` if a prior id
exists); the first menu item drawn that frame captures the init request. The open
transition is detected by "`DrawElement` ran this frame but not last" (`GuiMenuBar::Draw`
early-returns while hidden, so it only runs while visible). Verified: arrows traverse the
top-level menus and Up/Down walks submenu items. **Phase 1 must fold this focus fix into
`AccessibilityImGuiMenu_*` (gated behind the reader toggle).** Bonus observed: submenu
popup windows are named after their label (e.g. `"Enhancements###Menu_00"`), so top-level
labels are recoverable from the window name; leaf-item labels still need the UIWidgets
instrumentation (Phase 2/3).

### Phase 1 — menu session tracking (open/close)

New consumer-mod file pair, following the existing per-screen pattern:
`src/port/mods/accessibility_screens/ImGuiMenu.{cpp,h}` with a
`AccessibilityImGuiMenu_Register()` called from `Accessibility_Init` and a
`AccessibilityImGuiMenu_FrameTick()` called from `GameEngine::StartFrame()` (the menu
is not a gameplay event, so a direct engine tick beats the game-event bus here).

Each tick, compare `GetMenuOrMenubarVisible()` against the previous frame, and
re-evaluate `Accessibility_IsScreenReaderEnabled()` so toggling the CVar mid-session
behaves:

- On open (with reader enabled): `GetControlDeck()->BlockGameInput(<unique id>)`, set
  `ImGuiConfigFlags_NavEnableKeyboard`, `Tts_Speak("Menu opened", true)`.
- On close (or reader turned off): `UnblockGameInput(<same id>)`, clear the flag,
  `Tts_Speak("Menu closed", true)`.

Pick a distinctive int constant for the block id (grep libultraship/SOH heritage for
collisions first). Caveat to verify while testing: `WriteToOSContPad` early-returns
without zeroing the pad, so `sNextController` in `sys_joybus.c` keeps its last values —
a button held at the moment of opening may read as stuck-held by the game. If observed,
zero the pads on the open transition (e.g. set the extern `gControllerLock`, which
makes `Controller_ReadData` zero `sNextController` for N frames, or clear
`gControllerHold`/`gControllerPress` directly).

### Phase 2 — narration core

In the same module, a small narrator with a narrow API for UIWidgets, e.g.:

- `AccessibilityImGuiMenu_ItemFocused(label, role, stateText)` — call when
  `ImGui::IsItemFocused()`; dedup with `ImGui::GetItemID()` against the last
  spoken id so it speaks once per focus change. Speak "label, role, state" with
  interrupt=true.
- `AccessibilityImGuiMenu_ValueChanged(label, valueText)` — speak the new value with
  interrupt=true.
- Both no-op unless the menu session is active (reader on + menu visible), so
  UIWidgets stays cheap and PRISM-ignorant.

**Placement: the focus checks must go *inside* the helpers, immediately after each
focusable sub-widget — not after the helper call.** Both widget generations wrap
their contents in `ImGui::BeginGroup()`/`EndGroup()`, and after `EndGroup()` the
"last item" is the group itself, whose ID never receives nav focus — an
`IsItemFocused()` check placed after the helper returns will simply never fire for
sliders and combos. Helpers also contain multiple keyboard nav stops:

- Sliders (V2 `SliderInt/Float` with `options.showButtons`, legacy
  `EnhancementSliderInt/Float` with `PlusMinusButton`) are up to three stops:
  minus button, slider, plus button. The slider speaks "label, role, value"; the
  buttons speak "decrease <label>" / "increase <label>".
- Combo popups render one raw `ImGui::Selectable` per entry (V2 `Combobox`
  `UIWidgets.cpp:820-832`, legacy `EnhancementCombobox` similarly). Focus narration
  at the helper boundary only covers the collapsed combo — arrowing through the
  *open* popup would be silent until commit. Each `Selectable` in the popup loop
  needs its own focus check, speaking the highlighted option text. Same for the two
  raw `BeginCombo` backend pickers handled in phase 4.

Label handling: the interactive widgets all use invisible `"##label"` IDs, so the
spoken label is threaded down from the helper's argument (which instrumenting inside
the helpers gives for free), not recovered from ImGui state. Strip `##`/`###` ID
suffixes before speaking. Legacy slider labels are printf format strings rendered via
`ImGui::Text(text, val)` (e.g. `"Volume: %d%%"`) — format them with the current value
(or strip the specifiers) before speaking. Append "unavailable" plus the
`disabledTooltip` for disabled items; checkbox state is "checked/unchecked", combo
state is the selected entry text, slider state is the formatted value (respect
`isPercentage`), `WindowButton` state is whether its window is open.
Tooltips-on-demand can come later.

### Phase 3 — instrument UIWidgets

Add the narrator calls inside the helpers in `src/port/ui/UIWidgets.cpp`. Two
generations of helpers coexist; instrument the ones the menu actually uses (verify by
grepping `ImguiUI.cpp`):

- V2: `Checkbox`/`CVarCheckbox`, `Combobox`/`CVarCombobox`, `SliderInt`/`CVarSliderInt`,
  `SliderFloat`/`CVarSliderFloat`, `Button`, `WindowButton`, `MenuItem`, `BeginMenu`.
- Legacy still in use: `EnhancementCheckbox` (+Padded), `EnhancementCombobox`,
  `LabeledRightAlignedEnhancementCombobox`, `EnhancementSliderInt/Float` (+Padded),
  `EnhancementRadioButton`. Where legacy funnels into a shared low-level helper
  (`CustomCheckbox`), instrument the shared one.

Focus announcements go inside the helper after each focusable sub-widget (see the
placement rules in phase 2); value announcements where the helper already knows the
value changed (its `return true` paths — the same spots that call
`SaveConsoleVariablesNextFrame()`).

### Phase 4 — mop-up of non-helper items

- Convert the raw `ImGui::BeginCombo` call sites in `DrawSettingsMenu` (Audio API and
  Renderer API backend pickers) to helper combos, or add manual narrator calls.
- Sweep `ImguiUI.cpp` for any other raw `ImGui::` widgets in menu content.

## Known gaps (accepted, documented, deferred)

- Speaker-position canvas (Surround 5.1) — later: replace with four angle sliders.
- Resolution Editor window — its own pass later.
- `GameOverlay()->DrawSettings()` items in the Graphics menu — drawn by libultraship,
  won't speak; small.
- libultraship-owned windows: Controller Mapping (`InputEditorWindow`, 1400 lines),
  Gfx Debugger, Console. Announce open/close only. Real access needs a libultraship
  patch or a from-scratch binding flow — separate future project.

## Verification checklist

With the reader on: F1 announces "Menu opened" and the Arwing stops responding to
keyboard and gamepad; arrows/Tab traverse all six top-level menus and their submenus
with each item spoken (label, role, state); toggling a checkbox speaks the new state;
a slider speaks values while adjusting; combos speak the selection; window buttons
announce; F1 again speaks "Menu closed" and game input returns (verify no stuck
buttons). With the reader off: no keyboard nav flag, no input blocking, menu unchanged.
Toggle `gAccessibilityScreenReader` while the menu is open and confirm both directions
behave. Finally update the accessibility section of `CLAUDE.md` and cross-reference
this doc.
