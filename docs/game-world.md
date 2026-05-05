# Starship game-world model

This document explains how Star Fox 64 (this Starship port) represents its
playable world: the environment and obstacles you can crash into, the enemies
that fight you, and the items you can pick up. It is meant for two readers:

1. **A software engineer who is new to the codebase and to game runtime
   internals.** It explains the moving parts — what the data structures are,
   where they live, how the per-frame loop walks them — without assuming prior
   knowledge of console-era game architecture.
2. **Future Claude Code sessions.** It maps the concepts to the actual files,
   types, globals, and per-frame hooks needed to build accessibility audio
   cues (positional cues for enemies, hazards, and items) without
   rediscovering the whole pipeline each session.

For the audio side — how to emit a positional sound effect once you know what
to announce — see `docs/audio-system.md`. This document only covers the world
side: how to find out what is in the world and where it is.

---

## 1. The 90-second mental model

Star Fox 64's world is **not a scene graph** and **not a physics simulation**.
It is six fixed-size arrays that the game scans every frame.

- The level is a flat list of *spawn entries* (`ObjectInit`), sorted by
  forward distance along the level path. Each entry says "at distance Z, with
  rotation R, spawn an object of this `ObjectId`."
- As the player flies forward, a streaming loop pulls entries off the head of
  the list and copies them into one of six **active-world arrays**:
  `gScenery` (50), `gSprites` (40), `gActors` (60), `gBosses` (4), `gItems`
  (20), `gEffects` (100). Which array a spawn lands in is decided by the
  numeric range of its `ObjectId`.
- Once per frame, a single function (`Object_Update`) walks all six arrays in
  order and runs each live object's *action* — a C function pointer stored on
  the object that, when called, advances the object's per-frame logic. The
  action is what makes the enemy patrol, the meteor tumble, the ring spin.
- The player runs a separate per-frame collision pass that visits the same
  arrays and tests its body parts against each object's `hitbox`. This is the
  only thing that connects "enemy in the world" to "player took damage."
- Boss-fight ("all-range") levels use a different layout: instead of a list
  that streams entries in as the player advances forward, the entire arena is
  loaded up front into a separate `gScenery360[200]` array, and the player
  flies freely inside it.

If you remember just one sentence: **the world is a small handful of fixed-size
arrays of structs, the per-frame loop walks them, and `obj.pos` is where each
thing is.** Everything else is bookkeeping around that.

---

## 2. The world inventory: six active-world arrays

All declared in `include/sf64context.h:232–241`:

```c
extern Scenery   gScenery[50];     // static map geometry: arches, walls, mountains, buildings
extern Sprite    gSprites[40];     // small billboarded scenery: poles, trees, smoke
extern Actor     gActors[60];      // enemies, teammates, missiles, AI-driven props
extern Boss      gBosses[4];       // boss entities (multi-part bosses use multiple slots)
extern Item      gItems[20];       // pickups: rings, bombs, lasers, 1ups, checkpoints, warps
extern Effect    gEffects[100];    // explosions, smoke, projectile trails, hit marks
extern PlayerShot gPlayerShots[16];// the player's own lasers and bombs in flight
```

Plus two specialised collections:

```c
extern Scenery360* gScenery360;    // the all-range arena layout (200 slots, heap-allocated)
extern ObjectInit* gLevelObjects;  // the sorted spawn-entry list for the current level
```

These arrays hold every *active* thing in the world right now. There is no
"level mesh" beyond what is in `gScenery`, no live-enemy list beyond
`gActors`, no shadow item list hidden somewhere else. (Things further ahead
that the player hasn't reached yet are not in these arrays — they wait in
`gLevelObjects`, see section 4. But everything currently alive in the
world is here.)

The sizes are deliberate caps. The level designers had to stay under 60
simultaneous active enemies, 100 simultaneous effect particles, 20 pickups in
flight at once, and so on. When a slot is unused its `status` field is
`OBJ_FREE` (value `0`). When it holds a real object, `status` is `OBJ_INIT`,
`OBJ_ACTIVE`, or `OBJ_DYING`. Status is the predicate for "is this slot
real?" — you'll see `if (obj.status == OBJ_FREE) continue;` everywhere.

---

## 3. The base `Object` struct and the six wrappers

Every entry in the six arrays starts with the same 0x1C-byte header,
`Object` (`include/sf64object.h:136`):

```c
typedef struct Object {
    /* 0x00 */ u8    status;   // OBJ_FREE / OBJ_INIT / OBJ_ACTIVE / OBJ_DYING
    /* 0x02 */ u16   id;       // an ObjectId enum value (which kind of thing this is)
    /* 0x04 */ Vec3f pos;      // world position (X right, Y up, Z negative-forward)
    /* 0x10 */ Vec3f rot;      // rotation in degrees, XYZ
} Object;
```

That's it for the base. Every wrapper struct embeds an `Object obj;` at
offset 0, then an `ObjectInfo info;` (the per-class behaviour table — see
section 5), then per-class fields:

