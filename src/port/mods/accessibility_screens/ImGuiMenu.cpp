#include "ImGuiMenu.h"

#include <cstring>
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
// The slider (if any) whose edit-mode activation we've already announced, so the "how to
// adjust and exit" hint speaks once per activation rather than every frame while held.
static ImGuiID sActiveEditItemId = 0;
static bool sActiveEditWasTextInput = false;
// The nav "surface" the last-focused item lived on, so the directional "how to move around
// here" instructions are spoken once when the surface changes rather than on every item.
// Four surfaces, because their key semantics differ:
//   - MenuBar   : the horizontal top-level bar (left/right between menus, down to open).
//   - TopMenu   : an open top-level menu's item list. Vertical (up/down), but left/right here
//                 do NOT go "back" — ImGui switches to the neighbouring top-level menu — and
//                 escape is what closes back to the bar.
//   - Submenu   : a nested submenu (a menu opened from within a menu). Vertical, and here left
//                 really does go back to the parent.
//   - ComboList : an open combo-box dropdown. Vertical, but left does NOT go back (EndCombo has
//                 none of EndMenu's left-arrow handling) — only escape or picking an option
//                 closes it. Detected by role "option": those are the dropdown's entries.
// The distinction between MenuBar and the popup surfaces is g.NavLayer (menu-bar items live on
// ImGuiNavLayer_Menu); ComboList is split off first (by role) because a combo opened from inside
// a menu shares the menu's popup ancestry and would otherwise be mistaken for a nested submenu;
// TopMenu vs Submenu is then whether the focused popup's parent is itself a popup. Using nav
// state rather than the role string for the menu split is what keeps a nested submenu entry
// (role "menu", yet vertical) classified correctly.
// sPrevSurfaceKey is the focused popup window's id, so a lateral hop between two top-level menus
// (Left/Right while open) — same TopMenu surface, different menu — is still caught and re-voiced;
// without it, leaving any menu but the leftmost would land in the next menu silently.
enum FocusSurface { kSurfaceNone = 0, kSurfaceMenuBar, kSurfaceTopMenu, kSurfaceSubmenu, kSurfaceComboList };
static FocusSurface sPrevSurface = kSurfaceNone;
static ImGuiID sPrevSurfaceKey = 0;
// Consecutive frames the session has had keyboard focus outside the menu (no menu-bar item and
// no open popup). Pressing Escape closes the last menu popup and ImGui drops nav to the host's
// main layer, where the arrows do nothing — so when this persists we re-grab the menu bar. A
// two-frame threshold keeps a one-frame open/close gap from yanking focus mid-transition.
static int sFocusLostStreak = 0;

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
    sActiveEditItemId = 0;
    sActiveEditWasTextInput = false;
    sPrevSurface = kSurfaceNone;
    sPrevSurfaceKey = 0;
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
        sFocusLostStreak = 0;
        Tts_Speak("Menu closed", true);
    }
}

