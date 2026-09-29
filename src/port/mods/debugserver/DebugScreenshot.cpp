// `screenshot` — reads a freshly drawn game frame back from the renderer, so a client (an
// AI agent or a script) can see the running game even though tools/launch.ps1 keeps the
// window minimized.
//
// Where the pixels come from: the renderer only has a readable copy of the game frame when
// it draws into an offscreen render target, and the size it draws at follows the window's
// game area. Minimized, that area is ImGui's 32x32 minimum and the game draws straight
// into the (zero-sized) window, leaving nothing to read. N64 mode (the gLowResMode CVar)
// fixes both at once: the game always draws a native 320x240 frame offscreen, whatever
// the window is doing. So a capture switches N64 mode on for a couple of ticks when it is
// not already on, reads the frame with the backend's own ReadFramebufferToCPU, and puts
// the CVar back exactly as it was (removed again if it did not exist, so no config entry
// is left behind). The picture is therefore always 320x240 at 4:3, and a visible window
// flickers to the low-res look for those ticks — fine for a dev tool.
//
// Timing: the main loop runs game update + draw (Graphics_ThreadUpdate), then
// GameEngine::StartFrame, where DebugScreenshot_FrameTick runs before the transport's
// DebugServer_FrameTick. The handler arms a capture from inside DebugServer_FrameTick;
// each later tick in which the renderer actually drew counts toward kSettleDrawTicks, and
// the capture reads the render target right after such a draw, when it still holds that
// tick's final (non-interpolated) frame. Two drawn ticks when N64 mode had to be switched
// on: the first draw switches the frame size, but that tick's display list was built
// against the old aspect ratio (HUD edge anchoring); the second is built and drawn at
// 320x240. While debug-paused the game still redraws the frozen frame every tick, so a
// paused capture shows exactly the paused moment.
//
// As with `step` and `warp`, this game-thread tick enforces and the deferred poll only
// observes: if the client vanishes or the server stops mid-capture, the tick still
// finishes and restores the CVar, and DebugScreenshot_Exit restores it if the game quits
// first, before the exit-time config save.
//
// Readback format: ReadFramebufferToCPU returns RGBA5551 (the N64's own color depth);
// channels are expanded to 8 bits and sent as base64 RGB, rows top first. Verified on the
// DirectX 11 backend; the OpenGL and Metal row order is untested.

#include "DebugScreenshot.h"
#include "DebugServer.h"

#include "port/CGameCompat.h"
#include "port/Engine.h"

#include <graphic/Fast3D/interpreter.h>
#include <nlohmann/json.hpp>
#include <exception>
#include <memory>
#include <string>
#include <vector>

static constexpr const char* kLowResCVar = "gLowResMode";
static constexpr int kLowResN64 = 1;
// Drawn ticks to wait before reading: see the timing note above.
static constexpr int kSettleDrawTicksForced = 2;
static constexpr int kSettleDrawTicksAlreadyOn = 1;
// Give up if the renderer draws nothing for this long (~3 s): a capture must never leave
// N64 mode switched on indefinitely.
static constexpr int kTimeoutTicks = 90;

struct ShotState {
    // Arming
    bool forcedLowRes = false; // gLowResMode was switched on by this capture
    bool hadCVar = false;      // ... and whether it existed beforehand
    int32_t oldLowRes = 0;
    int settleDrawTicks = 0;
    uint64_t lastDrawn = 0;
    int ticks = 0;
    int drawTicks = 0;
    // Result
    bool done = false;
    bool captured = false;
    std::string note;
    uint32_t width = 0;
    uint32_t height = 0;
    int32_t frame = 0;
    bool paused = false;
    std::string rgbBase64;
};
static std::shared_ptr<ShotState> sShot; // game thread only; non-null while a capture is in flight

