#!/usr/bin/env python3
"""Static level-content manifest generator for Starship (Star Fox 64 PC port).

Reads the extracted asset archive (sf64.o2r, a plain zip) together with data
tables checked into the repo, and emits one manifest per level describing
everything the level can spawn: placed objects (scenery, obstacles, items)
from the ObjectInit spawn lists, and event-scripted enemies decoded from the
level event scripts. No ROM or build required beyond the extracted sf64.o2r.

The output is derived from the user's ROM and must stay out of git
(the default output directory `level_manifests/` is gitignored).

Data sources and formats (all traced from the game/port/Torch sources):
- ObjectInit resources (SF64:OBJECT_INIT): 0x40-byte resource header, then
  u32 count and count x 18-byte records {f32 zPos1; s16 zPos2, xPos, yPos,
  rot.x, rot.y, rot.z, id}.  Ids >= ACTOR_EVENT_ID (1000) are event actors:
  aiType = id - 1000 indexes the level's event-script table
  (src/engine/fox_enmy.c Object_Load / ActorEvent_Load).
- Script resources (SF64:SCRIPT): u32 count, then count x u64 CRC64 hashes of
  the child command-list archive paths "<entry>_cmd_<i>"
  (tools/Torch/src/factories/sf64/ScriptFactory.cpp ScriptBinaryExporter).
- Command lists (SF64:SCRIPT_CMD): u32 cmdCount, then cmdCount pairs of u16
  (word0, word1). opcode = (word0 >> 9) & 0x7F, arg1 = word0 & 0x1FF,
  arg2 = word1 (include/sf64event.h EV_OPC / EVENT_CMD).
- Script interpreter semantics from src/engine/fox_enmy2.c
  ActorEvent_ProcessScript / ActorEvent_TriggerBranch: jump targets < 200 are
  command indices within the same script (aiIndex = target * 2); targets
  >= 200 switch to script (target - 200) of the same level table.
- Object classification from gObjectInfo[] (src/engine/fox_edata_info.c) and
  sEventActorInfo[] (src/engine/fox_enmy2.c); enum names from
  include/sf64object.h and include/sf64event.h.
- Level -> asset mapping hardcoded below from gLevelObjectInits[] plus the
  phase-1 swaps in Object_LoadLevelObjects (src/engine/fox_enmy.c) and the
  gCurrentLevel switch in ActorEvent_ProcessScript (src/engine/fox_enmy2.c).
"""

import argparse
import collections
import json
import re
import struct
import sys
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

ACTOR_EVENT_ID = 1000
EV_CHANGE_AI = 200

# Opcodes the static walker must understand (include/sf64event.h EventOpcode).
EVOP_SET_TRIGGER = 96
EVOP_INIT_ACTOR = 104
EVOP_DROP_ITEM = 116
EVOP_LOOP = 126
EVOP_STOP_SCRIPT = 127

RES_OBJECT_INIT = 0x4F42494E  # 'OBIN'
RES_SCRIPT = 0x53435250       # 'SCRP'
RES_SCRIPT_CMD = 0x53434D44   # 'SCMD'

