# The obstacle-ahead cue: design record

Status: v1 implemented 2026-08-11; all-range support added 2026-08-18; poly-mesh
coverage (bounding boxes) added 2026-09-11. Companion to
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
  `Object_IsObstacle` (the classification: collidable hitbox, poly mesh or sphere
  collider + not lockable; also the
  predicate `AccessibilityTrainingMinimal` strips by, so "training removes exactly what
  the cue warns about" is an invariant, not a coincidence) and `Object_ReadSolidHitboxes`
  (the flat hitbox-array walk, stride-for-stride against `Player_CheckHitboxCollision`,
  `fox_play.c:1270-1291`), plus `Object_GetPolyCollider`/`Object_GetPolyBounds` (the
  poly-mesh dispatch lists and each mesh's stored bounding box — the engine's second
  collision mechanism) and `Object_GetSphereCollider` (the event-type → radius
  table for the engine's third collision mechanism, the hand-written sphere test; see
  "What is included").
- **`src/port/mods/accessibility_cues/ObstacleScan.{h,cpp}`** — the shared scan, sibling
  to `CueScan`: walks `gScenery`/`gActors`, plus `gScenery360` in all-range mode (the
  walk is mode-gated because that heap pointer dangles after an all-range level unloads
  — see the comment at the loop), applies the predicate plus the cue-side exclusions
  (below), and yields every collision shape — each solid hitbox record, a poly mesh's
  bounding box, a boxed collision sphere — as a world-space box with the
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
correct even mid-fight), and any future non-shootable hazard actor. Also included, via
a different route: **sphere-collided actor events** (`Object_GetSphereCollider` in
`ObjectQuery.h`) — today only Meteo's big meteor (`EVID_ME_BIG_METEOR`), which carries
`gNoHitbox` and is collided by a hand-written 900-unit sphere around `obj.pos`
(`fox_play.c:2169`; player shots use 1000, `fox_beam.c:787`). The radius is a code
literal, not object data, so the predicate keeps a small event-type → radius table that
passes such objects as collidable, and the scan boxes the sphere: one synthesized
record centered on `obj.pos` with ±radius half-extents, `record == -1`
(`kObstacleSphereRecord`) in the `cues` dump. The cube over-warns at its corners
(diagonal passes 900–1270 units off center); accepted for the same reason as the
rotation approximation — conservative is the right failure direction for an 1800-unit
rock, and one box shape keeps the course tests and the directional siblings simple.