static std::string Base64(const std::vector<uint8_t>& data) {
    static const char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += kTable[(v >> 18) & 63];
        out += kTable[(v >> 12) & 63];
        out += kTable[(v >> 6) & 63];
        out += kTable[v & 63];
    }
    if (i < data.size()) {
        bool two = i + 1 < data.size();
        uint32_t v = (data[i] << 16) | (two ? data[i + 1] << 8 : 0);
        out += kTable[(v >> 18) & 63];
        out += kTable[(v >> 12) & 63];
        out += two ? kTable[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

// The render target holding the last drawn game frame, or -1 (with the reason) when the
// frame went straight to the window. With MSAA the multisampled target cannot be copied
// to a CPU texture; the renderer's resolved copy can.
static int ReadableFramebuffer(Fast::Interpreter* interp, const char** why) {
    if (!interp->mRendersToFb) {
        *why = "the game frame was drawn straight to the window (its game area matches the render size exactly)";
        return -1;
    }
    if (interp->mMsaaLevel > 1) {
        if (interp->ViewportMatchesRendererResolution()) {
            *why = "with MSAA on, the game frame was resolved straight to the window";
            return -1;
        }
        return interp->mGameFbMsaaResolved;
    }
    return interp->mGameFb;
}

static void Capture(ShotState& shot) {
    Fast::Interpreter* interp = GameEngine_GetInterpreter();
    if (interp == nullptr) {
        shot.note = "no renderer";
        return;
    }
    const char* why = nullptr;
    int fb = ReadableFramebuffer(interp, &why);
    if (fb < 0) {
        shot.note = why;
        return;
    }
    uint32_t w = interp->mCurDimensions.width;
    uint32_t h = interp->mCurDimensions.height;
    // The DX11 readback copies each row at the driver's padded row pitch into a tightly
    // packed buffer, overrunning it unless rows need no padding; 64-pixel multiples are
    // safe (and N64 mode's 320 is one).
    if (w == 0 || h == 0 || w % 64 != 0) {
        shot.note = "render size " + std::to_string(w) + "x" + std::to_string(h) +
                    " cannot be read back safely (width must be a multiple of 64)";
        return;
    }
    std::vector<uint16_t> rgba16(w * h);
    try {
        interp->mRapi->ReadFramebufferToCPU(fb, w, h, rgba16.data());
    } catch (const std::exception& e) {
        shot.note = std::string("framebuffer readback failed: ") + e.what();
        return;
    }
    std::vector<uint8_t> rgb(w * h * 3);
    for (size_t i = 0; i < rgba16.size(); i++) {
        uint16_t p = rgba16[i];
        rgb[i * 3 + 0] = ((p >> 11) & 31) * 255 / 31;
        rgb[i * 3 + 1] = ((p >> 6) & 31) * 255 / 31;
        rgb[i * 3 + 2] = ((p >> 1) & 31) * 255 / 31;
    }
    shot.width = w;
    shot.height = h;
    shot.frame = gGameFrameCount;
    shot.paused = CVarGetInteger("gDebugPause", 0) != 0;
    shot.rgbBase64 = Base64(rgb);
    shot.captured = true;
}

static void RestoreLowRes(ShotState& shot) {
    if (!shot.forcedLowRes) {
        return;
    }
    shot.forcedLowRes = false;
    if (shot.hadCVar) {
        CVarSetInteger(kLowResCVar, shot.oldLowRes);
    } else {
        CVarClear(kLowResCVar);
    }
}

static void Finish(ShotState& shot) {
    RestoreLowRes(shot);
    shot.done = true;
    sShot = nullptr; // the deferred poll holds its own reference
}

void DebugScreenshot_FrameTick() {
    if (sShot == nullptr) {
        return;
    }
    ShotState& shot = *sShot;
    shot.ticks++;
    uint64_t drawn = GameEngine_DrawnFrameCount();
    if (drawn != shot.lastDrawn) {
        shot.lastDrawn = drawn;
        shot.drawTicks++;
    }
    if (shot.drawTicks >= shot.settleDrawTicks) {
        Capture(shot);
        Finish(shot);
    } else if (shot.ticks >= kTimeoutTicks) {
        shot.note = "the renderer drew no frame in time";
        Finish(shot);
    }
}

void DebugScreenshot_Exit() {
    if (sShot != nullptr) {
        sShot->note = "the game is shutting down";
        Finish(*sShot);
    }
}

static int32_t ScreenshotHandler(std::shared_ptr<Ship::Console> console, const std::vector<std::string>& args,
                                 std::string* output) {
    if (sShot != nullptr) {
        if (output != nullptr) {
            *output += "a screenshot is already in progress";
        }
        return 1;
    }
    auto shot = std::make_shared<ShotState>();
    bool deferred = DebugServer_Defer([shot](std::string* out) {
        if (!shot->done) {
            return false;
        }
        if (out != nullptr) {
            nlohmann::json j;
            j["captured"] = shot->captured;
            if (shot->captured) {
                j["width"] = shot->width;
                j["height"] = shot->height;
                j["frame"] = shot->frame;
                j["paused"] = shot->paused;
                j["format"] = "rgb8-base64";
                j["rgb"] = shot->rgbBase64;
            } else {
                j["note"] = shot->note;
            }
            *out += j.dump();
        }
        return true;
    });
    if (!deferred) {
        if (output != nullptr) {
            *output += "screenshot only works through the debug server socket "
                       "(python tools/debug_client.py screenshot <file.png>)";
        }
        return 1;
    }
    shot->hadCVar = CVarGet(kLowResCVar) != nullptr; // CVarExists is declared but never defined
    shot->oldLowRes = CVarGetInteger(kLowResCVar, 0);
    if (shot->oldLowRes != kLowResN64) {
        shot->forcedLowRes = true;
        CVarSetInteger(kLowResCVar, kLowResN64);
    }
    shot->settleDrawTicks = shot->forcedLowRes ? kSettleDrawTicksForced : kSettleDrawTicksAlreadyOn;
    shot->lastDrawn = GameEngine_DrawnFrameCount();
    sShot = shot;
    return 0;
}

void DebugScreenshot_Register() {
    Ship::Context::GetInstance()->GetConsole()->AddCommand(
        "screenshot", { ScreenshotHandler,
                        "Capture a freshly drawn 320x240 game frame (switches N64 mode on for a moment). "
                        "Debug server socket only: python tools/debug_client.py screenshot <file.png>.",
                        {} });
}