# ---------------------------------------------------------------------------
# Level table.
#
# ObjectInit arrays: gLevelObjectInits[] in src/engine/fox_enmy.c (indexed by
# LevelId) plus the gLevelPhase == 1 swaps in Object_LoadLevelObjects.
# Script tables: the gCurrentLevel switch in ActorEvent_ProcessScript
# (src/engine/fox_enmy2.c:1075).  Levels absent from that switch never load
# event scripts (their spawn lists contain no ids >= 1000).
# ---------------------------------------------------------------------------
LEVELS = [
    {
        "slug": "corneria", "name": "Corneria", "level_id": 0,
        "arrays": [("ast_corneria/aCoOnRailsLevelObjects", "on-rails")],
        "script": "ast_corneria/D_CO_603D9E8",
    },
    {
        "slug": "meteo", "name": "Meteo", "level_id": 1,
        "arrays": [("ast_meteo/D_ME_6026CC4", "phase 1"),
                   ("ast_meteo/D_ME_602B148", "phase 2")],
        "script": "ast_meteo/D_ME_602F3AC",
    },
    {
        "slug": "sector_x", "name": "Sector X", "level_id": 2,
        "arrays": [("ast_sector_x/D_SX_602A164", "phase 1"),
                   ("ast_sector_x/D_SX_602F18C", "phase 2")],
        "script": "ast_sector_x/D_SX_60320D0",
    },
    {
        "slug": "area_6", "name": "Area 6", "level_id": 3,
        "arrays": [("ast_area_6/D_A6_6023F64", "on-rails")],
        "script": "ast_area_6/D_A6_6027F50",
    },
    {
        "slug": "area_6_alt", "name": "Area 6 (alternate, LEVEL_UNK_4)", "level_id": 4,
        "arrays": [("ast_area_6/D_A6_60287A4", "on-rails")],
        "script": "ast_area_6/D_A6_60289FC",
    },
    {
        "slug": "sector_y", "name": "Sector Y", "level_id": 5,
        "arrays": [("ast_sector_y/D_SY_602E4F4", "on-rails")],
        "script": "ast_sector_y/D_SY_6032E18",
    },
    {
        "slug": "venom_1", "name": "Venom 1", "level_id": 6,
        "arrays": [("ast_venom_1/D_VE1_6007E74", "phase 1"),
                   ("ast_venom_1/D_VE1_6010088", "phase 2")],
        "script": "ast_venom_1/aVe1EventScript",
    },
    {
        "slug": "solar", "name": "Solar", "level_id": 7,
        "arrays": [("ast_solar/D_SO_601F234", "on-rails")],
        "script": "ast_solar/D_SO_6020DD0",
    },
    {
        "slug": "zoness", "name": "Zoness", "level_id": 8,
        "arrays": [("ast_zoness/D_ZO_6026714", "on-rails")],
        "script": "ast_zoness/D_ZO_602AAC0",
    },
    {
        "slug": "venom_andross", "name": "Venom (Andross route)", "level_id": 9,
        "arrays": [("ast_andross/D_ANDROSS_C035154", "phase 1"),
                   ("ast_andross/D_ANDROSS_C0356A4", "phase 2")],
        "script": "ast_andross/D_ANDROSS_C037E3C",
    },
    {
        "slug": "training", "name": "Training", "level_id": 10,
        "arrays": [("ast_training/D_TR_6006AA4", "on-rails")],
        "script": "ast_training/D_TR_6009B34",
    },
    {
        "slug": "macbeth", "name": "Macbeth", "level_id": 11,
        "arrays": [("ast_macbeth/D_MA_6031000", "on-rails")],
        "script": "ast_macbeth/D_MA_60381D8",
    },
    {
        "slug": "titania", "name": "Titania", "level_id": 12,
        "arrays": [("ast_titania/D_TI_6006C60", "on-rails")],
        "script": "ast_titania/D_TI_600631C",
    },
    {
        "slug": "aquas", "name": "Aquas", "level_id": 13,
        "arrays": [("ast_aquas/D_AQ_602E5C8", "on-rails")],
        "script": "ast_aquas/D_AQ_60308B8",
    },
    {
        "slug": "fortuna", "name": "Fortuna", "level_id": 14,
        "arrays": [("ast_fortuna/D_FO_600EAD4", "all-range")],
        "script": None,
    },
    {
        "slug": "katina", "name": "Katina", "level_id": 16,
        "arrays": [("ast_katina/D_KA_6011044", "all-range")],
        "script": None,
    },
    {
        "slug": "bolse", "name": "Bolse", "level_id": 17,
        "arrays": [("ast_bolse/D_BO_600FF74", "all-range")],
        "script": None,
    },
    {
        "slug": "sector_z", "name": "Sector Z", "level_id": 18,
        "arrays": [("ast_sector_z/D_SZ_6006EB4", "all-range")],
        "script": None,
    },
    {
        "slug": "venom_2", "name": "Venom 2", "level_id": 19,
        "arrays": [("ast_venom_2/D_VE2_6014D94", "all-range")],
        "script": None,
    },
    {
        # Stage arrays selected by gVersusStage / gVsMatchType in
        # Play_InitVsStage (src/engine/fox_play.c:349).
        "slug": "versus", "name": "Versus", "level_id": 20,
        "arrays": [("ast_versus/D_versus_302DE3C", "Corneria stage"),
                   ("ast_versus/D_versus_302E0E4", "Katina stage"),
                   ("ast_versus/D_versus_302E170", "Sector Z stage (point match)"),
                   ("ast_versus/D_versus_302E378", "Sector Z stage (time match)")],
        "script": None,
    },
]

