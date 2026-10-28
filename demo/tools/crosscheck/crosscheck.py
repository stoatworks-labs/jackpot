#!/usr/bin/env python3
"""The browser demo's WebAssembly against the plugin's own harness, frame for frame.

    python3 demo/tools/crosscheck/crosscheck.py --jptest build/jptest --url http://127.0.0.1:8719/

`--url` is demo/ served by anything that serves it (the page is static);
`--gl metal` (the default) draws with this Mac's GPU through Chrome's ANGLE,
`--gl swiftshader` on the CPU. Needs Google Chrome and a built jptest. Not run
by tools/verify.sh: it needs a browser and a server, and what it prints is a
measurement, recorded in AGENTS.md, rather than a gate.

------------------------------------------------------------------- what it does

Both sides are the same C++: jackpot-core.wasm is source/ compiled by
emscripten (demo/tools/build-wasm.sh), jptest is source/ compiled by Apple
clang. For each scenario below, jptest `--film` renders F frames of SW Jackpot
on its synthetic 60 fps clock with Play pressed at frame 0, and harness.js
drives the served .wasm in headless Chrome through exactly the same calls
(SetClockScaleForTest(1), the press, frame / 60 to SetTime, a clear, then
ProcessOpenGL). Then, per scenario:

  - the RESULT each side's play says it showed (the outcome's words, which the
    board prints) must be identical: results come from the integer draw or
    Fixed Number, so they are promised everywhere;
  - sampled frames are compared pixel by pixel and the differences PRINTED:
    the largest, the mean, and the share of pixels more than 2/255 apart.

The picture is not promised to be identical, and this measures by how much it
is not. Three things differ: the GPU (Apple's GL 4.1 driver against ANGLE on
Metal or SwiftShader, which round and filter differently); the output's last
rounding (jptest's lround of a float against the GPU's RGBA8 store, a level at
most); and the arithmetic of the simulation itself -- WebAssembly has no fused
multiply-add, and Apple clang fuses a*b+c on arm64 by default, so a spin can
take another path. `jptest` built with -ffp-contract=off removes that last
cause for everything but the maths library (emscripten's musl against Apple's
libm), and comparing both builds shows which differences are which.

The shower is off in the game scenarios (it throws a different spray on any
difference in its first contact), and has a scenario of its own.
"""
import argparse
import base64
import json
import os
import select
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
CHROME = os.environ.get("CHROME", "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome")

W, H = 640, 360
SPIN = 4.0  # the default Spin Time; Land On Time lands every play on it
REST = 1 + int(SPIN * 60) + 2  # jptest's "one play to rest": the press frame, the play, two more
SHOWCASE = os.path.join(REPO, "tools", "fixtures", "showcase.txt")

# name, settings (jptest --set order), frames, the frames compared
SCENARIOS = [
    ("slots jackpot", [("Game", "Slots"), ("Result", "Jackpot"), ("Shower", "Off")], REST, [30, 120, REST - 1]),
    ("slots 5 reels random", [("Game", "Slots"), ("Reels", "5"), ("Symbols", "Sevens and Bars"), ("Seed", "3"), ("Shower", "Off")], REST, [120, REST - 1]),
    ("roulette fixed 17", [("Game", "Roulette"), ("Result", "Fixed"), ("Fixed Number", "17"), ("Shower", "Off")], REST, [30, 120, REST - 1]),
    ("money wheel jackpot", [("Game", "Money Wheel"), ("Result", "Jackpot"), ("Shower", "Off")], REST, [30, 120, REST - 1]),
    ("wheel file, 12 in order", [("Game", "Money Wheel"), ("Wheel File", "showcase.txt"), ("Segments", "12"), ("Lines", "In Order"), ("Result", "Fixed"), ("Fixed Number", "5"), ("Shower", "Off")], REST, [REST - 1]),
    ("craps fixed 11", [("Game", "Craps"), ("Result", "Fixed"), ("Fixed Number", "11"), ("Shower", "Off")], REST, [30, 120, REST - 1]),
    ("lottery random", [("Game", "Lottery"), ("Result", "Random"), ("Shower", "Off")], REST, [30, 120, REST - 1]),
    ("shower only", [("Game", "Shower Only"), ("Pieces", "Mixed")], 150, [20, 60, 149]),
]


# ----------------------------------------------------------------- jptest
def jptest_frames(jptest, settings, frames, sample):
    args = [jptest, "--film", str(frames), "--size", f"{W}x{H}", "--play", "0"]
    for name, value in settings:
        if name == "Wheel File":
            value = SHOWCASE
        args += ["--set", f"{name}={value}"]
    size = W * H * 4
    out = {}
    with subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE) as proc:
        for f in range(frames):
            data = proc.stdout.read(size)
            if len(data) != size:
                raise RuntimeError(f"jptest gave {len(data)} bytes at frame {f}: {proc.stderr.read().decode()}")
            if f in sample:
                out[f] = data
        proc.wait()
    # The play's words: the plugin's own log line, read by jptest's describePlay
    # in single-play mode -- run it once more without --film for them.
    with tempfile.TemporaryDirectory() as scratch:
        single = [a for a in args if a not in ("--film", str(frames))] + ["--out", os.path.join(scratch, "rest.png")]
        text = subprocess.run(single, capture_output=True, text=True)
    return out, text.stdout.strip()


