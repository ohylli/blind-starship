#include "AimCue.h"
#include "CueCommon.h"
#include "CueScan.h"

#include <math.h>
#include <vector>

#include "port/CGameCompat.h"
#include "port/PlayerAim.h"
#include "port/hooks/Events.h"
#include "port/accessibility/Cue.h"
#include "port/mods/Accessibility.h"

// The aim cue: a repeated synthesized click whose stereo pan and pitch tell the player
// where they are AIMING — unlike the ring/enemy cues it encodes the aim itself, not a
// target. Rendered in CUE3D_MODE_PAN (plain stereo, no HRTF) at the backend's unity-gain
// distance, so distance attenuation never varies and pan/pitch are pure functions of the
// aim. On rails the encoded quantity is the projected aim point relative to the corridor
// center — lateral drift plus the stick deflection projected kAimCueProjDistCVar ahead,
// normalized by the corridor half-extents (pathWidth/pathHeight, the same box the engine
// clamps flight to) — so with a neutral stick the cue doubles as a "where am I on screen"
// indicator. In all-range there is no corridor: pan encodes the stick's yaw deflection
// (turning) and pitch the aim's world elevation angle. On top of both, the click repeats
// faster as the aim line passes closer to a lockable enemy (geiger-counter style),
// reusing the enemy cue's predicate and scoping (CueScan.h).
static Cue* sAimCue = nullptr;

static AimCueDebug sDebugState;

const AimCueDebug& AimCue_DebugState() {
    return sDebugState;
}

// Synthesized click: ~12 ms of silence, then a ~20 ms damped-sine tick, via the shared
// generator (CueCommon_GenerateDampedTone, which owns the lead-in and tail rationale).
// Aim-specific notes: the RESAMPLE pitch path scales the lead-in with pitch — at +1
// octave it halves to 6 ms, so the extremes give back a little loudness the gain-boost
// knob covers — and the buffer must finish inside the fastest geiger interval; the
// slider floor in ImguiUI.cpp budgets for pitched-DOWN clicks stretching longer, which
// is why there is no compile-time assert here (unlike the fixed-pitch obstacle buzz).
static std::vector<float> AimCue_GenerateClick(int sampleRate) {
    constexpr f32 kLeadInSec = 0.012f;
    constexpr f32 kTickSec = 0.02f;
    constexpr f32 kToneHz = 1500.0f;
    constexpr f32 kDecayPerSec = 150.0f;
    constexpr f32 kAmplitude = 0.95f;
    constexpr f32 kTailSec = 0.003f;
    return CueCommon_GenerateDampedTone(sampleRate, kLeadInSec, kTickSec, kToneHz, 0.0f, kDecayPerSec, kAmplitude,
                                        kTailSec);
}

// Normalized vertical aim [-1, 1] -> playback rate 2^(n * octaves). Deliberately separate
// from CueCommon_ComputeFreqModFromY: that maps a world-space HEIGHT (units per octave)
// for HRTF cues, this maps an already-normalized aim signal, and the aim cue must keep
// conveying the vertical even when the height-to-pitch toggle is off (in PAN mode pitch
// is the only vertical channel there is).
static f32 AimCue_Pitch(f32 n) {
    f32 octaves = CVarGetFloat(kAimCueOctavesCVar, kAimCueOctavesDefault);
    if (!(octaves >= 0.0f)) {
        octaves = kAimCueOctavesDefault; // negative or NaN
    }
    return powf(2.0f, CueCommon_ClampUnit(n) * octaves);
}

