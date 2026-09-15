#include "ImguiUI.h"
#include "UIWidgets.h"
#include "ResolutionEditor.h"

#include <spdlog/spdlog.h>
#include <imgui.h>
#define IMGUI_DEFINE_MATH_OPERATORS
#include "libultraship/src/Context.h"

#include <imgui_internal.h>
#include <libultraship/libultraship.h>
#include <Fast3D/interpreter.h>
#include "port/Engine.h"
#include "port/mods/accessibility_screens/ImGuiMenu.h"
#include "port/mods/accessibility_cues/CueCommon.h"
#include "port/mods/accessibility_cues/EnemyCue.h"
#include "port/mods/accessibility_cues/AimCue.h"
#include "port/mods/accessibility_cues/ObstacleCommon.h"
#include "port/mods/accessibility_cues/ObstacleAheadCue.h"
#include "port/mods/accessibility_cues/ObstacleDirectionCue.h"
#include "port/accessibility/Cue.h"
#include "port/accessibility/CueBench.h"
#include "port/notification/notification.h"
#include "utils/StringHelper.h"

#ifdef __SWITCH__
#include <port/switch/SwitchImpl.h>
#include <port/switch/SwitchPerformanceProfiles.h>
#endif

extern "C" {
#include "sys.h"
#include <sf64audio_provisional.h>
#include <sf64context.h>
}

namespace GameUI {
std::shared_ptr<GameMenuBar> mGameMenuBar;
std::shared_ptr<Ship::GuiWindow> mConsoleWindow;
std::shared_ptr<Ship::GuiWindow> mStatsWindow;
std::shared_ptr<Ship::GuiWindow> mInputEditorWindow;
std::shared_ptr<Ship::GuiWindow> mGfxDebuggerWindow;
std::shared_ptr<Notification::Window> mNotificationWindow;
std::shared_ptr<AdvancedResolutionSettings::AdvancedResolutionSettingsWindow> mAdvancedResolutionSettingsWindow;

void SetupGuiElements() {
    auto gui = Ship::Context::GetInstance()->GetWindow()->GetGui();

    auto& style = ImGui::GetStyle();
    style.FramePadding = ImVec2(4.0f, 6.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    style.Colors[ImGuiCol_MenuBarBg] = UIWidgets::Colors::DarkGray;

    mGameMenuBar = std::make_shared<GameMenuBar>("gOpenMenuBar", CVarGetInteger("gOpenMenuBar", 0));
    gui->SetMenuBar(mGameMenuBar);

    if (gui->GetMenuBar() && !gui->GetMenuBar()->IsVisible()) {
#if defined(__SWITCH__) || defined(__WIIU__)
        Notification::Emit({ .message = "Press - to access enhancements menu", .remainingTime = 10.0f });
#else
        Notification::Emit({ .message = "Press F1 to access enhancements menu", .remainingTime = 10.0f });
#endif
    }

    mStatsWindow = gui->GetGuiWindow("Stats");
    if (mStatsWindow == nullptr) {
        SPDLOG_ERROR("Could not find stats window");
    }

    mConsoleWindow = gui->GetGuiWindow("Console");
    if (mConsoleWindow == nullptr) {
        SPDLOG_ERROR("Could not find console window");
    }

    mInputEditorWindow = gui->GetGuiWindow("Input Editor");
    if (mInputEditorWindow == nullptr) {
        SPDLOG_ERROR("Could not find input editor window");
        return;
    }

    mGfxDebuggerWindow = gui->GetGuiWindow("GfxDebuggerWindow");
    if (mGfxDebuggerWindow == nullptr) {
        SPDLOG_ERROR("Could not find input GfxDebuggerWindow");
    }

    mAdvancedResolutionSettingsWindow = std::make_shared<AdvancedResolutionSettings::AdvancedResolutionSettingsWindow>("gAdvancedResolutionEditorEnabled", "Advanced Resolution Settings");
    gui->AddGuiWindow(mAdvancedResolutionSettingsWindow);
    mNotificationWindow = std::make_shared<Notification::Window>("gNotifications", "Notifications Window");
    gui->AddGuiWindow(mNotificationWindow);
    mNotificationWindow->Show();
}

void Destroy() {
    auto gui = Ship::Context::GetInstance()->GetWindow()->GetGui();
    gui->RemoveAllGuiWindows();

    mAdvancedResolutionSettingsWindow = nullptr;
    mConsoleWindow = nullptr;
    mStatsWindow = nullptr;
    mInputEditorWindow = nullptr;
    mNotificationWindow = nullptr;
}

std::string GetWindowButtonText(const char* text, bool menuOpen) {
    char buttonText[100] = "";
    if (menuOpen) {
        strcat(buttonText, ICON_FA_CHEVRON_RIGHT " ");
    }
    strcat(buttonText, text);
    if (!menuOpen) { strcat(buttonText, "  "); }
    return buttonText;
}
}

static const char* filters[3] = {
#ifdef __WIIU__
        "",
#else
        "Three-Point",
#endif
        "Linear", "None"
};

static const char* voiceLangs[] = {
    "Original", /*"Japanese",*/ "Lylat"
};
static const char* voiceLangsSPA[] = {
    "Español", /*"Japanese",*/ "Lylat"
};

void DrawSpeakerPositionEditor() {
    static ImVec2 lastCanvasPos;
    ImGui::Text("Speaker Position Editor");
    ImVec2 canvasSize = ImVec2(200, 200); // Static canvas size
    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImVec2 center = ImVec2(canvasPos.x + canvasSize.x / 2, canvasPos.y + canvasSize.y / 2);

    // Speaker positions
    static ImVec2 speakerPositions[4];
    static bool initialized = false;
    static float radius = 80.0f;

    // Reset positions if canvas position changed (window resized/moved)
    if (!initialized || (lastCanvasPos.x != canvasPos.x || lastCanvasPos.y != canvasPos.y)) {
        const char* cvarNames[4] = { "gPositionFrontLeft", "gPositionFrontRight", "gPositionRearLeft", "gPositionRearRight" };
        float angles[4] = { 240.f, 300.f, 160.f, 20.f }; // Default angles
        
        for (int i = 0; i < 4; i++) {
            int savedAngle = CVarGetInteger(cvarNames[i], -1);
            if (savedAngle != -1) {
                angles[i] = static_cast<float>(savedAngle);
            }

            float rad = angles[i] * (M_PI / 180.0f);
            speakerPositions[i] = ImVec2(center.x + radius * cosf(rad), center.y + radius * sinf(rad));
        }
        initialized = true;
        lastCanvasPos = canvasPos;
    }

    // Draw canvas
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y), IM_COL32(26, 26, 26, 255));
    drawList->AddCircleFilled(center, 5.0f, IM_COL32(255, 255, 255, 255)); // Central person

    // Draw circle line for speaker positions
    drawList->AddCircle(center, radius, IM_COL32(163, 163, 163, 255), 100);

    // Add markers at 0, 22.5, 45, etc.
    for (float angle = 0; angle < 360; angle += 22.5f) {
        float rad = angle * (M_PI / 180.0f);
        ImVec2 markerStart = ImVec2(center.x + (radius - 5) * cosf(rad), center.y + (radius - 5) * sinf(rad));
        ImVec2 markerEnd = ImVec2(center.x + radius * cosf(rad), center.y + radius * sinf(rad));
        drawList->AddLine(markerStart, markerEnd, IM_COL32(163, 163, 163, 255));
    }

    const char* speakerLabels[4] = { "L", "R", "RL", "RR" };
    const char* cvarNames[4] = { "gPositionFrontLeft", "gPositionFrontRight", "gPositionRearLeft", "gPositionRearRight" };

    const float snapThreshold = 2.5f; // Degrees within which snapping occurs

    for (int i = 0; i < 4; i++) {
        // Draw speaker as a darker blue circle
        drawList->AddCircleFilled(speakerPositions[i], 10.0f, IM_COL32(34, 52, 78, 255)); // Dark blue color
        drawList->AddText(ImVec2(speakerPositions[i].x - 6, speakerPositions[i].y - 6), IM_COL32(255, 255, 255, 255), speakerLabels[i]);

        // Handle dragging
        ImGui::SetCursorScreenPos(ImVec2(speakerPositions[i].x - 10, speakerPositions[i].y - 10));
        ImGui::InvisibleButton(speakerLabels[i], ImVec2(20, 20));
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            ImVec2 mouseDelta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
            ImVec2 newPos = ImVec2(speakerPositions[i].x + mouseDelta.x, speakerPositions[i].y + mouseDelta.y);

            // Constrain position to the circle
            ImVec2 direction = ImVec2(newPos.x - center.x, newPos.y - center.y);
            float length = sqrtf(direction.x * direction.x + direction.y * direction.y);
            ImVec2 constrainedPos = ImVec2(center.x + (direction.x / length) * radius, center.y + (direction.y / length) * radius);

            // Calculate angle of the constrained position
            float angle = atan2f(constrainedPos.y - center.y, constrainedPos.x - center.x) * (180.0f / M_PI);
            if (angle < 0) angle += 360.0f;

            // Snap to the nearest 22.5-degree marker if within the snap threshold
            float snappedAngle = roundf(angle / 22.5f) * 22.5f;
            if (fabsf(snappedAngle - angle) <= snapThreshold) {
                float rad = snappedAngle * (M_PI / 180.0f);
                constrainedPos = ImVec2(center.x + radius * cosf(rad), center.y + radius * sinf(rad));
            }

            speakerPositions[i] = constrainedPos;
            ImGui::ResetMouseDragDelta();

            // Save the updated angle to CVar after dragging
            float updatedAngle = atan2f(speakerPositions[i].y - center.y, speakerPositions[i].x - center.x) * (180.0f / M_PI);
            if (updatedAngle < 0) updatedAngle += 360.0f;
            CVarSetInteger(cvarNames[i], static_cast<int>(updatedAngle));
            Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame(); // Mark for saving
        }

        // Calculate angle and save to CVar
        float angle = atan2f(speakerPositions[i].y - center.y, speakerPositions[i].x - center.x) * (180.0f / M_PI);
        if (angle < 0) angle += 360.0f;
        CVarSetInteger(cvarNames[i], static_cast<int>(angle));
    }

    // Reset cursor position for button placement
    ImGui::SetCursorScreenPos(ImVec2(canvasPos.x, canvasPos.y + canvasSize.y + 10));
    if (ImGui::Button("Reset Positions")) {
        float defaultAngles[4] = { 240.f, 300.f, 160.f, 20.f };
        for (int i = 0; i < 4; i++) {
            float rad = defaultAngles[i] * (M_PI / 180.0f);
            speakerPositions[i] = ImVec2(center.x + radius * cosf(rad), center.y + radius * sinf(rad));
            CVarSetInteger(cvarNames[i], static_cast<int>(defaultAngles[i]));
        }
        Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }

    // Reset cursor position to ensure canvas size remains static
    ImGui::SetCursorScreenPos(ImVec2(canvasPos.x, canvasPos.y + canvasSize.y + 10));
}

