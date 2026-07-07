#pragma once

// Self-voicing session for the F1 ImGui menu — keyboard nav, input blocking, and
// narration hooks for the widget helpers. Design: docs/accessibility-imgui-menu-plan.md.
//
// Threading: everything here is main-thread-only (Tts_Speak requires it). That holds
// because both callers run inside push_frame() in Game.cpp — the ImGui draw via
// Graphics_ThreadUpdate/ProcessGfxCommands and GameEngine::StartFrame.

// Widget role labels spoken after a focused item ("checkbox", "slider", …) and matched by
// HintForRole to pick the item's activation hint. Defined once here so the call sites in
// UIWidgets.cpp / ImguiUI.cpp and the matcher in ImGuiMenu.cpp share a single spelling — a
// rename becomes a compile error instead of a silently dropped hint. Role "option" also flags
// the focus as being inside an open combo dropdown for the nav-surface classifier.
namespace AccessibilityRole {
constexpr const char* Checkbox = "checkbox";
constexpr const char* ComboBox = "combo box";
constexpr const char* Option = "option";
constexpr const char* Button = "button";
constexpr const char* MenuItem = "menu item";
constexpr const char* Menu = "menu";
constexpr const char* Slider = "slider";
} // namespace AccessibilityRole

void AccessibilityImGuiMenu_Register();

// Called every game frame from GameEngine::StartFrame(). A session is "menu visible
// AND screen reader enabled"; transitions block/unblock game input, toggle ImGui
// keyboard nav, and announce open/close.
void AccessibilityImGuiMenu_FrameTick();

// Called from GameMenuBar::DrawElement() right after BeginMenuBar() succeeds. Drops
// keyboard nav focus into the menu bar's nav layer once per session. Note: push_frame()
// in Game.cpp runs the ImGui draw before FrameTick, so the drop lands on the draw of the
// iteration after the session starts — one frame after the menu first draws (imperceptible).
void AccessibilityImGuiMenu_OnMenuBarDraw();

// True while the self-voicing session is running (menu visible AND screen reader on).
// The narrator functions below already no-op when inactive; check this at call sites that
// build strings (FormatValue etc.) every focused frame, so the formatting work is skipped
// too when narration would be dropped anyway.
bool AccessibilityImGuiMenu_IsSessionActive();

// Narrator API for the widget helpers (UIWidgets.cpp / ImguiUI.cpp). Both functions
// no-op unless the session is active, so call sites stay cheap and PRISM-ignorant.
// Labels are spoken with everything from the first "##"/"###" stripped; pass
// already-formatted text (no printf specifiers).

// Call right after a focusable sub-widget when ImGui::IsItemFocused() is true. Dedups
// on ImGui::GetItemID() so each focus change is spoken once, as "label, role, state"
// (empty parts are skipped). Interrupts current speech.
void AccessibilityImGuiMenu_ItemFocused(const char* label, const char* role, const char* stateText);

// Call when a widget's value changed. Speaks just the value — snappier for repeated
// slider ticks — falling back to the label if valueText is empty. Interrupts.
void AccessibilityImGuiMenu_ValueChanged(const char* label, const char* valueText);

// Call every frame right after a slider, with the slider's ImGui id (ImGui::GetItemID())
// and its current edit state. On the transition into an edit mode it speaks how to operate
// and leave it — tweak mode (entered with space): arrow keys adjust; text-input mode
// (entered with enter): type a value. Spoken once per activation; no-op while the slider is
// idle or the session is inactive. `id` is ImGuiID, passed as unsigned int to keep this
// header free of imgui includes.
void AccessibilityImGuiMenu_SliderActivated(unsigned int id, bool active, bool textInput);
