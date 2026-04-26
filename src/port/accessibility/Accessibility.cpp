#include "Accessibility.h"

#ifdef HAVE_PRISM

#include <prism.h>
#include <spdlog/spdlog.h>

namespace {
    PrismContext* g_ctx = nullptr;
    PrismBackend* g_backend = nullptr;
}

extern "C" void Accessibility_Init(void) {
    if (g_ctx != nullptr) {
        return;
    }

    PrismConfig cfg = prism_config_init();
    g_ctx = prism_init(&cfg);
    if (g_ctx == nullptr) {
        SPDLOG_ERROR("Accessibility: prism_init returned NULL; TTS disabled");
        return;
    }

    g_backend = prism_registry_acquire_best(g_ctx);
    if (g_backend == nullptr) {
        SPDLOG_WARN("Accessibility: no PRISM backend available; TTS disabled");
        return;
    }

    SPDLOG_INFO("Accessibility: PRISM backend '{}' ready", prism_backend_name(g_backend));
}

extern "C" void Accessibility_Speak(const char* text, bool interrupt) {
    if (g_backend == nullptr || text == nullptr) {
        return;
    }
    PrismError err = prism_backend_speak(g_backend, text, interrupt);
    if (err != 0) {
        SPDLOG_WARN("Accessibility: prism_backend_speak failed ({})", prism_error_string(err));
    }
}

extern "C" void Accessibility_Shutdown(void) {
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

extern "C" void Accessibility_Init(void) {}
extern "C" void Accessibility_Speak(const char* text, bool interrupt) {
    (void) text;
    (void) interrupt;
}
extern "C" void Accessibility_Shutdown(void) {}

#endif  // HAVE_PRISM
