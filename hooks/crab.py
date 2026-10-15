#!/usr/bin/env python3
"""Push a state to the crab puck over USB serial.

Two ways in:
    crab.py <state> [detail]     send a state directly
    crab.py --hook               read a Claude Code hook event on stdin

States: idle working approval done error asleep

Never raises and never blocks for long: a missing or busy port is a no-op, so
an unplugged puck can't wedge a hook.
"""

import glob
import json
import os
import sys

STATES = {"idle", "working", "approval", "done", "error", "asleep"}
BAUD = 115200

CLAUDE_DIR = os.path.join(os.path.expanduser("~"), ".claude")
SETTINGS = os.path.join(CLAUDE_DIR, "settings.json")

# Only the tail of a transcript is parsed. They reach tens of megabytes and a
# hook that reads the whole thing on every tool call is a hook you'd rip out.
TAIL_BYTES = 256 * 1024


# USB bridges fitted to T-Display boards: CH9102 on recent ones, CP2104 on
# older ones. Generic CH340 (1a86:7523) is left out on purpose - it is on half
# the Arduino clones out there, and the puck would happily write status lines
# at someone's 3D printer. Set CRAB_PORT to use a board on any other bridge.
BRIDGES = {(0x1A86, 0x55D4), (0x10C4, 0xEA60)}


def find_port():
    """Prefer an explicit CRAB_PORT, else sniff for the T-Display's bridge."""
    if os.environ.get("CRAB_PORT"):
        return os.environ["CRAB_PORT"]
    try:
        from serial.tools import list_ports

        for p in list_ports.comports():
            if (p.vid, p.pid) in BRIDGES:
                return p.device
    except Exception:
        pass
    return None


def send(state, detail="", model="", used=0, limit=0, echo=False):
    try:
        import serial
    except ImportError:
        return False

    line = "{}|{}|{}|{}|{}\n".format(
        state, detail.replace("|", " ")[:40], model.replace("|", " ")[:18],
        int(used), int(limit)
    )
    try:
        port = find_port()
        if not port:
            if echo:
                sys.stderr.write("crab: no puck found (set CRAB_PORT)\n")
            return False
        ser = serial.Serial()
        ser.port = port
        ser.baudrate = BAUD
        # Keep both handshake lines low across open(): the CH9102 auto-reset
        # circuit yanks EN otherwise and the puck reboots on every hook.
        ser.dtr = False
        ser.rts = False
        ser.timeout = 0.4
        ser.write_timeout = 0.4
        ser.open()
        ser.write(line.encode("ascii", "replace"))
        ser.flush()
        if echo:
            sys.stderr.write(ser.readline().decode(errors="replace"))
        ser.close()
        return True
    except Exception as exc:
        if echo:
            sys.stderr.write("crab: {}\n".format(exc))
        return False


def truncate(text, n=38):
    # plain dots, not an ellipsis: the display font is ASCII only
    text = " ".join(str(text).split())
    return text if len(text) <= n else text[: n - 3] + "..."


def short_model(model_id):
    """claude-opus-5 -> opus 5, claude-haiku-4-5-20251001 -> haiku 4.5"""
    if not model_id:
        return ""
    name = str(model_id)
    for prefix in ("us.anthropic.", "anthropic.", "claude-"):
        if name.startswith(prefix):
            name = name[len(prefix):]
    parts = [p for p in name.split("-") if not (p.isdigit() and len(p) == 8)]
    if parts and parts[-1].startswith("v") and parts[-1][1:].isdigit():
        parts = parts[:-1]
    # legacy ids put the version first: 3-7-sonnet -> sonnet 3.7
    if len(parts) >= 3 and parts[0].isdigit() and parts[1].isdigit():
        parts = parts[2:] + [parts[0] + "." + parts[1]]
    elif len(parts) >= 3 and parts[1].isdigit() and parts[2].isdigit():
        parts = [parts[0], parts[1] + "." + parts[2]] + parts[3:]
    return " ".join(parts)[:18]


def context_limit():
    """The [1m] suffix on the configured model means a 1M window."""
    try:
        with open(SETTINGS, encoding="utf-8") as fh:
            if "[1m]" in str(json.load(fh).get("model", "")):
                return 1000000
    except Exception:
        pass
    return 200000


def newest_transcript():
    pattern = os.path.join(CLAUDE_DIR, "projects", "*", "*.jsonl")
    files = glob.glob(pattern)
    return max(files, key=os.path.getmtime) if files else None


def read_usage(path):
    """Model and context size from the last assistant turn in a transcript."""
    if not path or not os.path.exists(path):
        return "", 0
    model, used = "", 0
    try:
        size = os.path.getsize(path)
        with open(path, "rb") as fh:
            if size > TAIL_BYTES:
                fh.seek(size - TAIL_BYTES)
                fh.readline()  # drop the partial line we landed in
            for raw in fh:
                try:
                    d = json.loads(raw.decode("utf-8", "replace"))
                except Exception:
                    continue
                if d.get("type") != "assistant":
                    continue
                msg = d.get("message") or {}
                usage = msg.get("usage") or {}
                if msg.get("model"):
                    model = msg["model"]
                total = (
                    usage.get("input_tokens", 0)
                    + usage.get("cache_creation_input_tokens", 0)
                    + usage.get("cache_read_input_tokens", 0)
                )
                if total:
                    used = total
    except Exception:
        pass
    return short_model(model), used


def from_hook(event):
    """Map a Claude Code hook payload onto a puck state."""
    name = event.get("hook_event_name", "")
    tool = event.get("tool_name", "")

    if name == "SessionStart":
        return "idle", "session started"
    if name == "UserPromptSubmit":
        return "working", truncate(event.get("prompt", "thinking"))
    if name == "PreToolUse":
        return "working", truncate(tool or "running a tool")
    if name == "PostToolUse":
        resp = event.get("tool_response") or {}
        if isinstance(resp, dict) and resp.get("success") is False:
            return "error", truncate("{} failed".format(tool or "tool"))
        return "working", truncate(tool or "working")
    if name == "Notification":
        return "approval", truncate(event.get("message", "needs you"))
    if name in ("Stop", "SubagentStop"):
        return "done", "your turn"
    if name == "PreCompact":
        return "working", "compacting context"
    if name == "SessionEnd":
        return "asleep", "session ended"
    return None, None


def main():
    args = sys.argv[1:]

    if args and args[0] == "--hook":
        try:
            event = json.load(sys.stdin)
        except Exception:
            return 0
        state, detail = from_hook(event)
        if state:
            model, used = read_usage(event.get("transcript_path"))
            limit = context_limit()
            send(state, detail, model, used, max(limit, used))
        return 0

    if not args or args[0] in ("-h", "--help"):
        sys.stderr.write(__doc__)
        return 0

    state = args[0].lower()
    if state not in STATES:
        sys.stderr.write("crab: unknown state {}\n".format(state))
        return 1

    model, used = read_usage(newest_transcript())
    limit = context_limit()
    ok = send(state, " ".join(args[1:]), model, used, max(limit, used), echo=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