// Smallest aim-to-enemy angle -> click repeat interval, linear in angle: at or past the max
// angle — and when no enemy is in scope (angle = INFINITY) — the click idles at the slow
// interval; dead on a target it reaches the fast one. NaN-guarded in the usual
// !(x >= lo) style (see CueCommon_ComputeFreqModFromY for the rationale).
static f32 AimCue_Interval(f32 angleRad) {
    f32 maxAngle = CVarGetFloat(kAimCueGeigerAngleCVar, kAimCueGeigerAngleDefault) * M_DTOR;
    f32 slow = CVarGetFloat(kAimCueGeigerSlowCVar, kAimCueGeigerSlowDefault);
    f32 fast = CVarGetFloat(kAimCueGeigerFastCVar, kAimCueGeigerFastDefault);
    if (!(maxAngle > 0.0f)) {
        maxAngle = kAimCueGeigerAngleDefault * M_DTOR;
    }
    if (!(slow > 0.0f)) {
        slow = kAimCueGeigerSlowDefault;
    }
    if (!(fast > 0.0f)) {
        fast = kAimCueGeigerFastDefault;
    }
    if (!(angleRad < maxAngle)) {
        return slow; // no target (INFINITY), NaN, or wider than the max angle
    }
    f32 t = angleRad / maxAngle;
    if (t < 0.0f) {
        t = 0.0f;
    }
    return fast + (slow - fast) * t;
}

// Smallest angle between the aim line and any in-scope cueable enemy, radians; INFINITY if
// none. Same predicate and scoping as the enemy cue's scan (CueScan.h). Assumes the caller
// just ran CueScan_BuildWorldToBodyMatrix (gCalcMatrix holds world -> body).
static f32 AimCue_MinEnemyAimAngle(Player* player, bool allRange) {
    f32 best = INFINITY;
    CueScan_ForEachCueableEnemy(player, allRange, nullptr,
                                [&](Actor* actor, s32 slot, const Vec3f& bodyDelta, f32 distSq) {
                                    (void) actor;
                                    (void) slot;
                                    (void) distSq;
                                    // 0 on the aim line, growing toward pi dead behind
                                    // (reachable in all-range only).
                                    f32 lateral = sqrtf((bodyDelta.x * bodyDelta.x) + (bodyDelta.y * bodyDelta.y));
                                    f32 angle = atan2f(lateral, -bodyDelta.z);
                                    if (angle < best) {
                                        best = angle;
                                    }
                                });
    return best;
}

