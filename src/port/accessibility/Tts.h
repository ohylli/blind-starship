#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void Tts_Init(void);
void Tts_Shutdown(void);
void Tts_Speak(const char* text, bool interrupt);
bool Tts_IsAvailable(void);

#ifdef __cplusplus
}
#endif