| Wrapper       | Size   | Notable extra fields |
|---------------|-------:|----------------------|
| `Scenery`     | 0x80   | `state`, `dmgType`, `dmgPart`, `vel`, `sfxSource[3]` (for breakable scenery) |
| `Sprite`      | 0x4C   | `sceneryId`, `destroy`, `toLeft` (for camera-facing decoration — sprites are 2D images that always orient toward the camera regardless of viewing angle) |
| `Actor`       | 0x2F4  | `health`, `dmgType`, `dmgPart`, `damage`, `vel`, `aiType`, `aiIndex`, `sfxSource[3]`, plus generic per-instance work buffers: `iwork[25]` (each slot is `uintptr_t` — an integer type wide enough to hold a pointer, so the same slot can store either an int or a pointer depending on what the actor's logic needs), `fwork[30]` (`f32`), `vwork[30]` (`Vec3f`) |
| `Boss`        | 0x408  | similar to `Actor` but bigger work buffers (`swork[40]` of `s16`, `fwork[50]`, `vwork[50]`) |
| `Item`        | 0x6C   | `state`, `collected`, `playerNum`, `width`, `sfxSource[3]` |
| `Effect`      | 0x8C   | `vel`, `state`, `scale1`, `scale2`, `sfxSource[3]` |

Two things to internalise:

- **`obj.pos` is the authoritative world position for every kind of entity.**
  An accessibility cue that wants to know where the boss is reads
  `gBosses[0].obj.pos`. Where the closest enemy is, walks `gActors[]` and
  reads `actor->obj.pos`. There is no separate "logical" position for any of
  these — except a couple of edge cases (Macbeth train cars store an extra Y
  offset in `actor->fwork[8]` and an extra X offset in `actor->fwork[25]`
  that are added to `obj.pos` at collision time).
- **`obj.id` tells you what kind of thing this is.** It indexes a giant enum
  `ObjectId` in `include/sf64object.h:317` that lists every possible thing the
  game can spawn — every named scenery piece, every enemy class, every item
  type. The enum is sliced into ranges (scenery, sprite, actor, boss, item,
  effect, env), and `OBJ_*_START` / `OBJ_*_MAX` sentinels mark the boundaries.
  This is what you check to ask "is this a boulder or a 1-up?".

`Actor` is worth a special note: it is the most overloaded slot in the game.
It holds normal enemies, but also AllRange-mode teammates and Star Wolf
fighters, missiles, the team's Arwings flying alongside in cutscenes, event
scripts (`OBJ_ACTOR_EVENT`, which runs a small bytecode sequence that can
spawn other actors), and pure cutscene props. The `aiType` field discriminates
roles within AllRange / event modes (the `AllRangeAi` enum at
`sf64object.h:775`). For accessibility purposes you will usually filter by
`obj.id` first to decide what the actor *is*, and ignore the AI type unless
you specifically care about who's shooting at you.

---

## 4. The level as a list of spawn entries

A level is a C array of these:

```c
typedef struct {
    /* 0x00 */ f32   zPos1;   // distance ahead along the path where this should spawn
    /* 0x04 */ s16   zPos2;   // fine Z sub-offset
    /* 0x06 */ s16   xPos;    // lateral offset (X) from the path centre
    /* 0x08 */ s16   yPos;    // vertical offset (Y) from the path centre
    /* 0x0A */ Vec3s rot;     // initial rotation
    /* 0x10 */ s16   id;      // ObjectId — what kind of thing to spawn
} ObjectInit;                  // size = 0x14
```

The arrays are generated from the YAML descriptors in
`assets/yaml/us/rev0/ast_<level>.yaml` by the Torch tool, into headers under
`src/assets/` that get included by `fox_enmy.c` and `fox_play.c`. They are
indexed in a single dispatch table `gLevelObjectInits[]` defined at
`src/engine/fox_enmy.c:27`, keyed by `LevelId`. Some levels have multiple
phases that swap to a different array partway through (e.g. Meteo, Sector X,
Venom Andross — see `fox_enmy.c:557–566`).

Entries in this array are **sorted by `zPos1`** — increasing distance from
the level start. That single fact is what enables the streaming loop:

### The streaming loop

`Object_LoadLevelObjects()` (`fox_enmy.c:548`) runs every frame. It maintains a
cursor `gObjectLoadIndex` into `gLevelObjects`. Each frame it walks forward
from the cursor, calling `Object_Load()` (`fox_enmy.c:436`) on each entry,
**stopping as soon as the entry's `zPos1` is more than 200 units ahead of
`gPathProgress`**. So at any given time, only the next ~200 units of the
level have been "lit up." Anything further ahead is still data sleeping in
the array. Anything behind has either been spawned (and now lives in one of
the active arrays) or has been culled out of view as the player flew past.

`Object_Load()` looks at the entry's `id`, decides which active array it
belongs to (by enum range), scans that array for an `OBJ_FREE` slot, and
calls the type-specific `*_Load()` (`Scenery_Load`, `Sprite_Load`,
`Actor_Load`, `Boss_Load`, `Item_Load` at lines 213–283; effects loaded
this way are handled by an inline switch inside `Object_Load` itself rather
than a dedicated `Effect_Load` function). All of them follow the same
template: zero the struct, set `status = OBJ_INIT`, copy X/Y/rot from the
`ObjectInit`, and compute the world Z as `-(zPos1) + (-3000.0f + zPos2)`.
The `-3000.0f` constant places newly spawned objects 3000 units in front of
the player's current position, so the player has time to react before flying
through them. (Derivation: `gPathProgress` ≈ `-player->trueZpos`, and
`obj.pos.z = -gPathProgress - 3000` puts the object 3000 units more
negative — i.e. 3000 units further "ahead" of — the player.)

That minus sign on `zPos1` is the **first piece of coordinate-system
confusion** to internalise. The world Z axis points *backwards*: the player
flies in the negative-Z direction, so something that is "ahead" of the player
has a *more negative* Z value. The `ObjectInit` stores its position as a
positive "distance from the start of the level," and the load step flips the
sign to convert it into a world-space Z.

### Streaming → active world is a one-way trip

Once an entry has been spawned out of `gLevelObjects` into (say) `gActors`,
the original `ObjectInit` entry is not consulted again. The active actor
moves on its own, lives, dies, and the slot is freed back to `OBJ_FREE`. If
the player then flies backwards (e.g. via a U-turn), the streaming loop does
*not* re-spawn earlier entries — once consumed, they are gone for the rest of
the level. This is why losing a checkpoint sends you back to the *start of
the level* with everything reloaded from scratch.

### Other ways things appear

Not every spawn comes from `gLevelObjects`. There are three other paths:

- **`Game_SpawnActor(ObjectId)`** (`fox_game.c:630`) — what game code calls
  to create an enemy, missile, or effect dynamically (e.g. a boss
  spawning a missile, an enemy dropping an item).
- **Item drops** — when an enemy dies, its `itemDrop` field (an `ItemDrop`
  enum like `DROP_SILVER_RING`, `DROP_BOMB`, `DROP_1UP`) tells `Item_Load` to
  spawn a corresponding `OBJ_ITEM_*` at the enemy's death position.
- **Versus item spawns** — `Play_SpawnVsItem` (`fox_play.c:7063`) places
  pickups at fixed scenery positions in versus mode.

For accessibility purposes the distinction matters because items "appearing
suddenly mid-air after an enemy dies" come from the second path, while items
"sitting in space ahead of you" come from the first. Both end up in the same
`gItems[20]` array.

---

## 5. The behaviour table: `ObjectInfo` and `gObjectInfo[]`

After the `obj` header, every wrapper has an `ObjectInfo info;` field. This is
not unique per instance — it is **copied from a master per-`ObjectId` table**
at load time. The master table is `gObjectInfo[]` in
`src/engine/fox_edata_info.c:118`, indexed by `ObjectId`. (Note:
`fox_edata_info.c` is in the `EXCLUDE` list of the CMake glob in
`CMakeLists.txt`. It is not compiled as a standalone unit; it is included
from `fox_edata.c` via `#include "fox_edata_info.c"`. The data is real and
live, just laid out this way for historical decomp reasons.)

The fields most relevant to accessibility work, with their roles:

```c
typedef struct ObjectInfo {
    union { ObjectFunc draw; Gfx* dList; }; // how to render it (function or display list)
    u8        drawType;        // 0 = use dList, !=0 = call draw fn. (Shares storage with the union — an N64 decomp artefact; in practice the engine reads `drawType` to choose the path, then accesses the union accordingly.)
    ObjectFunc action;         // per-frame action callback (NULL for inert scenery)
    f32*      hitbox;          // pointer to the hitbox spec (see section 7)
    f32       cullDistance;    // beyond this, the engine stops drawing it
    u8        damage;          // damage dealt to player on collision
    f32       targetOffset;    // y offset for lock-on cursor (0.0 = untargetable)
    u8        bonus;           // hits awarded when killed (>1 = "hit+" bonus)
} ObjectInfo;                  // size 0x24
```

(Three partially-understood fields — `unk_14`, `unk_16`, `unk_19` — are
omitted from this excerpt for clarity; see `include/sf64object.h:145` for the
full definition with their offsets. They affect death behaviour and
camera-related billboarding but are not used by accessibility code.)

Two fields are particularly important for accessibility work:

- **`hitbox`** is what the collision system reads to decide if the player has
  touched this object. If it points to `gNoHitbox` (just `{ 0.0f }`), the
  object is purely visual and cannot be collided with — useful to filter out
  decorative scenery.
- **`targetOffset == 0.0f` means the object cannot be lock-on targeted**.
  Most damage-dealing things are targetable. This is a quick proxy for "is
  this thing a meaningful enemy?" vs "is this just background dressing?"

The `damage` field is also a usable hazard signal: anything with `damage > 0`
will hurt the player on contact.

Hitbox literals are defined right above `gObjectInfo[]` in
`fox_edata_info.c`. There are reusable cubes (`gCubeHitbox100`,
`gCubeHitbox150`, `gCubeHitbox200`, `gCubeHitbox400`) shared by many object
types, and per-object specials (e.g. `aWzGateHitbox` for the Warp Zone gate is
4 records: one per pillar plus the cross-bar).

---

## 6. The per-frame update loop

The top-level game-frame entry point is `Play_Main()` (`fox_play.c:7088`).
For normal gameplay it calls `Play_Update()` (`fox_play.c:7018`), which runs:

1. Screen fade bookkeeping (`Play_UpdateFillScreen`).
2. Team shield damage for Falco/Peppy/Slippy.
3. **Player update** (`Player_Update` per camera) — input, physics, **and the
   player's collision pass** (player vs. world).
4. **`Object_Update()`** — the master world iterator (see below).
5. `PlayerShot_UpdateAll()` — the player's lasers and bombs in flight.
6. `BonusText_Update()` — floating "+10" / "GREAT!" / "1UP" score text.
7. Camera update and shake.
8. `Play_UpdateLevel()` — level-specific per-frame logic (Solar heat damage,
   Macbeth signal switching, dynamic water surface, etc.).

`Object_Update()` (`fox_enmy.c:3128`) is the master world iterator. In
on-rails mode it first calls `Object_LoadLevelObjects()` to stream new
entries, then iterates each active array in turn:

```
gScenery[50]   → Scenery_Update(scenery)   for each non-FREE slot
gScenery360[]  → only the few animated 360 types
gSprites[40]   → Sprite_Update(sprite)
gBosses[4]     → Boss_Update(boss)
gActors[60]    → Actor_Update(actor)
gItems[20]     → Item_Update(item)
gEffects[100]  → Effect_Update(effect)
TexturedLine_UpdateAll()
```

Every per-type update function follows the same shape (e.g.
`Actor_Update` at `fox_enmy.c:2854`):

```c
switch (this->obj.status) {
    case OBJ_INIT:    // first frame: do one-time setup, transition to OBJ_ACTIVE
    case OBJ_ACTIVE:  // every other frame: call info.action(&this->obj)
    case OBJ_DYING:   // playing death animation; will be set to OBJ_FREE soon
}
```

The per-frame "AI" or animation logic of each enemy lives behind
`info.action`. For accessibility we generally don't need to read those — we
only need to know an enemy *exists* and *where it is*. Both come from the
shared `obj.status` and `obj.pos`.

### The hooks the port has already inserted

This fork has already wrapped the relevant transitions in cancellable events
(see `src/port/hooks/list/ActorEvent.h`):

```c
DEFINE_EVENT(ObjectInitEvent,    ObjectEventType type; void* object;);
DEFINE_EVENT(ObjectUpdateEvent,  ObjectEventType type; void* object;);
DEFINE_EVENT(ObjectDestroyEvent, ObjectEventType type; void* object;);
```

with `ObjectEventType` distinguishing eight kinds: `OBJECT_TYPE_ACTOR`,
`OBJECT_TYPE_ACTOR_EVENT` (level-script "actors" that drive cutscenes and
mid-level scripted spawns — usually filter these out for accessibility cues
since they are not enemies or hazards), `OBJECT_TYPE_BOSS`,
`OBJECT_TYPE_SCENERY`, `OBJECT_TYPE_SCENERY360`, `OBJECT_TYPE_SPRITE`,
`OBJECT_TYPE_ITEM`, `OBJECT_TYPE_EFFECT`.

**Important: not every type fires every event.** The lifecycle events are
inserted in the per-type `*_Update` functions (`fox_enmy.c`), and only some
of those functions handle `OBJ_DYING`. The actual coverage:

| Type | Init | Update | Destroy |
|---|---|---|---|
| `OBJECT_TYPE_ACTOR` | yes (`fox_enmy.c:2856`) | yes (`:2867`) | yes (`:2877`) |
| `OBJECT_TYPE_BOSS` | yes (`:2914`) | yes (`:2923`) | yes (`:2933`) |
| `OBJECT_TYPE_SPRITE` | yes (`:2974`) | yes (`:2982`) | yes (`:2992`) |
| `OBJECT_TYPE_SCENERY` | yes (`:2951`) | yes (`:2960`) | **no** |
| `OBJECT_TYPE_ITEM` | yes (`:3011`) | yes (`:3020`) | **no** |
| `OBJECT_TYPE_EFFECT` | yes (`:3038`) | yes (`:3046`) | **no** |
| `OBJECT_TYPE_ACTOR_EVENT` | (no lifecycle events; only fires for draw events) | | |
| `OBJECT_TYPE_SCENERY360` | (no lifecycle events; only fires for draw events) | | |

So if you want to know "this scenery / item / effect was just removed,"
`ObjectDestroyEvent` will not help — its `*_Update` switch has no
`OBJ_DYING` case. For items, the practical workaround is to listen on
`ObjectUpdateEvent` and watch for `((Item*)object)->collected` becoming
non-zero (it's set on the contact frame and the slot stays `OBJ_ACTIVE`
into the next frame for its fly-to-player animation). For scenery and
effects, you'd need to track which slots you'd previously seen and notice
when one transitions back to `OBJ_FREE`.

One more quirk specific to **`OBJECT_TYPE_EFFECT`**: `Effect_Update`
(`fox_enmy.c:3031`) deliberately falls through from the `OBJ_INIT` case
into `OBJ_ACTIVE` in the same frame. So both `ObjectInitEvent` *and*
`ObjectUpdateEvent` fire on the first frame an effect exists. For every
other type, a freshly-spawned object fires `ObjectInitEvent` on frame *N*
and the first `ObjectUpdateEvent` on frame *N+1*. Most cue listeners won't
care, but if you write logic that assumes "by the time Update fires,
something Init already handled is done," that assumption breaks for
effects.

There is also a more specific item event in
`src/port/hooks/list/ItemEvent.h`:

```c
DEFINE_EVENT(ItemDropEvent, Item* item;);  // also CALL_CANCELLABLE_EVENT
```

`ItemDropEvent` fires whenever a new `Item` enters the world. The call sites
include `Item_Load` (level placement, `fox_enmy.c:281`); the stone-arch
ring-counter trigger that spawns an invisible `OBJ_ITEM_RING_CHECK` collider
(`Scenery_CoStoneArch_Init`, `fox_enmy.c:1075` — see section 9 about
filtering this); the enemy-death drop path (`fox_enmy.c:1730`); several
overlay-specific spawns (`fox_aq.c`, `fox_andross.c`, `fox_co.c`);
versus-mode spawns (`fox_play.c:7077`); and demo cutscene spawns
(`fox_demo.c:1585`). The enemy-drop site is the most common path for items
that appear unexpectedly mid-flight — that is where 1-ups, gold rings, and
silver rings show up after killing an enemy with `itemDrop` set.

**For accessibility cue work, registering listeners on `ObjectInitEvent` (or
`ItemDropEvent` for items) for the types you care about is the recommended
approach** — it is the same event-bus pattern that
`src/port/mods/Accessibility.c` already uses for screen-reader announcements
(see CLAUDE.md, "Recipe for adding a new announcement"). You don't need to
poll arrays once per frame just to learn that an enemy spawned; the engine
will hand it to you.

If you want a *continuous* per-frame stream (e.g. to update spatial-audio
positions every frame), `ObjectUpdateEvent` fires for every active object.
Filter by `type` and by `((Object*)object)->id` to narrow it down.

Two safety notes for accessibility listeners on these events:

- **All four — plus `ItemDropEvent` — are already `REGISTER_EVENT`'d** in
  `PortEnhancements_Register()`
  (`src/port/mods/PortEnhancements.c:464–471`). A new accessibility listener
  needs only a `REGISTER_LISTENER(...)` call — calling `REGISTER_EVENT`
  again would create a second, distinct event ID and the listener would
  never fire.
- **All five are cancellable.** They wrap real game-state transitions (e.g.
  the switch from `OBJ_INIT` to `OBJ_ACTIVE` happens *inside* the
  `CALL_CANCELLABLE_EVENT(ObjectInitEvent, ...)` block;
  `Object_SetInfo(&item->info, item->obj.id)` happens inside the
  `CALL_CANCELLABLE_EVENT(ItemDropEvent, ...)` block). Setting
  `event->cancelled = true` in your listener will *suppress the game's own
  initialisation / update / destruction / drop-setup logic for that
  object*, leaving it in a half-initialised state. Accessibility listeners
  must never cancel. The pattern is a simple early-return on the CVar gate,
  then read fields, then optionally emit a sound — never cancel.

---

## 7. Hitboxes and collision

Collision in Star Fox 64 is intentionally simple — there is no spatial
index (no octree, BVH, grid, or other "narrow down what can hit what"
structure) to filter candidate pairs. Every frame the player walks every
active-world array and tests its four body points against each occupied
slot's hitbox. With 50+40+60+4+20+100 ≈ 274 slots and most of them empty
at any moment, this brute-force pass is fast enough on the original N64,
and trivial on a modern PC.

### The hitbox format

`info.hitbox` is a flat `f32*` array with a slightly tricky layout. Element 0
is a count N (cast to int). What follows is N records, where the *normal*
record is **6 floats wide**:

```
[ z_offset ] [ z_size ] [ y_offset ] [ y_size ] [ x_offset ] [ x_size ]
```

Each `(offset, size)` pair describes the box extent on that axis (offset is
relative to the object's `obj.pos`; size is the *half*-extent, so a `size`
of 100.0 produces a box 200 units wide on that axis). A box like this is
"axis-aligned" — its faces run parallel to the world X/Y/Z axes, so no
rotation is implied.

The trick: special record variants are flagged by overloading the *first
float* of the record. The collision code reads `record[0]` and compares it
to a few sentinel values:

| `record[0]`    | Constant         | Meaning | Total record size |
|---------------:|------------------|---------|------------------:|
| any normal value | (none)         | Plain box. The first float *is* `z_offset`. | 6 floats |
| `200000.0`     | `HITBOX_ROTATED` | Followed by 3 rotation floats (xRot, yRot, zRot), then the 6 box floats. The box is rotated before testing. | 10 floats |
| `300000.0`     | `HITBOX_SHADOW`  | Followed by 6 box floats. No damage; just used for screen dimming under tall scenery. | 7 floats |
| `400000.0`     | `HITBOX_WHOOSH`  | Followed by 6 box floats. Triggers the near-miss whoosh sound rather than a hit. | 7 floats |

So plain boxes have no sentinel byte — the first float doubles as
`z_offset`. The sentinel values are chosen large enough (200000, 300000,
400000) that no real hitbox would ever happen to use them as a coordinate.

A simple example from `fox_edata_info.c:51`:

```c
f32 gCubeHitbox100[] = {
    1.0f, 0.0f, 50.0f, 0.0f, 50.0f, 0.0f, 50.0f,
};
```

This is "1 record, plain box centred on the object, 100 units wide on each
axis" (the size field is 50.0, the half-extent — so the full box is 100
units across in X, Y, and Z). The leading `1.0f` is the count, then six
floats of `(z_offset, z_size, y_offset, y_size, x_offset, x_size)`. Many
small enemies use this directly.