# ---------------------------------------------------------------------------
# CRC64 (matches libultraship/src/utils/StrHash64.cpp and Torch's
# lib/strhash64: CRC-64/ECMA-182, MSB-first, init all-ones, no final xor).
# The table constant 0x42F0E1EBA9EA3693 at index 1 identifies the polynomial.
# resolve_hashes() verifies the implementation against the archive contents.
# ---------------------------------------------------------------------------
_CRC64_POLY = 0x42F0E1EBA9EA3693
_MASK64 = (1 << 64) - 1


def _make_crc_table():
    table = []
    for i in range(256):
        crc = i << 56
        for _ in range(8):
            if crc & (1 << 63):
                crc = ((crc << 1) ^ _CRC64_POLY) & _MASK64
            else:
                crc = (crc << 1) & _MASK64
        table.append(crc)
    return table


_CRC64_TABLE = _make_crc_table()


def crc64(text):
    crc = _MASK64
    for byte in text.encode("utf-8"):
        crc = (_CRC64_TABLE[((crc >> 56) ^ byte) & 0xFF] ^ (crc << 8)) & _MASK64
    return crc


# ---------------------------------------------------------------------------
# C source parsing (enum names and classification tables from checked-in code)
# ---------------------------------------------------------------------------
def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return text


def parse_enum(text, enum_name):
    """Return {value: name} for a C enum (auto-increment plus explicit `= n`)."""
    m = re.search(r"typedef enum " + enum_name + r"\s*\{(.*?)\}", text, re.S)
    if not m:
        raise SystemExit(f"enum {enum_name} not found")
    names = {}
    val = -1
    for token in strip_comments(m.group(1)).split(","):
        token = token.strip()
        if not token:
            continue
        mm = re.match(r"(\w+)(?:\s*=\s*(-?\w+))?$", token)
        if not mm:
            raise SystemExit(f"unparseable {enum_name} entry: {token!r}")
        name, explicit = mm.groups()
        val = int(explicit, 0) if explicit is not None else val + 1
        names[val] = name
    return names


def parse_struct_array(text, array_decl):
    """Parse a positional C struct-array initializer into per-entry field lists.

    `array_decl` is a regex matching the declaration up to (not including) the
    opening brace of the array initializer, e.g. r"ObjectInfo gObjectInfo\\[\\] =".
    Returns a list of entries; each entry is a list of field strings.
    """
    m = re.search(array_decl + r"\s*\{", text)
    if not m:
        raise SystemExit(f"array not found: {array_decl}")
    body = strip_comments(text[m.end():])
    entries = []
    depth = 0
    current = None
    for i, ch in enumerate(body):
        if ch == "{":
            depth += 1
            if depth == 1:
                current = []
                field = []
            else:
                field.append(ch)
        elif ch == "}":
            if depth == 0:
                break  # closing brace of the array itself
            depth -= 1
            if depth == 0:
                current.append("".join(field).strip())
                entries.append([f for f in current])
                current = None
            else:
                field.append(ch)
        elif depth >= 1:
            if ch == "," and depth == 1:
                current.append("".join(field).strip())
                field = []
            else:
                field.append(ch)
        elif ch == ";":
            break
    return entries


