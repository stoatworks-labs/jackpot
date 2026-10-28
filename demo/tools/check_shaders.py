#!/usr/bin/env python3
"""The demo's copies of the plugin must be the plugin, character for character.

    python3 demo/tools/check_shaders.py           compare; exit 1 on any drift
    python3 demo/tools/check_shaders.py --write   regenerate demo/shaders.js

Called from `tools/verify.sh`. Exit code 1 means a copy has drifted.

------------------------------------------------------------------- why

The page at jackpot-demo.stoatworks-labs.com carries two things that are
copies of this repository, and copies drift quietly -- a roulette ring a
pocket out, or a money wheel read anticlockwise, still looks like a casino,
and a demo that is quietly wrong is worse than none.

1. **The GLSL.** `demo/shaders.js` holds every piece of `source/Shaders.cpp`
   (kVersion, kQuadVertex, kCommon, kText, each game's pieces, the shower's and
   the composite), AND the plugin's own list of programs: `shaders::Programs()`,
   which InitGL compiles and nothing else, each program named and each stage
   the pieces it joins in order. This script reads that function and the
   pieces out of the C++ and writes them into shaders.js as `PROGRAMS`. The
   page builds its expected text for each program's stages from that table,
   and at run time REQUIRES the text the compiled plugin hands to
   glShaderSource to equal one of them before the kit's port() is applied
   (demo/wasm/gl_shim.cpp). The pieces are compared exactly -- no whitespace
   normalisation, no comment stripping: a comment updated on one side only is
   drift worth catching. Then:

     - every `const char* const k... =` in Shaders.cpp must be a piece this
       script knows, and Shaders.h must declare exactly those, so a new one
       cannot be added to the plugin and silently left out of the page;
     - every piece must be used by some program (tools/glslc.sh's rule too);
     - and shaders.js must be byte for byte what `--write` produces, so
       nothing else can be typed into it.

   The one transformation is a decode: a backtick cannot sit raw in a template
   literal, so shaders.js escapes it as \\` (the GLSL's comments quote names
   in backticks), and kVersion, the one piece that is an ordinary C string,
   has its `\\n` decoded to the newline it is. Any other backslash, or a `${`,
   on the JS side is rejected; the C++ raw strings hold neither.

2. **The WebAssembly.** `demo/jackpot-core.wasm` is the plugin's C++
   compiled by `demo/tools/build-wasm.sh`, and committed, because the deploy
   has no build step. Building it needs emscripten, which verify.sh cannot
   assume, so this checks the next best thing: `demo/wasm/inputs.sha256`,
   written by the build, records the hash of every file that went in (every
   source/*.h, the seventeen plugin .cpp files, the SDK files, the glue, the
   shim and the build script) and of both outputs, and the SDK commit. A
   source file changed since the last build -- or an output that is not the
   one the build wrote -- fails here, and the fix is to rerun build-wasm.sh.

3. **The example wheel file.** `demo/showcase.txt` is what the page's
   Example button loads, and must be `tools/fixtures/showcase.txt` (the
   README's example, and docs/wheel-file.png's) byte for byte.

------------------------------------------------------------------- what it cannot

It cannot tell whether the .wasm really is what those inputs compile to (only
rebuilding can), and it says nothing about demo/plugin.js, the page around the
plugin: its panel is built from the plugin's own declarations at run time, but
its read-out units, its tooltips and its layout are the page's.
"""
import hashlib
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.dont_write_bytecode = True

# C++ symbol (source/Shaders.cpp) -> its JS name, in the order Shaders.h lists them.
PIECES = [
    ("kVersion", "VERSION"),
    ("kQuadVertex", "QUAD_VERTEX"),
    ("kCommon", "COMMON"),
    ("kText", "TEXT"),
    ("kSlots", "SLOTS"),
    ("kSlotsMain", "SLOTS_MAIN"),
    ("kScene", "SCENE"),
    ("kRoulette", "ROULETTE"),
    ("kRouletteMain", "ROULETTE_MAIN"),
    ("kWheel", "WHEEL"),
    ("kWheelMain", "WHEEL_MAIN"),
    ("kCraps", "CRAPS"),
    ("kCrapsMain", "CRAPS_MAIN"),
    ("kLottery", "LOTTERY"),
    ("kLotteryMain", "LOTTERY_MAIN"),
    ("kBlank", "BLANK"),
    ("kShowerVertex", "SHOWER_VERTEX"),
    ("kShowerFragment", "SHOWER_FRAGMENT"),
    ("kComposite", "COMPOSITE"),
]
JS = dict(PIECES)