### The collision pass

All player collision is centralised in one large function in
`Player_Update`'s call tree (the function around `fox_play.c:1785`, which
calls `Player_UpdateHitbox` and then walks every array). The two lower-level
test functions are:

- **`Player_CheckHitboxCollision()`** (`fox_play.c:1258`) — tests up to four
  player hitpoints (`hit3`, `hit4`, `hit1`, `hit2` — body, tail, right wing,
  left wing) against each hitbox record. Returns 1–4 for which hitpoint hit,
  `-1` for shadow, `-2` for whoosh, `0` for no hit.
- **`Object_CheckHitboxCollision()`** (`fox_enmy.c:710`) — the shot-vs-world
  variant; same algorithm minus the whoosh handling.

Routing inside the main collision pass:

- **Ground**: a single Y comparison against `gGroundHeight + 13.0f`. No mesh.
  See section 8.
- **`gScenery[50]`**: most use the plain-box hitbox test directly. A small
  list of big shapes (Meteo molar rock, Fortuna mountains, Great Fox, base
  structures) instead use polygon-mesh tests via
  `Player_CheckPolyCollision()` (`fox_enmy.c:790`), routed through
  `func_col1_*` (`fox_col1.c`, integer mesh) or `func_col2_*` (`fox_col2.c`,
  float mesh). Polygon tests have a 1100-unit Manhattan XZ guard (a cheap
  early-out so the expensive triangle test only runs when the player is
  within 1100 units in either X or Z). There is also a Z-early-out for
  scenery: only objects with `obj.pos.z > player->trueZpos - 2000.0f` are
  considered — i.e. objects too far ahead of the player (more than ~2000
  units in the negative-Z forward direction) are skipped until the player
  closes the gap (`fox_play.c:1970`).