def parse_c_number(token, default=0.0):
    token = token.strip().rstrip("fF")
    try:
        return float(token)
    except ValueError:
        return default


def load_object_table(repo):
    """ObjectId names plus per-id classification from gObjectInfo[]."""
    header = (repo / "include/sf64object.h").read_text(encoding="utf-8", errors="replace")
    object_ids = parse_enum(header, "ObjectId")
    item_drops = parse_enum(header, "ItemDrop")

    edata = (repo / "src/engine/fox_edata_info.c").read_text(encoding="utf-8", errors="replace")
    entries = parse_struct_array(edata, r"ObjectInfo\s+gObjectInfo\[\]\s*=")
    info = {}
    for idx, fields in enumerate(entries):
        if len(fields) != 11:
            raise SystemExit(f"gObjectInfo[{idx}] has {len(fields)} fields, expected 11")
        hitbox = fields[3].split()[-1]
        info[idx] = {
            "hitbox": hitbox,
            "collidable": hitbox not in ("NULL", "gNoHitbox"),
            "damage": int(parse_c_number(fields[7])),
            "targetable": parse_c_number(fields[9]) != 0.0,
            "bonus": int(parse_c_number(fields[10])),
        }
    return object_ids, info, item_drops


def load_event_table(repo):
    """EventActorId names plus per-id data from sEventActorInfo[]."""
    header = (repo / "include/sf64event.h").read_text(encoding="utf-8", errors="replace")
    evids = parse_enum(header, "EventActorId")

    enmy2 = (repo / "src/engine/fox_enmy2.c").read_text(encoding="utf-8", errors="replace")
    entries = parse_struct_array(enmy2, r"EventActorInfo\s+sEventActorInfo\[\d*\]\s*=")
    info = {}
    for idx, fields in enumerate(entries):
        if len(fields) != 11:
            raise SystemExit(f"sEventActorInfo[{idx}] has {len(fields)} fields, expected 11")
        hitbox = fields[1].split()[-1]
        info[idx] = {
            "hitbox": hitbox,
            "collidable": hitbox not in ("NULL", "gNoHitbox"),
            "scale": parse_c_number(fields[2]),
            "targetable": parse_c_number(fields[9]) != 0.0,
            "bonus": int(parse_c_number(fields[10])),
        }
    return evids, info


# ---------------------------------------------------------------------------
# O2R archive reading
# ---------------------------------------------------------------------------
class Archive:
    def __init__(self, path):
        self.zip = zipfile.ZipFile(path)
        self.names = set(self.zip.namelist())
        self.by_hash = {}
        for name in self.names:
            key = name[:-5] if name.endswith(".meta") else name
            self.by_hash[crc64(key)] = key

    def read_resource(self, name, expected_type):
        data = self.zip.read(name)
        if len(data) < 0x44:
            raise SystemExit(f"{name}: resource too small ({len(data)} bytes)")
        endian = "<" if data[0] == 0 else ">"
        res_type = struct.unpack_from(endian + "I", data, 4)[0]
        if res_type != expected_type:
            raise SystemExit(
                f"{name}: resource type 0x{res_type:08X}, expected 0x{expected_type:08X}")
        return endian, data[0x40:]

    def read_object_init(self, name):
        endian, body = self.read_resource(name, RES_OBJECT_INIT)
        count = struct.unpack_from(endian + "I", body, 0)[0]
        records = []
        off = 4
        for _ in range(count):
            z1, z2, x, y, rx, ry, rz, oid = struct.unpack_from(endian + "fhhhhhhh", body, off)
            off += 18
            records.append({"z": z1, "z2": z2, "x": x, "y": y,
                            "rot": [rx, ry, rz], "id": oid})
        return records

    def read_script_table(self, name):
        """Return a list of command lists; index = aiType (script slot)."""
        endian, body = self.read_resource(name, RES_SCRIPT)
        count = struct.unpack_from(endian + "I", body, 0)[0]
        scripts = []
        for i in range(count):
            h = struct.unpack_from(endian + "Q", body, 4 + 8 * i)[0]
            child = self.by_hash.get(h)
            if child is None:
                raise SystemExit(
                    f"{name}: slot {i} hash {h:#018x} does not match any archive entry "
                    f"(CRC64 mismatch or corrupt archive)")
            scripts.append(self.read_script_cmds(child))
        return scripts

    def read_script_cmds(self, name):
        endian, body = self.read_resource(name, RES_SCRIPT_CMD)
        count = struct.unpack_from(endian + "I", body, 0)[0]
        return list(struct.iter_unpack(endian + "HH", body[4:4 + count * 4]))


