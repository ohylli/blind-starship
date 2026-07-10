#pragma once

#include "global.h"
#include "port/hooks/impl/EventSystem.h"

DEFINE_EVENT(TitleSequenceStartEvent);
DEFINE_EVENT(TitleScreenReadyEvent);
DEFINE_EVENT(MainMenuReadyEvent);
DEFINE_EVENT(MainMenuCursorEvent);
DEFINE_EVENT(SoundMenuReadyEvent);
DEFINE_EVENT(SoundMenuCursorEvent);
DEFINE_EVENT(SoundMenuValueChangedEvent);
DEFINE_EVENT(PauseMenuReadyEvent);
DEFINE_EVENT(PauseMenuCursorEvent);
// The developer level selector is active on the Lylat map while gLevelSelector is enabled.
// Payloads mirror the text currently displayed by the selector so consumers do not need to
// depend on its private map state.
DEFINE_EVENT(LevelSelectorReadyEvent, const char* levelName; const char* startOption;);
DEFINE_EVENT(LevelSelectorSelectionChangedEvent, const char* levelName; const char* startOption;);
DEFINE_EVENT(LevelSelectorStartOptionChangedEvent, const char* levelName; const char* startOption;);