void DrawSettingsMenu(){
    if(UIWidgets::BeginMenu("Settings")){
        if (UIWidgets::BeginMenu("Audio")) {
            UIWidgets::CVarSliderFloat("Master Volume", "gGameMasterVolume", 0.0f, 1.0f, 1.0f, {
                .format = "%.0f%%",
                .isPercentage = true,
            });
            // Music/voice/SFX volumes are backed by the game save, not just the CVar: at boot
            // GSTATE_INIT rebuilds these CVars from gSaveFile (fox_game.c), so unless we persist
            // the save the change reverts on the next launch. Update the live audio on every change,
            // but flush the save only when the slider is released (IsItemDeactivatedAfterEdit) so a
            // mouse drag doesn't hammer the emulated-EEPROM write path each frame — matching the
            // native sound menu, which saves once on exit (fox_option.c).
            if (UIWidgets::CVarSliderFloat("Main Music Volume", "gMainMusicVolume", 0.0f, 1.0f, 1.0f, {
                .format = "%.0f%%",
                .isPercentage = true,
            })) {
                float val = CVarGetFloat("gMainMusicVolume", 1.0f) * 100;
                gSaveFile.save.data.musicVolume = (u8) val;
                Audio_SetVolume(AUDIO_TYPE_MUSIC, (u8) val);
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                Save_Write();
            }
            if (UIWidgets::CVarSliderFloat("Voice Volume", "gVoiceVolume", 0.0f, 1.0f, 1.0f, {
                .format = "%.0f%%",
                .isPercentage = true,
            })) {
                float val = CVarGetFloat("gVoiceVolume", 1.0f) * 100;
                gSaveFile.save.data.voiceVolume = (u8) val;
                Audio_SetVolume(AUDIO_TYPE_VOICE, (u8) val);
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                Save_Write();
            }
            if (UIWidgets::CVarSliderFloat("Sound Effects Volume", "gSFXMusicVolume", 0.0f, 1.0f, 1.0f, {
                .format = "%.0f%%",
                .isPercentage = true,
            })) {
                float val = CVarGetFloat("gSFXMusicVolume", 1.0f) * 100;
                gSaveFile.save.data.sfxVolume = (u8) val;
                Audio_SetVolume(AUDIO_TYPE_SFX, (u8) val);
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                Save_Write();
            }

            static std::unordered_map<Ship::AudioBackend, const char*> audioBackendNames = {
                    { Ship::AudioBackend::WASAPI, "Windows Audio Session API" },
                    { Ship::AudioBackend::SDL, "SDL" },
            };

            ImGui::Text("Audio API (Needs reload)");
            auto currentAudioBackend = Ship::Context::GetInstance()->GetAudio()->GetCurrentAudioBackend();

            if (Ship::Context::GetInstance()->GetAudio()->GetAvailableAudioBackends()->size() <= 1) {
                UIWidgets::DisableComponent(ImGui::GetStyle().Alpha * 0.5f);
            }
            bool audioApiComboOpen = ImGui::BeginCombo("##AApi", audioBackendNames[currentAudioBackend]);
            if (ImGui::IsItemFocused()) {
                AccessibilityImGuiMenu_ItemFocused("Audio API", AccessibilityRole::ComboBox, audioBackendNames[currentAudioBackend]);
            }
            if (audioApiComboOpen) {
                for (uint8_t i = 0; i < Ship::Context::GetInstance()->GetAudio()->GetAvailableAudioBackends()->size(); i++) {
                    auto backend = Ship::Context::GetInstance()->GetAudio()->GetAvailableAudioBackends()->data()[i];
                    bool isSelected = backend == currentAudioBackend;
                    if (ImGui::Selectable(audioBackendNames[backend], isSelected)) {
                        Ship::Context::GetInstance()->GetAudio()->SetCurrentAudioBackend(backend);
                        AccessibilityImGuiMenu_ValueChanged("Audio API", audioBackendNames[backend]);
                    }
                    if (ImGui::IsItemFocused()) {
                        AccessibilityImGuiMenu_ItemFocused(audioBackendNames[backend], AccessibilityRole::Option, isSelected ? "selected" : nullptr);
                    }
                }
                ImGui::EndCombo();
            }
            if (Ship::Context::GetInstance()->GetAudio()->GetAvailableAudioBackends()->size() <= 1) {
                UIWidgets::ReEnableComponent("");
            }
            
            UIWidgets::PaddedEnhancementCheckbox("Surround 5.1 (Needs reload)", "gAudioChannelsSetting", 1, 0);
            
            if (CVarGetInteger("gAudioChannelsSetting", 0) == 1) {
                // Subwoofer threshold
                UIWidgets::CVarSliderInt("Subwoofer threshold (Hz)", "gSubwooferThreshold", 10u, 1000u, 80u, {
                    .tooltip = "The threshold for the subwoofer to be activated. Any sound under this frequency will be played on the subwoofer.",
                    .format = "%d",
                });

                // Rear music volume slider
                UIWidgets::CVarSliderFloat("Rear music volume", "gVolumeRearMusic", 0.0f, 1.0f, 1.0f, {
                    .format = "%.0f%%",
                    .isPercentage = true,
                });

                // Configurable positioning of speakers
                DrawSpeakerPositionEditor();
            }

            ImGui::EndMenu();
        }
        
        if (!GameEngine::HasVersion(SF64_VER_JP) || GameEngine::HasVersion(SF64_VER_EU)) {
            UIWidgets::Spacer(0);
            if (UIWidgets::BeginMenu("Language")) {
                ImGui::Dummy(ImVec2(150, 0.0f));
                if (!GameEngine::HasVersion(SF64_VER_JP) && (GameEngine::HasVersion(SF64_VER_EU) || GameEngine::HasVersion(SF64_VER_EU_SPA))) {
                    if (GameEngine::HasVersion(SF64_VER_EU_SPA)) {
                        //UIWidgets::Spacer(0);
                        if (UIWidgets::CVarCombobox("Voices", "gVoiceLanguage", voiceLangsSPA, 
                            {
                                .tooltip = "Changes the language of the voice acting in the game",
                                .defaultIndex = 0,
                            })) {
                                Audio_SetVoiceLanguage(CVarGetInteger("gVoiceLanguage", 0));
                            };
                    } else {
                        //UIWidgets::Spacer(0);
                        if (UIWidgets::CVarCombobox("Voices", "gVoiceLanguage", voiceLangs, 
                            {
                                .tooltip = "Changes the language of the voice acting in the game",
                                .defaultIndex = 0,
                            })) {
                                Audio_SetVoiceLanguage(CVarGetInteger("gVoiceLanguage", 0));
                            };
                    }
                } else {
                    if (UIWidgets::Button("Install JP/EU Audio")) {
                        if (GameEngine::GenAssetFile(false)){
                            GameEngine::ShowMessage("Success", "Audio assets installed. Changes will be applied on the next startup.", SDL_MESSAGEBOX_INFORMATION);
                        }
                        Ship::Context::GetInstance()->GetWindow()->Close();
                    }
                }
                ImGui::EndMenu();
            }
        }
        
        UIWidgets::Spacer(0);

        if (UIWidgets::BeginMenu("Controller")) {
            UIWidgets::WindowButton("Controller Mapping", "gInputEditorWindow", GameUI::mInputEditorWindow);

            UIWidgets::Spacer(0);

            UIWidgets::CVarCheckbox("Menubar Controller Navigation", "gControlNav", {
                .tooltip = "Allows controller navigation of the SOH menu bar (Settings, Enhancements,...)\nCAUTION: This will disable game inputs while the menubar is visible.\n\nD-pad to move between items, A to select, and X to grab focus on the menu bar"
            });

            UIWidgets::CVarCheckbox("Invert Y Axis", "gInvertYAxis",{
                .tooltip = "Inverts the Y axis for controlling vehicles"
            });

            ImGui::EndMenu();
        }

        UIWidgets::Spacer(0);

        if (UIWidgets::BeginMenu("Blind Starship")) {
            UIWidgets::CVarCheckbox("Screen reader", "gAccessibilityScreenReader", {
                .tooltip = "Speaks menus, screens, and gameplay events through your system screen reader.",
                .defaultValue = true
            });
            UIWidgets::CVarCheckbox("Audio cues", kAudioCuesEnabledCVar, {
                .tooltip = "Positional audio cues that guide you toward the next ring and the closest lockable "
                           "enemies, and warn about obstacles ahead.",
                .defaultValue = true
            });
            UIWidgets::CVarSliderInt("Enemy locator voices", kEnemyCueVoicesCVar, 1,
                                     kAccessibilityEnemyCueMaxVoices, kAccessibilityEnemyCueDefaultVoices, {
                .tooltip = "How many of the closest lockable enemies the enemy locator sounds at once."
            });
            UIWidgets::CVarCheckbox("Aim guide", kAimCueEnabledCVar, {
                .tooltip = "A repeating click that tells you where you are aiming: pan for left/right, pitch for "
                           "up/down; it clicks faster as your aim nears a lockable enemy. Arwing only.",
                .defaultValue = true
            });
            UIWidgets::CVarCheckbox("Obstacle warning", kObstacleCueEnabledCVar, {
                .tooltip = "Obstacle cues: a low buzz that beats faster as you close on something solid on your "
                           "course that you cannot shoot down, plus three chords that tell you the space beside, "
                           "above or below you is closed. The buzz works on rails and in solo all-range battles; "
                           "the chords on rails. Note: Minimal training removes Training's obstacles, so these "
                           "cues stay silent there.",
                .defaultValue = true
            });
            if (UIWidgets::BeginMenu("Cue volumes")) {
                // Slider changes are pushed to the live sources immediately so a running
                // preview (and any in-level cue) tracks the drag; gameplay ticks would pick
                // the CVars up anyway, but the preview only re-reads gain when told to.
                if (UIWidgets::CVarSliderFloat("All cues", kCueMasterVolumeCVar, 0.0f, 1.0f, 1.0f, {
                    .format = "%.0f%%",
                    .isPercentage = true,
                })) {
                    for (Cue* cue : CueRegistry_All()) {
                        cue->PushGain();
                    }
                }
                // CVarSliderFloat's label doubles as the printf-style format for its value
                // readout (UIWidgets::FormatValue), so any literal '%' arriving from a cue
                // name must be escaped to '%%' or a mismatched specifier is UB.
                auto escapePercents = [](const char* text) {
                    std::string out;
                    for (const char* p = text; *p != '\0'; p++) {
                        out += *p;
                        if (*p == '%') {
                            out += '%';
                        }
                    }
                    return out;
                };
                for (Cue* cue : CueRegistry_All()) {
                    if (cue->HiddenFromSettings()) {
                        continue; // bench/internal cues own their own controls, not this list
                    }
                    std::string sliderLabel = escapePercents(cue->Name()) + "##CueVolume" + cue->Id();
                    if (UIWidgets::CVarSliderFloat(sliderLabel.c_str(), cue->VolumeCVar(), 0.0f, 1.0f, 1.0f, {
                        .tooltip = cue->Description(),
                        .format = "%.0f%%",
                        .isPercentage = true,
                    })) {
                        cue->PushGain();
                    }
                    // "###" keeps one ImGui ID across the label flip so focus stays put.
                    std::string buttonLabel = StringHelper::Sprintf(
                        "%s %s###CuePreview%s", cue->IsPreviewing() ? "Stop preview of" : "Preview", cue->Name(),
                        cue->Id());
                    if (UIWidgets::Button(buttonLabel.c_str(), {
                        .tooltip = "Plays a short sample of this cue, straight ahead, at the volume set above.",
                    })) {
                        if (cue->IsPreviewing()) {
                            cue->StopPreview();
                        } else {
                            cue->StartPreview();
                        }
                    }
                }
                ImGui::EndMenu();
            }
            UIWidgets::CVarCheckbox("Score announcements", "gAccessibilityScoreAnnounce", {
                .tooltip = "Speaks hit/bonus popups and the Training ring streak.",
                .defaultValue = true
            });
            UIWidgets::CVarCheckbox("Radio message text", "gAccessibilityRadioText", {
                .tooltip = "Speaks radio messages that have no voice acting, such as the Training mode instructions.",
                .defaultValue = true
            });
            UIWidgets::CVarCheckbox("Minimal training", "gAccessibilityTrainingMinimal", {
                .tooltip = "Strips collidable obstacles from Training while keeping enemies.",
                .defaultValue = true
            });
            ImGui::EndMenu();
        }

        ImGui::EndMenu();
    }

    ImGui::SetCursorPosY(0.0f);
    if (UIWidgets::BeginMenu("Graphics")) {
        UIWidgets::WindowButton("Resolution Editor", "gAdvancedResolutionEditorEnabled", GameUI::mAdvancedResolutionSettingsWindow);

        UIWidgets::Spacer(0);

        // Previously was running every frame, and nothing was setting it? Maybe a bad copy/paste?
        // Ship::Context::GetInstance()->GetWindow()->SetResolutionMultiplier(CVarGetFloat("gInternalResolution", 1));
        // UIWidgets::Tooltip("Multiplies your output resolution by the value inputted, as a more intensive but effective form of anti-aliasing");
#ifndef __WIIU__
        if (UIWidgets::CVarSliderInt("MSAA: %d", "gMSAAValue", 1, 8, 1, {
            .tooltip = "Activates multi-sample anti-aliasing when above 1x up to 8x for 8 samples for every pixel"
        })) {
            Ship::Context::GetInstance()->GetWindow()->SetMsaaLevel(CVarGetInteger("gMSAAValue", 1));
        }
#endif

        { // FPS Slider
            const int minFps = 30;
            static int maxFps = 360;
            int currentFps = 0;
        #ifdef __WIIU__
            UIWidgets::Spacer(0);
            // only support divisors of 60 on the Wii U
            if (currentFps > 60) {
                currentFps = 60;
            } else {
                currentFps = 60 / (60 / currentFps);
            }

            int fpsSlider = 1;
            if (currentFps == 30) {
                ImGui::Text("FPS: Original (30)");
            } else {
                ImGui::Text("FPS: %d", currentFps);
                if (currentFps == 30) {
                    fpsSlider = 2;
                } else { // currentFps == 60
                    fpsSlider = 3;
                }
            }
            if (CVarGetInteger("gMatchRefreshRate", 0)) {
                UIWidgets::DisableComponent(ImGui::GetStyle().Alpha * 0.5f);
            }

            if (ImGui::Button(" - ##WiiUFPS")) {
                fpsSlider--;
                AccessibilityImGuiMenu_ValueChanged("FPS", StringHelper::Sprintf("%d", fpsSlider).c_str());
            }
            if (ImGui::IsItemFocused()) {
                AccessibilityImGuiMenu_ItemFocused("Decrease FPS", AccessibilityRole::Button, "");
            }
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() - 7.0f);

            UIWidgets::Spacer(0);

            ImGui::PushItemWidth(std::min((ImGui::GetContentRegionAvail().x - 60.0f), 260.0f));
            ImGui::SliderInt("##WiiUFPSSlider", &fpsSlider, 1, 3, "", ImGuiSliderFlags_AlwaysClamp);
            if (ImGui::IsItemFocused()) {
                AccessibilityImGuiMenu_ItemFocused("FPS", AccessibilityRole::Slider, StringHelper::Sprintf("%d", fpsSlider).c_str());
            }
            ImGui::PopItemWidth();

            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() - 7.0f);
            if (ImGui::Button(" + ##WiiUFPS")) {
                fpsSlider++;
                AccessibilityImGuiMenu_ValueChanged("FPS", StringHelper::Sprintf("%d", fpsSlider).c_str());
            }
            if (ImGui::IsItemFocused()) {
                AccessibilityImGuiMenu_ItemFocused("Increase FPS", AccessibilityRole::Button, "");
            }

            if (CVarGetInteger("gMatchRefreshRate", 0)) {
                UIWidgets::ReEnableComponent("");
            }
            if (fpsSlider > 3) {
                fpsSlider = 3;
            } else if (fpsSlider < 1) {
                fpsSlider = 1;
            }

            if (fpsSlider == 1) {
                currentFps = 20;
            } else if (fpsSlider == 2) {
                currentFps = 30;
            } else if (fpsSlider == 3) {
                currentFps = 60;
            }
            CVarSetInteger("gInterpolationFPS", currentFps);
            Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
            // WiiU uses a raw slider + buttons above, so it still needs the standalone hover
            // tooltip; the non-WiiU path carries it in the CVarSliderInt options below so the
            // screen reader speaks it as part of the slider's focus announcement.
            UIWidgets::Tooltip(
                "Uses Matrix Interpolation to create extra frames, resulting in smoother graphics. This is purely "
                "visual and does not impact game logic, execution of glitches etc.\n\n"
                "A higher target FPS than your monitor's refresh rate will waste resources, and might give a worse result."
            );
        #else
            bool matchingRefreshRate = CVarGetInteger("gMatchRefreshRate", 0);
            UIWidgets::CVarSliderInt((currentFps == 30) ? "FPS: Original (30)" : "FPS: %d", "gInterpolationFPS", minFps, maxFps, 60, {
                .tooltip = "Uses Matrix Interpolation to create extra frames, resulting in smoother graphics. This is purely "
                           "visual and does not impact game logic, execution of glitches etc.\n\n"
                           "A higher target FPS than your monitor's refresh rate will waste resources, and might give a worse result.",
                .disabled = matchingRefreshRate
            });
        #endif
        } // END FPS Slider

        UIWidgets::PaddedEnhancementCheckbox("Match Refresh Rate", "gMatchRefreshRate", true, false, false, "",
                                             UIWidgets::CheckboxGraphics::Cross, false,
                                             "Matches interpolation value to the refresh rate of your display.");

        if (Ship::Context::GetInstance()->GetWindow()->GetWindowBackend() == Ship::WindowBackend::FAST3D_DXGI_DX11) {
            UIWidgets::PaddedEnhancementCheckbox("Render parallelization","gRenderParallelization", true, false, false, "",
                                                 UIWidgets::CheckboxGraphics::Cross, true,
                "This setting allows the CPU to work on one frame while GPU works on the previous frame.\n"
                "Recommended if you can't reach the FPS you set, despite it being set below your refresh rate "
                "or if you notice other performance problems.\n"
                "Adds up to one frame of input lag under certain scenarios.");
        }
      
        UIWidgets::PaddedSeparator(true, true, 3.0f, 3.0f);

        static std::unordered_map<Ship::WindowBackend, const char*> windowBackendNames = {
                { Ship::WindowBackend::FAST3D_DXGI_DX11, "DirectX" },
                { Ship::WindowBackend::FAST3D_SDL_OPENGL, "OpenGL"},
                { Ship::WindowBackend::FAST3D_SDL_METAL, "Metal" }
        };

        ImGui::Text("Renderer API (Needs reload)");
        Ship::WindowBackend runningWindowBackend = Ship::Context::GetInstance()->GetWindow()->GetWindowBackend();
        Ship::WindowBackend configWindowBackend;
        int configWindowBackendId = Ship::Context::GetInstance()->GetConfig()->GetInt("Window.Backend.Id", -1);
        if (configWindowBackendId != -1 && configWindowBackendId < static_cast<int>(Ship::WindowBackend::WINDOW_BACKEND_COUNT)) {
            configWindowBackend = static_cast<Ship::WindowBackend>(configWindowBackendId);
        } else {
            configWindowBackend = runningWindowBackend;
        }

        if (Ship::Context::GetInstance()->GetWindow()->GetAvailableWindowBackends()->size() <= 1) {
            UIWidgets::DisableComponent(ImGui::GetStyle().Alpha * 0.5f);
        }
        bool rendererApiComboOpen = ImGui::BeginCombo("##RApi", windowBackendNames[configWindowBackend]);
        if (ImGui::IsItemFocused()) {
            AccessibilityImGuiMenu_ItemFocused("Renderer API", AccessibilityRole::ComboBox, windowBackendNames[configWindowBackend]);
        }
        if (rendererApiComboOpen) {
            for (size_t i = 0; i < Ship::Context::GetInstance()->GetWindow()->GetAvailableWindowBackends()->size(); i++) {
                auto backend = Ship::Context::GetInstance()->GetWindow()->GetAvailableWindowBackends()->data()[i];
                bool isSelected = backend == configWindowBackend;
                if (ImGui::Selectable(windowBackendNames[backend], isSelected)) {
                    Ship::Context::GetInstance()->GetConfig()->SetInt("Window.Backend.Id", static_cast<int>(backend));
                    Ship::Context::GetInstance()->GetConfig()->SetString("Window.Backend.Name",
                                                                        windowBackendNames[backend]);
                    Ship::Context::GetInstance()->GetConfig()->Save();
                    AccessibilityImGuiMenu_ValueChanged("Renderer API", windowBackendNames[backend]);
                }
                if (ImGui::IsItemFocused()) {
                    AccessibilityImGuiMenu_ItemFocused(windowBackendNames[backend], AccessibilityRole::Option, isSelected ? "selected" : nullptr);
                }
            }
            ImGui::EndCombo();
        }
        if (Ship::Context::GetInstance()->GetWindow()->GetAvailableWindowBackends()->size() <= 1) {
            UIWidgets::ReEnableComponent("");
        }

        if (Ship::Context::GetInstance()->GetWindow()->CanDisableVerticalSync()) {
            UIWidgets::PaddedEnhancementCheckbox("Enable Vsync", "gVsyncEnabled", true, false, false, "", UIWidgets::CheckboxGraphics::Cross, true,
                                                 "Removes tearing, but clamps your max FPS to your displays refresh rate.");
        }

        if (Ship::Context::GetInstance()->GetWindow()->SupportsWindowedFullscreen()) {
            UIWidgets::PaddedEnhancementCheckbox("Windowed fullscreen", "gSdlWindowedFullscreen", true, false);
        }

        if (Ship::Context::GetInstance()->GetWindow()->GetGui()->SupportsViewports()) {
            UIWidgets::PaddedEnhancementCheckbox("Allow multi-windows", "gEnableMultiViewports", true, false, false, "", UIWidgets::CheckboxGraphics::Cross, true,
                                                 "Allows windows to be able to be dragged off of the main game window. Requires a reload to take effect.");
        }

        UIWidgets::PaddedEnhancementCheckbox("Enable Alternative Assets", "gEnhancements.Mods.AlternateAssets");
        // If more filters are added to LUS, make sure to add them to the filters list here
        ImGui::Text("Texture Filter (Needs reload)");
        UIWidgets::EnhancementCombobox("gTextureFilter", filters, 0);

        UIWidgets::PaddedEnhancementCheckbox("Apply Point Filtering to UI Elements", "gHUDPointFiltering", true, false, false, "", UIWidgets::CheckboxGraphics::Cross, true);
        UIWidgets::Spacer(0);

        Ship::Context::GetInstance()->GetWindow()->GetGui()->GetGameOverlay()->DrawSettings();

        ImGui::EndMenu();
    }
}

