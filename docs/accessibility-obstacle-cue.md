# The obstacle-ahead cue: design record

Status: v1 implemented 2026-08-11. Companion to `docs/accessibility-cues-tuning.md`
(which documents the knobs); this file records what the cue covers, *why* it is shaped
this way, and the catalogue of intentional misses — read this before "fixing" a silence.

## What it is

A collision **warning**: a low synthesized buzz that sounds only while the player is on a
collision course with a collidable, non-lockable obstacle, pulsing faster as the impact
nears. It deliberately encodes nothing else — rendered dead center (`CUE3D_MODE_DIRECT`,
no HRTF, no distance attenuation), constant loudness, constant pitch. "How soon" rides
the pulse interval alone, so it can never be confused with the distance-loudness or
height-pitch conventions the ring/enemy cues use. The planned directional obstacle cues
(left/right pan, above/below) will carry the "which way to dodge" half; this cue is the
"how soon" half. On-rails levels only for now, any vehicle.

## The three layers

- **`src/port/mods/ObjectQuery.h`** — generic decomp knowledge, shared beyond the cues:
  `Object_IsObstacle` (the classification: collidable hitbox + not lockable; also the
  predicate `AccessibilityTrainingMinimal` strips by, so "training removes exactly what
  the cue warns about" is an invariant, not a coincidence) and `Object_ReadSolidHitboxes`
  (the flat hitbox-array walk, stride-for-stride against `Player_CheckHitboxCollision`,
  `fox_play.c:1270-1291`).
- **`src/port/mods/accessibility_cues/ObstacleScan.{h,cpp}`** — the shared scan, sibling
  to `CueScan`: walks `gScenery`/`gActors`, applies the predicate plus the cue-side
  exclusions (below), and yields every solid hitbox record as a world-space box with the
  player-relative geometry derived (`dx`/`dy`, per-axis footprint clearance, gap to the
  near Z face). No thresholding — every filter beyond "warn-worthy obstacle" is the
  consumer's policy. Both roadmap items (all-range, directional cues) consume this same
  stream.
- **`src/port/mods/accessibility_cues/ObstacleAheadCue.{h,cpp}`** — policy: gates, the
  course test, nearest-gap selection, the gap→interval mapping, the buzz, the debug
  mirror for the debug server's `cues` command.

## The collision-course test

Coordinates: +X right, +Y up, +Z **backward** — the player flies toward −Z; the player's
real world Z is `trueZpos` (`pos.z` is the path scroll). A hitbox record is offsets
(relative to `obj.pos`) plus half-extents per axis (`docs/game-world.md` §7).

A record is *on course* when all of:

1. **Still ahead**: its near face — the maximum-Z face, `center.z + half.z`, the face the
   player meets first — is at positive gap `trueZpos − nearZ`. The moment the player is
   level with the face the record is dropped: past that, the engine's own collision has
   resolved the encounter and a warning is noise.
2. **Within the warning distance** (`gAccessibilityObstacleCueWarnDist`).
3. **Footprint overlap**: the player's current `(pos.x, pos.y)` lies inside the record's
   X/Y extent expanded by the safety margin (`gAccessibilityObstacleCueMarginXY`). The
   margin covers the Arwing's physical span (the engine collides four body/wing points,
   wings ~±40 units; the test uses the center point) plus reaction slack.

The nearest on-course record wins and its gap maps linearly to the pulse interval (slow
at the warning distance, fast at contact). Per-**record**, not per-object: a compound
hitbox like a pillar–lintel–pillar gate stays silent when the player is lined up with
the gap — that is correct, not a bug.

## What is included

Active (`OBJ_ACTIVE`) entries of `gScenery` and `gActors` that have a real hitbox
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
  wingmate would fire a false "about to crash" buzz.

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
- **Lockable actors never buzz** — including one parked dead on the flight path. They
  are the enemy cue's domain, and warning "obstacle" about a thing the player is
  supposed to shoot would be noise. If by-ear testing wants a collision warning for
  enemy bodies too, that is a *policy* change in the cue's filter, not the shared
  predicate. (Bosses and teammates are excluded separately — see "What is included".)
- **Ground, water, and lava are not objects** (a single `gGroundHeight` float,
  `docs/game-world.md` §8) and are out of scope. A ground-proximity cue would be its own
  file with its own mechanism.
- **Sprites and items are not scanned**: sprites never collide with the player; items
  only get collected.
- **Rotation is ignored twice**: `obj.rot` (the engine rotates the player's points into
  the box frame even for plain boxes when the object is rotated) and `HITBOX_ROTATED`'s
  rotation floats (the unrotated box is used as-is). A yawed wall's effective footprint
  is therefore somewhat wrong; the safety margin absorbs typical cases. Attach point:
  rotate the player offset into the box frame in `ObstacleScan` (the engine's own
  approach, `fox_play.c:1275-1311`).
- **Shadow and whoosh hitbox records are skipped** (`HITBOX_SHADOW` dims the screen,
  `HITBOX_WHOOSH` plays the near-miss sound; neither damages).
- **The course is a straight −Z ray from the current position.** Lateral velocity and
  path curvature are not projected, so the prediction degrades near sharp path bends
  (over- and under-warning both possible). Attach point: project along the path heading
  (`xRot_120`/`yRot_114`).
- **All-range mode is gated out** (`gLevelMode == LEVELMODE_ON_RAILS`). The planned
  extension scans `gScenery360` too and replaces the Z-gap course test with a
  heading-projected one — new policy over the same `ObstacleBox` stream.
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

## Testing notes

- **Minimal training silences the cue in Training by design** — it strips the same
  predicate's object set. Tune on Corneria; the `obstacle-scout` debug checkpoint
  (`tools/checkpoints.json`) sits just before a rock-wall pair and a building, with
  `obstacle-ahead` ~250 units further in.
- The debug server's `cues` command reports the full decision: gates, scan counters
  (`active`/`obstacles`/`boxes`, plus `onCourse` after the course filter), the effective
  knobs, and the winning record (array/slot/objId/record, `gapZ`, per-axis
  clearance/delta/half-extents, the pushed `intervalSec`). Join `target.slot` against
  `objects scenery` to cross-check positions.
- The F1 volume-slider Preview plays the buzz as a seamless drone (previews force
  interval 0 — pre-existing behavior shared with the aim cue): it checks timbre and
  volume, not cadence.
