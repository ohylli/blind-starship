#include "RingCue.h"
#include "CueCommon.h"

#include "port/CGameCompat.h"
#include "port/hooks/Events.h"
#include "port/accessibility/Cue.h"
#include "port/mods/Accessibility.h"

// The ring cue: guides the player to the next Training ring. Registered in
// RingCue_Register; process-lifetime (the registry never frees).
static Cue* sRingCue = nullptr;

static RingCueDebug sDebugState;

const RingCueDebug& RingCue_DebugState() {
    return sDebugState;
}

static Item* RingCue_FindNextTrainingRing() {
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

static void RingCue_OnPostUpdate(IEvent* event) {
    (void) event;

    sDebugState = RingCueDebug{};
    RingCueDebug& dbg = sDebugState;
    // PlayerHasControl also covers the gPlayer null guard: gPlayer is a pointer
    // (sf64context.h:324), zero-initialized at process start and only allocated
    // when a level loads, and this listener fires on GamePostUpdateEvent which
    // can tick before that.
    dbg.enabled = CueCommon_IsEnabled();
    dbg.inTraining = (gCurrentLevel == LEVEL_TRAINING);
    dbg.control = Accessibility_PlayerHasControl();
    dbg.frame = (int32_t) gGameFrameCount;
    if (!dbg.enabled || !dbg.inTraining || !dbg.control) {
        sRingCue->Stop();
        return;
    }

    Item* target = RingCue_FindNextTrainingRing();
    if (target == NULL) {
        sRingCue->Stop();
        return;
    }

    // Player-relative bypass of Object_SetSfxSourceToPos. The converter pans
    // by camera position, but on-rails mode (which training is) decouples the
    // camera from the Arwing's lateral drift; a ring "in front of the camera"
    // can be off to the player's right. See docs/audio-system.md section 6.
    Player* player = &gPlayer[0];
    dbg.active = true;
    dbg.itemIndex = (int32_t) (target - gItems);
    dbg.dx = target->obj.pos.x - player->pos.x;
    dbg.dy = target->obj.pos.y - player->pos.y;
    dbg.dz = -(target->obj.pos.z - player->trueZpos);
    CueCommon_ComputeCueTarget(dbg.dx, dbg.dy, dbg.dz, dbg.src, &dbg.freq);
    sRingCue->SetTarget(dbg.src[0], dbg.src[1], dbg.src[2], dbg.freq);
    sRingCue->Start();
}

void RingCue_Register() {
    sRingCue = CueRegistry_Register(kRingCueId, "Ring guide", "Guides you toward the next training ring.",
                                    { .wavPath = "assets/accessibility/ring.wav" });
    REGISTER_LISTENER(GamePostUpdateEvent, RingCue_OnPostUpdate, EVENT_PRIORITY_NORMAL);
}