static void AimCue_OnPostUpdate(IEvent* event) {
    (void) event;

    // Loudness normalization against the sustained cues (the setter sanitizes and no-ops
    // while stable). Pushed before the gate, not after: the settings-menu preview must
    // honor the boost (and track its slider) even when gameplay is gated off.
    sAimCue->SetGainBoost(CVarGetFloat(kAimCueBoostCVar, kAimCueBoostDefault));

    sDebugState = AimCueDebug{};
    AimCueDebug& dbg = sDebugState;
    dbg.enabled = CueCommon_IsEnabled();
    dbg.aimEnabled = (CVarGetInteger(kAimCueEnabledCVar, 1) == 1);
    dbg.modeOk = CueScan_ModeInScope(&dbg.allRange);
    dbg.control = Accessibility_PlayerHasControl();
    // v1 is Arwing-only: the mappings below read the Arwing's aim fields. Landmaster /
    // Blue-Marine / on-foot need their own mappings (future work). `control` guarantees
    // gPlayer is non-null before the form read.
    dbg.arwing = dbg.control && (gPlayer[0].form == FORM_ARWING);
    dbg.frame = (int32_t) gGameFrameCount;
    if (!dbg.enabled || !dbg.aimEnabled || !dbg.modeOk || !dbg.arwing) {
        sAimCue->Stop();
        return;
    }
    bool allRange = dbg.allRange;
    Player* player = &gPlayer[0];

    // Normalized aim signals, both [-1, 1]: nx -> pan (positive = right), ny -> pitch
    // (positive = up). Sign notes, derived from the engine and worth keeping straight:
    // the stick is NEGATED into rot (fox_play.c:4019/:4064), so rot.y < 0 means "aiming
    // right", and a positive total pitch angle means "aiming up" (Player_SetupArwingShot's
    // matrix maps positive pitch to +Y velocity). Hence the minus on the yaw terms and the
    // plus on the pitch terms below.
    f32 nx;
    f32 ny;
    if (allRange) {
        f32 yawRange = CVarGetFloat(kAimCueYawRangeCVar, kAimCueYawRangeDefault);
        if (!(yawRange >= 1.0f)) {
            yawRange = kAimCueYawRangeDefault; // tiny, negative, or NaN
        }
        f32 pitchRange = CVarGetFloat(kAimCuePitchRangeDegCVar, kAimCuePitchRangeDegDefault);
        if (!(pitchRange >= 1.0f)) {
            pitchRange = kAimCuePitchRangeDegDefault;
        }
        nx = CueCommon_ClampUnit(-player->rot.y / yawRange);
        ny = CueCommon_ClampUnit(Player_AimPitch(*player) / pitchRange);
    } else {
        f32 dist = CVarGetFloat(kAimCueProjDistCVar, kAimCueProjDistDefault);
        if (!(dist >= 0.0f)) {
            dist = kAimCueProjDistDefault; // negative or NaN
        }
        // Projected aim point relative to the corridor center: lateral drift plus the
        // stick deflection carried `dist` units ahead. xRot_120/yRot_114 (the path's own
        // direction) stay out of the deflection — the path forward IS the neutral center.
        f32 horiz = (player->pos.x - player->xPath) - dist * sinf(player->rot.y * M_DTOR);
        f32 vert = (player->pos.y - player->yPath) + dist * sinf((player->rot.x + player->aerobaticPitch) * M_DTOR);
        nx = CueCommon_ClampUnit(horiz / player->pathWidth);
        ny = CueCommon_ClampUnit(vert / player->pathHeight);
    }

    CueScan_BuildWorldToBodyMatrix(player);
    f32 minAngle = AimCue_MinEnemyAimAngle(player, allRange);
    f32 interval = AimCue_Interval(minAngle);

    // Place the source on the unity-gain arc: PAN mode derives its pan purely from the
    // horizontal direction (x over the x/z length), so radius * (nx, 0, sqrt(1 - nx^2))
    // renders a constant-power pan of exactly nx with distance attenuation pinned at
    // unity. Y is irrelevant to PAN; the vertical is carried by pitch instead.
    f32 radius = Cue3D_GetUnityGainDistance();
    if (!(radius > 0.0f)) {
        radius = 100.0f; // backend not up yet; any positive radius pans the same
    }
    CueTarget target;
    target.x = radius * nx;
    target.y = 0.0f;
    target.z = radius * sqrtf(1.0f - nx * nx);
    target.pitch = AimCue_Pitch(ny);
    target.intervalSec = interval;
    sAimCue->SetTarget(target);
    sAimCue->Start();

    dbg.active = true;
    dbg.nx = nx;
    dbg.ny = ny;
    dbg.minEnemyAngleRad = minAngle;
    dbg.intervalSec = interval;
    dbg.pitch = target.pitch;
}

void AimCue_Register() {
    CVarRegisterInteger(kAimCueEnabledCVar, 1);
    CVarRegisterFloat(kAimCueProjDistCVar, kAimCueProjDistDefault);
    CVarRegisterFloat(kAimCueYawRangeCVar, kAimCueYawRangeDefault);
    CVarRegisterFloat(kAimCuePitchRangeDegCVar, kAimCuePitchRangeDegDefault);
    CVarRegisterFloat(kAimCueOctavesCVar, kAimCueOctavesDefault);
    CVarRegisterFloat(kAimCueGeigerAngleCVar, kAimCueGeigerAngleDefault);
    CVarRegisterFloat(kAimCueGeigerFastCVar, kAimCueGeigerFastDefault);
    CVarRegisterFloat(kAimCueGeigerSlowCVar, kAimCueGeigerSlowDefault);
    CVarRegisterFloat(kAimCueBoostCVar, kAimCueBoostDefault);

    // PAN render mode: the pan/pitch ARE the signal, so no HRTF; pinned RESAMPLE pitch: the
    // spectral shifter's latency and transient softening would smear the click's attack.
    sAimCue = CueRegistry_Register(kAimCueId, "Aim guide",
                                   "A repeating click that tells you where you are aiming: pan for "
                                   "left/right, pitch for up/down; it clicks faster as your aim nears "
                                   "a lockable enemy.",
                                   { .generator = AimCue_GenerateClick,
                                     .mode = CUE3D_MODE_PAN,
                                     .pitchStyle = CUE3D_SOURCE_PITCH_RESAMPLE });

    REGISTER_LISTENER(GamePostUpdateEvent, AimCue_OnPostUpdate, EVENT_PRIORITY_NORMAL);
}