# ----------------------------------------------------------------- Chrome over a pipe
class Chrome:
    """Headless Chrome on --remote-debugging-pipe: no debugging port is opened."""

    def __init__(self, gl):
        self.profile = tempfile.mkdtemp(prefix="jackpot-crosscheck-")
        args = [CHROME, "--headless=new", "--remote-debugging-pipe", f"--user-data-dir={self.profile}",
                "--no-first-run", "--no-default-browser-check", "--disable-extensions",
                "--disable-background-timer-throttling", "--disable-renderer-backgrounding"]
        args += ["--use-angle=swiftshader", "--enable-unsafe-swiftshader"] if gl == "swiftshader" else \
                ["--use-angle=metal", "--enable-gpu", "--ignore-gpu-blocklist"]
        to_chrome_r, self.w = os.pipe()
        self.r, from_chrome_w = os.pipe()

        def child():
            a, b = os.dup(to_chrome_r), os.dup(from_chrome_w)
            os.dup2(a, 3)
            os.dup2(b, 4)

        self.proc = subprocess.Popen(args, close_fds=False, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL, preexec_fn=child)
        os.close(to_chrome_r)
        os.close(from_chrome_w)
        self.buf, self.next, self.session, self.errors = b"", 0, None, []
        target = self.call("Target.createTarget", {"url": "about:blank"})["targetId"]
        self.session = self.call("Target.attachToTarget", {"targetId": target, "flatten": True})["sessionId"]
        self.call("Runtime.enable")

    def call(self, method, params=None, timeout=600):
        self.next += 1
        msg = {"id": self.next, "method": method, "params": params or {}}
        if self.session:
            msg["sessionId"] = self.session
        data = json.dumps(msg).encode() + b"\0"
        while data:
            data = data[os.write(self.w, data):]
        end = time.time() + timeout
        while True:
            while b"\0" not in self.buf:
                if not select.select([self.r], [], [], max(0.0, end - time.time()))[0]:
                    raise TimeoutError(method)
                chunk = os.read(self.r, 1 << 22)
                if not chunk:
                    raise EOFError("Chrome went away")
                self.buf += chunk
            raw, self.buf = self.buf.split(b"\0", 1)
            reply = json.loads(raw)
            if reply.get("method") == "Runtime.exceptionThrown":
                self.errors.append(reply["params"]["exceptionDetails"].get("text", ""))
            if reply.get("id") == self.next:
                if "error" in reply:
                    raise RuntimeError(f"{method}: {reply['error']}")
                return reply.get("result", {})

    def evaluate(self, expression):
        r = self.call("Runtime.evaluate", {"expression": expression, "returnByValue": True, "awaitPromise": True})
        if "exceptionDetails" in r:
            raise RuntimeError(r["exceptionDetails"].get("exception", {}).get("description") or r["exceptionDetails"])
        return r["result"].get("value")

    def close(self):
        try:
            self.call("Browser.close", timeout=5)
        except Exception:
            pass
        self.proc.wait(10)
        shutil.rmtree(self.profile, ignore_errors=True)


# ----------------------------------------------------------------- compare
def compare(a, b):
    n = len(a)
    worst, total, over2, over32 = 0, 0, 0, 0
    for i in range(0, n, 4):
        d = max(abs(a[i] - b[i]), abs(a[i + 1] - b[i + 1]), abs(a[i + 2] - b[i + 2]), abs(a[i + 3] - b[i + 3]))
        worst = max(worst, d)
        total += abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1]) + abs(a[i + 2] - b[i + 2])
        over2 += d > 2
        over32 += d > 32
    pixels = n // 4
    return worst, total / (3 * pixels), 100.0 * over2 / pixels, 100.0 * over32 / pixels


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--jptest", required=True, help="a built tools/jptest")
    parser.add_argument("--url", default="http://127.0.0.1:8719/", help="demo/, served")
    parser.add_argument("--gl", default="metal", choices=["metal", "swiftshader"])
    parser.add_argument("--only", help="run the scenarios whose name contains this")
    parser.add_argument("--save", help="write each compared frame pair here as raw RGBA")
    args = parser.parse_args()

    chrome = Chrome(args.gl)
    try:
        # A page of the demo's origin that does not start the demo itself.
        chrome.call("Page.navigate", {"url": args.url.rstrip("/") + "/showcase.txt"})
        time.sleep(1.0)
        with open(os.path.join(HERE, "harness.js")) as f:
            chrome.evaluate(f.read())
        mismatched = 0
        for name, settings, frames, sample in SCENARIOS:
            if args.only and args.only not in name:
                continue
            ours, said = jptest_frames(args.jptest, settings, frames, sample)
            got = chrome.evaluate(f"jackpotCrosscheck({json.dumps({'width': W, 'height': H, 'effect': False, 'settings': settings, 'frames': frames, 'plays': [0], 'sample': sample})})")
            words = said.split("; ", 1)[1] if "; " in said else said
            print(f"{name}: jptest '{words}' | wasm '{got['text']}' warp {got['warp']:.6f}")
            if got["text"] and got["text"] not in said:
                print("   RESULT DIFFERS")
                mismatched += 1
            for frame in got["frames"]:
                theirs = base64.b64decode(frame["rgba"])
                worst, mean, over2, over32 = compare(ours[frame["frame"]], theirs)
                print(f"   frame {frame['frame']:4d}: max {worst:3d}/255, mean {mean:6.3f}, "
                      f"{over2:6.2f}% of pixels > 2/255, {over32:6.2f}% > 32/255")
                if args.save:
                    os.makedirs(args.save, exist_ok=True)
                    stem = os.path.join(args.save, f"{name.replace(' ', '-').replace(',', '')}-{frame['frame']}")
                    open(stem + ".jptest.rgba", "wb").write(ours[frame["frame"]])
                    open(stem + ".wasm.rgba", "wb").write(theirs)
        print(f"\nbrowser: {got['renderer'] if SCENARIOS else ''}; shaders refused: {got['refused']}")
        if chrome.errors:
            print("page errors:", chrome.errors)
        return 1 if mismatched or chrome.errors else 0
    finally:
        chrome.close()


if __name__ == "__main__":
    sys.exit(main())