# ---------------------------------------------------------------------------
# Static event-script walker
# ---------------------------------------------------------------------------
def walk_scripts(scripts, entry_slots):
    """Statically walk a level's script table from the given entry slots.

    Returns {entry_slot: {"identities": [(evid, health)], "drops": [drop_id],
    "scripts_visited": [slot...]}}.  Follows every possible jump: trigger and
    loop targets < 200 are command indices in the current script, targets
    >= 200 switch to script (target - 200).  Conditions are runtime state, so
    everything reachable is reported as "can spawn".
    """
    results = {}
    for entry in sorted(entry_slots):
        identities = []
        drops = set()
        seen_idents = set()
        visited = set()
        if entry >= len(scripts):
            results[entry] = {"error": f"slot {entry} out of range (table has {len(scripts)})"}
            continue
        stack = [(entry, 0)]
        while stack:
            slot, ci = stack.pop()
            if (slot, ci) in visited:
                continue
            visited.add((slot, ci))
            cmds = scripts[slot]
            if ci >= len(cmds):
                continue  # ran off the end; game data always ends in STOP_SCRIPT
            w0, w1 = cmds[ci]
            op = (w0 >> 9) & 0x7F
            arg1 = w0 & 0x1FF

            if op == EVOP_STOP_SCRIPT:
                continue
            if op == EVOP_INIT_ACTOR:
                if (w1, arg1) not in seen_idents:
                    seen_idents.add((w1, arg1))
                    identities.append((w1, arg1))  # (eventType, health)
            elif op == EVOP_DROP_ITEM:
                drops.add(w1)
            elif op in (EVOP_SET_TRIGGER, EVOP_LOOP):
                # SET_TRIGGER with condition EVC_NONE (0) never fires.
                if not (op == EVOP_SET_TRIGGER and w1 == 0):
                    if arg1 < EV_CHANGE_AI:
                        stack.append((slot, arg1))
                    else:
                        stack.append((arg1 - EV_CHANGE_AI, 0))
            stack.append((slot, ci + 1))

        results[entry] = {
            "identities": identities,
            "drops": sorted(drops),
            "scripts_visited": sorted({s for s, _ in visited}),
        }
    return results


# ---------------------------------------------------------------------------
# Manifest assembly
# ---------------------------------------------------------------------------
CATEGORY_ORDER = ["scenery", "sprite", "actor", "boss", "item", "effect", "other"]


def category_of(name):
    for cat in ("SCENERY", "SPRITE", "ACTOR", "BOSS", "ITEM", "EFFECT"):
        if name.startswith("OBJ_" + cat + "_"):
            return cat.lower()
    return "other"


