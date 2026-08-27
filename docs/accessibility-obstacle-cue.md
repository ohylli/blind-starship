# The obstacle-ahead cue: design record

Status: v1 implemented 2026-08-11; all-range support added 2026-08-18. Companion to
`docs/accessibility-cues-tuning.md`
(which documents the knobs); this file records what the cue covers, *why* it is shaped
this way, and the catalogue of intentional misses — read this before "fixing" a silence.
Silences that are *not* deliberate are kept separately under "Known defects" so the two
are never confused.

## What it is

A collision **warning**: a low synthesized buzz that sounds only while the player is on a
collision course with a collidable, non-lockable obstacle, pulsing faster as the impact
nears. It deliberately encodes nothing else — rendered dead center (`CUE3D_MODE_DIRECT`,
no HRTF, no distance attenuation), constant loudness, constant pitch. "How soon" rides
the pulse interval alone, so it can never be confused with the distance-loudness or
height-pitch conventions the ring/enemy cues use. The planned directional obstacle cues
(left/right pan, above/below) will carry the "which way to dodge" half; this cue is the
"how soon" half. Covers on-rails levels (any vehicle) and solo all-range battles
(Versus is out of scope, like every cue — see `CueScan_ModeInScope`).

## The three layers

- **`src/port/mods/ObjectQuery.h`** — generic decomp knowledge, shared beyond the cues:
  `Object_IsObstacle` (the classification: collidable hitbox + not lockable; also the
  predicate `AccessibilityTrainingMinimal` strips by, so "training removes exactly what
  the cue warns about" is an invariant, not a coincidence) and `Object_ReadSolidHitboxes`
  (the flat hitbox-array walk, stride-for-stride against `Player_CheckHitboxCollision`,
  `fox_play.c:1270-1291`).
- **`src/port/mods/accessibility_cues/ObstacleScan.{h,cpp}`** — the shared scan, sibling
  to `CueScan`: walks `gScenery`/`gActors`, plus `gScenery360` in all-range mode (the
  walk is mode-gated because that heap pointer dangles after an all-range level unloads
  — see the comment at the loop), applies the predicate plus the cue-side exclusions
  (below), and yields every solid hitbox record as a world-space box with the
  player-relative geometry derived (the full 3D center delta `dx`/`dy`/`dz`, per-axis
  footprint clearance, gap to the near Z face). No thresholding — every filter beyond
  "warn-worthy obstacle" is the consumer's policy. The remaining roadmap item (the
  directional cues) consumes this same stream.
- **`src/port/mods/accessibility_cues/ObstacleAheadCue.{h,cpp}`** — policy: gates, the
  course test, nearest-gap selection, the gap→interval mapping, the buzz, the debug
  mirror for the debug server's `cues` command.

## The collision-course test

Coordinates: +X right, +Y up, +Z **backward** — on rails the player flies toward −Z; the
player's real world Z is `trueZpos` (`pos.z` is the path scroll). A hitbox record is
offsets (relative to `obj.pos`) plus half-extents per axis (`docs/game-world.md` §7).

The test is per mode. **On rails** a record is *on course* when all of:

1. **Still ahead**: its near face — the maximum-Z face, `center.z + half.z`, the face the
   player meets first — is at positive gap `trueZpos − nearZ`. The moment the player is
   level with the face the record is dropped: past that, the engine's own collision has
   resolved the encounter and a warning is noise.
2. **Within the warning distance** (`gAccessibilityObstacleCueWarnDist`).
3. **Footprint overlap**: the player's current `(pos.x, pos.y)` lies inside the record's
   X/Y extent expanded by the safety margin (`gAccessibilityObstacleCueMarginXY`). The
   margin covers the Arwing's physical span (the engine collides four body/wing points,
   wings ~±40 units; the test uses the center point) plus reaction slack.

**In all-range** the ship flies in any direction, so the fixed −Z course is replaced by
a ray cast along the aim heading: the forward unit vector from `Player_AimForward`
(`src/port/PlayerAim.h`, the composition verified live against
velocity — `player->rot` alone is *not* a heading there), intersected with each box via
a standard ray-vs-AABB slab test with the box expanded by the same margin on all three
axes (in all-range "lateral" is not axis-aligned; the extra margin on the ray axis is
noise against the warn distance). The record is on course when the ray's entry distance
is positive and within the warning distance — entry ≤ 0 means the player is level with,
past, or already inside the expanded box, and the engine's own collision owns the
encounter, mirroring the rails rule. The heading composition is the Arwing's (and Blue
Marine's); the forms that compose differently (Landmaster, on-foot) never appear in
solo all-range, and the cue stops rather than guesses if one ever does.

