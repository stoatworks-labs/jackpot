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
- Everything: `tools/verify.sh` (~3 min: reserved words and reversed smoothsteps,
  glslc, the pin, a fresh universal build, both bundles through lipo/plist/codesign/
  oxbow probe+selftest, the programs through the driver, every check, `--offline`,
  the dice tables across three builds, the `--pipe` format, the negative controls,
  the mutants, the sweep, the bench)
- **The results**: `--slots`, `--roulette`, `--wheel`, `--craps`, `--lottery`;
  out of the pixels: `--slots-readback`, `--roulette-readback`, `--wheel-readback`.
- **The physics**: `--nearmiss`, `--blur`, `--shower`, `--duration`, and the
  C37/C54 identities, the departure and the clapper inside `--roulette`/`--wheel`.
- **The plugin**: `--defaults`, `--names`, `--determinism`, `--over-check`,
  `--resize`, `--fonts`, `--state`.
- **The checks can fail**: `--negative` (21 wrong models), `tools/mutate.sh`
  (three GLSL and seven C++ one-character mutants).
- What CI runs: `--offline` (the checks that need no GL context, and their
  negative controls; says loudly that the pixel checks were not run) and
  `tools/glslc.sh`.
- No dead controls: `python3 tools/sweep.py` (46 parameters, both plugins;
  `--no-context` empties the context table to show what it holds up).
- Cost: `--bench` (720p/1080p/4K for every game; the planners' ms).

## Notes
- **Units are metres, kilograms, seconds** in the games; the shower's world is in
  frame heights and scales its physics by the piece's real size.
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
- Not yet on GitHub. When it is: a `v*` tag runs `release.yml`; CI only if the repo
  is public (the fleet's rule for Actions minutes).

## Not done yet
- In Resolume only on this Mac (two plays, 2026-10-09); no OpenFX port, no browser
  demo, no presets.
- `StoatworksAbout.h` and `ATTRIBUTIONS.md` are provisional hand copies until
  Jackpot is registered in stoatworks-backend (whose sync scripts then own them,
  with the issue forms and FUNDING.yml).

## Diagnostics

`source/Diag.{h,cpp}` is a log file only, with no crash handler (this runs inside
Resolume). It records the GL vendor/renderer, a shader that failed to compile, the
font in use (and a named font that is not installed), and every play's result,
warp and kind (win, jackpot, near miss).

    ~/Library/Logs/jackpot/jackpot.YYYY-MM-DD.log