def build_level(level, archive, object_ids, object_info, item_drops, evids, event_info):
    arrays = []
    event_slots = set()
    warnings = []
    for entry, phase in level["arrays"]:
        records = archive.read_object_init(entry)
        # Arrays end with an OBJ_INVALID (-1) terminator record (sometimes a
        # run of them as padding); the game's load loops stop at the first
        # id <= OBJ_INVALID (fox_enmy.c:627, fox_play.c:375).
        for i, rec in enumerate(records):
            if rec["id"] <= -1:
                if any(r["id"] > -1 for r in records[i:]):
                    warnings.append(
                        f"{entry}: non-terminator records found after the "
                        f"OBJ_INVALID at index {i}; ignoring them")
                records = records[:i]
                break
        for rec in records:
            oid = rec["id"]
            if oid >= ACTOR_EVENT_ID:
                rec["name"] = f"EVENT_{oid - ACTOR_EVENT_ID}"
                rec["category"] = "event"
                event_slots.add(oid - ACTOR_EVENT_ID)
            elif oid in object_ids:
                rec["name"] = object_ids[oid]
                rec["category"] = category_of(object_ids[oid])
            else:
                rec["name"] = f"UNKNOWN_{oid}"
                rec["category"] = "other"
                warnings.append(f"{entry}: unknown object id {oid}")
        arrays.append({"entry": entry, "phase": phase, "records": records})

    # Aggregate placed-object stats per type across all arrays of the level.
    types = {}
    for arr in arrays:
        for rec in arr["records"]:
            t = types.setdefault(rec["name"], {
                "name": rec["name"], "id": rec["id"], "category": rec["category"],
                "count": 0, "z_min": float("inf"), "z_max": float("-inf"),
                "y_min": float("inf"), "y_max": float("-inf"), "phases": set(),
            })
            t["count"] += 1
            t["z_min"] = min(t["z_min"], rec["z"])
            t["z_max"] = max(t["z_max"], rec["z"])
            t["y_min"] = min(t["y_min"], rec["y"])
            t["y_max"] = max(t["y_max"], rec["y"])
            t["phases"].add(arr["phase"])
    for t in types.values():
        t["phases"] = sorted(t["phases"])
        if t["id"] < ACTOR_EVENT_ID and t["id"] in object_info:
            t.update(object_info[t["id"]])

    # Event scripts.
    events = None
    if level["script"] is not None:
        scripts = archive.read_script_table(level["script"])
        walked = walk_scripts(scripts, event_slots)
        events = {"table": level["script"], "slot_count": len(scripts), "slots": {}}
        for slot, res in sorted(walked.items()):
            if "error" in res:
                warnings.append(f"{level['script']}: {res['error']}")
                continue
            idents = []
            for evid, health in res["identities"]:
                ident = {"evid": evid,
                         "name": evids.get(evid, f"EVID_{evid}"),
                         "health": health}
                if evid in event_info:
                    ident.update(event_info[evid])
                else:
                    ident["special"] = True  # >= EVID_200: hard-coded behaviour
                idents.append(ident)
            events["slots"][slot] = {
                "identities": idents,
                "drops": [item_drops.get(d, f"DROP_{d}") for d in res["drops"]],
                "scripts_visited": res["scripts_visited"],
            }
    elif event_slots:
        warnings.append(
            f"level has event entries {sorted(event_slots)} but no script table mapped")

    return {
        "slug": level["slug"], "name": level["name"], "level_id": level["level_id"],
        "arrays": arrays, "types": types, "events": events, "warnings": warnings,
    }


# ---------------------------------------------------------------------------
# Emitters
# ---------------------------------------------------------------------------
def fmt_range(lo, hi):
    if lo == hi:
        return f"{lo:.0f}"
    return f"{lo:.0f} to {hi:.0f}"


def hazard_line(t):
    bits = []
    if t.get("damage"):
        bits.append(f"contact damage {t['damage']}")
    bits.append("lock-on targetable" if t.get("targetable") else "not targetable")
    if t.get("bonus", 0) > 1:
        bits.append(f"worth {t['bonus']} hits")
    return ", ".join(bits)