void AccessibilityImGuiMenu_OnMenuBarDraw() {
    if (!sSessionActive) {
        return;
    }
    ImGuiContext& g = *ImGui::GetCurrentContext();
    // Focus is "in the menu" when a menu-bar item holds it (Menu nav layer) or some popup is open
    // (a submenu or a combo list — those live on the popup's main layer). Anything else means the
    // arrows are dead: either the one-shot drop at open has not run yet, or Escape just closed the
    // last popup and ImGui parked nav on the host's main layer.
    const bool focusInMenu = (g.NavLayer == ImGuiNavLayer_Menu) || (g.OpenPopupStack.Size > 0);
    if (focusInMenu) {
        sFocusLostStreak = 0;
    }
    bool drop = sFocusDropPending;
    sFocusDropPending = false;
    if (!focusInMenu && ++sFocusLostStreak >= 2) {
        drop = true;
        sFocusLostStreak = 0;
    }
    if (!drop) {
        return;
    }
    // Menu-bar items live on ImGuiNavLayer_Menu, which stock ImGui only enters via the
    // Alt key — the keyboard-nav flag alone leaves focus on the host window's main layer
    // and the arrows do nothing. Replicate ImGui's Alt toggle (imgui.cpp
    // NavUpdateWindowing "apply_toggle_layer"); verified in the Phase 0 spike
    // (docs/accessibility-imgui-menu-plan.md).
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

// A short "how to activate this" clause appended after the item's value when it is focused, so a
// keyboard user learns the interaction without having to discover it by trial. This covers the
// *item*; the directional "how to move around here" instructions are spoken once per surface
// transition in ItemFocused instead (see there), so on a transition frame the clause below is
// replaced by the directional one rather than stacked with it.
// Sliders are the subtle case: enter opens a text-input mode while space enters arrow-key tweak
// mode (imgui.cpp NavUpdate, PreferInput vs PreferTweak), so both are spelled out.
static const char* HintForRole(const char* role) {
    if (role == nullptr) {
        return nullptr;
    }
    if (std::strcmp(role, AccessibilityRole::Slider) == 0) {
        return "space to adjust, enter to input a value";
    }
    if (std::strcmp(role, AccessibilityRole::Checkbox) == 0) {
        return "enter to toggle";
    }
    if (std::strcmp(role, AccessibilityRole::ComboBox) == 0) {
        return "enter to open the list";
    }
    if (std::strcmp(role, AccessibilityRole::Button) == 0) {
        return "enter to activate";
    }
    if (std::strcmp(role, AccessibilityRole::MenuItem) == 0) {
        return "enter to select";
    }
    if (std::strcmp(role, AccessibilityRole::Menu) == 0) {
        return "enter to open";
    }
    if (std::strcmp(role, AccessibilityRole::Option) == 0) {
        return "enter to choose";
    }
    return nullptr;
}

// Which nav surface the currently focused item lives on, plus (via outKey) the focused popup
// window's id so a lateral hop between two same-surface menus can still be detected. Reads
// ImGui nav internals; keep it next to OnMenuBarDraw's similar coupling. `role` is the app-level
// role string of the focused item — "option" is the only reliable signal that focus is inside an
// open combo dropdown rather than a menu, since a combo opened from inside a menu shares the
// menu's popup ancestry.
static FocusSurface ClassifyFocusSurface(const char* role, ImGuiID& outKey) {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    outKey = 0;
    if (g.NavLayer == ImGuiNavLayer_Menu) {
        return kSurfaceMenuBar;
    }
    if (role != nullptr && std::strcmp(role, AccessibilityRole::Option) == 0) {
        outKey = (g.NavWindow != nullptr) ? g.NavWindow->ID : 0;
        return kSurfaceComboList;
    }
    ImGuiWindow* navWin = g.NavWindow;
    // A top-level menu popup is spawned by the host window; a nested submenu is spawned by
    // another menu popup. So a popup parent that is itself a popup/menu => nested (vertical,
    // left goes back); otherwise it is a top-level menu (left/right switch menus instead).
    const bool nested = navWin != nullptr && navWin->ParentWindow != nullptr &&
                        (navWin->ParentWindow->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_ChildMenu)) != 0;
    outKey = (navWin != nullptr) ? navWin->ID : 0;
    return nested ? kSurfaceSubmenu : kSurfaceTopMenu;
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

    // Pick the trailing hint clause. When focus arrives on a different nav surface — the menu bar,
    // a top-level menu, a nested submenu, or a combo dropdown — or hops laterally to a different
    // top-level menu, speak the directional instructions for where the user now is; otherwise fall
    // back to the item's own activate clause. The two are mutually exclusive, so one announcement
    // never stacks "up and down arrows..." with "enter to toggle".
    ImGuiID surfaceKey = 0;
    const FocusSurface surface = ClassifyFocusSurface(role, surfaceKey);

    const bool lateralTopMenuSwitch = (surface == kSurfaceTopMenu && surfaceKey != sPrevSurfaceKey);
    const char* transitionHint = nullptr;
    if (surface != sPrevSurface || lateralTopMenuSwitch) {
        switch (surface) {
            case kSurfaceMenuBar:
                transitionHint = "left and right arrows to move between menus, down arrow to open, "
                                 "F1 to close";
                break;
            case kSurfaceTopMenu:
                transitionHint = "up and down arrows to move, enter to select, left and right "
                                 "arrows for the other menus, escape to go back to the menu bar";
                break;
            case kSurfaceSubmenu:
                transitionHint = "up and down arrows to move, enter to select, left arrow to go back";
                break;
            case kSurfaceComboList:
                transitionHint = "up and down arrows to move, enter to choose, escape to close the list";
                break;
            default:
                break;
        }
    }
    sPrevSurface = surface;
    sPrevSurfaceKey = surfaceKey;
    SPDLOG_TRACE("accessibility menu: focus surface {} (key {})", static_cast<int>(surface), surfaceKey);
    const char* hint = (transitionHint != nullptr) ? transitionHint : HintForRole(role);

    std::string text = StripImGuiIdSuffix(label);
    for (const char* part : { role, stateText, hint }) {
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

void AccessibilityImGuiMenu_SliderActivated(unsigned int id, bool active, bool textInput) {
    if (!sSessionActive) {
        return;
    }
    if (active) {
        // Tracking by id (not a plain was-active bool) keeps the announcement correct regardless
        // of draw order: the many idle sliders call this with active == false every frame, and
        // only the one that just became active — or switched edit modes — trips the transition.
        if (sActiveEditItemId != id || sActiveEditWasTextInput != textInput) {
            sActiveEditItemId = id;
            sActiveEditWasTextInput = textInput;
            Tts_Speak(textInput ? "type a value, enter to confirm, escape to cancel"
                                : "left and right to adjust, escape to exit",
                      true);
        }
    } else if (sActiveEditItemId == id) {
        sActiveEditItemId = 0;
        sActiveEditWasTextInput = false;
    }
}
