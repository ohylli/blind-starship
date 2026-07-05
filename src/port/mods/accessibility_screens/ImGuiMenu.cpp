#include "ImGuiMenu.h"

#include <string>
#include <imgui.h>
#include <imgui_internal.h>
#include <spdlog/spdlog.h>

#include "port/CGameCompat.h" // gControllerLock (sf64thread.h)
#include "port/mods/Accessibility.h"
#include "port/accessibility/Tts.h"

// ControlDeck::BlockGameInput blockers are keyed by id; the only in-tree caller is
// libultraship's InputEditorWindow (95237929), so any other distinctive value is safe.
static constexpr int32_t kMenuGameInputBlockId = 0x53464141; // "SFAA"

static bool sSessionActive = false;
static bool sFocusDropPending = false;
static ImGuiID sLastSpokenItemId = 0;

void AccessibilityImGuiMenu_Register() {
    // Nothing to register: no CVar of its own (gated on the shared screen-reader toggle)
    // and no event-bus listeners — the session is driven by the per-frame tick below.
}

void AccessibilityImGuiMenu_FrameTick() {
    const auto context = Ship::Context::GetInstance();
    const bool shouldBeActive =
        context->GetWindow()->GetGui()->GetMenuOrMenubarVisible() && Accessibility_IsScreenReaderEnabled();
    if (shouldBeActive == sSessionActive) {
        return;
    }
    sSessionActive = shouldBeActive;
    sLastSpokenItemId = 0;
    if (shouldBeActive) {
        context->GetControlDeck()->BlockGameInput(kMenuGameInputBlockId);
        // WriteToOSContPad early-returns while blocked without zeroing the pad, so a button
        // held at menu-open would read as stuck-held for the whole session. gControllerLock
        // makes Controller_ReadData zero sNextController for N frames instead of reading it
        // (docs/accessibility-imgui-menu-plan.md, Phase 1 caveat).
        gControllerLock = 3;
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        // Consumed by OnMenuBarDraw. push_frame() in Game.cpp runs the ImGui draw before
        // this tick, so the next draw — one iteration after the menu first drew — performs
        // the drop (imperceptible). Also covers the reader being toggled on while the menu
        // is already open.
        sFocusDropPending = true;
        Tts_Speak("Menu opened", true);
    } else {
        context->GetControlDeck()->UnblockGameInput(kMenuGameInputBlockId);
        // While blocked, sNextController stayed zeroed, so a button physically held right now
        // would read as a fresh press edge on the first unblocked frame; lock a few frames to
        // flush it (docs/accessibility-imgui-menu-plan.md, Phase 1 caveat).
        gControllerLock = 3;
        ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
        sFocusDropPending = false;
        Tts_Speak("Menu closed", true);
    }
}

void AccessibilityImGuiMenu_OnMenuBarDraw() {
    if (!sSessionActive || !sFocusDropPending) {
        return;
    }
    sFocusDropPending = false;
    // Menu-bar items live on ImGuiNavLayer_Menu, which stock ImGui only enters via the
    // Alt key — the keyboard-nav flag alone leaves focus on the host window's main layer
    // and the arrows do nothing. Replicate ImGui's Alt toggle (imgui.cpp
    // NavUpdateWindowing "apply_toggle_layer"); verified in the Phase 0 spike
    // (docs/accessibility-imgui-menu-plan.md).
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImGuiWindow* host = g.CurrentWindow; // dockspace host window, owns the menu bar
    ImGui::ClearActiveID();
    ImGui::FocusWindow(host); // ensures g.NavWindow == host
    g.NavLayer = ImGuiNavLayer_Menu;
    if (host->NavLastIds[ImGuiNavLayer_Menu] != 0) {
        ImGui::SetNavID(host->NavLastIds[ImGuiNavLayer_Menu], ImGuiNavLayer_Menu, 0,
                        host->NavRectRel[ImGuiNavLayer_Menu]);
    } else {
        ImGui::NavInitWindow(host, true); // first menu item drawn this frame grabs the init request
    }
    SPDLOG_TRACE("accessibility menu: dropped nav focus into Menu layer of '{}'", host->Name);
}

bool AccessibilityImGuiMenu_IsSessionActive() {
    return sSessionActive;
}

// "##" hides the prefix from display, "###" replaces the id — either way the spoken
// text is everything before the first "##" (a "###" also matches).
static std::string StripImGuiIdSuffix(const char* label) {
    std::string text = (label != nullptr) ? label : "";
    const size_t idStart = text.find("##");
    if (idStart != std::string::npos) {
        text.resize(idStart);
    }
    return text;
}

void AccessibilityImGuiMenu_ItemFocused(const char* label, const char* role, const char* stateText) {
    if (!sSessionActive) {
        return;
    }
    const ImGuiID itemId = ImGui::GetItemID();
    if (itemId != 0 && itemId == sLastSpokenItemId) {
        return;
    }
    sLastSpokenItemId = itemId;
    std::string text = StripImGuiIdSuffix(label);
    for (const char* part : { role, stateText }) {
        if (part != nullptr && part[0] != '\0') {
            if (!text.empty()) {
                text += ", ";
            }
            text += part;
        }
    }
    if (!text.empty()) {
        Tts_Speak(text.c_str(), true);
    }
}

void AccessibilityImGuiMenu_ValueChanged(const char* label, const char* valueText) {
    if (!sSessionActive) {
        return;
    }
    if (valueText != nullptr && valueText[0] != '\0') {
        Tts_Speak(valueText, true);
        return;
    }
    const std::string text = StripImGuiIdSuffix(label);
    if (!text.empty()) {
        Tts_Speak(text.c_str(), true);
    }
}
