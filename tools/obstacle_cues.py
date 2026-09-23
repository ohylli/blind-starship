#!/usr/bin/env python3
"""Report what the directional obstacle accessibility cues are currently sounding for.

Asks the running game's debug server (tools/debug_client.py, CVar gDebugServer.Enabled)
for the cue state and prints, per cue:

  below  - the vertical clearance to the object's top and the cue level
  above  - the vertical clearance to the object's bottom and the cue level
  side   - for each of left/right, the sideways clearance to the object's near face,
           the forward gap, and the stereo pan the cue is rendering

The cues run on rails and in solo all-range. On rails the directions are fixed to the
track; in all-range they are relative to the heading (aim yaw and pitch, bank ignored), so
"above" means toward the canopy and "ahead" is along the aim line. Each report names the
mode it was measured in.

When a cue is silent it says why, using the cue's own policy gates rather than guessing.

    python tools/obstacle_cues.py [--port N] [--cue below|above|side|all] [--watch SECONDS]

--cue picks one cue (default: all three). --watch repeats the report every SECONDS until
Ctrl+C, printing only when the report text changes, so it can be left running in a
terminal (or under a screen reader) while flying. Exit codes: 0 reported, 2 could not
connect, 1 unexpected reply.
"""

import argparse
import json
import sys
import time

from debug_client import DEFAULT_PORT, DebugConnection

# Cue id on the server -> (label, direction blocks inside its policy, level or pan signal).
# The side cue renders two voices (left/right) whose closeness is a stereo pan; above and
# below render one voice each whose closeness is the loudness ("level").
CUES = {
    "below": ("ObstacleBelow", ["below"], "level"),
    "above": ("ObstacleAbove", ["above"], "level"),
    "side": ("ObstacleSide", ["left", "right"], "pan"),
}


def fetch_cues(conn):
    envelope = conn.request(["cues"])
    if envelope.get("status") != "ok":
        raise RuntimeError(envelope.get("error", "unknown error"))
    payload = json.loads(envelope["output"])
    return {cue.get("id"): cue for cue in payload.get("cues", [])}


# Policy gates in the order the cue checks them -> what a closed one means.
GATE_REASONS = {
    "enabled": "audio cues are off",
    "obstacleEnabled": "the obstacle cues toggle is off",
    "modeOk": "not in an on-rails or solo all-range level",
    "control": "the player does not have control",
    "aimValid": "all-range heading not valid (not flying by the aim composition)",
    "noManeuver": "U-turn or somersault in progress",
}


def mode_name(policy):
    return "all-range, heading-relative" if policy.get("allRange") else "on rails"


def describe_inactive(name, policy, directions):
    """Explain a silent cue from its policy block, most specific reason first."""
    if policy is None:
        return "%s cue inactive: no policy tick recorded yet (not in a level?)" % name
    if not policy.get("fresh", True):
        return "%s cue inactive: last policy tick is stale (game paused or not in play)" % name
    gates = policy.get("gates", {})
    ordered = [g for g in GATE_REASONS if g in gates] + sorted(g for g in gates if g not in GATE_REASONS)
    closed = [gate for gate in ordered if not gates[gate]]
    if closed:
        return "%s cue inactive: %s" % (name, "; ".join(GATE_REASONS.get(g, g + " gate closed") for g in closed))
    candidates = sum((policy.get(d) or {}).get("candidates", 0) for d in directions)
    if candidates == 0:
        where = " or ".join(directions) if len(directions) > 1 else directions[0]
        return "%s cue inactive (%s): nothing %s you within range (%s)" % (
            name,
            mode_name(policy),
            where,
            scan_summary(policy),
        )
    return "%s cue inactive (%s): %d candidate(s) but none chosen" % (name, mode_name(policy), candidates)


def scan_summary(policy):
    """Why boxes were not candidates: how many are in play and how many were set aside."""
    parts = ["%d obstacle boxes in play" % policy.get("scan", {}).get("boxes", 0)]
    claimed = policy.get("aheadClaimed", 0)
    if claimed:
        parts.append("%d left to the ahead cue" % claimed)
    walk_claimed = policy.get("terrainWalk", {}).get("claimed", 0)
    if walk_claimed:
        parts.append("%d terrain box(es) partly left to the ahead cue" % walk_claimed)
    fighters = policy.get("fightersSkipped", 0)
    if fighters:
        parts.append("%d wingmate/ally craft skipped" % fighters)
    return ", ".join(parts)