- **`gScenery360[]`**: in all-range mode. Iterates all 200 slots with an
  explicit radius check (1100 units normally; 4000 in Sector Y / Venom
  Andross).
- **`gBosses[4]`**: poly test for Great Fox / boss bases, plain box
  otherwise.
- **`gActors[60]`**: special-cased for molar rock (poly), event SY ship
  (poly), big meteor (sphere), Macbeth train cars (plain box with
  `fwork`-derived position offset). Everything else: standard plain box.
- **`gItems[20]`**: `Player_CheckItemCollect()` (`fox_play.c:1656`) — same
  box test, but on hit it sets `item->collected = 1` and `item->playerNum =
  N` rather than dealing damage.

For an accessibility mod, **you do not need to re-implement any of this**.
The collision system is for the player's body. You read `obj.pos` and the
player position and compute distances yourself, with whatever metric and
range you want (typically a sphere / radius check is plenty).

---

## 8. Ground, water, and "terrain"

There is essentially **no terrain mesh** in this game.

- **`gGroundHeight`** is a single `f32` global (`include/sf64context.h:28`),
  set per level. For most planet levels it is `0.0f`. For space levels and
  Aquas it is `-25000.0f` (effectively unreachable). For Venom Andross it is
  forced to `-25000.0f` mid-level to disable the ground.
