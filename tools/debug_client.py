#!/usr/bin/env python3
"""Client for the Starship debug control server (src/port/mods/debugserver).

The server listens on 127.0.0.1:7764 (CVars gDebugServer.Enabled / gDebugServer.Port,
off by default) and speaks a line protocol: one command per request line, one JSON object per
response line — {"status": "ok", "output": "..."} or {"status": "error", "error": "..."}.
Dump commands (health, player, objects) put compact JSON *as a string* in "output"; this
client parses and pretty-prints it (--raw to see the output string verbatim).

Usage:
    python tools/debug_client.py health
    python tools/debug_client.py objects actors shots
    python tools/debug_client.py set gDebugPause 1
    python tools/debug_client.py --wait 30            # poll until the game answers
    python tools/debug_client.py --repl               # one command per stdin line

Exit codes: 0 ok, 1 command error, 2 could not connect / connection lost.

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
import socket
import sys
import time

DEFAULT_PORT = 7764


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
    parser.add_argument("command", nargs="*", help="command and arguments to send")
    args = parser.parse_args()

    if args.wait is not None:
        envelope = wait_for_server(args.host, args.port, args.wait)
        if envelope is None:
            print("debug server did not answer within %gs" % args.wait, file=sys.stderr)
            return 2
        return print_response(envelope, raw=args.raw)

    if not args.repl and not args.command:
        parser.error("no command given (or use --wait / --repl)")

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
                code = print_response(conn.request(tokens), raw=args.raw)
            return code
        return print_response(conn.request(args.command), raw=args.raw)
    except (ConnectionError, OSError) as e:
        print(str(e), file=sys.stderr)
        return 2
    finally:
        conn.close()


if __name__ == "__main__":
    sys.exit(main())
