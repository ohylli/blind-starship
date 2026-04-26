#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void Accessibility_Init(void);
void Accessibility_Shutdown(void);
void Accessibility_Speak(const char* text, bool interrupt);

#ifdef __cplusplus
}
#endif