HEADER = """// GENERATED from source/Shaders.cpp by demo/tools/check_shaders.py --write.
// Do not edit: tools/verify.sh fails if a character of this differs from the
// plugin's. The one escape is \\` for a backtick inside a comment.
"""

MANIFEST = os.path.join("demo", "wasm", "inputs.sha256")
OUTPUTS = ("demo/jackpot-core.js", "demo/jackpot-core.wasm")
REGISTRATIONS = ("SourcePlugin.cpp", "EffectPlugin.cpp")
EXAMPLE = ("demo/showcase.txt", "tools/fixtures/showcase.txt")

RAW = re.compile(r'const char\* const (k\w+) = R"\((.*?)\)";', re.S)
PLAIN = re.compile(r'const char\* const (k\w+) = "((?:[^"\\\n]|\\.)*)";')


class Stale(Exception):
    """The C++ is not shaped the way this script reads it: fail, never guess."""


def read(*parts):
    with open(os.path.join(REPO, *parts), encoding="utf-8") as handle:
        return handle.read()


def declared(cpp):
    """Every `const char* const k... =` definition in Shaders.cpp, in order."""
    return re.findall(r"^const char\* const (k\w+)\s*=", cpp, re.M)


def pieces_of(cpp):
    """symbol -> its text, as the compiler sees it."""
    out = {}
    for match in RAW.finditer(cpp):
        out[match.group(1)] = match.group(2)
    for match in PLAIN.finditer(cpp):
        body = match.group(2)
        if re.search(r"\\[^n]", body):
            raise Stale(f"{match.group(1)} holds an escape other than \\n; this script decodes only that")
        out[match.group(1)] = body.replace("\\n", "\n")
    return out


def unknown_symbols(cpp, shaders_h):
    """Pieces in Shaders.cpp or Shaders.h that this script does not copy."""
    found = declared(cpp) + re.findall(r"extern const char\* const (k\w+);", shaders_h)
    return sorted(set(s for s in found if s not in JS))


def programs(cpp):
    """shaders::Programs(), read from the C++: [(name, [vertex symbols], [fragment symbols])]."""
    start = cpp.find("std::vector< Program > Programs()")
    if start < 0:
        raise Stale("source/Shaders.cpp has no shaders::Programs()")
    body = cpp[start:]
    end = body.find("\n}\n")
    if end < 0:
        raise Stale("could not find the end of shaders::Programs()")
    body = body[:end]
    quad = re.search(r"const std::string quad = Assemble\(\s*\{([^}]*)\}\s*\);", body)
    if quad is None:
        raise Stale("shaders::Programs() no longer assembles `quad` the way this script reads it")
    rows = re.findall(r'\{\s*"(\w+)",\s*(quad|Assemble\(\s*\{[^}]*\}\s*\)),\s*Assemble\(\s*\{([^}]*)\}\s*\)\s*\}', body)
    if not rows:
        raise Stale("could not read the programs in shaders::Programs()")
    split = lambda text: [p.strip() for p in text.split(",") if p.strip()]
    out = []
    for name, vertex, fragment in rows:
        vpieces = quad.group(1) if vertex == "quad" else re.search(r"\{([^}]*)\}", vertex).group(1)
        out.append((name, split(vpieces), split(fragment)))
    # The plugin's InitGL maps the list by position: one per Game, then the
    # shower and the composite. Count what it compiles, so a row this script
    # failed to read cannot pass for a shorter list.
    entries = len(re.findall(r'\{\s*"\w+",', body))
    if entries != len(out):
        raise Stale(f"shaders::Programs() has {entries} entries and only {len(out)} could be read")
    for name, v, f in out:
        for symbol in v + f:
            if symbol not in JS:
                raise Stale(f"program {name} uses {symbol}, a piece this script does not copy")
    return out


def render(cpp):
    """shaders.js as --write produces it."""
    pieces = pieces_of(cpp)
    out = [HEADER]
    for symbol, name in PIECES:
        if symbol not in pieces:
            raise Stale(f"{symbol} not found in source/Shaders.cpp")
        body = pieces[symbol]
        if "\\" in body or "${" in body:
            raise Stale(f"{symbol} holds a backslash or ${{; the escape scheme cannot carry it")
        out.append(f"\n// {symbol}, source/Shaders.cpp\n"
                   f"export const {name} = `{body.replace('`', chr(92) + '`')}`;\n")
    out.append(
        "\n// shaders::Programs(), in its order: every program the plugin's InitGL compiles,\n"
        "// each stage the pieces it joins (Assemble), in the order they are written.\n"
        "export const PROGRAMS = [\n"
    )
    for name, v, f in programs(cpp):
        js = lambda symbols: ", ".join(repr(JS[s]) for s in symbols)
        out.append(f"  {{ name: {name!r}, vertex: [{js(v)}], fragment: [{js(f)}] }},\n")
    out.append("];\n")
    return "".join(out)