void DrawMenuBarIcon() {
    static bool gameIconLoaded = false;
    if (!gameIconLoaded) {
        // Ship::Context::GetInstance()->GetWindow()->GetGui()->LoadGuiTexture("Game_Icon", "textures/icons/gIcon.png", ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        gameIconLoaded = false;
    }

    if (Ship::Context::GetInstance()->GetWindow()->GetGui()->GetTextureByName("Game_Icon")) {
#ifdef __SWITCH__
        ImVec2 iconSize = ImVec2(20.0f, 20.0f);
        float posScale = 1.0f;
#elif defined(__WIIU__)
        ImVec2 iconSize = ImVec2(16.0f * 2, 16.0f * 2);
        float posScale = 2.0f;
#else
        ImVec2 iconSize = ImVec2(20.0f, 20.0f);
        float posScale = 1.5f;
#endif
        ImGui::SetCursorPos(ImVec2(5, 2.5f) * posScale);
        ImGui::Image(Ship::Context::GetInstance()->GetWindow()->GetGui()->GetTextureByName("Game_Icon"), iconSize);
        ImGui::SameLine();
        ImGui::SetCursorPos(ImVec2(25, 0) * posScale);
    }
}

void DrawGameMenu() {
    if (UIWidgets::BeginMenu("Starship")) {
        if (UIWidgets::MenuItem("Reset", "F4")) {
            gNextGameState = GSTATE_BOOT;
        }
#if !defined(__SWITCH__) && !defined(__WIIU__)

        if (UIWidgets::MenuItem("Toggle Fullscreen", "F11")) {
            Ship::Context::GetInstance()->GetWindow()->ToggleFullscreen();
        }
#endif

        if (UIWidgets::MenuItem("Quit")) {
            Ship::Context::GetInstance()->GetWindow()->Close();
        }
        ImGui::EndMenu();
    }
}

