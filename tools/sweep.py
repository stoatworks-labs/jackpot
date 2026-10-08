#!/usr/bin/env python3
"""Move every parameter and fail if any of them made no difference to the frame.

**This is the only thing in the repo that catches a dead control.** A GLSL
uniform whose name does not match the C++ is silently ignored --
`glGetUniformLocation` returns -1 and `glUniform` on -1 is a documented no-op --
so a slider can be stone dead while everything compiles, links and renders.

Each render is one play to rest (jptest's default: Play on frame 0, then the
frames to Spin Time), so the controls that act on the PLAY -- Spin Time, Seed,
Strip -- show up as a different result, or (with --frames) a different moment.

## The context table

Most controls belong to one game, and the table puts the render in that game.
The rest give a control the moment it acts in: Blur only mid-spin, Reel Bounce
as the last reel is caught, Display and the shower's On Win once a play has
paid, Land On only when Spin Time is not already a whole number of beats and
bars, Strip at a seed where the two strips' maps of one draw part company. Emptying the table makes every one of those go dead -- which is how the
table was checked.

Usage::

    tools/sweep.py [--build BUILD_DIR] [--verbose] [--jobs N]
"""

import argparse
import concurrent.futures
import os
import pathlib
import subprocess
import sys
import tempfile
import zlib

REPO = pathlib.Path(__file__).resolve().parent.parent

SIZE = "320x180"

FONT_FILE = "/System/Library/Fonts/Supplemental/Georgia.ttf"

ROULETTE = ["--set", "Game=Roulette"]
WHEEL = ["--set", "Game=Money Wheel"]
CRAPS = ["--set", "Game=Craps"]
LOTTERY = ["--set", "Game=Lottery"]
SHOWER = ["--set", "Game=Shower Only", "--set", "Shower=Always", "--frames", "150"]
PAID = ["--set", "Result=Jackpot", "--frames", "300"]

CONTEXT = {
    "Spin Time": ["--frames", "100"],
    "Land On": ["--frames", "100", "--set", "Spin Time=0.4"],
    "Fixed Number": ["--set", "Result=Fixed"],
    "Auto Play": ["--frames", "400", "--set", "Interval=0"],
    "Interval": ["--frames", "400", "--set", "Auto Play=1"],
    "Blur": ["--frames", "90"],
    # Both strips map the same draw in strip order, so for many seeds they land
    # on the same stops (Seed 0 does); Seed 1 is one where they differ.
    "Strip": ["--set", "Seed=1"],
    "Reel Bounce": ["--frames", "236"],
    "Wheel": ROULETTE,
    "Rotor Speed": ROULETTE,
    "Deflectors": ROULETTE,
    "Light Angle": ROULETTE,
    "Tilt": ROULETTE,
    "Clapper": WHEEL + ["--frames", "150"],
    "Pyramids": CRAPS,
    "Puck": CRAPS,
    "Balls": LOTTERY + ["--frames", "200"],
    "Draw": LOTTERY,
    "Air": LOTTERY + ["--frames", "300"],
    "Shower": PAID,
    "Shower Now": ["--frames", "60"],
    "Pattern": SHOWER,
    "Pieces": SHOWER,
    "Amount": SHOWER,
    "Piece Size": SHOWER,
    "Pile": SHOWER,
    "Felt Colour": ["--set", "Backdrop=Felt"],
    "Felt_Green": ["--set", "Backdrop=Felt"],
    "Felt_Blue": ["--set", "Backdrop=Felt"],
    "Display": PAID,
}

# The awkward values are load-bearing: an angle swept at 0, 0.5 and 1 can land
# on the same picture.
SWEEP_VALUES = [0.0, 0.137, 0.611, 1.0]

# Discrete parameters, by the values they take (--list reports the kind but
# not the element count, so these track Controls.h by hand).
DISCRETE = {
    "Game": [0, 1, 2, 3, 4, 5],
    "Land On": [0, 1, 2],
    "Result": [0, 1, 2, 3, 4, 5],
    "Fixed Number": [1, 7, 10],
    "Seed": [0, 1, 2],
    "Auto Play": [0, 1],
    "Reels": [0, 1],
    "Symbols": [0, 1],
    "Strip": [0, 1],
    "Wheel": [0, 1],
    "Deflectors": [0, 1],
    "Pyramids": [0, 1],
    "Puck": [0, 1],
    "Balls": [10, 30, 75],
    "Draw": [1, 3, 7],
    "Shower": [0, 1, 2, 3],
    "Shower Now": [0, 1],
    "Pattern": [0, 1, 2, 3],
    "Pieces": [0, 1, 2],
    "Pile": [0, 1],
    "Backdrop": [0, 1, 2],
    "Display": [0, 1],
    "Font": [0, 1, 40],
}