- The visual ground you see scrolling beneath you is a **single flat
  textured polygon** drawn by `Background_DrawGround()` (`fox_bg.c:1320`) at
  `y = -3.0f` relative to the path centre, scrolled with `gPathTexScroll`.
- **`gGroundType`** (a `GroundType` enum at `sf64level.h:39`) and
  **`gGroundSurface`** (`SURFACE_GRASS / SURFACE_ROCK / SURFACE_WATER`) are
  flags read at level init. They control visual style and what hitting the
  ground does — splash on water, damage on rock, etc.
- **`gWaterLevel`** (`f32`) is the equivalent for Aquas; the Blue Marine
  submarine is bounded by it.

The single exception is **Titania**, which has a real heightmap. `Ground_Init`
is called with a tile size at `fox_play.c:2922`, and `Ground_801B6AEC` /
`Ground_801B6E20` query the height at a given `(x, z + gPathProgress)`
position. This is the only level where "the ground" is more than a plane.

Solar's lava and Zoness's water surface use a 17×17 dynamic vertex grid
(`Play_UpdateDynaFloor`, `fox_play.c:79`) for the wavy animation, but
collision against them is still effectively flat-plane.

For accessibility purposes: do not look for "what's under the player" by
sampling a heightmap. The hazard is rarely the ground itself; it's individual
scenery / actor objects that happen to sit on the ground. Treat
`gGroundHeight` and `gWaterLevel` as global constants for the current level.