def from_js(source, name):
    match = re.search(r"^export const " + name + r" = `(.*?)`;$", source, re.S | re.M)
    if match is None:
        return None, None
    body = match.group(1)
    stray = re.search(r"\\(?!`)", body)
    if stray is not None:
        line = body[: stray.start()].count("\n") + 1
        return None, f"backslash that is not an escaped backtick, at line {line}"
    if "${" in body:
        return None, "template substitution"
    return body.replace("\\`", "`"), None


def first_difference(a, b):
    left, right = a.splitlines(), b.splitlines()
    for i in range(max(len(left), len(right))):
        x = left[i] if i < len(left) else "<missing>"
        y = right[i] if i < len(right) else "<missing>"
        if x != y:
            return i + 1, x, y
    return None


def check_shaders(cpp, shaders_h, js):
    problems = 0

    for symbol in unknown_symbols(cpp, shaders_h):
        print(f"FAIL  {symbol} is a shader piece of source/Shaders.cpp this script does not copy -- add it to PIECES")
        problems += 1
    for symbol, _ in PIECES:
        if not re.search(r"extern const char\* const " + symbol + r";", shaders_h):
            print(f"FAIL  source/Shaders.h does not declare {symbol}")
            problems += 1

    pieces = pieces_of(cpp)
    for symbol, name in PIECES:
        if symbol not in pieces:
            print(f"FAIL  {symbol} not found in source/Shaders.cpp")
            problems += 1
            continue
        js_text, complaint = from_js(js, name)
        if complaint is not None:
            print(f"FAIL  {name} in demo/shaders.js has a {complaint}")
            problems += 1
        elif js_text is None:
            print(f"FAIL  {name} not found in demo/shaders.js")
            problems += 1
        elif pieces[symbol] == js_text:
            print(f"ok    {name:<16} matches {symbol} ({len(js_text)} chars)")
        else:
            problems += 1
            print(f"FAIL  {name} has drifted from {symbol}")
            where = first_difference(pieces[symbol], js_text)
            if where:
                print(f"        first difference at line {where[0]}")
                print(f"          C++: {where[1]}")
                print(f"          js : {where[2]}")

    try:
        table = programs(cpp)
    except Stale as stale:
        print(f"FAIL  the plugin's program list could not be read: {stale}")
        return problems + 1
    used = set()
    for _, v, f in table:
        used |= set(v) | set(f)
    for symbol, _ in PIECES:
        if symbol in pieces and symbol not in used:
            print(f"FAIL  {symbol} is a piece no program in shaders::Programs() uses")
            problems += 1
    block = re.search(r"^export const PROGRAMS = \[\n(.*?)^\];$", js, re.S | re.M)
    rows = [] if block is None else re.findall(r"^  \{ name: '(\w+)', vertex: \[(.*?)\], fragment: \[(.*?)\] \},$", block.group(1), re.M)
    if len(rows) != len(table):
        print(f"FAIL  PROGRAMS in demo/shaders.js has {len(rows)} programs, shaders::Programs() {len(table)}")
        problems += 1
    for i, (name, v, f) in enumerate(table):
        want = (name, ", ".join(repr(JS[s]) for s in v), ", ".join(repr(JS[s]) for s in f))
        have = rows[i] if i < len(rows) else None
        if have == want:
            print(f"ok    PROGRAMS[{i}] {name:<9} {' + '.join(JS[s] for s in f)}")
        else:
            print(f"FAIL  PROGRAMS[{i}] is {have}, the plugin's is {want}")
            problems += 1

    # Anything else typed into the file.
    try:
        expected = render(cpp)
    except Stale as stale:
        print(f"FAIL  {stale}")
        return problems + 1
    if expected != js:
        where = first_difference(expected, js)
        print("FAIL  demo/shaders.js is not exactly what --write produces")
        if where:
            print(f"        first difference at line {where[0]}")
            print(f"          want: {where[1]}")
            print(f"          have: {where[2]}")
        problems += 1
    return problems