static const char* hudAspects[] = {
    "Expand", "Custom", "Original (4:3)", "Widescreen (16:9)", "Nintendo 3DS (5:3)", "16:10 (8:5)", "Ultrawide (21:9)"
};

static const char* radioCommBox[] = {
    "Original", "Expand"
};

void DrawEnhancementsMenu() {
    if (UIWidgets::BeginMenu("Enhancements")) {

        if (UIWidgets::BeginMenu("Gameplay")) {
            UIWidgets::CVarCheckbox("No Level of Detail (LOD)", "gDisableLOD", {
                .tooltip = "Disable Level of Detail (LOD) to avoid models using lower poly versions at a distance",
                .defaultValue = true
            });
            UIWidgets::CVarCheckbox("Character heads inside Arwings at all times", "gTeamFaces", {
                .tooltip = "Character heads are displayed inside Arwings in all cutscenes",
                .defaultValue = true
            });
            UIWidgets::CVarCheckbox("Use red radio backgrounds for enemies.", "gEnemyRedRadio");
            UIWidgets::CVarSliderInt("Cockpit Glass Opacity: %d", "gCockpitOpacity", 0, 255, 120);
            

            ImGui::EndMenu();
        }
        
        if (UIWidgets::BeginMenu("Fixes")) {
            UIWidgets::CVarCheckbox("Macbeth: Level ending cutscene camera fix", "gMaCameraFix", {
                .tooltip = "Fixes a camera bug found in the code of the game"
            });

            UIWidgets::CVarCheckbox("Sector Z: Spawn all actors", "gSzActorFix", {
                .tooltip = "Fixes a bug found in Sector Z, where only 10 of 12 available actors are spawned, this causes two 'Space Junk Boxes' to be missing from the level."
            });

            ImGui::EndMenu();
        }

        if (UIWidgets::BeginMenu("Restoration")) {
            UIWidgets::CVarCheckbox("Sector Z: Missile cutscene bug", "gSzMissileBug", {
                .tooltip = "Restores the missile cutscene bug present in JP 1.0"
            });

            UIWidgets::CVarCheckbox("Beta: Restore beta bomb explosion", "gRestoreBetaBombExplosion", {
                .tooltip = "Restores the beta bomb explosion found inside the game"
            });

            UIWidgets::CVarCheckbox("Beta: Restore beta coin", "gRestoreBetaCoin", {
                .tooltip = "Restores the beta coin that got replaced with the gold ring"
            });

            UIWidgets::CVarCheckbox("Beta: Restore beta boost/brake gauge", "gRestoreBetaBoostGauge", {
                .tooltip = "Restores the beta boost gauge that was seen in some beta footage"
            });

            ImGui::EndMenu();
        }

        if (UIWidgets::BeginMenu("HUD")) {
            if (UIWidgets::CVarCombobox("Radio Communication Box", "gRadioCommBox.Selection", radioCommBox, 
            {
                .tooltip = "Which Aspect Ratio to use when drawing the Radio Communication Box",
                .defaultIndex = 0,
            })) {
                switch (CVarGetInteger("gRadioCommBox.Selection", 0)) {
                    case 0:
                        CVarSetInteger("gRadioCommBox.expand", 0);
                        break;
                    case 1:
                        CVarSetInteger("gRadioCommBox.expand", 1);
                        break;
                }
            }

            if (UIWidgets::CVarCombobox("HUD Aspect Ratio", "gHUDAspectRatio.Selection", hudAspects, 
            {
                .tooltip = "Which Aspect Ratio to use when drawing the HUD (Radar, gauges and radio messages)",
                .defaultIndex = 0,
            })) {
                CVarSetInteger("gHUDAspectRatio.Enabled", 1);
                switch (CVarGetInteger("gHUDAspectRatio.Selection", 0)) {
                    case 0:
                        CVarSetInteger("gHUDAspectRatio.Enabled", 0);
                        CVarSetInteger("gHUDAspectRatio.X", 0);
                        CVarSetInteger("gHUDAspectRatio.Y", 0);
                        break;
                    case 1:
                        if (CVarGetInteger("gHUDAspectRatio.X", 0) <= 0){
                            CVarSetInteger("gHUDAspectRatio.X", 1);
                        }
                        if (CVarGetInteger("gHUDAspectRatio.Y", 0) <= 0){
                            CVarSetInteger("gHUDAspectRatio.Y", 1);
                        }
                        break;
                    case 2:
                        CVarSetInteger("gHUDAspectRatio.X", 4);
                        CVarSetInteger("gHUDAspectRatio.Y", 3);
                        break;
                    case 3:
                        CVarSetInteger("gHUDAspectRatio.X", 16);
                        CVarSetInteger("gHUDAspectRatio.Y", 9);
                        break;
                    case 4:
                        CVarSetInteger("gHUDAspectRatio.X", 5);
                        CVarSetInteger("gHUDAspectRatio.Y", 3);
                        break;
                    case 5:
                        CVarSetInteger("gHUDAspectRatio.X", 8);
                        CVarSetInteger("gHUDAspectRatio.Y", 5);
                        break;
                    case 6:
                        CVarSetInteger("gHUDAspectRatio.X", 21);
                        CVarSetInteger("gHUDAspectRatio.Y", 9);
                        break;                    
                }
            }
            
            if (CVarGetInteger("gHUDAspectRatio.Selection", 0) == 1)
            {
                UIWidgets::CVarSliderInt("Horizontal: %d", "gHUDAspectRatio.X", 1, 100, 1);
                UIWidgets::CVarSliderInt("Vertical: %d", "gHUDAspectRatio.Y", 1, 100, 1);
            }

            ImGui::Dummy(ImVec2(ImGui::CalcTextSize("Nintendo 3DS (5:3)").x + 35, 0.0f));
            ImGui::EndMenu();
        }

        if (UIWidgets::BeginMenu("Accessibility")) { 
            UIWidgets::CVarCheckbox("Disable Gorgon (Area 6 boss) screen flashes", "gDisableGorgonFlash", {
                .tooltip = "Gorgon flashes the screen repeatedly when firing its beam or when teleporting, which causes eye pain for some players and may be harmful to those with photosensitivity.",
                .defaultValue = false
            });
            UIWidgets::CVarCheckbox("Add outline to Arwing and Wolfen in radar", "gFighterOutlines", {
                .tooltip = "Increases visibility of ships in the radar.",
                .defaultValue = false
            });
            ImGui::EndMenu();
        }

        ImGui::EndMenu();
    }
}

