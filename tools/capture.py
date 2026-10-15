#!/usr/bin/env python3
"""Record the crab puck's screen to GIFs.

Needs the capture build on the board (`pio run -e capture -t upload`), plus
pyserial and Pillow. The board streams every frame it draws, so the GIFs are
pixel-exact and play back at the real 25 fps however slow the link is.

    python tools/capture.py [PORT] [--only name,name]

Writes media/<scene>.gif and media/<scene>.png (a still). Reflash the normal
build afterwards (`pio run -e crabpuck -t upload`): the hooks talk at 115200,
the capture build at 2 Mbaud.
"""

import os
import struct
import sys
import time

import serial
from serial.tools import list_ports
from PIL import Image

W, H = 240, 135
SCALE = 2
BAUD = 2000000
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "media")

MODEL = "opus 5.5|84000|200000"

# Vignette / motif numbers follow the enums in src/main.cpp.
V = dict(mooch=0, bubbles=1, dance=2, dig=3, wave=4, stretch=5, sidestep=6,
         scratch=7, ponder=8, snooze=9, doubletake=10, casual=11, applaud=12,
         startle=13, handstand=14, box=15, nod=16, peek=17, itch=18, sneeze=19,
         moonwalk=20, flex=21, hiccup=22, stare=23)
V_FRAMES = [200, 150, 160, 170, 120, 130, 320, 140, 170, 110,
            150, 190, 130, 140, 150, 160, 220, 180, 200, 150,
            170, 190, 160, 170]
M = dict(scuttle=0, type=1, juggle=2, hammer=3, rummage=4, carry=5, spin=6,
         point=7, panic=8, stir=9)


def idle(name):
    return ("idle-" + name,
            ["working|warming up|" + MODEL, "idle|waiting for claude|" + MODEL,
             "!vig %d" % V[name]],
            V_FRAMES[V[name]])


def work(name, detail):
    return ("working-" + name,
            ["working|%s|%s" % (detail, MODEL), "!motif %d" % M[name]], 125)


SCENES = [
    idle("dance"), idle("bubbles"), idle("dig"), idle("moonwalk"),
    idle("handstand"), idle("sneeze"), idle("flex"), idle("snooze"),
    idle("ponder"), idle("applaud"),
    work("scuttle", "Edit src/main.cpp"), work("type", "Write README.md"),
    work("juggle", "Bash pio run"), work("hammer", "Grep TODO"),
    ("approval", ["approval|needs your permission|" + MODEL], 100),
    ("done", ["working|x|" + MODEL, "done|your turn|" + MODEL], 100),
    ("error", ["error|Bash failed|" + MODEL], 100),
    ("asleep", ["asleep|session ended|" + MODEL], 100),
    ("game", ["!ai 1", "!game"], 600),
    ("game-over", ["!ai 2"], 700),          # continues the game above
]


def find_port():
    for p in list_ports.comports():
        if (p.vid, p.pid) in {(0x1A86, 0x55D4), (0x10C4, 0xEA60)}:
            return p.device
    sys.exit("no T-Display found; pass the port as an argument")


def read_exact(ser, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = ser.read(n - len(buf))
        if not chunk:
            raise TimeoutError("frame stalled at %d/%d bytes" % (len(buf), n))
        buf += chunk
    return bytes(buf)


def read_frame(ser):
    window = b""
    while not window.endswith(b"FRM"):
        c = ser.read(1)
        if not c:
            raise TimeoutError("no frame header")
        window = (window + c)[-3:]
    (length,) = struct.unpack("<I", read_exact(ser, 4))
    data = read_exact(ser, length)
    rgb = bytearray(W * H * 3)
    o = 0
    for i in range(0, length, 3):
        run = data[i]
        v = (data[i + 1] << 8) | data[i + 2]   # stored byte-swapped: swap back
        r = ((v >> 11) & 0x1F) * 255 // 31
        g = ((v >> 5) & 0x3F) * 255 // 63
        b = (v & 0x1F) * 255 // 31
        rgb[o:o + run * 3] = bytes((r, g, b)) * run
        o += run * 3
    if o != W * H * 3:
        raise ValueError("frame decoded to %d pixels" % (o // 3))
    return Image.frombytes("RGB", (W, H), bytes(rgb))


def record(ser, lines, count):
    ser.reset_input_buffer()
    for line in lines:
        ser.write((line + "\n").encode())
    ser.write(b"!stream %d\n" % count)
    return [read_frame(ser) for _ in range(count)]


def save(name, frames, still_at=0.5):
    big = [f.resize((W * SCALE, H * SCALE), Image.NEAREST) for f in frames]
    # one shared palette keeps colours from flickering between frames
    pal = big[len(big) // 2].quantize(colors=255, method=Image.MEDIANCUT)
    q = [f.quantize(palette=pal, dither=Image.Dither.NONE) for f in big]
    q[0].save(os.path.join(OUT, name + ".gif"), save_all=True,
              append_images=q[1:], duration=40, loop=0, optimize=True,
              disposal=1)
    big[int(len(big) * still_at)].save(os.path.join(OUT, name + ".png"))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    only = None
    if "--only" in sys.argv:
        only = set(sys.argv[sys.argv.index("--only") + 1].split(","))
        args = [a for a in args if a not in only and "," not in a]
    port = args[0] if args else find_port()
    os.makedirs(OUT, exist_ok=True)

    def connect():
        for _ in range(20):
            try:
                s = serial.Serial()
                s.port, s.baudrate, s.timeout = port, BAUD, 3
                s.dtr = s.rts = False      # don't trip the auto-reset
                s.open()
                time.sleep(0.3)
                return s
            except serial.SerialException:
                time.sleep(1)
        sys.exit("could not open %s" % port)

    ser = connect()
    for name, lines, count in SCENES:
        if only and name not in only:
            continue
        t = time.time()
        # USB on these boards occasionally drops out for a moment; reconnect
        # and record the scene again rather than give up.
        for attempt in range(3):
            try:
                frames = record(ser, lines, count)
                break
            except (serial.SerialException, TimeoutError) as exc:
                print("%-20s retry after: %s" % (name, exc))
                try:
                    ser.close()
                except Exception:
                    pass
                time.sleep(2)
                ser = connect()
                ser.write(b"!stream 0\n")
        else:
            continue
        if name == "game-over":
            frames = frames[-60:]      # just the card at the end
            save(name, frames, still_at=0.99)
        else:
            save(name, frames, still_at=0.75 if name == "game" else 0.5)
        kb = os.path.getsize(os.path.join(OUT, name + ".gif")) // 1024
        print("%-20s %4d frames  %5.1fs  %4d KB" % (name, count, time.time() - t, kb))
    ser.close()


if __name__ == "__main__":
    main()
