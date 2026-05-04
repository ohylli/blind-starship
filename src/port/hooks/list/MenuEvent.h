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