def emit_markdown(data, out_path):
    lines = []
    add = lines.append
    add(f"# {data['name']} - level content manifest")
    add("")
    add("Generated by `tools/level_manifest.py` from `sf64.o2r`. This file is "
        "derived from ROM data - do not commit it.")
    add("")

    total = sum(len(a["records"]) for a in data["arrays"])
    add("## Overview")
    add("")
    parts = [f"`{a['entry']}` ({a['phase']}, {len(a['records'])} entries)"
             for a in data["arrays"]]
    add(f"This level's spawn data comes from {len(data['arrays'])} object list(s): "
        + "; ".join(parts) + f". Total {total} placed spawn entries.")
    all_z = [r["z"] for a in data["arrays"] for r in a["records"]]
    if all_z:
        add(f"Spawn positions run from distance {min(all_z):.0f} to {max(all_z):.0f} "
            "along the level path (zPos1: higher = further into the level; the game "
            "places the object at world z = -zPos1 - 3000 + zPos2).")
    add("")

    types = sorted(data["types"].values(), key=lambda t: -t["count"])
    hazards = [t for t in types if t["category"] != "event" and t.get("collidable")]
    decoration = [t for t in types if t["category"] != "event" and "hitbox" in t
                  and not t["collidable"] and t["category"] != "item"]
    unclassified = [t for t in types if t["category"] != "event" and "hitbox" not in t]
    items = [t for t in types if t["category"] == "item" and "hitbox" in t]
    event_types = [t for t in types if t["category"] == "event"]

    add("## Collidable objects (hazards, obstacles, shootable objects)")
    add("")
    if hazards:
        add("These placed objects have hitboxes, so the player can collide with "
            "them or shoot them:")
        add("")
        for t in hazards:
            add(f"- {t['name']} ({t['category']}): {t['count']} placed, "
                f"{hazard_line(t)}. Distance {fmt_range(t['z_min'], t['z_max'])}, "
                f"height {fmt_range(t['y_min'], t['y_max'])}."
                + (f" Phases: {', '.join(t['phases'])}." if len(data['arrays']) > 1 else ""))
    else:
        add("None - every placed object in this level is decoration, an item, "
            "or an event actor.")
    add("")

    add("## Items")
    add("")
    if items:
        for t in items:
            add(f"- {t['name']}: {t['count']} placed. "
                f"Distance {fmt_range(t['z_min'], t['z_max'])}.")
    else:
        add("No items are placed directly in the spawn list (items may still "
            "arrive as enemy drops - see below).")
    add("")

    add("## Decoration (no collision)")
    add("")
    if decoration:
        add("These have no hitbox and can be ignored for hazard cues:")
        add("")
        for t in decoration:
            add(f"- {t['name']} ({t['category']}): {t['count']} placed.")
    else:
        add("None.")
    add("")

    if unclassified:
        add("## Unclassified object ids")
        add("")
        add("These ids have no row in `gObjectInfo` (they are above the last "
            "classified id), so hazard data is unknown:")
        add("")
        for t in unclassified:
            add(f"- {t['name']} (id {t['id']}): {t['count']} placed. "
                f"Distance {fmt_range(t['z_min'], t['z_max'])}.")
        add("")

    add("## Event-scripted enemies")
    add("")
    if data["events"] is None:
        add("This level has no event-script table; enemies here are driven by "
            "level-specific code (all-range battle logic), not scripts.")
    else:
        used = data["events"]["slots"]
        add(f"The spawn lists reference {len(event_types)} distinct event entries "
            f"(ids 1000 and up), which index the script table "
            f"`{data['events']['table']}` ({data['events']['slot_count']} slots). "
            "Each entry below lists every enemy identity the script can "
            "statically reach - which one actually appears can depend on "
            "runtime conditions (player position, difficulty, teammates).")
        add("")
        roster = collections.Counter()
        for slot in sorted(used):
            info = used[slot]
            idents = info["identities"]
            if not idents:
                add(f"- Script slot {slot}: no INIT_ACTOR reachable "
                    "(pure control/ambience script).")
                continue
            names = []
            for ident in idents:
                roster[ident["name"]] += 1
                extra = f"health {ident['health']}"
                if ident.get("bonus", 0) > 1:
                    extra += f", worth {ident['bonus']} hits"
                if ident.get("special"):
                    extra += ", special-cased behaviour"
                names.append(f"{ident['name']} ({extra})")
            line = f"- Script slot {slot}: " + "; ".join(names) + "."
            if info["drops"]:
                line += " Can drop: " + ", ".join(info["drops"]) + "."
            add(line)
        add("")
        if roster:
            add("Deduplicated enemy roster for this level (name, and how many "
                "script slots can produce it):")
            add("")
            for name, n in roster.most_common():
                add(f"- {name}: reachable from {n} slot(s)")
    add("")

    if data["warnings"]:
        add("## Warnings")
        add("")
        for w in data["warnings"]:
            add(f"- {w}")
        add("")

    add("## Caveats")
    add("")
    add("This manifest covers the static spawn data only. Not included: objects "
        "spawned dynamically by game code (`Game_SpawnActor` calls, boss attacks), "
        "item drops hard-coded in C outside the DROP_ITEM script command, and "
        "enemies that transform into other object types on death (special cases "
        "in `fox_enmy2.c`). Script branch *conditions* are runtime state, so this "
        "lists everything that *can* spawn, not what will spawn on a given run.")
    add("")
    out_path.write_text("\n".join(lines), encoding="utf-8")