# Text and file controls, by the strings they take.
TEXTS = {
    "Font File": ["", FONT_FILE],
    "Font Name": ["", "Georgia"],
}

SKIP_KINDS = {"buffer"}
SKIP_NAMES = {"About", "Play", "User guide", "Project page", "Source on GitHub", "Support the work"}
OVER_ONLY = {"Mix"}


def read_png(path):
    data = path.read_bytes()
    pos, idat = 8, b""
    while pos < len(data):
        length = int.from_bytes(data[pos:pos + 4], "big")
        if data[pos + 4:pos + 8] == b"IDAT":
            idat += data[pos + 8:pos + 8 + length]
        pos += 12 + length
    return zlib.decompress(idat)


def parameters(harness, over):
    args = [str(harness), "--list"] + (["--over"] if over else [])
    out = subprocess.run(args, capture_output=True, text=True, check=True).stdout
    found = []
    for line in out.splitlines()[1:]:
        parts = line.split()
        if len(parts) >= 4:
            found.append((" ".join(parts[1:-2]), parts[-2]))
    return found


def render(harness, tmp, over, setting, extra, index):
    out = tmp / f"sweep-{index}.png"
    args = [str(harness), "--out", str(out), "--size", SIZE]
    args += (["--over"] if over else []) + extra + ["--set", setting]
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"jptest failed: {' '.join(args)}\n{result.stderr.strip()}")
    return read_png(out)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="build")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--no-context", action="store_true", help="empty the context table (to see what it holds up)")
    args = parser.parse_args()
    context = {} if args.no_context else CONTEXT

    harness = REPO / args.build / "jptest"
    if not harness.exists():
        print(f"no jptest at {harness} -- build first", file=sys.stderr)
        return 2

    if not os.path.exists(FONT_FILE):
        print(f"  note: {FONT_FILE} is not on this machine; Font File is swept with no file only")
        TEXTS["Font File"] = [""]

    work = []
    for name, kind in parameters(harness, False):
        if kind in SKIP_KINDS or name in SKIP_NAMES:
            continue
        work.append((False, name, kind))
    for name, kind in parameters(harness, True):
        if name in OVER_ONLY:
            work.append((True, name, kind))
    # The effect's own symbol set: the clip on the reels (in place of the seven,
    # so a jackpot shows it on the line).
    work.append((True, "Symbols", "option-over"))

    dead, checked = [], 0
    with tempfile.TemporaryDirectory() as tmp, concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        tmp = pathlib.Path(tmp)
        jobs = {}
        index = 0
        for over, name, kind in work:
            extra = context.get(name, [])
            if kind == "option-over":
                values = [0, 2]
                extra = [] if args.no_context else ["--set", "Result=Jackpot"]
            elif name in TEXTS:
                values = TEXTS[name]
            else:
                values = DISCRETE.get(name, SWEEP_VALUES)
            futures = []
            for value in values:
                futures.append(pool.submit(render, harness, tmp, over, f"{name}={value}", extra, index))
                index += 1
            jobs[(over, name, kind)] = futures
        for (over, name, kind), futures in jobs.items():
            frames = [f.result() for f in futures]
            if len(frames) < 2:
                continue
            checked += 1
            label = f"{name}{' (Over)' if over else ''}"
            if all(f == frames[0] for f in frames[1:]):
                dead.append(label)
                print(f"  DEAD {label}")
            elif args.verbose:
                print(f"  ok   {label}")

    print()
    if dead:
        print(f"sweep: {checked} parameters, {len(dead)} made no difference:")
        for entry in dead:
            print(f"  - {entry}")
        return 1
    print(f"sweep: {checked} parameters, all live")
    return 0


if __name__ == "__main__":
    sys.exit(main())
