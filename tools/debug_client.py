#!/usr/bin/env python3
"""Client for the Starship debug control server (src/port/mods/debugserver).

The server listens on 127.0.0.1:7764 (CVars gDebugServer.Enabled / gDebugServer.Port,
off by default) and speaks a line protocol: one command per request line, one JSON object per
response line — {"status": "ok", "output": "..."} or {"status": "error", "error": "..."}.
Dump commands (health, player, objects, cues) put compact JSON *as a string* in "output";
this client parses and pretty-prints it (--raw to see the output string verbatim).

Usage:
    python tools/debug_client.py health
    python tools/debug_client.py objects actors shots
    python tools/debug_client.py set gDebugPause 1
    python tools/debug_client.py warp corneria --no-intro --paused
    python tools/debug_client.py checkpoint-save big-asteroids --desc "asteroid cluster"
    python tools/debug_client.py warp --checkpoint big-asteroids --paused
    python tools/debug_client.py checkpoint-list
    python tools/debug_client.py --wait 30            # poll until the game answers
    python tools/debug_client.py --repl               # one command per stdin line

Named checkpoints: `checkpoint-save <id>` captures the current flight position from the
running game (server command `checkpoint`) into tools/checkpoints.json under the given
id; `warp --checkpoint <id>` expands the id into the stored level plus explicit
--at/--load/--ground arguments before sending, so the server never sees checkpoint names.
`checkpoint-list` / `checkpoint-delete <id>` manage the file offline. The file is
plain committed-friendly JSON — a shared vocabulary of documented test spots ("test in
meteo at big-asteroids"). --repl lines get the same warp expansion; the
checkpoint-save/list/delete client commands are one-shot only.

Exit codes: 0 ok, 1 command error — including a warp that answered but did not complete
(server-side timeout) and a step that ended early, so scripts can trust exit 0 —
2 could not connect / connection lost.

Server-side quoting: arguments are split on whitespace; double quotes group one token,
no escape sequences. Arguments containing spaces are re-quoted automatically here.

Quirks of the stock libultraship set/get commands (documented, not fixed):
- `set` infers the CVar type from the value text, and anything not starting with a digit
  — including negative numbers like -1 — becomes a *string* CVar.
- `get` on a missing CVar reports status ok with "Could not find variable" text, so test
  the output text, not the status, when probing for existence.
"""

import argparse
import json
import os
import socket
import sys
import time

DEFAULT_PORT = 7764
DEFAULT_CHECKPOINT_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "checkpoints.json")