def emit_json(data, out_path):
    out_path.write_text(json.dumps(data, indent=1), encoding="utf-8")


def emit_index(all_data, out_dir):
    lines = ["# Level content manifests", "",
             "Generated by `tools/level_manifest.py` from `sf64.o2r` - ROM-derived, "
             "do not commit.", ""]
    for data in all_data:
        total = sum(len(a["records"]) for a in data["arrays"])
        hazards = sum(1 for t in data["types"].values()
                      if t["category"] != "event" and t.get("collidable"))
        n_events = len([t for t in data["types"].values() if t["category"] == "event"])
        ev = f"{n_events} event entries" if data["events"] else "no event scripts"
        lines.append(f"- [{data['name']}]({data['slug']}.md): {total} spawn entries, "
                     f"{hazards} collidable object types, {ev}.")
    lines.append("")
    (out_dir / "index.md").write_text("\n".join(lines), encoding="utf-8")


# ---------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--o2r", default=str(REPO / "sf64.o2r"),
                        help="path to sf64.o2r (default: repo root)")
    parser.add_argument("--out", default=str(REPO / "level_manifests"),
                        help="output directory (default: level_manifests/, gitignored)")
    parser.add_argument("--level", help="only this level slug (e.g. corneria)")
    args = parser.parse_args()

    object_ids, object_info, item_drops = load_object_table(REPO)
    evids, event_info = load_event_table(REPO)
    print(f"parsed {len(object_ids)} ObjectIds, {len(object_info)} gObjectInfo rows, "
          f"{len(event_info)} sEventActorInfo rows, {len(evids)} EVID names")

    archive = Archive(args.o2r)
    levels = [lv for lv in LEVELS if args.level in (None, lv["slug"])]
    if not levels:
        raise SystemExit(f"unknown level slug {args.level!r}; known: "
                         + ", ".join(lv["slug"] for lv in LEVELS))

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    all_data = []
    for level in levels:
        missing = [e for e, _ in level["arrays"] if e not in archive.names]
        if level["script"] and level["script"] not in archive.names:
            missing.append(level["script"])
        if missing:
            raise SystemExit(f"{level['slug']}: archive entries missing: {missing}")
        data = build_level(level, archive, object_ids, object_info,
                           item_drops, evids, event_info)
        emit_markdown(data, out_dir / f"{data['slug']}.md")
        emit_json(data, out_dir / f"{data['slug']}.json")
        all_data.append(data)
        total = sum(len(a["records"]) for a in data["arrays"])
        n_ev = len(data["events"]["slots"]) if data["events"] else 0
        print(f"{data['slug']}: {total} entries, {len(data['types'])} object types, "
              f"{n_ev} event slots" + (f", WARNINGS: {data['warnings']}"
                                       if data["warnings"] else ""))

    if args.level is None:
        emit_index(all_data, out_dir)
    print(f"wrote manifests to {out_dir}")


if __name__ == "__main__":
    main()
