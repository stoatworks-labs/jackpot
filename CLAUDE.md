# jackpot

The casino floor for Resolume Arena/Avenue, as two FFGL plugins from one core:
`SW Jackpot` (`JP01`, source: a one-armed bandit, a roulette wheel, the money wheel, a
craps table or the lottery drum, with chips and coins showering on a win) and
`SW Jackpot Over` (`JP02`, effect: the same over the clip, which can also be a reel
symbol). C++/GLSL, CMake MODULE → two universal `.bundle`s (macOS) + Windows `.dll`s
(built by CI, MSVC). MIT. Bundle ids `com.stoatworks.ffgl.jackpot` and
`com.stoatworks.ffgl.jackpot.over`.

Read `AGENTS.md` before touching a planner, the paint shift of any game, the
shader's mirrored constants, or the harness's readbacks.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install to Resolume: `cmake --install build` (never from `~/Projects`)
- One play to rest: `./build/jptest --out /tmp/jackpot.png`
- The effect on the harness's card: `./build/jptest --over --out /tmp/o.png`
- List parameters: `./build/jptest --list` (`--over` for the effect's)
- Set anything by name, text and file parameters too:
  `./build/jptest --set "Game=Roulette" --set "Result=Fixed" --set "Fixed Number=17"`
- Press Play on frame N: `--play N` (repeatable; default frame 0). `--frames N`
  renders exactly N frames instead of one play to rest.
- Film: `./build/jptest --film 600 --size 1280x720 --play 10 --set "Game=Craps" | ffmpeg -f rawvideo -pix_fmt rgba -s 1280x720 -r 60 -i - -c:v libx264 -pix_fmt yuv420p craps.mp4`
- A clip through the effect: `ffmpeg -i in.mov -f rawvideo -pix_fmt rgba - | ./build/jptest --over --pipe --size WxH | ffmpeg …`
  A cue line is `frame  Parameter Name  value` (`#` starts a comment), in the same
  units as `--set`, numbers only. Values interpolate linearly between a name's cues
  and hold before the first and after the last, so a press of Play is three cues
  (0, 1, 0). Frame *n* is clocked at n / 60 s. An unknown name exits 2 before any
  frame; a partial frame at EOF ends the stream with exit 0; a reader that hangs up
  ends `--pipe`/`--film` with exit 1 (SIGPIPE is ignored), never a silent 141.
- Every program as the plugin compiles it, and through this Mac's driver:
  `./build/jptest --shaders --out DIR`

## Verify
- Everything: `tools/verify.sh` (~5 min: reserved words and reversed smoothsteps,
  glslc, the pin, a fresh universal build, both bundles through lipo/plist/codesign/
  oxbow probe+selftest, the programs through the driver, every check, `--offline`,
  the dice tables across three builds, the `--pipe` format, the negative controls,
  the mutants, the sweep, the bench)
- **The results**: `--slots`, `--roulette`, `--wheel`, `--craps`, `--lottery`;
  out of the pixels: `--slots-readback`, `--roulette-readback`, `--wheel-readback`.
- **Wheel files** (jackpot#1, not released): `--wheel-file` (what a file becomes, the
  fallbacks, a non-ASCII path), `--wheel-draw` (In Order, Random, Short File, the draw
  holding between spins), `--wheel-custom` (every Result on 2-54 segments, only marked
  segments win, C_P), `--wheel-geometry` (GL: equal segments, labels inside their own).
  Fixtures in `tools/fixtures/`; a wheel by hand:
  `./build/jptest --set "Game=Money Wheel" --set "Wheel File=$PWD/tools/fixtures/showcase.txt" --set Segments=12 --set "Lines=In Order"`
- **The physics**: `--nearmiss`, `--blur`, `--shower`, `--pile` (the pile clears,
  stays on the table, never stalls the pool, leaves the table flat: jackpot#3),
  `--duration`, and the C37/C54 identities, the departure and the clapper inside
  `--roulette`/`--wheel`.
- **The plugin**: `--defaults`, `--names` (also: Game and Play in no group, every
  group one run of ids: jackpot#2), `--determinism`, `--over-check`, `--resize`,
  `--fonts`, `--state`.
- **The checks can fail**: `--negative` (35 wrong models), `tools/mutate.sh`
  (four GLSL and thirteen C++ one-character mutants).
- What CI runs: `--offline` (the checks that need no GL context, and their
  negative controls; says loudly that the pixel checks were not run) and
  `tools/glslc.sh`.
- No dead controls: `python3 tools/sweep.py` (50 parameters, both plugins;
  `--no-context` empties the context table to show what it holds up).
- Cost: `--bench` (720p/1080p/4K for every game; the planners' ms).

## Notes
- **Units are metres, kilograms, seconds** in the games; the shower's world is in
  frame heights and scales its physics by the piece's real size.
- **The shower's pile ages** (0.1.1): every piece fades `Shower::kLinger` (12 s) after
  it is thrown, the cap fades the oldest past 90% of the 900-piece pool, |z| past the
  table is culled, and the height field is rebuilt whenever a piece at rest goes. No
  control; never clear it on a Pieces change. AGENTS.md "The pile, and how it goes".
- **Game and Play are in no parameter group**; the Game group starts at Spin Time.
  Resolume groups each run of consecutive ids, so a group must stay one run (`--names`).
  A new control goes INSIDE its group's run and the ids after it move: Arena stores
  values by name (Controls.h says why; jackpot#1 moved Pyramids onwards up four).
- **The money wheel's face** (jackpot#1): the Big Six or a Wheel File's lines, built in
  `WheelFace.cpp` on the render thread when its inputs change and painted at rest. At
  54 segments with no file the shader runs v0.1.1's own path (`Custom == 0`), proved
  byte-identical; anything else reads its labels from a texture (AGENTS.md "Wheel files").
- **Files by path go through `files::ReadFileBytes`** (radar's): UTF-16 on Windows, as a
  narrow `fopen` cannot open a non-ASCII path there. Wheel File and Font File both.
- **Every game draws its result by turning paint, never the physics.** Roulette's
  frets and the money wheel's pegs see only the FRACTIONAL part of their angle in
  pockets/segments; whole ones are invisible by construction, which is what makes
  the shift exact (`--roulette`, `--wheel` hold the paths bit-identical).
- **The planners run on a worker** (`std::async`), except the slot machine's, which
  is a schedule (microseconds). A play starts on the frame its plan is ready,
  back-dated to the press so Land On still lands. The harness plans on the render
  thread (`SetSynchronousForTest`). An abandoned plan's future is kept until it
  finishes: a `std::async` future's destructor blocks.
- **The shader's constants are mirrored from the C++** (roulette's profile, the
  money wheel's geometry, the craps table, the lottery drum, the text spans): each
  block says `= mirrored in`. The readbacks are what catches a drift.
- **One list of programs**: `shaders::Programs()`. The plugin compiles exactly it,
  `tools/glslc.sh` reads it from the source text, `jptest --shaders` writes it.
- **The clip is premultiplied** (Resolume's DXV clips with alpha are) and is
  un-premultiplied before it is lit in linear.
- `smoothstep` with edge0 >= edge1 is undefined in GLSL: use `sstep`. verify.sh
  greps for both orders.
- **`near` and `far`** are macros under windef.h and GLSL reserved words; `noise1`–
  `noise4` are GLSL built-ins. verify.sh greps for all of them.
- **Arena addresses parameters by name**, lower case, spaces removed: names must
  be unique that way (`--names`) and must not contain `/`.
- Option parameters arrive as the element's value; `OptionIndex()` clamps.
- Override `SetTextParameter` to return FF_SUCCESS for the About block, or no
  host can instantiate the plugin.
- `jackpot_core` is an OBJECT library: the registrations are file-scope
  constructors nothing references.
- Randomness is PCG integer hashing, never `fract(sin(...))`; draws are Lemire's
  unbiased multiply-shift.
- Public at github.com/stoatworks-labs/jackpot: a `v*` tag runs `release.yml`, and
  CI runs because the repo is public (the fleet's rule for Actions minutes).

## Not done yet
- In Resolume: two plays on this Mac, and the Windows gate (17/17, no play pressed),
  both 2026-10-09; no OpenFX port, no browser demo, no presets. The 0.1.1 pile fix
  (jackpot#3) was run in Arena on Windows, 2026-10-12 (Shower Only); Play's place (jackpot#2) has not been looked at.
- **Wheel files (jackpot#1) have not been loaded into Resolume** on either platform.
  Wanted on winlab: a Wheel File from a folder with a non-ASCII name (the UTF-16 open is
  checked only as far as the conversion, on the Mac), a file saved by Notepad as UTF-8,
  "Unicode" and ANSI, re-choosing an edited file, Segments and the Fixed lines, and that
  a saved composition restores the renumbered controls (Arena stores them by name).
- **plugin-bench `arena/expect/jackpot.json` needs re-seeding** from a running Arena: four
  new controls and every id from Pyramids on moved up four (source 54, effect 55
  declared). The new ones change an idle picture only with Game on Money Wheel (Lines
  and Short File also need a file of more or fewer lines than Segments).
- **ATTRIBUTIONS (generated)**: `source/FileBytes.cpp` is radar's `ReadFileBytes` /
  `Utf8ToUtf16`; stoatworks-backend's `derived.json` may want an entry for it before the
  next sync. `docs/USER-GUIDE.pdf` is not rebuilt for the wheel files.

## Diagnostics

`source/Diag.{h,cpp}` is a log file only, with no crash handler (this runs inside
Resolume). It records the GL vendor/renderer, a shader that failed to compile, the
font in use (and a named font that is not installed), and every play's result,
warp and kind (win, jackpot, near miss).

    ~/Library/Logs/jackpot/jackpot.YYYY-MM-DD.log
