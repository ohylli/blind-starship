#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void Accessibility_Init(void);
void Accessibility_Exit(void);

bool Accessibility_IsScreenReaderEnabled(void);

#ifdef __cplusplus
}
#endif