void DrawCheatsMenu() {
    if (UIWidgets::BeginMenu("Cheats")) {
        UIWidgets::CVarCheckbox("Infinite Lives", "gInfiniteLives");
        UIWidgets::CVarCheckbox("Invincible", "gInvincible");
        UIWidgets::CVarCheckbox("Unbreakable Wings", "gUnbreakableWings");
        UIWidgets::CVarCheckbox("Infinite Bombs", "gInfiniteBombs");
        UIWidgets::CVarCheckbox("Infinite Boost/Brake", "gInfiniteBoost");
        UIWidgets::CVarCheckbox("Hyper Laser", "gHyperLaser");
        UIWidgets::CVarSliderInt("Laser Range Multiplier: %d%%", "gLaserRangeMult", 15, 800, 100,
            { .tooltip = "Changes how far your lasers fly." });
        UIWidgets::CVarCheckbox("Rapid-fire mode", "gRapidFire", {
                .tooltip = "Hold A to keep firing. Release A to start charging a shot."
            });
            if (CVarGetInteger("gRapidFire", 0) == 1) {
                ImGui::Dummy(ImVec2(22.0f, 0.0f));
                ImGui::SameLine();
                UIWidgets::CVarCheckbox("Hold L to Charge", "gLtoCharge", {
                    .tooltip = "If you prefer to not have auto-charge."
                });
            }
        UIWidgets::CVarCheckbox("Self destruct button", "gHit64SelfDestruct", {
                .tooltip = "Press Down on the D-PAD to instantly self destruct."
            });
        UIWidgets::CVarCheckbox("Start with Falco dead", "gHit64FalcoDead", {
                .tooltip = "Start the level with with Falco dead."
            });
        UIWidgets::CVarCheckbox("Start with Slippy dead", "gHit64SlippyDead", {
                .tooltip = "Start the level with with Slippy dead."
            });
        UIWidgets::CVarCheckbox("Start with Peppy dead", "gHit64PeppyDead", {
                .tooltip = "Start the level with with Peppy dead."
            });
        
        UIWidgets::CVarCheckbox("Score Editor", "gScoreEditor", { .tooltip = "Enable the score editor" });

        if (CVarGetInteger("gScoreEditor", 0) == 1) {
            UIWidgets::CVarSliderInt("Score: %d", "gScoreEditValue", 0, 999, 0,
                { .tooltip = "Increase or decrease the current mission score number" });
        }

        ImGui::EndMenu();
    }
}