---

## 9. Items and pickups

The full list of pickup `ObjectId`s lives at `sf64object.h:641`:

| ObjectId                  | Effect |
|---------------------------|--------|
| `OBJ_ITEM_LASERS`         | Upgrade laser strength (single → twin → hyper). Wing repair if a wing is broken. |
| `OBJ_ITEM_BOMB`           | Add one to `gBombCount[playerNum]`. |
| `OBJ_ITEM_SILVER_RING`    | `+32` shields. |
| `OBJ_ITEM_GOLD_RING`      | `gGoldRingCount[]++`; awards a 1-up at count 6. |
| `OBJ_ITEM_1UP`            | `gLifeCount[playerNum]++`. |
| `OBJ_ITEM_SILVER_STAR`    | Big shield ring (`+128` shields). |
| `OBJ_ITEM_WING_REPAIR`    | Restore broken wing(s). |
| `OBJ_ITEM_CHECKPOINT`     | Set respawn checkpoint. |
| `OBJ_ITEM_METEO_WARP`     | Trigger warp-zone transition (Meteo / Sector Y). |
| `OBJ_ITEM_TRAINING_RING`  | Training-mode ring for time-trial. |
| `OBJ_ITEM_RING_CHECK`     | Invisible counter-trigger that increments `gRingPassCount` when the player passes through a ring formation. Not a visible pickup. |
| `OBJ_ITEM_PATH_*`         | Invisible "path-change" items in all-range / multi-path levels (turn left, turn right, split, etc.). |

All of them spawn into `gItems[20]`, run their action callback once per
frame, and are picked up via the same `Player_CheckItemCollect()` pass. The
action functions are in `fox_enmy.c`:

- `ItemPickup_Update` (`fox_enmy.c:2227`) — bombs, lasers.
- `ItemLasers_Update` (`fox_enmy.c:2288`) — wraps the above with the wing-repair
  substitution.
- `ItemSupplyRing_Update` (`fox_enmy.c:2300`) — silver rings, gold rings,
  silver stars.

The pickup happens in two stages. Frame *N*: the player collision pass calls
`Player_CheckItemCollect()`, which sets `item->collected = 1` (this is the
moment of physical contact). Frame *N+1*: the item's action function notices
`collected` is set, applies the effect to the player, and starts the
"fly-to-player" death animation (a state machine driven by `item->state`
inside the action function).

For accessibility cue work, the cleanest hook for "an item appeared" is the
existing `ItemDropEvent`, which fires at every spawn site (level placement,
ring formations, enemy drops, versus, cutscenes — see section 6). For "an
item was picked up," **`ObjectDestroyEvent` will not work** — the
`Item_Update` switch has no `OBJ_DYING` case, so that event never fires for
items. The correct path is `ObjectUpdateEvent` filtered to
`OBJECT_TYPE_ITEM`, watching for `((Item*)object)->collected` becoming
non-zero. Note the timing: this fires on frame *N+1* — one frame *after*
the player physically touched the item — because that is when the game
itself reads the flag and runs its post-collection state machine. If you
specifically need the moment of contact rather than the moment the effect
is applied, you'd need a separate hook in `Player_CheckItemCollect`; for
cue purposes one frame's lag is inaudible.

