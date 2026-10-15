---
name: crab-puck
description: Test, troubleshoot or drive the crab puck, the T-Display status crab that mirrors Claude Code's state over USB serial. Use when the user mentions the crab, the puck, or the crab not moving / showing the wrong thing.
---

# Crab puck

The puck is a LilyGO T-Display running a pixel-art crab. This plugin's hooks call
`${CLAUDE_PLUGIN_ROOT}/hooks/crab.py --hook` on SessionStart, UserPromptSubmit,
PreToolUse, Notification, Stop and SessionEnd. Each call writes one line to the
board over USB serial.

## Test it

```
python "${CLAUDE_PLUGIN_ROOT}/hooks/crab.py" working "hello from Claude"
```

A healthy puck replies `ok working` and the crab starts scuttling. States are
`idle working approval done error asleep`.

## When the crab does not move

`crab.py` swallows every error in hook mode on purpose, so it can never block a
session. That means a broken setup fails **silently**. Check these in order:

1. **pyserial is installed** for the Python the hooks use:
   `python -c "import serial"`. If it fails, run `pip install pyserial`.
2. **The board is found.** Run the test command above. `crab: no puck found`
   means auto-detection saw no CH9102 (`1a86:55d4`) or CP2104 (`10c4:ea60`)
   bridge.
3. **It is the right board.** If several T-Displays or other boards with the
   same USB chip are plugged in, pin the puck with the `CRAB_PORT` environment
   variable (`COM5`, `/dev/ttyACM0`, `/dev/cu.wchusbserial...`).
4. **The port is free.** A serial monitor (Arduino IDE, `pio device monitor`)
   holding the port blocks every hook.
5. **The firmware is on it.** On boot the board prints
   `crab-puck ready: send state|detail|model|used|max lines` at 115200 baud.
   If it doesn't, reflash it from the web flasher linked in the README.

## Wire protocol

Newline-terminated `state|detail|model|used|max` at 115200 baud. Every field
after the state is optional. The board replies `ok <state>` or `err ...`.