The nearest on-course record wins and its gap maps geometrically (log-domain
interpolation) to the pulse interval (slow at the warning distance, fast at contact), so
the pulse rate climbs by equal-sounding tempo steps as the gap closes instead of
saving nearly all of its rise for the last half second. Per-**record**, not per-object: a compound
hitbox like a pillar–lintel–pillar gate stays silent when the player is lined up with
the gap — that is correct, not a bug.

## What is included

Active (`OBJ_ACTIVE`) entries of `gScenery` and `gActors` — plus `gScenery360`, the
all-range arena's scenery array, when in all-range mode — that have a real hitbox
(`info.hitbox` with ≥1 record, not `gNoHitbox`) and are **not** lock-on-targetable
(`info.targetOffset == 0`, actors only — scenery is never lockable). That is: buildings,
arches, rock walls, scripted barriers, big non-lockable hazard actors (Zoness's Dodora
body, Titania's Delphor body — flying into them is a real collision, so warning is
correct even mid-fight), and any future non-shootable hazard actor. Two cue-side
exclusions sit on top of the predicate, in `ObstacleScan`:

- **`gBosses` is not scanned.** `targetOffset` cannot separate "boss you fight" from
  "boss-shaped wall" — nearly every `gBosses` entry has `targetOffset == 0` even though
  it is shootable (Sarumarine is the game's one lockable boss), so a lockability test
  excludes nothing, and droning a crash warning through an on-rails boss fight (Meteo,
  Area 6, Sector X, …) would bury the aim/enemy cues exactly when they matter most.
  Note the enemy cue does not cover `gBosses` either (it scans `gActors` only) — boss
  encounters are their own future cue category, as `docs/accessibility-enemy-cue.md`
  already records.
- **Teammate Arwings (`OBJ_ACTOR_TEAM_BOSS`) are id-excluded.** The wingmates escorting
  you into a boss run carry a small collidable hitbox with `targetOffset == 0` and weave
  ahead of the player on-rails (Meteo, Area 6); without the exclusion every crossing
  wingmate would fire a false "about to crash" buzz. That id test turns out to be
  *incomplete* — it misses the wingmates that fly the rest of the level. See "Known
  defects" below.

All-range coverage per arena (from the level manifests, live-checked 2026-08-18):
Sector Z is the richest (62 space-junk scenery pieces plus actor junk), Bolse has its
poles and buildings plus the non-lockable installations, Fortuna its towers (3 solid
records each; the mountains are whoosh-only — see the poly-mesh miss below), and
Venom-Andross its 29 tunnel passages. **Katina places no spawn-list scenery at all** —
its base and mothership are boss/poly geometry — so the only obstacles there are the
non-lockable allied fighters (below): near-silence, an accepted consequence of what the
arena contains rather than a gap in the cue.

One consequence of the classification, observed live: in all-range dogfights the
**allied fighters** (non-lockable `OBJ_ACTOR_ALLRANGE` craft — lockable ones are
enemies and stay the enemy cue's domain) pass the obstacle predicate, so an ally
crossing the player's course inside the warn distance fires a brief buzz. That is an
honest warning about a real collidable craft, and unlike the on-rails wingmates (small
hitboxes weaving constantly at the player's nose in formation) it is rare — allies
cannot be id-excluded anyway, since enemies share the same object id and only
lockability separates them. Judged acceptable; revisit only if by-ear play finds it
noisy.

## Intentional misses — the "why is it silent/noisy" catalogue

Each of these is a deliberate v1 decision with a known attach point, not an oversight.

- **Poly-mesh scenery is invisible.** A handful of big shapes collide via
  `Player_CheckPolyCollision`, not hitboxes, and carry `gNoHitbox` — Corneria's terrain
  bumps (`OBJ_SCENERY_CO_BUMP_1..5`), highways, Aquas coral/bumps, Fortuna mountains,
  Meteo's molar rock, Great Fox and boss bases. The predicate rejects them, so the
  biggest terrain features in some levels get no warning. Attach point: an explicit
  id → bounding-box table (or the poly mesh's own AABB) consulted in `ObstacleScan` when
  the hitbox is empty. Conversely `OBJ_SCENERY_ZO_ISLAND` has *both* a hitbox and a poly
  path — the engine collides the poly, the cue reads the hitbox: benign over-warning.
  Fortuna's mountains are this miss's all-range face: their hitbox tables exist but hold
  whoosh records only (confirmed live — 12 towers × 3 solid records + the actor boxes
  account for every box the scan yields; no mountain ever came on course), so the
  biggest terrain in that arena gives no warning.
- **Lockable actors never buzz** — including one parked dead on the flight path. They
  are the enemy cue's domain, and warning "obstacle" about a thing the player is
  supposed to shoot would be noise. If by-ear testing wants a collision warning for
  enemy bodies too, that is a *policy* change in the cue's filter, not the shared
  predicate. (Bosses and teammates are excluded separately — see "What is included".)
- **Ground, water, and lava are not objects** (a single `gGroundHeight` float,
  `docs/game-world.md` §8) and are out of scope. A ground-proximity cue would be its own
  file with its own mechanism.
- **Sprites and items are not scanned.** Items only get collected, but sprites *do*
  collide — Fortuna's 26 poles and Corneria's trees/poles stagger the Arwing without
  damage (`fox_play.c`'s sprite loop). Keeping `gSprites` out of the scan (both modes)
  is a deliberate scope decision, revisitable as its own change with its own by-ear
  pass.
- **Rotation is ignored twice**: `obj.rot` (the engine rotates the player's points into
  the box frame even for plain boxes when the object is rotated) and `HITBOX_ROTATED`'s
  rotation floats (the unrotated box is used as-is). A yawed wall's effective footprint
  is therefore somewhat wrong; the safety margin absorbs typical cases. Attach point:
  rotate the player offset into the box frame in `ObstacleScan` (the engine's own
  approach, `fox_play.c:1275-1311`).
- **Shadow and whoosh hitbox records are skipped** (`HITBOX_SHADOW` dims the screen,
  `HITBOX_WHOOSH` plays the near-miss sound; neither damages).
- **The on-rails course is a straight −Z ray from the current position.** Lateral
  velocity and path curvature are not projected, so the prediction degrades near sharp
  path bends (over- and under-warning both possible). Attach point: project along the
  path heading (`xRot_120`/`yRot_114`) — the all-range branch already casts along the
  aim heading, so this would converge the two tests.
- **Actor-event obstacles are invisible for one tick after spawn** (hitbox and
  targetOffset install on the second update tick — see `Object_HasCollidableHitbox`'s
  comment). Harmless: 33 ms against a ~2 s warning window.
- **Landmaster-ground scenery may false-positive.** The engine's Arwing collision pass
  id-excludes Macbeth/Titania driving surfaces (`OBJ_SCENERY_TI_BRIDGE`,
  `MA_TRAIN_TRACK_13`, `MA_BUILDING_1/2`, `MA_TOWER`, `MA_WALL_2/3`, `MA_FLOOR_1..5`,
  `MA_TERRAIN_BUMP` — `fox_play.c:1962-1971`) because the tank handles them separately;
  the cue does not, so those levels may buzz about the floor being driven on. Untested in
  v1. Remedies if it bites: replicate that id skip-list in the cue's filter, or gate the
  cue by vehicle form.
- **Corridor-wide hitboxes could drone.** A wall spanning the whole flyable corridor
  passes the footprint test wherever the player flies. The debug mirror reports the
  winning record's half-extents precisely so this is diagnosable from one `cues` dump;
  the remedy, if seen, is a "wider than the corridor" skip (compare against
  `pathWidth`) as policy.

## Known defects — found after v1, not yet fixed

Unlike the catalogue above, these two are *not* deliberate. Both surfaced on 2026-08-27
while evaluating the `big-asteroid` Meteo checkpoint as an obstacle-cue test spot, and
each is meant to be fixed on its own.

- **Meteo's big meteors are invisible to the cue** (`EVID_ME_BIG_METEOR`). This is a
  *third* collision mechanism, neither hitbox nor poly mesh: the event row carries
  `gNoHitbox` (`fox_enmy2.c:1012`) and the engine collides it with a hand-written
  900-unit **sphere** around `obj.pos` (`fox_play.c:2155`); player shots use a matching
  1000-unit sphere (`fox_beam.c:787`). It is non-lockable (`targetOffset == 0`) and
  destructible (health 500), so by the cue's own policy it is squarely a warn-worthy
  obstacle — `Object_IsObstacle` simply cannot see it, rejecting it on the hitbox test.
  Confirmed live from the `big-asteroid` checkpoint: with a big meteor active in
  `gActors` slot 37 the scan reported `obstacles = 15`, which is exactly the 9
  `METEOR_7` + 4 `METEOR_6` + 1 `SECRET_MARKER_1` + 1 wingmate alive at that moment —
  the big meteor is not in the count. Attach point: the box emission in `ObstacleScan`,
  either synthesizing an axis-aligned box from the sphere radius for this event type or
  teaching `ObstacleBox` a sphere record kind (the ray-vs-AABB and footprint tests both
  have straightforward sphere analogues). Note the radius is a *code constant*, not
  data, so some form of event-type → radius table is unavoidable. Meteo's ordinary rocks
  (`METEOR_6` ±200, `METEOR_7` ±100) do carry cube hitboxes and are already covered.
- **Normal-flight wingmates fire false warnings.** The teammate exclusion in
  `ObstacleScan.h` tests `obj.id == OBJ_ACTOR_TEAM_BOSS`, which covers only the handful
  of boss-approach escorts (Meteo places three, around path 303774). Through the rest of
  an on-rails level the wingmates are `OBJ_ACTOR_EVENT` (id 200) with
  `eventType == EVID_TEAMMATE`, whose event row carries `gCubeHitbox100` and
  `targetOffset == 0` (`fox_enmy2.c:992`) — so they classify as obstacles and slip past
  the exclusion. Observed live: over ~330 frames of straight flight from the
  `big-asteroid` checkpoint the *only* record that ever came on course was `gActors`
  slot 34, that wingmate — half-extents 50/50, gap 1141 growing to 1246, the cue active
  throughout. Precisely the noise the id exclusion was written to prevent. Fix: extend
  that exclusion to also drop `OBJ_ACTOR_EVENT` actors whose `eventType` is
  `EVID_TEAMMATE`.

## Testing notes

- **Minimal training silences the cue in Training by design** — it strips the same
  predicate's object set. Tune on Corneria; the `obstacle-scout` debug checkpoint
  (`tools/checkpoints.json`) sits just before a rock-wall pair and a building, with
  `obstacle-ahead` ~250 units further in.
- **All-range testing has no checkpoints** (the checkpoint machinery captures the
  on-rails `pathProgress` tuple only) and the player cannot be steered from the debug
  server, so deterministic tests use Sector Z (densest junk) with the margin CVar
  temporarily widened (1500–2500) until objects near the spawn flight line come on
  course, driven by `warp sector_z --no-intro --paused` plus `step`. Beware that a
  huge margin also expands the box along the ray, so very close objects get swallowed
  by the "already inside → silent" rule — that is the test setup's artifact, not a bug.
  The `cues` dump reports the heading (`forward`) and the winning ray gap (`target.gap`)
  precisely so the slab test can be recomputed offline from one dump. Missions also
  script control away mid-fight (`gates.control` flips false) — poll around it.
- The debug server's `cues` command reports the full decision: gates (`modeOk` and
  the all-range form gate `aimValid` among them), the top-level `allRange` flag, scan
  counters (`active`/`obstacles`/`boxes`, plus `onCourse` after the course filter), the
  effective knobs, the unit `forward` heading (all-range only), and the winning record
  (array/slot/objId/record, the course `gap` plus the raw `gapZ`, per-axis
  clearance/delta/half-extents, the pushed `intervalSec`). Join `target.slot` against
  `objects scenery` to cross-check positions.
- The F1 volume-slider Preview plays the buzz as a seamless drone (previews force
  interval 0 — pre-existing behavior shared with the aim cue): it checks timbre and
  volume, not cadence.