static const char* debugInfoPages[6] = {
    "Object",
    "Check Surface",
    "Map",
    "Stage",
    "Effect",
    "Enemy",
};

static const char* logLevels[] = {
    "trace", "debug", "info", "warn", "error", "critical", "off",
};

// Cue3D render modes for the test bench combo — index maps 1:1 to Cue3DMode
// (HRTF = 0, PAN = 1, DIRECT = 2).
static const char* cueBenchModes[] = {
    "HRTF", "Pan", "Direct",
};

void DrawDebugMenu() {
    if (UIWidgets::BeginMenu("Developer")) {
        if (UIWidgets::CVarCombobox("Log Level", "gDeveloperTools.LogLevel", logLevels, {
            .tooltip = "The log level determines which messages are printed to the "
                        "console. This does not affect the log file output",
            .defaultIndex = 1,
        })) {
            Ship::Context::GetInstance()->GetLogger()->set_level((spdlog::level::level_enum)CVarGetInteger("gDeveloperTools.LogLevel", 1));
        }

#ifdef __SWITCH__
        if (UIWidgets::CVarCombobox("Switch CPU Profile", "gSwitchPerfMode", SWITCH_CPU_PROFILES, {
            .tooltip = "Switches the CPU profile to a different one",
            .defaultIndex = (int)Ship::SwitchProfiles::STOCK
        })) {
            SPDLOG_INFO("Profile:: %s", SWITCH_CPU_PROFILES[CVarGetInteger("gSwitchPerfMode", (int)Ship::SwitchProfiles::STOCK)]);
            Ship::Switch::ApplyOverclock();
        }
#endif

        UIWidgets::WindowButton("Gfx Debugger", "gGfxDebuggerEnabled", GameUI::mGfxDebuggerWindow, {
            .tooltip = "Enables the Gfx Debugger window, allowing you to input commands, type help for some examples"
        });

        // UIWidgets::CVarCheckbox("Debug mode", "gEnableDebugMode", {
        //     .tooltip = "TBD"
        // });

        UIWidgets::CVarCheckbox("Level Selector", "gLevelSelector", {
            .tooltip = "Allows you to select any level from the main menu"
        });

        UIWidgets::CVarCheckbox("Skip Briefing", "gSkipBriefing", {
            .tooltip = "Allows you to skip the briefing sequence in level select"
        });

        UIWidgets::CVarCheckbox("Enable Expert Mode", "gForceExpertMode", {
            .tooltip = "Allows you to force expert mode"
        });

        UIWidgets::CVarCheckbox("SFX Jukebox", "gSfxJukebox", {
            .tooltip = "Press L in the Expert Sound options to play sound effects from the game"
        });

        UIWidgets::CVarCheckbox("Disable Starfield interpolation", "gDisableStarsInterpolation", {
            .tooltip = "Disable starfield interpolation to increase performance on slower CPUs"
        });
        UIWidgets::CVarCheckbox("Disable Gamma Boost (Needs reload)", "gGraphics.GammaMode", {
            .tooltip = "Disables the game's Built-in Gamma Boost. Useful for modders",
            .defaultValue = false
        });

        UIWidgets::CVarCheckbox("Spawner Mod", "gSpawnerMod", {
            .tooltip = "Spawn Scenery, Actors, Bosses, Sprites, Items, Effects and even Event Actors.\n"
                       "\n"
                       "Controls:\n"
                       "D-Pad left and right to set the object Id.\n"
                       "C-Right to change between spawn modes.\n"
                       "Analog stick sets the spawn position.\n"
                       "L-Trigger to spawn the object.\n"
                       "D-Pad UP to kill all objects.\n"
                       "D-Pad DOWN to freeze/unfreeze the ship speed.\n"
                       "WARNING: Spawning an object that's not loaded in memory will likely result in a crash."
        });

        UIWidgets::CVarCheckbox("Jump To Map", "gDebugJumpToMap", {
            .tooltip = "Press Z + R + C-UP to get back to the map"
        });

        UIWidgets::CVarCheckbox("L To Warp Zone", "gDebugWarpZone", {
            .tooltip = "Press L to get into the Warp Zone"
        });

        UIWidgets::CVarCheckbox("L to Level Complete", "gDebugLevelComplete", {
            .tooltip = "Press L to Level Complete"
        });

        UIWidgets::CVarCheckbox("L to All-Range mode", "gDebugJumpToAllRange", {
            .tooltip = "Press L to switch to All-Range mode"
        });

        UIWidgets::CVarCheckbox("Disable Collision", "gDebugNoCollision", {
            .tooltip = "Disable vehicle collision"
        });
        
        UIWidgets::CVarCheckbox("Speed Control", "gDebugSpeedControl", {
            .tooltip = "Arwing speed control. Use D-PAD Left and Right to Increase/Decrease the Arwing Speed, D-PAD Down to stop movement."
        });

        UIWidgets::CVarCheckbox("Debug Ending", "gDebugEnding", {
            .tooltip = "Jump to credits at the main menu"
        });

        UIWidgets::CVarCheckbox("Debug Pause", "gLToDebugPause", {
            .tooltip = "Press L to toggle Debug Pause"
        });
        if (CVarGetInteger("gLToDebugPause", 0)) {
            ImGui::Dummy(ImVec2(22.0f, 0.0f));
            ImGui::SameLine();
            UIWidgets::CVarCheckbox("Frame Advance", "gLToFrameAdvance", {
            .tooltip = "Pressing L again advances one frame instead"
        });
        }

        // "Level%d", not a bare "%d": a numeric key turns the nested config JSON into an
        // array, which libultraship's CVar loader silently drops on load — the old keys
        // lost every saved checkpoint across restarts.
        if (CVarGetInteger(StringHelper::Sprintf("gCheckpoint.Level%d.Set", gCurrentLevel).c_str(), 0)) {
            if (UIWidgets::Button("Clear Checkpoint")) {
                CVarClear(StringHelper::Sprintf("gCheckpoint.Level%d.Set", gCurrentLevel).c_str());
                Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
            }
        } else if (gPlayer != NULL && gGameState == GSTATE_PLAY) {
            if (UIWidgets::Button("Set Checkpoint")) {
                CVarSetInteger(StringHelper::Sprintf("gCheckpoint.Level%d.Set", gCurrentLevel).c_str(), 1);
                CVarSetInteger(StringHelper::Sprintf("gCheckpoint.Level%d.gSavedGroundSurface", gCurrentLevel).c_str(), gGroundSurface);
                CVarSetFloat(StringHelper::Sprintf("gCheckpoint.Level%d.gSavedPathProgress", gCurrentLevel).c_str(), (-gPlayer->pos.z) - 250.0f);
                CVarSetInteger(StringHelper::Sprintf("gCheckpoint.Level%d.gSavedObjectLoadIndex", gCurrentLevel).c_str(), gObjectLoadIndex);
                Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
            }
        }

        UIWidgets::Spacer(0);

        UIWidgets::WindowButton("Stats", "gStatsEnabled", GameUI::mStatsWindow, {
            .tooltip = "Shows the stats window, with your FPS and frametimes, and the OS you're playing on"
        });
        UIWidgets::WindowButton("Console", "gConsoleEnabled", GameUI::mConsoleWindow, {
            .tooltip = "Enables the console window, allowing you to input commands, type help for some examples"
        });

        UIWidgets::Spacer(0);

        if (UIWidgets::BeginMenu("Blind Starship")) {
            UIWidgets::CVarCheckbox("Object spawn log", "gObjectSpawnLog", {
                .tooltip = "Logs one trace line per object spawn (used to tune Minimal training). Also requires Log Level = trace.",
                .defaultValue = false
            });
            UIWidgets::CVarCheckbox("Enemy audio cue logging", kEnemyCueLogCVar, {
                .tooltip = "Verbose per-frame trace of enemy-cue targeting and backend state.",
                .defaultValue = false
            });
            if (UIWidgets::BeginMenu("Cue3D test bench")) {
                // Live bench for the 3D-cue capabilities (CueBench.{h,cpp}). Every control is
                // a CVar the bench listener re-reads each tick, so all of it takes effect with
                // no restart. Order follows the plan's D4 control table.
                UIWidgets::CVarCheckbox("Test bench active", kCueBenchActiveCVar, {
                    .tooltip = "Master gate. On plays a source through the 3D audio backend; off silences everything the bench owns immediately.",
                    .defaultValue = false,
                });
                UIWidgets::CVarCheckbox("Synthesized tone", kCueBenchSynthToneCVar, {
                    .tooltip = "Switch the continuous source between the enemy WAV and a generated sine blip (exercises Cue3D_LoadPcm).",
                    .defaultValue = false,
                });
                UIWidgets::CVarCheckbox("Orbit", kCueBenchOrbitCVar, {
                    .tooltip = "On: the source sweeps a 4 s horizontal circle (front -> right -> behind -> left). Off: fixed straight ahead.",
                    .defaultValue = true,
                });
                UIWidgets::CVarCombobox("Render mode", kCueBenchModeCVar, cueBenchModes, {
                    .tooltip = "HRTF (binaural + rear effect), Pan (constant-power stereo, front/back collapses), or Direct (centered, no spatialization).",
                    .defaultIndex = 0,
                });
                UIWidgets::CVarSliderFloat("Pitch", kCueBenchPitchCVar, 0.5f, 2.0f, 1.0f, {
                    .tooltip = "Playback-rate multiplier (1.0 = native, 2.0 = one octave up).",
                    .flags = ImGuiSliderFlags_AlwaysClamp,
                    .format = "%.2fx",
                    .step = 0.05f,
                });
                UIWidgets::CVarSliderFloat("Interval", kCueBenchIntervalCVar, 0.0f, 2.0f, 0.0f, {
                    .tooltip = "Restart cadence for the looping source. 0 = seamless loop. Sounds longer than the "
                               "interval are cut at the restart and may click - author blips shorter than the fastest interval.",
                    .flags = ImGuiSliderFlags_AlwaysClamp,
                    .format = "%.2f s",
                    .step = 0.05f,
                });
                UIWidgets::CVarSliderFloat("Low-pass", kCueBenchLowPassCVar, 0.0f, 8000.0f, 0.0f, {
                    .tooltip = "Per-source muffle cutoff, applied in every render mode. 0 = off.",
                    .flags = ImGuiSliderFlags_AlwaysClamp,
                    .format = "%.0f Hz",
                    .step = 250.0f,
                });
                if (UIWidgets::Button("Play one-shot", { .tooltip = "Fire the hidden one-shot cue once at the bench's current position (exercises PlayOnce + the gen-counter re-trigger fix)." })) {
                    CueBench_RequestOneShot();
                }
                UIWidgets::CVarCheckbox("Rapid re-trigger", kCueBenchRapidRetriggerCVar, {
                    .tooltip = "Fire the one-shot every few ticks. Stress for the re-trigger race fix and the start ramps: listen for swallowed plays or clicks.",
                    .defaultValue = false,
                });
                UIWidgets::CVarCheckbox("Start/stop stress", kCueBenchStartStopStressCVar, {
                    .tooltip = "Toggle the continuous source Play/Stop every few ticks. Stress for the ramps: listen for clicks.",
                    .defaultValue = false,
                });
                ImGui::EndMenu();
            }
            if (UIWidgets::BeginMenu("Aim guide")) {
                // Aim-cue tuning (docs/accessibility-cues-tuning.md). Every knob is a CVar the
                // aim listener re-reads each tick, so all of it is live. AlwaysClamp on all:
                // the enter-to-type path accepts out-of-range values, and several of these
                // feed divisions or the interval math.
                UIWidgets::CVarSliderFloat("Projection distance", kAimCueProjDistCVar, 400.0f, 3000.0f,
                                           kAimCueProjDistDefault, {
                    .tooltip = "How far ahead (world units) the stick deflection is projected on rails before "
                               "comparing the aim point to the corridor center.",
                    .flags = ImGuiSliderFlags_AlwaysClamp,
                    .format = "%.0f",
                    .step = 100.0f,
                });
                UIWidgets::CVarSliderFloat("All-range pan range", kAimCueYawRangeCVar, 20.0f, 90.0f,
                                           kAimCueYawRangeDefault, {
                    .tooltip = "Steering deflection that pans the click fully to one side in all-range mode.",
                    .flags = ImGuiSliderFlags_AlwaysClamp,
                    .format = "%.0f deg",
                    .step = 5.0f,
                });
                UIWidgets::CVarSliderFloat("All-range pitch range", kAimCuePitchRangeDegCVar, 30.0f, 90.0f,
                                           kAimCuePitchRangeDegDefault, {
                    .tooltip = "Aim elevation that bends the click pitch fully up/down in all-range mode.",
                    .flags = ImGuiSliderFlags_AlwaysClamp,
                    .format = "%.0f deg",
                    .step = 5.0f,
                });
                UIWidgets::CVarSliderFloat("Aim pitch range", kAimCueOctavesCVar, 0.0f, 2.0f,
                                           kAimCueOctavesDefault, {
                    .tooltip = "Octaves the click bends up/down at the vertical aim extremes.",
                    .flags = ImGuiSliderFlags_AlwaysClamp,
                    .format = "%.2f oct",
                    .step = 0.25f,
                });
                UIWidgets::CVarSliderFloat("Geiger angle", kAimCueGeigerAngleCVar, 5.0f, 90.0f,
                                           kAimCueGeigerAngleDefault, {
                    .tooltip = "Aim-to-enemy angle where the click rate starts rising: slowest at or beyond "
                               "this, fastest dead on target.",
                    .flags = ImGuiSliderFlags_AlwaysClamp,
                    .format = "%.0f deg",
                    .step = 5.0f,
                });
                // Floor at the click buffer's ~32 ms length (AimCue_GenerateClick,
                // lead-in + tick): a shorter interval would truncate each click at the restart,
                // and pitched-down clicks stretch further still.
                UIWidgets::CVarSliderFloat("Geiger fast interval", kAimCueGeigerFastCVar, 0.04f, 0.3f,
                                           kAimCueGeigerFastDefault, {
                    .tooltip = "Click repeat interval when aiming dead on a lockable enemy.",
                    .flags = ImGuiSliderFlags_AlwaysClamp,
                    .format = "%.2f s",
                    .step = 0.01f,
                });
                UIWidgets::CVarSliderFloat("Geiger slow interval", kAimCueGeigerSlowCVar, 0.2f, 2.0f,
                                           kAimCueGeigerSlowDefault, {
                    .tooltip = "Click repeat interval with no lockable enemy near the aim.",
                    .flags = ImGuiSliderFlags_AlwaysClamp,
                    .format = "%.2f s",
                    .step = 0.05f,
                });
                UIWidgets::CVarSliderFloat("Click loudness", kAimCueBoostCVar, 0.5f, 4.0f, kAimCueBoostDefault, {
                    .tooltip = "Loudness boost for the aim click relative to the other cues - a short click "
                               "reads quieter than a sustained loop at the same level. Applied under the "
                               "volume sliders.",
                    .flags = ImGuiSliderFlags_AlwaysClamp,
                    .format = "%.2fx",
                    .step = 0.25f,
                });
                ImGui::EndMenu();
            }
            if (UIWidgets::BeginMenu("Obstacle warning")) {
                // Obstacle-cue tuning (docs/accessibility-cues-tuning.md). Every knob is a
                // CVar the listener re-reads each tick, so all of it is live. AlwaysClamp on
                // all: the enter-to-type path accepts out-of-range values, and several of
                // these feed the interval math or a division.
                UIWidgets::CVarSliderFloat("Warning distance", kObstacleCueWarnDistCVar, 500.0f, kObstacleCueWarnDistMax,
                                           kObstacleCueWarnDistDefault, {
                    .tooltip = "How far ahead (world units) an on-course obstacle starts buzzing. Obstacles "
                               "stream in 3000 or more units out depending on the level, so above 3000 some "
                               "obstacles start buzzing mid-ramp the moment they appear.",
                    .flags = ImGuiSliderFlags_AlwaysClamp,
                    .format = "%.0f",
                    .step = 100.0f,
                });
                UIWidgets::CVarSliderFloat("Slow interval", kObstacleCueSlowCVar, 0.2f, 1.5f,
                                           kObstacleCueSlowDefault, {
                    .tooltip = "Buzz repeat interval at the warning distance.",
                    .flags = ImGuiSliderFlags_AlwaysClamp,
                    .format = "%.2f s",
                    .step = 0.05f,
                });
                // Floor at kObstacleCueMinIntervalSec, the buzz buffer's length budget
                // (ObstacleAheadCue_GenerateBuzz): a shorter interval would truncate each
                // pulse at the restart.
                UIWidgets::CVarSliderFloat("Fast interval", kObstacleCueFastCVar, kObstacleCueMinIntervalSec, 0.4f,
                                           kObstacleCueFastDefault, {
                    .tooltip = "Buzz repeat interval at the moment of contact.",
                    .flags = ImGuiSliderFlags_AlwaysClamp,
                    .format = "%.2f s",
                    .step = 0.01f,
                });
                UIWidgets::CVarSliderFloat("Safety margin", kObstacleCueMarginCVar, 0.0f, 500.0f,
                                           kObstacleCueMarginDefault, {
                    .tooltip = "How far outside an obstacle's width and height still counts as a collision "
                               "course. Higher warns about near misses; lower only warns about direct hits.",
                    .flags = ImGuiSliderFlags_AlwaysClamp,
                    .format = "%.0f",
                    .step = 25.0f,
                });
                UIWidgets::CVarSliderFloat("Buzz loudness", kObstacleCueBoostCVar, 0.5f, 4.0f,
                                           kObstacleCueBoostDefault, {
                    .tooltip = "Loudness boost for the buzz relative to the other cues, applied under the "
                               "volume sliders.",
                    .flags = ImGuiSliderFlags_AlwaysClamp,
                    .format = "%.2fx",
                    .step = 0.25f,
                });
                ImGui::EndMenu();
            }
            if (UIWidgets::BeginMenu("Obstacle direction")) {
                // The directional obstacle cues' knobs (ObstacleDirectionCue.h,
                // docs/accessibility-cues-tuning.md). Live CVars like the buzz's; the
                // safety margin they share with the buzz is the slider above.
                UIWidgets::CVarSliderFloat("Lookahead", kObstacleDirLookaheadCVar, 200.0f, kObstacleDirLookaheadMax,
                                           kObstacleDirLookaheadDefault, {
                    .tooltip = "How far ahead (world units) an obstacle beside, above or below your course "
                               "starts sounding. About 1200 is one second of flight. Obstacles already "
                               "alongside you sound regardless.",
                    .flags = ImGuiSliderFlags_AlwaysClamp,
                    .format = "%.0f",
                    .step = 100.0f,
                });
                UIWidgets::CVarSliderFloat("Side distance", kObstacleSideDistCVar, 200.0f, 2500.0f,
                                           kObstacleSideDistDefault, {
                    .tooltip = "How far to the side (world units, beyond the safety margin) an obstacle "
                               "still sounds. At this distance the chord is panned fully to its side; at "
                               "the margin it sits near the center.",
                    .flags = ImGuiSliderFlags_AlwaysClamp,
                    .format = "%.0f",
                    .step = 50.0f,
                });
                UIWidgets::CVarSliderFloat("Side pan floor", kObstacleSidePanFloorCVar, 0.0f, 0.8f,
                                           kObstacleSidePanFloorDefault, {
                    .tooltip = "How far off center the beside chord stays for the closest possible "
                               "obstacle, so left and right never merge. 0 = dead center, 1 = hard.",
                    .flags = ImGuiSliderFlags_AlwaysClamp,
                    .format = "%.2f",
                    .step = 0.05f,
                });
                UIWidgets::CVarSliderFloat("Vertical distance", kObstacleVertDistCVar, 200.0f, 2500.0f,
                                           kObstacleVertDistDefault, {
                    .tooltip = "How far above or below (world units, beyond the safety margin) an obstacle "
                               "still sounds. Loudness runs from full at the margin to the level floor here.",
                    .flags = ImGuiSliderFlags_AlwaysClamp,
                    .format = "%.0f",
                    .step = 50.0f,
                });
                UIWidgets::CVarSliderFloat("Vertical level floor", kObstacleVertLevelFloorCVar, 0.0f, 0.8f,
                                           kObstacleVertLevelFloorDefault, {
                    .tooltip = "Loudness (fraction of the volume slider) of the above/below chords at the "
                               "vertical distance, so their onset is audible rather than a fade from silence.",
                    .flags = ImGuiSliderFlags_AlwaysClamp,
                    .format = "%.2f",
                    .step = 0.05f,
                });
                UIWidgets::CVarSliderFloat("Chord loudness", kObstacleDirBoostCVar, 0.25f, 4.0f,
                                           kObstacleDirBoostDefault, {
                    .tooltip = "Loudness boost shared by the three direction chords relative to the other "
                               "cues, applied under the volume sliders.",
                    .flags = ImGuiSliderFlags_AlwaysClamp,
                    .format = "%.2fx",
                    .step = 0.25f,
                });
                ImGui::EndMenu();
            }
            // Height->pitch tuning: how the cue bends pitch by target height to
            // compensate for weak HRTF elevation (docs/accessibility-cues-tuning.md).
            // Read live by the cue mapping each tick, so no push-to-backend is needed.
            UIWidgets::CVarCheckbox("Height-to-pitch cue", kCuePitchForHeightCVar, {
                .tooltip = "Bend cue pitch by target height (higher = higher pitch), compensating "
                           "for weak HRTF elevation. Off = native pitch, height not conveyed.",
                .defaultValue = true,
            });
            bool pitchOn = CVarGetInteger(kCuePitchForHeightCVar, 1) == 1;
            UIWidgets::CVarSliderFloat("Pitch range", kCuePitchRangeOctavesCVar, 0.0f, 2.0f,
                                       kCuePitchRangeOctavesDefault, {
                .tooltip = "Maximum octaves the cue pitch bends up/down at the height extremes.",
                .disabled = !pitchOn,
                .format = "%.2f oct",
                .step = 0.25f,
            });
            UIWidgets::CVarSliderFloat("Pitch height sensitivity", kCuePitchScaleCVar, 250.0f, 4000.0f,
                                       kCuePitchScaleDefault, {
                .tooltip = "World height per octave of pitch bend; lower = pitch reacts to smaller "
                           "height changes (reaches full range sooner).",
                .disabled = !pitchOn,
                .format = "%.0f/oct",
                .step = 250.0f,
            });
            // HOW pitch is realized (Cue3D_SetPitchStyle) — deliberately NOT disabled with
            // the height-to-pitch toggle: the bench's pitch slider goes through it too.
            UIWidgets::CVarCheckbox("Spectral pitch shifter", kCuePitchShiftCVar, {
                .tooltip = "Bend cue pitch with a spectral shifter, keeping each sound's length and "
                           "character (off = classic playback-rate change: higher also means faster "
                           "and thinner). Costs about a tenth of a second of cue-sound latency.",
                .defaultValue = kCuePitchShiftDefault != 0,
            });
            // Rear-effect tuning: how strongly the cue backend exaggerates "behind you"
            // (docs/accessibility-cues-tuning.md). Re-read and pushed to the audio thread
            // every tick by CueRegistry_Tick, so the knobs can be A/B'd by ear against the
            // spatial test's orbiting tone or a live in-level cue.
            // AlwaysClamp on all four: the enter-to-type path (which the screen-reader
            // session advertises) otherwise accepts values outside the slider range, and a
            // cutoff <= 0 would destabilize the backend's one-pole filter. The seam clamps
            // too — this just keeps the UI from ever offering a bad value.
            UIWidgets::CVarSliderFloat("Rear muffle cutoff", kCueRearCutoffCVar, 250.0f, 8000.0f,
                                       CUE3D_REAR_CUTOFF_HZ_DEFAULT, {
                .tooltip = "Low-pass cutoff for cue sounds behind you; lower = more muffled dead behind.",
                .flags = ImGuiSliderFlags_AlwaysClamp,
                .format = "%.0f Hz",
                .step = 250.0f,
            });
            UIWidgets::CVarSliderFloat("Rear volume dip", kCueRearGainDipCVar, 0.0f, 1.0f,
                                       CUE3D_REAR_GAIN_DIP_DEFAULT, {
                .tooltip = "How much quieter a cue dead behind you is. Keep mild: volume also encodes distance.",
                .flags = ImGuiSliderFlags_AlwaysClamp,
                .format = "%.0f%%",
                .step = 0.05f,
                .isPercentage = true,
            });
            UIWidgets::CVarSliderFloat("Rear tremolo depth", kCueRearTremoloDepthCVar, 0.0f, 1.0f,
                                       CUE3D_REAR_TREMOLO_DEPTH_DEFAULT, {
                .tooltip = "Pulsing strength for cue sounds behind you; 0 disables the tremolo.",
                .flags = ImGuiSliderFlags_AlwaysClamp,
                .format = "%.0f%%",
                .step = 0.05f,
                .isPercentage = true,
            });
            UIWidgets::CVarSliderFloat("Rear tremolo rate", kCueRearTremoloHzCVar, 2.0f, 16.0f,
                                       CUE3D_REAR_TREMOLO_HZ_DEFAULT, {
                .tooltip = "How fast the rear tremolo pulses.",
                .flags = ImGuiSliderFlags_AlwaysClamp,
                .format = "%.1f Hz",
                .step = 0.5f,
            });
            ImGui::EndMenu();
        }

        ImGui::EndMenu();
    }
}

void GameMenuBar::DrawElement() {
    if(ImGui::BeginMenuBar()){
        AccessibilityImGuiMenu_OnMenuBarDraw();

        DrawMenuBarIcon();

        DrawGameMenu();

        ImGui::SetCursorPosY(0.0f);

        DrawSettingsMenu();

        ImGui::SetCursorPosY(0.0f);

        DrawEnhancementsMenu();

        ImGui::SetCursorPosY(0.0f);

        DrawCheatsMenu();

        ImGui::SetCursorPosY(0.0f);

        ImGui::SetCursorPosY(0.0f);

        DrawDebugMenu();

        ImGui::EndMenuBar();
    }
}