Two families of items are worth flagging separately as **not visible
pickups**: the `OBJ_ITEM_PATH_*` group (invisible navigation hints used by
all-range and branching-path levels to rewrite the player's path; IDs
328–333) and `OBJ_ITEM_RING_CHECK` (ID 334, a transparent collision item
that simply increments `gRingPassCount` when the player flies through a
ring formation — see `ItemRingCheck_Update` at `fox_enmy.c:2501`). Both
have hitboxes and behave as items mechanically, but a blind player should
not get a "ring!" cue when crossing them.

The catch is that they sit *between* the real pickup IDs in the enum —
`OBJ_ITEM_1UP` is 335, `OBJ_ITEM_GOLD_RING` is 336, `OBJ_ITEM_WING_REPAIR`
is 337, `OBJ_ITEM_TRAINING_RING` is 338. A naive `obj.id <
OBJ_ITEM_PATH_SPLIT_X` filter would silently drop those four real pickups.
The correct filter is either an explicit allow-list of the IDs you care
about, or an explicit exclusion of the non-visual range:

```c
if (obj->id >= OBJ_ITEM_PATH_SPLIT_X && obj->id <= OBJ_ITEM_RING_CHECK)
    return;
```

(`OBJ_ITEM_RING_CHECK` is 334, immediately after `OBJ_ITEM_PATH_TURN_DOWN`
at 333, so this single contiguous range covers both invisible families.)

---

## 10. Coordinates and how to filter "what's near the player"

### Coordinate convention

- **X**: positive right.
- **Y**: positive up.
- **Z**: positive **backward** — objects ahead of the player have *more
  negative* Z. The player flies in the negative-Z direction.
- All distances are in arbitrary "world units." Hitbox sizes give a feel:
  100 units ≈ a small enemy; 1000 units is large; 5000 units is the SFX-pan
  clamp limit. Empirically, "near" the player for cue purposes is on the
  order of 1000–4000 units.

### Player position fields (`include/sf64player.h:170`)

The player struct has a couple of position-ish fields that **don't all mean
what their names suggest**:

- **`gPlayer[0].pos.x`** — the lateral (X) position. Plain.
- **`gPlayer[0].pos.y`** — the altitude (Y). Plain.
- **`gPlayer[0].pos.z`** — *not* the Arwing's actual Z. This is the position
  along the path scroll, which stays near zero in on-rails mode.
- **`gPlayer[0].trueZpos`** — **this is the actual world Z of the Arwing.**
  This is what you compare against `obj.pos.z`.
- **`gPlayer[0].cam.eye`** (`Vec3f`) — the camera position in world space.
  Used by the SFX panning code.
- **`gPlayer[0].sfxSource[3]`** — the camera-space position the audio engine
  uses when playing player sounds.

So a "distance from player to object" calculation in world space is:

```c
f32 dx = obj.pos.x - gPlayer[0].pos.x;
f32 dy = obj.pos.y - gPlayer[0].pos.y;
f32 dz = obj.pos.z - gPlayer[0].trueZpos;
f32 dist = sqrtf(dx*dx + dy*dy + dz*dz);
```

This pattern is exactly what the existing collision code uses (e.g.
`fox_play.c:2155`).

### World-space → camera-space (for the audio system)

If you want to turn a world position into something `Audio_PlaySfx` can pan,
the converter already exists: **`Object_SetSfxSourceToPos(f32* sfxSrc,
Vec3f* worldPos)`** at `fox_edisplay.c:1578`. See `docs/audio-system.md`
section 4 for details. In short: pass it your three-float scratch buffer and
the world position; it does the camera transform and clamping for you.

### "Active Z window" in on-rails mode

In on-rails mode, the only objects the engine considers "real" at any given
moment live roughly in the Z range
`[player->trueZpos - 2000, player->trueZpos + ~200]` — anything earlier has
been despawned, anything later hasn't streamed in yet. So an
on-rails-mode cue scanner does not need to filter aggressively by Z; the
engine has already done it for you. You can iterate `gActors[]` /
`gItems[]` etc. and trust that everything you see is plausibly nearby.

In all-range mode the entire arena is loaded at once into `gScenery360[]`
and `gActors[]`, so range filtering on your end is essential.

---

## 11. Cookbook: writing an accessibility audio cue

The general shape, mirroring the screen-reader pattern in
`src/port/mods/Accessibility.c`:

1. **Decide what triggers the cue.** A discrete event ("a missile lock just
   acquired me," "a 1-up just spawned in front of me") is best handled by
   registering on the relevant existing event:
   - `ObjectInitEvent` filtered by `type` and `((Object*)object)->id` for "a
     thing of this kind appeared."
   - `ItemDropEvent` for items spawned by enemy drops.
   - `ObjectDestroyEvent` for "a thing of this kind was killed / removed."
   - `ObjectUpdateEvent` for "this thing changed state mid-life" (you'll
     usually want to compare against your own previous-frame snapshot to
     detect the transition).

2. **For continuous proximity cues** (e.g. "the closest hazard is on your
   left"), register on `GamePostUpdateEvent` (declared in
   `src/port/hooks/list/EngineEvent.h`, fires once per game frame after
   world update) and walk `gActors[]` / `gScenery[]` yourself, comparing
   `obj.pos` to `gPlayer[0]`. Filter:
   - `obj.status == OBJ_ACTIVE` (skip `OBJ_FREE`, `OBJ_INIT`, `OBJ_DYING`).
   - `obj.id` matches the kind of thing you're announcing.
   - For actor events, skip `type == OBJECT_TYPE_ACTOR_EVENT` — those are
     level scripts, not gameplay entities.
   - Optionally `info.damage > 0` for "actually a hazard," or
     `info.targetOffset > 0.0f` for "actually a meaningful enemy." Note
     that `info` is a per-class shared snapshot (copied from `gObjectInfo[]`
     at load), so the same `info.damage` value applies to every instance of
     that `ObjectId`.

3. **Use `REGISTER_LISTENER`, not `REGISTER_EVENT`.** All the lifecycle
   events listed above (`ObjectInitEvent`, `ObjectUpdateEvent`,
   `ObjectDestroyEvent`, `ItemDropEvent`) and `GamePostUpdateEvent` are
   already registered in `PortEnhancements_Register()`
   (`src/port/mods/PortEnhancements.c:435`). Calling `REGISTER_EVENT(X)` a
   second time creates a *different* event ID; your listener would attach
   to that and never fire. Just `REGISTER_LISTENER(EventName, MyCallback,
   EVENT_PRIORITY_NORMAL)` from `Accessibility_Init()`.

4. **Gate at fire time, not at registration.** Put `if
   (!Accessibility_IsAudioCuesEnabled()) return;` (or whatever CVar gate
   you're using) at the top of the listener callback — mirroring the
   `Accessibility_IsScreenReaderEnabled()` check used in the existing
   menu-event listeners in `src/port/mods/Accessibility.c`. This lets the
   user toggle the feature at runtime.

5. **Never cancel a lifecycle event.** Do not set `event->cancelled = true`
   from your listener — that would suppress the game's own per-object
   init/update/destroy logic for that object. Read fields, optionally play
   a sound, return.

6. **Convert to camera space and emit a positional SFX.** Use
   `Object_SetSfxSourceToPos(sfxSrc, &obj->pos)` for the conversion (see
   `docs/audio-system.md` section 4), then play via the macro:
   `AUDIO_PLAY_SFX(sfxId, sfxSrc, token)` from `include/sfx.h`. The
   third argument is a `u8` instance token, not flags — see
   `docs/audio-system.md` sections 3 and 5 for SFX IDs, ranges, the bank
   model, and how the token works.

7. **Don't poll the world for things the engine will tell you.** If there's
   already an event for the transition you care about, use it — that's the
   pattern this fork has settled on (see CLAUDE.md, "Event-driven
   announcements").

---

## 12. Cheat-sheet: where everything lives

| Concern | File:line / Symbol |
|---|---|
| All struct definitions (Object, Actor, Boss, Scenery, Item, Effect, Sprite, Hitbox, ObjectInit, ObjectInfo, ObjectId enum, ItemDrop enum) | `include/sf64object.h` |
| Global active-world arrays | `include/sf64context.h:232–241` |
| Player struct (pos, trueZpos, cam.eye, hit1–hit4, sfxSource) | `include/sf64player.h:169` |
| Level / scene / planet enums, GroundType, GroundSurface | `include/sf64level.h` |
| Per-level spawn-list dispatch table | `gLevelObjectInits[]` — `src/engine/fox_enmy.c:27` |
| Per-`ObjectId` behaviour table (hitbox, action, damage, targetOffset) | `gObjectInfo[]` — `src/engine/fox_edata_info.c:118`; hitbox literals begin at line 48 |
| Streaming loop + Z-window | `Object_LoadLevelObjects` — `src/engine/fox_enmy.c:548` |
| Spawn dispatch by ObjectId range | `Object_Load` — `src/engine/fox_enmy.c:436` |
| Per-type slot-finders | `Scenery_Load` / `Sprite_Load` / `Actor_Load` / `Boss_Load` / `Item_Load` — `src/engine/fox_enmy.c:213–283` (effects loaded from `gLevelObjects` are handled inline by `Object_Load`'s switch, not a dedicated function) |
| Master per-frame world iterator | `Object_Update` — `src/engine/fox_enmy.c:3128` |
| Per-type per-frame update (where the lifecycle hooks fire) | `Actor_Update` (`fox_enmy.c:2813`), `Boss_Update` (`:2889`), `Scenery_Update` (`:2944`), `Sprite_Update` (`:2971`), `Item_Update` (`:3001`), `Effect_Update` (`:3031`) |
| Top-level game frame | `Play_Main` / `Play_Update` — `src/engine/fox_play.c:7088 / 7018` |
| Player collision pass (player vs. world) | starting around `src/engine/fox_play.c:1785`, with helpers at `:1258` (hitbox) and `:1656` (item) |
| Scenery polygon collision tables | `src/engine/fox_col1.c`, `src/engine/fox_col2.c` |
| World→camera-space SFX position converter | `Object_SetSfxSourceToPos` — `src/engine/fox_edisplay.c:1578` |
| Existing port-side object-lifecycle events | `src/port/hooks/list/ActorEvent.h` (`ObjectInitEvent`, `ObjectUpdateEvent`, `ObjectDestroyEvent`) |
| Existing port-side item-drop event | `src/port/hooks/list/ItemEvent.h` (`ItemDropEvent`) |
| Port-side events umbrella include | `src/port/hooks/Events.h` |
| Generated per-level `ObjectInit` arrays (gitignored) | `src/assets/...` (built by Torch from `assets/yaml/<region>/<rev>/ast_*.yaml`) |
| Audio side of cues | `docs/audio-system.md` |