def sha256(path):
    digest = hashlib.sha256()
    with open(os.path.join(REPO, path), "rb") as handle:
        for block in iter(lambda: handle.read(1 << 16), b""):
            digest.update(block)
    return digest.hexdigest()


def check_wasm():
    try:
        lines = read(MANIFEST).splitlines()
    except FileNotFoundError:
        print(f"FAIL  {MANIFEST} is missing -- run demo/tools/build-wasm.sh")
        return 1

    problems = 0
    entries = 0
    recorded = set()
    pinned = False
    for line in lines:
        pin = re.match(r"^# FFGL SDK ([0-9a-f]{40})$", line)
        if pin:
            pinned = True
            try:
                head = subprocess.run(["git", "-C", os.path.join(REPO, "external", "ffgl"), "rev-parse", "HEAD"],
                                      capture_output=True, text=True, check=True).stdout.strip()
            except (OSError, subprocess.CalledProcessError):
                print("FAIL  cannot read the FFGL SDK's commit -- is the submodule checked out?")
                problems += 1
                continue
            if head != pin.group(1):
                print(f"FAIL  the .wasm was built against FFGL SDK {pin.group(1)[:7]}, the submodule is at {head[:7]}")
                problems += 1
            continue
        if not line or line.startswith("#"):
            continue
        want, path = line.split(None, 1)
        recorded.add(path)
        entries += 1
        if not os.path.exists(os.path.join(REPO, path)):
            print(f"FAIL  {path} went into the .wasm and no longer exists")
            problems += 1
        elif sha256(path) != want:
            print(f"FAIL  {path} has changed since demo/jackpot-core.wasm was built")
            problems += 1

    if not pinned:
        print(f"FAIL  {MANIFEST} does not record the FFGL SDK's commit")
        problems += 1

    # A source file added to source/ since the build is an input the build
    # never saw: every header, and every .cpp but the two registrations.
    for name in sorted(os.listdir(os.path.join(REPO, "source"))):
        path = f"source/{name}"
        if name in REGISTRATIONS or path in recorded:
            continue
        if name.endswith(".h") or name.endswith(".cpp"):
            print(f"FAIL  {path} is newer than the .wasm's manifest -- rebuild")
            problems += 1

    for output in OUTPUTS:
        if output not in recorded:
            print(f"FAIL  {MANIFEST} does not record {output}")
            problems += 1
    if problems == 0:
        print(f"ok    {'WASM':<16} built from these sources ({entries} files hashed, SDK pin agrees)")
    return problems


def check_example():
    try:
        with open(os.path.join(REPO, EXAMPLE[0]), "rb") as a, open(os.path.join(REPO, EXAMPLE[1]), "rb") as b:
            same = a.read() == b.read()
    except FileNotFoundError as missing:
        print(f"FAIL  {missing.filename} is missing")
        return 1
    if not same:
        print(f"FAIL  {EXAMPLE[0]} is not {EXAMPLE[1]} byte for byte -- copy it again")
        return 1
    print(f"ok    {'EXAMPLE':<16} {EXAMPLE[0]} is {EXAMPLE[1]}")
    return 0


def main(argv):
    cpp = read("source", "Shaders.cpp")
    shaders_h = read("source", "Shaders.h")
    if "--write" in argv:
        unknown = unknown_symbols(cpp, shaders_h)
        if unknown:
            print(f"FAIL  {', '.join(unknown)}: piece(s) this script does not copy -- add them to PIECES")
            return 1
        try:
            text = render(cpp)
        except Stale as stale:
            print(f"FAIL  {stale}")
            return 1
        with open(os.path.join(REPO, "demo", "shaders.js"), "w", encoding="utf-8") as handle:
            handle.write(text)
        print("wrote demo/shaders.js")
        return 0
    try:
        js = read("demo", "shaders.js")
    except FileNotFoundError:
        print("FAIL  demo/shaders.js is missing -- run demo/tools/check_shaders.py --write")
        return 1
    problems = check_shaders(cpp, shaders_h, js) + check_wasm() + check_example()
    print()
    if problems:
        print(f"{problems} problem(s) -- rerun demo/tools/check_shaders.py --write for the GLSL,"
              " demo/tools/build-wasm.sh for the .wasm, and copy the fixture for the example;"
              " never edit an output by hand")
        return 1
    print(f"all {len(PIECES)} shader pieces and the plugin's {len(programs(cpp))} programs are the plugin's,"
          " the .wasm is built from the current sources, and the example wheel file is the fixture")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