class DebugConnection:
    def __init__(self, host, port, timeout=10.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.reader = self.sock.makefile("r", encoding="utf-8", newline="\n")

    def close(self):
        try:
            self.reader.close()
            self.sock.close()
        except OSError:
            pass

    def request(self, args):
        """Send one command (list of tokens), return the parsed response envelope."""
        line = " ".join(quote(a) for a in args)
        self.sock.sendall((line + "\n").encode("utf-8"))
        response = self.reader.readline()
        if not response:
            raise ConnectionError("server closed the connection")
        return json.loads(response)


def quote(token):
    if token == "" or any(c.isspace() for c in token):
        return '"' + token + '"'
    return token


def print_response(envelope, raw=False):
    """Render an envelope; return the exit code."""
    if envelope.get("status") == "ok":
        output = envelope.get("output", "")
        if not raw:
            try:
                print(json.dumps(json.loads(output), indent=2))
                return 0
            except (json.JSONDecodeError, TypeError):
                pass
        # Some commands (e.g. set) succeed silently; say so rather than print nothing.
        print(output if output else "ok")
        return 0
    print(envelope.get("error", "unknown error"), file=sys.stderr)
    return 1


def incomplete_note(tokens, envelope):
    """A warp that timed out (completed false) or a step that ended early still answers
    status ok; dig the failure out of the payload so scripts see it in the exit code —
    launch.ps1 relies on warp's exit 0 meaning "sitting in the level"."""
    if envelope.get("status") != "ok" or not tokens:
        return None
    try:
        payload = json.loads(envelope.get("output", ""))
    except (json.JSONDecodeError, TypeError):
        return None
    if not isinstance(payload, dict):
        return None
    if tokens[0] == "warp" and payload.get("completed") is False:
        return "warp did not complete: %s" % payload.get("note", "no reason reported")
    if tokens[0] == "step" and payload.get("note"):
        return "step ended early: %s" % payload["note"]
    return None


def run_command(conn, tokens, raw=False):
    """Send one server command, print the response, return the exit code."""
    envelope = conn.request(tokens)
    code = print_response(envelope, raw=raw)
    if code == 0:
        note = incomplete_note(tokens, envelope)
        if note:
            print(note, file=sys.stderr)
            code = 1
    return code


def load_checkpoints(path):
    try:
        # utf-8-sig: tolerate the BOM Windows editors (and PowerShell redirects) prepend.
        with open(path, "r", encoding="utf-8-sig") as f:
            data = json.load(f)
    except FileNotFoundError:
        return {}
    except json.JSONDecodeError as e:
        raise ValueError("checkpoint file %s is not valid JSON: %s" % (path, e))
    if not isinstance(data, dict):
        raise ValueError("checkpoint file %s is not a JSON object" % path)
    return data


def save_checkpoints(path, checkpoints):
    # Write-then-rename: a crash mid-write (or two clients saving at once) must not
    # truncate the store — it is gitignored, so there is no history to recover from.
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        json.dump(checkpoints, f, indent=2, sort_keys=True)
        f.write("\n")
    os.replace(tmp, path)


WARP_VALUE_FLAGS = ("--at", "--load", "--ground")


def expand_warp_command(tokens, checkpoint_file):
    """Resolve `warp --checkpoint <id>` into the stored level + explicit --at/--load/--ground."""
    if "--checkpoint" not in tokens:
        return tokens
    out = ["warp"]
    checkpoint_id = None
    has_level = False
    i = 1
    while i < len(tokens):
        t = tokens[i]
        if t == "--checkpoint":
            if i + 1 >= len(tokens):
                raise ValueError("--checkpoint needs an id")
            checkpoint_id = tokens[i + 1]
            i += 2
            continue
        if t in WARP_VALUE_FLAGS:
            raise ValueError("%s conflicts with --checkpoint (the checkpoint provides it)" % t)
        if not t.startswith("--"):
            has_level = True
        out.append(t)
        i += 1
    if has_level:
        raise ValueError("give either a level or --checkpoint, not both (the checkpoint stores its level)")
    checkpoints = load_checkpoints(checkpoint_file)
    if checkpoint_id not in checkpoints:
        known = ", ".join(sorted(checkpoints)) if checkpoints else "none saved"
        raise ValueError("unknown checkpoint id %r (known: %s)" % (checkpoint_id, known))
    entry = checkpoints[checkpoint_id]
    # The file is advertised as hand-editable; a malformed entry should fail like any
    # other usage error, not as a KeyError traceback.
    if not isinstance(entry, dict):
        raise ValueError("checkpoint %r in %s is not a JSON object" % (checkpoint_id, checkpoint_file))
    try:
        expanded = [str(entry["level"])]
        if entry.get("phase"):
            expanded.append(str(entry["phase"]))
        expanded += ["--at", str(entry["pathProgress"]), "--load", str(entry["objectLoadIndex"])]
    except KeyError as e:
        raise ValueError("checkpoint %r in %s is missing key %s" % (checkpoint_id, checkpoint_file, e))
    if "groundSurface" in entry:
        expanded += ["--ground", str(entry["groundSurface"])]
    return out[:1] + expanded + out[1:]


def cmd_checkpoint_save(conn, tokens, checkpoint_file, raw=False):
    ident = None
    desc = None
    force = False
    i = 1
    while i < len(tokens):
        t = tokens[i]
        if t == "--desc":
            if i + 1 >= len(tokens):
                raise ValueError("--desc needs a value")
            desc = tokens[i + 1]
            i += 2
        elif t == "--force":
            force = True
            i += 1
        elif not t.startswith("--") and ident is None:
            ident = t
            i += 1
        else:
            raise ValueError("unexpected argument: %s" % t)
    if not ident:
        raise ValueError("usage: checkpoint-save <id> [--desc TEXT] [--force]")
    checkpoints = load_checkpoints(checkpoint_file)
    if ident in checkpoints and not force:
        raise ValueError("checkpoint %r already exists (--force to overwrite)" % ident)
    envelope = conn.request(["checkpoint"])
    if envelope.get("status") != "ok":
        return print_response(envelope, raw=raw)
    data = json.loads(envelope["output"])
    entry = {
        "level": data["level"],
        "levelName": data["levelName"],
        # Warp-zone alternate routes (Meteo, Sector X) are on-rails phase 1 with their own
        # object tables; replaying their captures into phase 0 restores the wrong corridor.
        "phase": data.get("phase", 0),
        "pathProgress": data["pathProgress"],
        "objectLoadIndex": data["objectLoadIndex"],
        "groundSurface": data["groundSurface"],
    }
    if desc:
        entry["description"] = desc
    checkpoints[ident] = entry
    save_checkpoints(checkpoint_file, checkpoints)
    print("saved %s: %s, path progress %.1f, load index %d (%s)"
          % (ident, data["levelName"], data["pathProgress"], data["objectLoadIndex"], checkpoint_file))
    return 0


def cmd_checkpoint_list(checkpoint_file):
    checkpoints = load_checkpoints(checkpoint_file)
    if not checkpoints:
        print("no checkpoints saved in %s" % checkpoint_file)
        return 0
    for ident in sorted(checkpoints):
        e = checkpoints[ident]
        line = "%s: %s, path progress %.1f, load index %s" % (
            ident, e.get("levelName", e.get("level")), e.get("pathProgress", 0.0), e.get("objectLoadIndex", "?"))
        if e.get("phase"):
            line += ", phase %s" % e["phase"]
        if e.get("description"):
            line += " - " + e["description"]
        print(line)
    return 0


def cmd_checkpoint_delete(tokens, checkpoint_file):
    if len(tokens) != 2 or tokens[1].startswith("--"):
        raise ValueError("usage: checkpoint-delete <id>")
    checkpoints = load_checkpoints(checkpoint_file)
    if tokens[1] not in checkpoints:
        raise ValueError("unknown checkpoint id %r" % tokens[1])
    del checkpoints[tokens[1]]
    save_checkpoints(checkpoint_file, checkpoints)
    print("deleted %s" % tokens[1])
    return 0


def wait_for_server(host, port, timeout_s):
    """Poll connect + health until the server answers or the timeout passes."""
    deadline = time.monotonic() + timeout_s
    while True:
        try:
            conn = DebugConnection(host, port, timeout=2.0)
            try:
                envelope = conn.request(["health"])
                if envelope.get("status") == "ok":
                    return envelope
            finally:
                conn.close()
        except (OSError, ValueError):
            pass
        if time.monotonic() >= deadline:
            return None
        time.sleep(0.25)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--raw", action="store_true",
                        help="print the output string verbatim instead of pretty-printing inner JSON")
    parser.add_argument("--wait", type=float, metavar="SECONDS",
                        help="poll until the server answers health, then print it")
    parser.add_argument("--repl", action="store_true",
                        help="read command lines from stdin, print one response line each")
    parser.add_argument("--checkpoints-file", default=DEFAULT_CHECKPOINT_FILE, metavar="PATH",
                        help="named-checkpoint store used by checkpoint-save/list/delete and warp --checkpoint")
    # REMAINDER so command options like `warp corneria --paused` pass through untouched;
    # client options (--raw, --wait, ...) must come before the command.
    parser.add_argument("command", nargs=argparse.REMAINDER, help="command and arguments to send")
    args = parser.parse_args()

    if args.wait is not None:
        envelope = wait_for_server(args.host, args.port, args.wait)
        if envelope is None:
            print("debug server did not answer within %gs" % args.wait, file=sys.stderr)
            return 2
        return print_response(envelope, raw=args.raw)

    if not args.repl and not args.command:
        parser.error("no command given (or use --wait / --repl)")

    command = list(args.command)
    try:
        if command and command[0] == "checkpoint-list":
            return cmd_checkpoint_list(args.checkpoints_file)
        if command and command[0] == "checkpoint-delete":
            return cmd_checkpoint_delete(command, args.checkpoints_file)
        if command and command[0] == "warp":
            command = expand_warp_command(command, args.checkpoints_file)
    except ValueError as e:
        print(str(e), file=sys.stderr)
        return 1

    try:
        conn = DebugConnection(args.host, args.port)
    except OSError as e:
        print("could not connect to %s:%d: %s" % (args.host, args.port, e), file=sys.stderr)
        return 2

    try:
        if args.repl:
            code = 0
            for line in sys.stdin:
                # Windows PowerShell prepends a UTF-8 BOM when piping text.
                tokens = line.lstrip(chr(0xFEFF)).split()
                if not tokens:
                    continue
                try:
                    if tokens[0] == "warp":
                        tokens = expand_warp_command(tokens, args.checkpoints_file)
                except ValueError as e:
                    print(str(e), file=sys.stderr)
                    code = 1
                    continue
                # Same per-command timeout as the one-shot path: warp waits for the level
                # to come up, a long step waits for its frames — both can exceed 10 s.
                conn.sock.settimeout(120.0 if tokens[0] in ("warp", "step") else 10.0)
                code = run_command(conn, tokens, raw=args.raw)
            return code
        if command[0] in ("warp", "step"):
            conn.sock.settimeout(120.0)
        if command[0] == "checkpoint-save":
            try:
                return cmd_checkpoint_save(conn, command, args.checkpoints_file, raw=args.raw)
            except ValueError as e:
                print(str(e), file=sys.stderr)
                return 1
        return run_command(conn, command, raw=args.raw)
    except (ConnectionError, OSError) as e:
        print(str(e), file=sys.stderr)
        return 2
    finally:
        conn.close()


if __name__ == "__main__":
    sys.exit(main())
