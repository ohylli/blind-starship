#include "Tts.h"

#ifdef HAVE_PRISM

#include <prism.h>
#include <spdlog/spdlog.h>

namespace {
    PrismContext* g_ctx = nullptr;
    PrismBackend* g_backend = nullptr;
}

extern "C" void Tts_Init(void) {
    if (g_ctx != nullptr) {
        return;
    }

    PrismConfig cfg = prism_config_init();
    g_ctx = prism_init(&cfg);
    if (g_ctx == nullptr) {
        SPDLOG_ERROR("Tts: prism_init returned NULL; TTS disabled");
        return;
    }

    g_backend = prism_registry_acquire_best(g_ctx);
    if (g_backend == nullptr) {
        SPDLOG_WARN("Tts: no PRISM backend available; TTS disabled");
        return;
    }

    SPDLOG_INFO("Tts: PRISM backend '{}' ready", prism_backend_name(g_backend));
}

extern "C" void Tts_Speak(const char* text, bool interrupt) {
    if (g_backend == nullptr || text == nullptr) {
        return;
    }
    PrismError err = prism_backend_output(g_backend, text, interrupt);
    if (err != 0) {
        SPDLOG_WARN("Tts: prism_backend_speak failed ({})", prism_error_string(err));
    }
}

extern "C" bool Tts_IsAvailable(void) {
    return g_backend != nullptr;
}

extern "C" void Tts_Shutdown(void) {
    if (g_backend != nullptr) {
        prism_backend_free(g_backend);
        g_backend = nullptr;
    }
    if (g_ctx != nullptr) {
        prism_shutdown(g_ctx);
        g_ctx = nullptr;
    }
}

#else  // HAVE_PRISM not defined — no-op stubs

extern "C" void Tts_Init(void) {}
extern "C" void Tts_Speak(const char* text, bool interrupt) {
    (void) text;
    (void) interrupt;
}
extern "C" bool Tts_IsAvailable(void) { return false; }
extern "C" void Tts_Shutdown(void) {}

#endif  // HAVE_PRISM
