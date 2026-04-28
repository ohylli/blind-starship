#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// All Tts_* functions must be called from the main thread only. The
// PRISM SAPI backend is COM-based and prefers a single apartment; if
// off-thread speech is ever needed, build a queue + worker inside the
// transport rather than locking around these globals.

// One-shot init. If prism_init succeeds but no backend is acquired,
// subsequent Tts_Init calls no-op (g_ctx is held). To retry, call
// Tts_Shutdown first.
void Tts_Init(void);
void Tts_Shutdown(void);
void Tts_Speak(const char* text, bool interrupt);
// Optional: Tts_Speak silently no-ops when TTS is unavailable, so
// callers do not need to pre-check. Exposed for UI gating only (e.g.,
// disabling a menu entry when no backend is present).
bool Tts_IsAvailable(void);

#ifdef __cplusplus
}
#endif