Also included, via the engine's **second** mechanism: **poly-mesh objects**. A fixed set
of object ids is routed by `Player_CollisionCheck` to `Player_CheckPolyCollision`
*instead of* the hitbox test — the Corneria terrain bumps 1–5, Aquas's bumps and coral
reefs, Zoness's island, Fortuna's three mountain types and Venom 2's mountain (all-range),
Meteo's molar rock, and the Sector Y capital ships (`EVID_SY_SHIP_2`), plus the four
poly boss bases the cue never sees. For those ids the hitbox is dead data as far as
crashing goes, whether it is `gNoHitbox` (most) or a real record table the engine simply
ignores (the capital ship, the island, the reefs — the cue used to read those and warn
about the wrong shape). `Object_GetPolyCollider` mirrors the three dispatch lists and
resolves the mesh through the engine's own id → mesh map (`Play_GetPolyColId`, split out
of `Play_CheckPolyCollision` for exactly this), and the scan yields **one box per
object, the mesh's stored bounding box** (`CollisionHeader.min/max`,
`fox_colheaders.c`) around `obj.pos`, `record == -2` (`kObstaclePolyRecord`) in the
`cues` dump, *replacing* any hitbox records. Rotation is ignored as everywhere else.
The catch: the engine has two mesh families and only one is a solid. The
`CollisionHeader` family (the molar rock, Fortuna mountains 2 and 3, the capital ship,
the boss bases) is a swept-triangle test, and its box is a fair stand-in. The
`CollisionHeader2` family (every bump, both reefs, the island, Fortuna mountain 1,
Venom's mountain) is a **heightfield**: the engine finds the triangle under the player's
X/Z and hits only when the player's Y is at or below that surface (`fox_col2.c`). Its
box spans the whole hill, so the box alone over-warns whenever the player is inside the
footprint below the peak, even on a course that clears the slope — see "Known defects"
for the planned fix.

Two cue-side exclusions sit on top of the predicate, in `ObstacleScan`:

- **`gBosses` is not scanned.** `targetOffset` cannot separate "boss you fight" from
  "boss-shaped wall" — nearly every `gBosses` entry has `targetOffset == 0` even though
  it is shootable (Sarumarine is the game's one lockable boss), so a lockability test
  excludes nothing, and droning a crash warning through an on-rails boss fight (Meteo,
  Area 6, Sector X, …) would bury the aim/enemy cues exactly when they matter most.
  Bosses also have hand-written collision spheres of their own (`OBJ_BOSS_BO_BASE_SHIELD`,
  1500 units and `gNoHitbox`, `fox_play.c:2092`; `OBJ_BOSS_KA_SAUCERER`, 2700 units,
  `fox_play.c:2113`) that `Object_GetSphereCollider` deliberately leaves untabled for the
  same reason — a future boss cue would need to add them.
  Note the enemy cue does not cover `gBosses` either (it scans `gActors` only) — boss
  encounters are their own future cue category, as `docs/accessibility-enemy-cue.md`
  already records.
- **Teammate Arwings are excluded.** Wingmates carry a small collidable hitbox with
  `targetOffset == 0` and weave ahead of the player on-rails; without the exclusion
  every crossing wingmate would fire a false "about to crash" buzz. They come in two
  forms, both dropped by `ObstacleScanDetail::IsTeammate`: `OBJ_ACTOR_TEAM_BOSS` (the
  handful of escorts placed for a boss run in Meteo and Area 6) and `OBJ_ACTOR_EVENT`
  actors with `eventType == EVID_TEAMMATE`, which fly the rest of an on-rails level
  (their event row installs `gCubeHitbox100`, `fox_enmy2.c:992`). The id-only test
  originally shipped and missed the second form — see "Fixed defects" below.

All-range coverage per arena (from the level manifests, live-checked 2026-08-18):
Sector Z is the richest (62 space-junk scenery pieces plus actor junk), Bolse has its
poles and buildings plus the non-lockable installations, Fortuna its towers (3 solid
records each) and, since the poly-mesh change, its 33 mountains (their hitbox tables
hold whoosh records only; the poly box is what warns now), and
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

- **Poly meshes are boxes, and rotated meshes use the unrotated box.** Covered since
  2026-09-11 (see "What is included"); what remains deliberate is the shape: one
  axis-aligned bounding box per mesh, `obj.rot` ignored like every other box. The
  earlier version of this entry listed Corneria's highways and Aquas's coral among the
  invisible objects; that was wrong — highways 1, 2 and 5–9 have real hitboxes and were
  always covered, highways 3 and 4 collide with nothing at all, and both coral reefs
  have hitboxes (they were the island's case: poly-collided, hitbox-read). It also
  missed Venom 2's mountain and the Sector Y capital ships. Meteo's tunnel
  (`OBJ_SCENERY_ME_TUNNEL`) has a mesh header but no call site dispatches it, so it is
  not a collider and not a miss.
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
  `MA_TERRAIN_BUMP` — `fox_play.c:1976-1985`) because the tank handles them separately;
  the cue does not, so those levels may buzz about the floor being driven on. Untested in
  v1. Remedies if it bites: replicate that id skip-list in the cue's filter, or gate the
  cue by vehicle form.
- **Corridor-wide hitboxes could drone.** A wall spanning the whole flyable corridor
  passes the footprint test wherever the player flies. The debug mirror reports the
  winning record's half-extents precisely so this is diagnosable from one `cues` dump;
  the remedy, if seen, is a "wider than the corridor" skip (compare against
  `pathWidth`) as policy.

## Known defects — found after v1, not yet fixed

Unlike the catalogue above, entries here are *not* deliberate and are meant to be fixed.

- **Heightfield meshes over-warn** (known since the poly change, 2026-09-11 — the
  planned second step of that work). The `CollisionHeader2` family (all terrain bumps,
  the reefs, the island, Fortuna mountain 1, Venom's mountain) is a surface the player
  only hits from above, but the scan yields its whole bounding box and the cue treats it
  as solid: on Corneria, which places 178 bumps, the box test buzzes whenever the player
  is low inside a bump's footprint even on a course that clears the slope. Planned fix,
  as *cue policy* (the scan stays box-only so the directional siblings share one shape):
  once a heightfield box passes the course test, walk the course through the box in
  ~100-unit steps at the player's current altitude and ask the engine's own surface
  test (`func_col2_800A3690`, after rotating the offset by `-obj.rot.y` as
  `Player_CheckPolyCollision` does) whether that point is at or below the surface; the
  first hit is the impact distance, no hit drops the box. Solid meshes keep the plain
  box.

## Fixed defects

- **Meteo's big meteors were invisible to the cue** (`EVID_ME_BIG_METEOR`, fixed
  2026-08-27). A *third* collision mechanism, neither hitbox nor poly mesh: the event
  row carries `gNoHitbox` (`fox_enmy2.c:1012`) and the engine collides it with a
  hand-written 900-unit sphere around `obj.pos` (`fox_play.c:2169`), so
  `Object_IsObstacle` rejected it on the hitbox test. Confirmed live from the
  `big-asteroid` checkpoint: with a big meteor active in `gActors` slot 37 the scan
  reported `obstacles = 15` — the 9 `METEOR_7` + 4 `METEOR_6` + 1 `SECRET_MARKER_1` + 1
  wingmate, the big meteor absent. Fixed by the sphere-collider table + boxed record
  described under "What is included".

- **Normal-flight wingmates fired false warnings** (fixed 2026-08-27). The teammate
  exclusion originally tested only `obj.id == OBJ_ACTOR_TEAM_BOSS`, which covers the
  handful of boss-approach escorts (Meteo places three, around path 303774). Through
  the rest of an on-rails level the wingmates are `OBJ_ACTOR_EVENT` (id 200) with
  `eventType == EVID_TEAMMATE`, whose event row carries `gCubeHitbox100` and
  `targetOffset == 0` (`fox_enmy2.c:992`) — so they classified as obstacles and slipped
  past the exclusion. Observed live: over ~330 frames of straight flight from the
  `big-asteroid` checkpoint the *only* record that ever came on course was `gActors`
  slot 34, that wingmate — half-extents 50/50, gap 1141 growing to 1246, the cue active
  throughout. The exclusion now also drops `OBJ_ACTOR_EVENT` actors whose `eventType`
  is `EVID_TEAMMATE` (`ObstacleScanDetail::IsTeammate`); re-checked live from the same
  checkpoint and from `obstacle-scout`, where the rock wall still warns.

## Testing notes

- **Minimal training silences the cue in Training by design** — it strips the same
  predicate's object set (Training places none of the poly-mesh ids, so the poly change
  did not alter what it strips). Tune on Corneria; the `obstacle-scout` debug checkpoint
  (`tools/checkpoints.json`) sits just before a rock-wall pair and a building, with
  `obstacle-ahead` ~250 units further in. The same checkpoint doubles as the poly-box
  test: a Corneria bump 4 (`objId 4`) sits centered on the corridor ~2600 units past
  the rock walls, but the Arwing flies at altitude ~350 over a box 174 tall, so with the
  default margin it is correctly off course (`clearY` ≈ 165 against 150). Set
  `gAccessibilityObstacleCueMarginXY` to `800.0` (a float, or the CVar becomes an int
  the cue ignores) and step past the walls: the winner becomes `record -2` with
  half-extents 1311.5/87/1311.5 — the `CollisionHeader2` index 1 bounds — and its
  `delta` equals the bump's `objects scenery` position plus the mesh's center offset.
  Verified live 2026-09-11.
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
