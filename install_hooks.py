#!/usr/bin/env python3
"""Merge the crab puck hooks into ~/.claude/settings.json (backing it up first).

Run with --remove to take them back out again.
"""

import json
import os
import shutil
import sys

SETTINGS = os.path.join(os.path.expanduser("~"), ".claude", "settings.json")
PY = sys.executable
SENDER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hooks", "crab.py")
COMMAND = '"{}" "{}" --hook'.format(PY, SENDER)

# One entry per event. PostToolUse is deliberately absent: it would double the
# hook latency on every single tool call to say something PreToolUse already said.
EVENTS = {
    "SessionStart": None,
    "UserPromptSubmit": None,
    "PreToolUse": "*",
    "Notification": None,
    "Stop": None,
    "SessionEnd": None,
}


def block(matcher):
    entry = {"hooks": [{"type": "command", "command": COMMAND}]}
    if matcher:
        entry["matcher"] = matcher
    return entry


def is_ours(entry):
    return any("crab.py" in h.get("command", "") for h in entry.get("hooks", []))


def main():
    remove = "--remove" in sys.argv

    settings = {}
    if os.path.exists(SETTINGS):
        shutil.copyfile(SETTINGS, SETTINGS + ".bak")
        with open(SETTINGS, encoding="utf-8") as fh:
            settings = json.load(fh)

    hooks = settings.get("hooks", {})

    for event in EVENTS:
        hooks[event] = [e for e in hooks.get(event, []) if not is_ours(e)]

    if not remove:
        for event, matcher in EVENTS.items():
            hooks[event].append(block(matcher))

    settings["hooks"] = {k: v for k, v in hooks.items() if v}
    if not settings["hooks"]:
        del settings["hooks"]

    with open(SETTINGS, "w", encoding="utf-8") as fh:
        json.dump(settings, fh, indent=2)
        fh.write("\n")

    print("{} crab hooks in {}".format("removed" if remove else "installed", SETTINGS))
    print("backup at {}.bak".format(SETTINGS))


if __name__ == "__main__":
    main()