def describe_target(direction, target, signal, cue):
    """One direction's winner: clearance, signal (level or pan), placement, identity."""
    clearance = target.get("clear")
    lines = []
    if clearance is None:
        lines.append("%s: active, clearance not reported" % direction)
    elif direction == "below" and target.get("fromTerrainWalk"):
        lines.append("below: terrain surface is %.0f units below you" % clearance)
    elif direction == "below":
        lines.append("below: object top is %.0f units below you" % clearance)
    elif direction == "above":
        lines.append("above: object bottom is %.0f units above you" % clearance)
    else:
        lines.append("%s: object's near face is %.0f units to your %s" % (direction, clearance, direction))

    if signal == "pan":
        pan = target.get("pan")
        if pan is not None:
            # The pan magnitude is the closeness signal, reversed: a far object sits hard to
            # its side and a near one drifts toward the center (down to the pan floor).
            lines.append("  pan: %.2f (%.0f%% toward the %s; nearer objects pan closer to center)"
                         % (pan, abs(pan) * 100.0, direction))
    else:
        # The voice's raw gain also carries the cue's fixed baseGain/gainBoost, so report
        # loudness relative to the cue at full level: the closeness level times the slider.
        level = target.get("level", 0.0)
        volume = cue.get("volume", 1.0)
        lines.append("  cue volume: %.0f%% of full (level %.2f x slider %.2f)" % (level * volume * 100.0, level, volume))

    if target.get("upcoming"):
        # A terrain-walk winner is a surface sample, which is what `upcoming` is about; its
        # `gap` is the whole terrain box's near face, which goes negative once over the box.
        gap = target.get("sampleT") if target.get("fromTerrainWalk") else target.get("gap")
        lines.append(
            "  not yet alongside: %.0f units ahead along your course" % gap if gap is not None else "  not yet alongside"
        )
    elif target.get("fromTerrainWalk"):
        lines.append("  directly beneath you")
    else:
        lines.append("  already alongside it")
    array = target.get("array", {})
    lines.append(
        "  object: %s slot %s, object id %s, %s record"
        % (array.get("name", "?"), target.get("slot", "?"), target.get("objId", "?"), target.get("recordKind", "?"))
    )
    return lines


def describe(name, cue):
    _, directions, signal = CUES[name]
    policy = cue.get("policy")
    active = [d for d in directions if ((policy or {}).get(d) or {}).get("active")]
    if not active:
        return describe_inactive(name, policy, directions)
    lines = ["%s cue active (%s)" % (name, mode_name(policy))]
    for direction in active:
        lines.extend(describe_target(direction, policy[direction], signal, cue))
    return "\n".join(lines)


def report(conn, names):
    cues = fetch_cues(conn)
    parts = []
    for name in names:
        cue_id = CUES[name][0]
        if cue_id not in cues:
            raise RuntimeError("no cue with id %r in the server's reply" % cue_id)
        parts.append(describe(name, cues[cue_id]))
    return "\n".join(parts)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--cue", choices=sorted(CUES) + ["all"], default="all", help="which cue to report")
    parser.add_argument("--watch", type=float, metavar="SECONDS", help="repeat, printing on change")
    args = parser.parse_args()
    names = ["below", "above", "side"] if args.cue == "all" else [args.cue]

    try:
        conn = DebugConnection("127.0.0.1", args.port)
    except OSError as e:
        print("could not connect to the debug server on port %d: %s" % (args.port, e), file=sys.stderr)
        return 2

    last = None
    try:
        while True:
            try:
                text = report(conn, names)
            except (RuntimeError, KeyError, ValueError, ConnectionError) as e:
                print("unexpected reply: %s" % e, file=sys.stderr)
                return 1
            if text != last:
                print(text)
                print()
                last = text
            if args.watch is None:
                return 0
            time.sleep(args.watch)
    except KeyboardInterrupt:
        return 0
    finally:
        conn.close()


if __name__ == "__main__":
    sys.exit(main())
