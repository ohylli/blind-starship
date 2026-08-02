#pragma once

// Flight-direction (aim) angles for the player, shared by every port-side consumer
// (accessibility cues, debug-server dumps) so the composition can never drift between
// them.
//
// `player->rot` alone is not the craft's orientation — it is a modifier layered on top
// of the heading fields, and reads 0 on y even while the player is mid-turn in all-range
// mode. The real flight direction is the composition below. Together aimYaw/aimPitch
// reproduce velocity exactly through the standard spherical form, with no special case
// anywhere:
//
//     forward = (-sin(aimYaw)*cos(aimPitch), sin(aimPitch), -cos(aimYaw)*cos(aimPitch))
//
// Verified live on Fortuna against measured velocity at speed 40, in four samples that
// agree to every printed digit: two in normal flight (aimPitch 0 and 30.4278) and two
// mid-U-turn (aimPitch 66.0 and 138.0559). aerobaticPitch is part of aimPitch, exactly as
// fox_display.c:970 and fox_demo.c:1548 add it. Past 90 degrees the Arwing is inverted and
// travels backward relative to its nose, but that needs no branch — cos(aimPitch) turns
// negative and reverses the horizontal terms on its own. The 138.0559 sample is the one
// that pins this down: climbing 41.9441 degrees by velocity, i.e. 180 - aimPitch.
//
// Form coverage: this is the *Arwing* composition (Player_MoveArwing360 fox_play.c:3851,
// Player_MoveArwingOnRails fox_play.c:3961, Player_PerformLoop), and the Blue Marine
// flies by the identical composition (fox_aq.c:979-980). It is NOT the Landmaster's —
// Player_MoveTank360 (fox_play.c:4246) composes velocity from rot_104 in X->Z->Y order
// with no xRot_120 or aerobaticPitch at all — and not on-foot Fox's either
// (Player_MoveOnFoot writes only vel.x/vel.z from its own composition). Gate on
// Player_AimAnglesValid before treating these as a flight direction.
//
// `faceYaw` is the +180 model-draw convention (fox_display.c:967), useful only for
// reasoning about rendering — do not steer by it. Neither composition folds in
// damageShake, which some draw sites add, and fox_beam.c:861 uses a different yaw offset
// for its shot type, so exact projectile trajectories still want the raw fields.

#include "port/CGameCompat.h"

inline bool Player_AimAnglesValid(const Player& p) {
    return (p.form == FORM_ARWING) || (p.form == FORM_BLUE_MARINE);
}

inline f32 Player_AimYaw(const Player& p) {
    return p.yRot_114 + p.rot.y;
}

inline f32 Player_AimPitch(const Player& p) {
    return p.xRot_120 + p.rot.x + p.aerobaticPitch;
}

inline f32 Player_FaceYaw(const Player& p) {
    return Player_AimYaw(p) + 180.0f;
}
