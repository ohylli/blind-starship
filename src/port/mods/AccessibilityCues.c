#include "AccessibilityCues.h"

#include <math.h>

#include "global.h"
#include "sfx.h"
#include "port/hooks/Events.h"

// Picked from a scan of bank-1/2/3 SFX in include/sfx.h: range 3 (audible from
// ~6350 units, well beyond the ~3000-unit ring spawn distance), no
// SFX_FLAG_22 so distance attenuation still applies, no SFX_FLAG_23 so the
// Y->pitch mapping isn't muddied by random per-frame wobble, and a high
// importance byte so the cue won't get evicted under polyphony pressure.
//
// Other bank-1/2/3 candidates considered, all range 3 unless noted:
//   NA_SE_EN_GRN_BEAM_CHARGE  (0x3103605B) — boss green-beam charge, tonal,
//                                            clean for pitch shifting; reads
//                                            faintly as "incoming attack."
//   NA_SE_KA_UFO_ENGINE       (0x11037025) — Katina UFO engine whir,
//                                            sustained, high importance.
//   NA_SE_GREATFOX_ENGINE     (0x11030010) — low ship drone; importance 0
//                                            means it evicts first when the
//                                            SFX pool fills.
//   NA_SE_EN_S_BEAM_CHARGE    (0x31016056) — cleanest tonal charge of the
//                                            shortlist, but range 1 (~2200u)
//                                            so it cuts out near ring spawn.
#define ACCESSIBILITY_CUE_SFX NA_SE_EN_GRN_BEAM_CHARGE

// Engine reads these pointers every audio frame, so they must outlive the
// Audio_PlaySfx call. File-static is the right scope.
static f32 sCueSrc[3] = { 0.0f, 0.0f, 0.0f };
static f32 sCueFreqMod = 1.0f;
static f32 sCueVolMod = 1.0f;
static s8 sCueReverb = 0;
static bool sCueActive = false;

static bool AccessibilityCues_IsEnabled(void) {
    return CVarGetInteger("gAccessibilityAudioCues", 1) == 1;
}

static Item* AccessibilityCues_FindNextTrainingRing(void) {
    Player* player = &gPlayer[0];
    Item* best = NULL;
    // dz < 0 means the ring is ahead of the player (player flies in -Z).
    // We want the ring closest to the player while still ahead, i.e. the dz
    // closest to zero from below, i.e. the maximum dz that is still negative.
    f32 bestDz = -1.0e9f;

    for (s32 i = 0; i < ARRAY_COUNT(gItems); i++) {
        Item* item = &gItems[i];
        if (item->obj.status != OBJ_ACTIVE) {
            continue;
        }
        if (item->obj.id != OBJ_ITEM_TRAINING_RING) {
            continue;
        }
        // state 1 = ring is in its fly-to-player animation after being
        // collected; no longer a navigation target.
        if (item->state != 0) {
            continue;
        }
        f32 dz = item->obj.pos.z - player->trueZpos;
        if (dz >= 0.0f) {
            continue;
        }
        if (dz > bestDz) {
            bestDz = dz;
            best = item;
        }
    }
    return best;
}

static void AccessibilityCues_RefreshSource(Item* ring) {
    Player* player = &gPlayer[0];
    // Player-relative bypass of Object_SetSfxSourceToPos. The converter pans
    // by camera position, but on-rails mode (which training is) decouples the
    // camera from the Arwing's lateral drift; a ring "in front of the camera"
    // can be off to the player's right. See docs/audio-system.md section 6.
    sCueSrc[0] = ring->obj.pos.x - player->pos.x;
    sCueSrc[1] = ring->obj.pos.y - player->pos.y;
    sCueSrc[2] = -(ring->obj.pos.z - player->trueZpos);
    Object_ClampSfxSource(sCueSrc);

    f32 octaves = sCueSrc[1] / 1000.0f;
    if (octaves > 1.0f) {
        octaves = 1.0f;
    } else if (octaves < -1.0f) {
        octaves = -1.0f;
    }
    sCueFreqMod = powf(2.0f, octaves);
}

static void AccessibilityCues_StartCue(Item* ring) {
    AccessibilityCues_RefreshSource(ring);
    Audio_PlaySfx(ACCESSIBILITY_CUE_SFX, sCueSrc, 0, &sCueFreqMod, &sCueVolMod, &sCueReverb);
    sCueActive = true;
}

static void AccessibilityCues_StopCue(void) {
    if (!sCueActive) {
        return;
    }
    Audio_KillSfxBySource(sCueSrc);
    sCueActive = false;
}

static void AccessibilityCues_OnGamePostUpdate(IEvent* event) {
    (void) event;

    if (!AccessibilityCues_IsEnabled() || gCurrentLevel != LEVEL_TRAINING) {
        AccessibilityCues_StopCue();
        return;
    }

    Item* target = AccessibilityCues_FindNextTrainingRing();
    if (target == NULL) {
        AccessibilityCues_StopCue();
        return;
    }

    if (!sCueActive) {
        AccessibilityCues_StartCue(target);
    } else {
        AccessibilityCues_RefreshSource(target);
    }
}

void AccessibilityCues_Init(void) {
    CVarRegisterInteger("gAccessibilityAudioCues", 1);
    REGISTER_LISTENER(GamePostUpdateEvent, AccessibilityCues_OnGamePostUpdate, EVENT_PRIORITY_NORMAL);
}

void AccessibilityCues_Exit(void) {
    AccessibilityCues_StopCue();
}
