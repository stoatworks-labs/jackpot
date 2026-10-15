# AGENTS.md — jackpot

The why. `CLAUDE.md` is the command reference; `README.md` is for people who will
use the plugin. This file is for whoever changes it next.

## The idea

Allan asked (2026-10-06) for "a new resolume plugin that recreates a one arm bandit,
roulette wheel, craps table and other visually distinctive casino games, having some
'chip shower' type modes would be cool too". The spec is
`~/Projects/resolume/specs/SPEC-jackpot.md`.

Two easy answers were both wrong, as they were for polyhedral. **Animate a canned
spin and swap the number at the end**: the number visibly changes, or every spin is
the same spin. **Simulate and report whatever lands**: the result cannot be chosen,
and "random" is only as fair as the initial conditions.

What the plugin does rests on one observation: **every casino game has a symmetry
the physics cannot see.** A roulette rotor's frets are identical, so turning the
numbered ring by whole pockets changes nothing the ball meets. The money wheel's pegs
are identical. A die is face-transitive. Lottery balls are identical. So each game is
simulated honestly, ahead of time and deterministically from the seed, and then the
PAINT is turned (ring, segments, R·S, the numbers dealt to the balls) so the result
shown is the one drawn. The slot machine needs no symmetry: since Telnaes's virtual
reel the random number at the pull IS the result and the reels are a display, so the
plan is a schedule that drives them to it.

The turn is never seen: roulette's ring is taken there by the croupier's spin-up
before the ball is released; the money wheel's by the operator's pull before it is
let go; the lottery's drum is empty between draws and the new balls drop in already
painted; a die's S is a rotation of the solid onto itself.

## Shape of the code

    Maths.h, Geometry.*, Physics.*   polyhedral's dice engine (Physics gains a
                     pyramid-studded wall plane)
    Common.*         the seeded Stream, the playback rate curve, the Outcome
    Controls.*       parameter ids (inserted within a group: see Controls.h) and every 0..1 -> unit mapping
    Transport.h      honeydew's beat and bar clock
    Slots.*          strips, the virtual reel, the pay table, the reel schedule
    SymbolArt.*      the reel symbols, painted on the CPU into an atlas
    Roulette.*       the ball on a profile of cones, frets, diamonds; the plan
    MoneyWheel.*     the wheel against its clapper (N m pegs); the plan
    WheelFace.*      what the wheel wears: the Big Six or a Wheel File's lines;
                     reading, cleaning, the draw, fitting labels (jackpot#1)
    FileBytes.*      a file by its UTF-8 path, UTF-16 on Windows (radar's)
    Craps.*          the throw on polyhedral's engine; the pass line
    Lottery.*        spheres in a sphere, the air, the tube; the deal
    Shower.*         live rigid discs; the pile, a height field that ages away
    Typeface.*       polyhedral's font scan and SDF atlas, widened to capitals
    Shaders.*        one pass per game, the shower's instances, the composite;
                     shaders::Programs(), the one list
    Jackpot.*        the plugin: parameters, clock, requests, uploads, draw
    tools/jptest     the harness

## Decisions

- **Name and ids** are the spec's: `SW Jackpot` / `JP01` (source) and `SW Jackpot
  Over` / `JP02` (effect, 15 characters, inside FFGL's 16). No fleet id collides
  (checked against every source tree in `~/Projects/resolume` and `~/dev`). Bundles
  `Jackpot.bundle` and `Jackpot Over.bundle`.
- **Two sessions built it.** The first (2026-10-06) wrote the spec, the scaffold,
  every planner, the slot shader and most of the plugin class, and stopped at a
  usage limit before anything was built. The second (2026-10-08) built it, drew the
  other games, wrote the harness and the tooling, and found most of the traps
  below. The commits carry the model that wrote them (Claude Opus 5.5), not the
  BRIEF's Fable trailer.
- **The About block, ATTRIBUTIONS.md, FUNDING.yml and the issue forms are generated**
  by stoatworks-backend's sync scripts since Jackpot was registered (2026-10-09). The
  block has four buttons (the guide among them), so the Over effect's Mix is id 54 (50
  before jackpot#1's four Money Wheel controls went in after Clapper).
  A registration change that adds or drops a button moves Mix: the static_assert in
  Jackpot.cpp names the PT_ABOUT_BUTTON_n to add or remove, then rebuild and run
  tools/verify.sh before tagging.
- **2D where the game is seen face on**, 3D where it is not. The slot machine and the
  money wheel are drawn in the canvas (y up, the frame 1 tall at Zoom 1); roulette,
  craps and the lottery are ray-traced from one camera (`SetCamera`: south, Tilt up,
  a right-handed view). The lottery's camera is Tilt minus 35 degrees (a machine is
  seen from the front); Light Angle and Tilt do nothing to the 2D games.
- **Roulette's numbers are read clockwise, as wheels are always written, and stored
  anticlockwise**, which is the direction angle increases here (`Anticlockwise()`).
  The numbers' tops point outward.
- **The money wheel's values are stacked upright**, one character under another, so
  the segment at the clapper reads level. Written along the radius they read
  sideways exactly where the result is read.
- **Spin Time is one control for five games.** 4 s by default: a quick roulette spin,
  a natural slot pull, a slow-ish craps throw. Craps meets a long Spin Time with the
  shooter's hold (below); the lottery cannot load 49 balls in under 2 s, so short
  draws play fast, and the README says so.
- **The result board** (Display) shows each game's result in words over the top of
  the frame once it is at rest; the slot machine has its own banner instead.
- **Supersampling**: up to 1080p every 3D pixel takes four rays (rotated grid); above
  it, four only where the 2×2 quad's derivative of the hit id (`HitId`, `fwidth`)
  says two surfaces meet. That took roulette at 4K from 15.3 to 5.4 ms. An edge
  that runs between two quads is single-sampled; at 4K that is a small pixel.
- **The shower is seen from 15 degrees above, orthographically.** Straight on, the
  pieces in the pile were edge-on lines.
- **The pile lasts a fixed 12 s, not a control** (0.1.1, jackpot#3). Allan chose a
  fixed linger over a new parameter: no new id, nothing for a saved composition to
  restore. Twelve seconds from the throw is about ten lying in the pile, which outlasts
  the next play after a win at the defaults (Auto Play every 10 s, Spin Time 4 s), so
  a run of wins keeps one pile; a single win's pile is gone about 15 s after its
  shower began. A Pieces change does not clear the table: the old kind ages out.
- **Game and Play are in no group** (0.1.1, jackpot#2). FFGL gives a parameter one
  group, and Resolume makes a group of each run of consecutive ids that share a name,
  so with the Game group closed Play was hidden. The group starts at Spin Time now;
  nothing was renumbered then, and Arena stores parameters by name. (jackpot#1 did
  renumber, on that footing: see "Wheel files".)
  `--names` holds Play ungrouped and every group one run of ids.
- **No presets, no audio trigger, no history board, no pay table on screen** in
  0.1.0; the spec's out-of-scope list (cards, pachinko, OpenFX, browser demo, Arena
  gate) stands.

## Meeting Spin Time, honestly

Every game is simulated at its own pace and played through `Playback`
(polyhedral's): a rate falling linearly so the mean rate is natural / duration, with
the end at least 65% of the start (`--duration` checks the bound). Each planner first
aims the natural time at the duration by choosing the launch:

- **Slots** is a schedule: each reel's run speed is solved so it covers whole laps
  plus the distance to its stop and the detent catches it exactly; the last reel is
  at rest at Spin Time to the bit.
- **Roulette** chooses the ball's speed by secant steps on log time (6 tries) after
  the croupier's 1.2 s spin-up. Warps measured 0.98–1.20.
- **The money wheel** does the same after a 1 s pull. Its settle time is **not**
  monotonic in speed, and before the clapper was made lossy (below) the secant went
  astray: a 3 s spin played at 2.9×. Now 0.95–1.65.
- **Craps never throws harder to fill time.** Dice settle in 0.6–1 s however hard
  they are thrown; a 4 s throw rebounded the length of the table to the shooter's
  rail. Every throw is 1.9–2.5 m/s (seeded), rethrown softly for the first six tries
  until both dice rest on the printed layout, and Spin Time is met by a hold after
  the sweep plus at most 0.6× slow motion.
- **The lottery** spreads mixing and the gaps between draws over what is left after
  loading; under about 6 s it cannot keep up and plays fast (3 s: about 3×).

## The pile, and how it goes (0.1.1)

jackpot#3, reproduced in Arena 7.27.1 on Windows with the 0.1.0 DLL: chips showered,
Pieces switched to Coins, and the chips stayed; with Shower Off the pile did not change
for two minutes, and only Pile off cleared it. Four causes in `Shower.cpp`:

- **Nothing aged.** The only drain was a count cap: past 700 pieces *at rest*, the
  oldest faded. Now every piece has an `age`, and at `kLinger` (12 s from the throw)
  it fades in `kFadeTime` (0.5 s). The list is kept in throw order (appended, removed
  in place), so its front is the oldest.
- **A piece that stopped on a slant never came to rest.** The settle asked for the
  tilt to be within 16°; pieces stopped at 17–26° against a step of the height field
  were still for ever, never faded, and the cap never counted them. Now any piece in
  contact and still for 0.25 s comes to rest: within 16° it is laid flat as before,
  steeper it stays as it lies, and `Stamp` raises the field to its tilted face.
- **Nothing left the table front or back.** Only y and |x| were culled and `FloorAt`
  is flat past the grid, so chips rolled to z = −22. Now a piece past |z| = 0.6
  (`kTableHalfZ`, the grid's edge) fades in `kQuickFade` (0.2 s) and is culled a
  radius further on. The back edge is on screen (about a quarter of the way up, seen
  from 15° above), so a slow roller fades rather than popping out.
- **The height field only rose.** `Settle` wrote heights as a max and nothing lowered
  them, so new pieces landed on faded, invisible ones, and Pile off and on left the
  old pile's height behind. Now whenever a piece at rest goes (or Pile changes) the
  field is rebuilt (`Rebuild`) from the pieces still at rest, in the order they came
  to rest, each brought down onto what is now under it, so nothing floats; with Pile
  off it is empty.

Because of the second and third, under Always the 900-piece pool filled with pieces
that never left and the shower stopped throwing: at the default Amount, 17 of 810 in
the ten seconds to the fifth minute and 0 to 9 from the sixth on (a 15-minute probe). The cap now counts **every** piece, in the air or not, and past 90% of the
pool (`kCapPieces`, 810) fades the oldest in 0.2 s: 90 places freed every 0.2 s pass
450 pieces a second, more than Amount's 400. At Amount 1.0 a piece then lasts about
two seconds, so the pool, not the linger, sets how deep the pile gets: a churn, not
a heap.

`--pile` measures all of it (A–D below; 0.1.0's numbers from the same harness built
against 0.1.0's `Shower`, with the new fields shimmed in):

| | 0.1.0 | 0.1.1 |
| --- | --- | --- |
| A: Chips, three Shower Nows, Coins, three more, quiet; 30 s after the last | 743 of 1,080 left, 363 of them chips | 0 of 1,223 |
| B: furthest any piece's near rim got past the table's edge in z | 21.9 (z = −22.5) | 0 |
| C: Always at Amount 1.0 for 60 s, thrown in the last 10 s of 4,000 | 234 | 4,000 |
| D: the most FloorAt stands above the floor, once empty / after Pile off and on | 0.029 / 0.049 | 0 / 0 |

A 12-minute run of C threw 4,000 of 4,000 in its last ten seconds; 15 minutes at the
default Amount, 810 of 810. The negative controls switch each mechanism off
(`ShowerSettings::noAgeing` fails A, C and D; `noEdgeCull` fails B; `noRebuild` fails
D), and two mutants pin the shipped lines (the age's `+=`; the cap's choice of a
piece). `--pile` costs about 12 s, most of it C's minute at 400 pieces a second.

`--shower` used to judge only the pieces that passed the settle gate (277 of 362):
the rest were off the frame, still rolling, or stuck on a slant, and nothing told
them apart. It now judges every piece: at rest (flat, or laid as it lay), gone, or
still moving -- and a piece still moving must be moving: a copy stepped on for half
a second comes to rest, travels a thousandth of a frame or turns a degree. On a
dense pile (eight presses, 60 s) that finds 46 stuck pieces of 82 unsettled in 0.1.0
(tilted 17–26°) and none in 0.1.1. The check's own scenario had none stuck even in
0.1.0, so its census guards the gate rather than reproducing #3; `--pile` does that.

Known and left: a piece at rest across the edge of a step of the pile can fall into
a limit cycle -- pushed up out of the step by the position correction and falling
back, eight steps round -- at up to 2.6 cm/s instantaneously while going nowhere
(0.09 thousandths of a frame in half a second), so the stillness test never passes
and it is never stamped. It ages out with the rest. Seen once in 306 pieces of a dense pile.

## Wheel files (jackpot#1, not released)

VJPandaAU (a working VJ, on Windows 11) asked for their own values on the money
wheel: a number of segments, a text file of values, N of them drawn at random or the
first N in order, a character limit that grows as the segments get fewer, emoji as a
bonus. Allan approved it, and VJPandaAU answered the questions on the issue: any markers
will do, fewer lines than segments should be a choice (default repeat), draw again
only when something changes ("loading the wheel with values and then it is spun
multiple times"), only the marked segments win, and "KISS -- common ones" for emoji.

Four controls in the Money Wheel group, after Clapper: **Segments** (integer 2-54,
default 54), **Wheel File** (`SetFileParamInfo` only, "txt"), **Lines** (*In Order*,
*Random*; default Random, which was the ask) and **Short File** (*Repeat*, *Fewer
Segments*). Inserting them moved Pyramids onwards up four (Mix 50 -> 54). That is
safe because Arena stores a composition's values by NAME (vectrix `docs/NOTES.md`
proved it: a composition saved, an id moved, every value reloaded onto the right
control; radar's Sprite controls went in the same way); the group has to stay one run
of ids, so appending them at the end would have split it.

**Reading** (`WheelFace.cpp`, `FileBytes.cpp`). `SetTextParameter` only stores the
path under the text mutex (and must succeed for the empty default the SDK pushes);
`UpdateWheelFace`, at the top of `Tick`, reads it on the render thread when the path
was SET, the same path again included, so choosing an edited file again re-reads it.
Never in the constructor. The bytes come through `files::ReadFileBytes`, radar's
(`source/SpriteImage.cpp`, radar#5) with its cap made the caller's: on Windows the
host's UTF-8 path is widened by a strict `Utf8ToUtf16` for `_wfopen_s`, and a path
that is not UTF-8 at all is opened narrow. Decoding: a UTF-8 BOM is stripped, UTF-16
LE/BE with a BOM is read, NUL bytes or over 5% control characters are refused as "not
a text file", valid UTF-8 is UTF-8 and anything else is Windows-1252 (what an old
Notepad saved). LF, CRLF and CR end lines; blank lines are skipped but counted, so a
value's line number is an editor's. 1 MB and 1,000 values at most. A failure leaves
the Big Six, says why in `diag::warn` and `LogToHost` once per distinct message
(hosts may set a path repeatedly), and is kept in `WheelFileStatus()` for the harness.

**Cleaning.** A leading `*` (after trimming) marks the jackpot style, `%` the joker
style, and is removed. The atlas has digits, A-Z and `$'+-.:/!`, so: lowercase is
folded up, Latin-1 and Latin Extended-A letters lose their accents (two tables in
`Fold`), curly single quotes and dashes become ' and -, an ellipsis three dots, runs
of whitespace one space, and the rest is DROPPED -- including any other `*` or `%`,
which is why there is no escape for a literal marker (there is nothing to print it
with). Emoji: a line that is exactly one of 🍒 🍋 🍊 🔔 🍉 💎 7️⃣ (variation selectors
ignored; 7 + U+20E3 with or without U+FE0F) paints that `SymbolArt` tile, and the
board says its name. **Alone only** -- inline emoji would have needed a second kind of
"glyph" in every text path (advance, ink box, colour rather than distance); an emoji
inside text is dropped like any other character, and an unmapped emoji alone leaves a
blank segment.

**The draw.** Random is Fisher-Yates over ALL the values, keyed on Seed and the
number of values only, and the first Segments taken. So it changes only with the
file, Seed, Segments, Lines or Short File (`FaceKey`), never from spin to spin; a
larger Segments adds to the draw instead of redrawing; editing a line's text leaves
every value where it was. Fewer values than segments: Repeat cycles them, Fewer
Segments makes the wheel `max( 2, values )`. The values go CLOCKWISE from segment 0
(segment `( n - k ) % n`), so the wheel, pulled anticlockwise, brings them past the
clapper in order. Plain values cycle the Big Six's five money colours (the last
takes purple when it would match the first); `*` is black with a gold star (the
logo's look), `%` red with a gold star -- in v0.1.1 the joker and logo look alike, and
the two markers ought to be told apart.

**A new face is never painted over a spin or its result.** `UpdateWheelFace` builds
the face only when its key changes, keeps the shared v0.1.1 face (`DefaultFace()`)
whenever the result is the 54-segment Big Six, and replaces the pointer only when the
contents differ. `Tick` lays the wheel out again (`Relayout`) with the new face only
when nothing is showing: an idle wheel at once; a play only once it has landed, been
through `OnRest` (the shower, `winSince`, the log) and kept its wheel, highlight and
board for `kBannerFor` (3.5 s, the slot machine's banner time, now in Jackpot.h). A spin
under way finishes on the wheel it started with (its plan holds its face), and a plan
still on the worker counts as under way. Play always takes the new face (`WheelRequest`
reads it): pressed during a result's 3.5 s, or mid-spin after a change, the new play
repaints the wheel as the pull begins. That is cosmetic -- the pull turns the paint
anyway -- and left as it is.

**The physics: P = N m pegs.** m = round( 54 / N ), so P is 37-72 and the clapper meets
about what it always met. A peg at every divider and m - 1 inside each segment. The
simulation is in pegs (`Release::pegs`); the paint turns by whole SEGMENTS, i.e. m
whole pegs, so C54 becomes C_P and stays exact (`--wheel-custom` holds it for 37, 48,
56 and 72); the segment under the clapper is the peg gap divided by m (`Plan::Shown`).
Each segment stays equally likely because the draw is `Below( N )` and the landing is
the draw. At 54 every expression is v0.1.1's (`2 pi / pegs` with pegs 54 is the same
IEEE division as the old constexpr), which the frame comparison below confirms. A
wheel of 2 plays the 54's very trajectories: the physics sees only P. Warps
(`--duration`): 0.95-1.65 at 54 and on 2; 1.00-1.91 on 37 (37 pegs, the fewest, so a
3 s spin plays fastest); 6 s spins 0.91-1.06 everywhere.

**Labels in a texture.** The v0.1.1 path keeps the 54 values in `Layout[54]` and seven
spans in the text uniforms (24 spans, 192 characters at most); 54 labels of 7 do not
fit. So a custom face's labels go in an RGBA32F texture, a row a segment, read with
`texelFetch` (unit 4, NEAREST, created at InitGL with one row so it is never
incomplete): texel 0 (style, symbol, characters, set across), texel 1 (height or the
symbol's half size, anchor, ink left, ink width), then a glyph index a texel.
`FitLabels` sizes each label on the CPU from the atlas's ink boxes, so it is redone
when the font changes (`atlasGeneration`). Up to 12 segments a label is set ACROSS (one
line, level at the top: the anchor is its middle's radius, kept at 0.62 m when the
type allows); from 13 it is STACKED as v0.1.1's, from 0.775 m in (nearer the hub when
big type would reach the rim). The type is the largest (to 0.15 m across on 2-4
segments, 0.10 on 5-12, 0.11 stacked) that keeps every character's box, a quarter of a
height of slack above and below a capital, kMargin (10 mm) clear of both dividers,
inside the outer band and clear of the hub (0.30 m; 0.41 on a marked segment, above
its star). Caps: 32 characters on 2, 24 on 3-4, 16 on 5-8, 12 on 9-12, 8 on 13-40, 7 on
41-54 (7 so JACKPOT, the Big Six's own longest word, fits); the board keeps 32.
Symbols are a square tile fitted the same way, as far out as fits when stacked.

**Results on a file's wheel.** "Only the marked segments win" makes Win and Fixed part
company, where on the Big Six they were the same pick (the bet's value): Win lands on
a marked segment (either kind), Jackpot on a `*`, Near Miss on an unmarked segment
beside a `*`, Lose on an unmarked one, and Fixed on the LINE numbered Fixed Number,
counted as an editor counts (with Random the segment number would be meaningless to
the operator; a line is what they can see in the file). Nothing that fits -- Jackpot
with no `*`, a line Random did not draw -- is drawn as Random, as v0.1.1's `pick` already
did for an impossible ask, and the play's log line says so (`Plan::fellBack`). The
outcome: `win` = marked, `jackpot` = `*`, `nearMiss` = not a win beside a `*`, the
board's words the line (to 32). The Big Six keeps v0.1.1's rules against the values
its wheel has (the bet snaps to the nearest value present).

**Segments with no file** is a smaller Big Six: the joker at 0, the logo opposite at
N/2 from 4 segments up, and the money values between them in the Big Six's own order
(`Layout()` without its two top segments). At 54 that is `Layout()` itself.

**Defaults change nothing.** The v0.1.1 face, the v0.1.1 pegs and v0.1.1's uniforms run
whenever the face is the 54-segment Big Six (`Custom == 0`); the custom face is a
separate block beside it, and the v0.1.1 block moved only by an indent. Proved, not
assumed: the v0.1.1 harness (built from the tag's source, a8ff0ba) and this one, `--film`,
byte for byte over 4,500 frames in ten scenes -- the money wheel Random (two plays), on
Jackpot with the board, on Fixed 20 over Felt at 1080p, the Over on Near Miss, 900 frames
of Auto Play at 640x360, and slots, roulette, craps, lottery and the shower -- all
identical. (8-bit frames, as radar compared Sprite Count 0; the float picture was not
compared.)

**Cost** (`--bench`, three runs each, against the v0.1.1 harness at its defaults): the
Big Six at 1080p 0.48-0.72 ms before, 0.49-0.56 now; a wheel of 54 from a file of 80
long lines 0.50-0.71 ms; at 4K 1.40-1.41, 1.37-1.38 and 1.42-1.44. Within the bench's
noise (an idle-ish GPU's clock moves a 0.5 ms frame by half again).

**Decisions taken without asking** (the issue settled the rest):

- `*` jackpot, `%` joker (`$` collides with money); no literal marker, as neither
  character can be printed anyway.
- Lines defaults to Random (the ask), Short File to Repeat (the issue's choice).
- Win = any marked segment; Fixed = a line of the file; an ask nothing fits plays
  Random and logs it; Near Miss is beside a `*` only.
- The joker style is red, so the two markers look different; the Big Six's joker
  keeps v0.1.1's black.
- Emoji alone only; other characters folded or dropped as above.
- Re-choosing the same path re-reads the file (edit the list, choose it again).
- The draw keyed on Seed and the number of values, not the text.
- A new face waits for the spin to land and its result's 3.5 s (the reviewer's rule,
  after the first version lost results: see the traps).
- The result board shows the whole line (to 32) even when the segment cut it.
- Segments without a file: the smaller Big Six above.
- Typeface's own `fopen` of Font File (and its second open for the family name)
  now goes through `ReadFileBytes`, with a narrow retry on Windows for a path the
  ANSI font scan (`FindFirstFileA`) produced. The scan itself still opens narrow:
  its paths ARE in the ANSI code page.

**Traps hit:**

- **Sampling the Big Six at a stride aliased with its pattern.** `Layout()[ j * 54 / N ]`
  gave 13 segments six $2s in a row and no $1 (a stride of about 4 hits one parity of a
  pattern of period 2). Now built from the pattern in order.
- **Big stacked type could never grow.** The stack's top was fixed at v0.1.1's 0.775,
  so a bigger character broke the rim and the fit fell back to v0.1.1's size on every
  wheel. Moving the anchor in by height alone still failed, as the first character's
  corner (its half width) then sat just outside the circle; the anchor is now set from
  that corner.
- **6 characters for 41-54 cut JACKPOT** (the design's first number); 7.
- **The old GLSL mutant's text appeared twice** once the v0.1.1 face sat in a branch
  beside the custom one; both mutants now carry the line before (SEGMENTS / Segments).
- **A face change during a spin lost the spin's result** (found in review, after the
  first push). The first version re-laid the wheel out as soon as the spin's time was up,
  and `Relayout` runs before the `OnRest` check in `Tick` and sets `restHandled`: on
  the landing frame the new face won, so there was no shower, no `winSince`, no board,
  no highlight and no log line. The same with a worker's plan made on the old face.
  Changed at rest, it wiped the board on the next frame. Now gated on `restHandled`
  and `kBannerFor`; `--wheel-draw` changes Segments mid-spin (synchronous, on the worker,
  and on a file) and at rest, and its negative control (`faceSwapAtOnce`, the old rule)
  fails it: landed NO, shower NO, board empty.
- **Values that clean to nothing loaded without a word.** A Cyrillic, Greek or CJK file,
  or unmapped emoji, gave blank segments and an "ok" log. Now counted (`blank`), noted in
  the log, and a file whose every value is blank warns through `LogToHost` too.
- **Setting the same path again re-read the file every time** on the render thread (a
  host re-sending a path, a slow share). Now only when its size or modified time has
  changed (`files::StampFile`: `stat` here, `GetFileAttributesExW` on Windows), so an
  edited file chosen again is still read.
- **`int( mod( k, float( Segments ) ) )` can be Segments itself** at a float boundary,
  a `texelFetch` past the last row. Clamped (`segmentIndex`) in the custom branch only;
  the v0.1.1 branch is untouched and still compares byte for byte.

**Checks** (all in `verify.sh`; the first three offline, so CI runs them):

| check | what it holds | negative controls |
| --- | --- | --- |
| `--wheel-file` | nine texts as expected (markers, blank lines counted, LF/CRLF/CR, BOM, UTF-16 LE/BE, Windows-1252, the emoji, folding, dropping) incl. `glücksrad.txt` by path; six bad files refused with the reason; the 1,000-value cap; a Latin/Cyrillic/emoji file name read, and its path's UTF-16 equal to the compiler's own `u""` literal, five malformed sequences refused; values the type cannot print counted, noted, and warned of when they are all there is; the same path set again read only when its size or time changed (reads 1 1 2 2 over choose, choose, edit, choose); the plugin's empty default, a file, a missing and a broken one (Big Six, and why), cleared | the path widened byte by byte (a narrow fopen); the BOM and CR kept, LF the only line end; markers ignored; the file read again whenever its path is set |
| `--wheel-draw` | In Order clockwise and met in order by the clapper; Random distinct, stable per Seed, a prefix as Segments grows; chi-square over every Seed (which are drawn, which lands first); Short File both ways; the cuts; through the plugin, six plays on one draw, Seed away (painted after the result's 3.5 s) and back, an edited file re-read with its lines in place, re-chosen unchanged the same face; Segments changed mid-spin and at rest (synchronous, on the worker, a file): the spin lands, showers and shows its board, keeps its wheel 3.5 s, then the new one; idle at once; Play takes the new one | Sattolo's off-by-one shuffle; the draw redone every play; the new wheel painted the moment the spin ends (the first version) |
| `--wheel-custom` | 128 plans on wheels of 2-54 (a file, the Big Six on 2 and 13), every Result: at rest on the drawn segment, read under the clapper as painted, of the kind asked, falling back only when nothing fits; outcomes (only marked win; the Big Six as v0.1.1); 6 s warps 0.85-1.30; a file with no markers never wins; chi-square per N; C_P for 37, 48, 56, 72 pegs | no shift; Random below N - 1; every segment wins; a peg a segment (P = N: a 2-segment wheel plays 1.84x) |
| `--wheel-geometry` (GL) | 2, 8, 24, 54 segments of awkward labels, 1280x720: the id picture's arcs within 2 px of 2 pi / N, in order; `TestFlat` 3 (R the segment's own ink, G a neighbour's ink reaching in): no G, no own ink within a divider's half width and a pixel, none off the bands, every label and symbol showing | every label at its largest; the shader told N + 1 segments |
| `--wheel-readback` (added rows) | Fixed lines on wheels of 2, 8, 24, 54, read under the clapper from the id picture, 640x360 and 320x180 | no shift (the existing control) |
| `--duration`, `--determinism` (added rows) | the wheel on 2, 8, 24, 36, 37 segments; a file's wheel played twice | the existing ones |

The fixtures are in `tools/fixtures/` (`.gitattributes` keeps their bytes:
`bom-crlf.txt` and `cp1252.txt` are what they are named). `showcase.txt` is
`docs/wheel-file.png`'s.

## The traps, in the order they cost time

**The roulette contact read the wrong side of the surface.** `Profile()` took the
sign of the distance from the side of the nearest segment's LINE. Past the end of a
steep segment — a pocket's step — that line runs through open air, so a ball 15 cm
away read as deep inside, and on its first step it was thrown 4 cm inward and pinned
there whatever its speed. Solid is now "below the profile as a height field, or
beyond the lip"; the nearest segment only gives the distance.

**The pocket lost the whole pockets the rotor had turned.** The physics places the
frets from the rotor's fractional phase alone (that is the C37 claim), and the rest
pocket was counted in that frame — but the painted ring had turned the whole pockets
too, so the shift was applied in the wrong frame by however many pockets the rotor
turned during the spin. A ball in 35's pocket reported 22. The pocket is now counted
from the rotor's pocket 0, whole turns and all (`RotorPockets`).

**A fret standing proud held the ball for ever.** Frets ran out to the apron's edge
4 mm above it; a ball rolling inward on the apron, carried by the rotor, leaned on a
fret's end in exact equilibrium (four of 78 launches never settled). The frets now
stop at the steps' feet and are flush with the rims (`kFretHeight` 0.014).

**A mirror-image wheel.** The tables were written in the usual clockwise order and
read anticlockwise. Nothing checks a wheel's handedness except a person who knows
32 follows 0 clockwise; the render did.

**The lottery's swirl was a centrifuge.** A tangential push several times the jet's
lift spun the whole cloud up until every ball pressed against the glass in a ring
round the equator (about 120 m/s² outward), out of the jet; no ball reached the
mouth in 70 s. The swirl is now a trace (0.08), the jet 2.5× stronger and slower to
fall off, and the open tube draws in what is near its mouth.

**The lottery's capture asked a ball under the mouth to be rising.** A ball pressed
against the glass there is not rising. The mouth is a hole: a ball under it while it
is open goes up the tube. (Found by counting zone entries: a probe that counted
samples in the zone was fooled by the captured balls, whose keys freeze there.)

**Infinity times zero is NaN.** A highlight evaluated far from its shape — a ball's
"normal" grows without bound, and `pow( x, 60 )` of it overflows — times a coverage
of zero is NaN, and drew black wedges twice (the slot's knob, the money wheel's
pegs). `layer()` now returns what was under for zero coverage, whatever the colour.

**`smoothstep` with its edges reversed is undefined** (GLSL 4.10 §8.3). Apple's
driver does the obvious thing; another need not. Reversed ramps go through `sstep`,
and verify.sh greps for literal reversed pairs (it found two more after the first
fix).

**Craps threw hard enough to fill Spin Time** and the dice came back to the shooter.
See above.

**The money wheel was not exactly symmetric.** Its angle was one double; turning the
pegs by k segments added k·2π/54 to it, and rounding moved the path by 2×10⁻⁸ rad —
small, and not the identity the spec claims. It is now whole segments plus a phase,
only the phase reaches the pegs (as roulette's frets), and `--wheel` holds the phase
and the clapper bit-identical.

**The clapper handed everything back.** Elastic both ways, a wheel too slow to clear
a peg was pushed back by the spring with all it had put in, and rocked backwards
over three or four pegs, several times (five reversals). Real leather is lossy: the
spring now straightens at a third of its stiffness while pressed against a peg.

**Roulette's rest pose came 0.3 s late.** The ball is "at rest" after 0.3 s under
12 mm/s relative to the pocket, and the pose was taken at the END of that window; the
keys stop at its start, so the ball jumped a few microns at Spin Time. Then, at
exactly Spin Time, simT − spinUp rounded a hair short of the rest time and drew the
last keys instead. The pose is now the one at the window's start, and the play's end
selects it.

**The clip is premultiplied.** The effect took it as straight and multiplied by alpha
again, darkening every soft edge (0.22 at a half-clear pixel). colourunder measured
Resolume's DXV clips with alpha: rgb ≤ a on 99.6% of pixels. `--over-check`'s first
version tested only the frame's edge columns, where the card was wholly clear or
wholly opaque, and could not see it; its top row now crosses all three alphas.

**mutate.sh let a mutant survive by not building it — twice.** make compares times
to the second. A mutant written in the second the last build finished was "up to
date"; then Shaders.cpp, which compiles in under a second, produced an object in the
old harness's second and the link was skipped. Either way the old binary ran and the
mutant "survived" — a different one each run, which is what gave it away. The
mutated file's object and the harness are now deleted before every build.

**A negative control that could not fail.** Dropping the gyroscopic term was meant
to break Euler's disk; it did not, because the whirr comes from the rolling contact,
not the free-body term. That model now has its own check (a tumbling chip keeps its
angular momentum to 0.5%; without the term, 15%), and Euler's disk is broken
instead by giving the coin a ball's moments of inertia.

**`remainder()` flips at ±π.** The duration check compared the ball's angle in the
ring frame with it and saw a 2π jump at rest; it compares sine and cosine now.

**Strip looked dead in the sweep.** Both strips map one random draw in strip order,
so for many seeds they land on the same stops (seed 0 does). Not dead; its context
is a seed where they part (Seed 1).

**The slot readback nearly tied.** Judged on every sampled pixel, the wrong symbol
was only 1.0–1.2× worse than the right one at 320×180: most samples are paper for
both, and the reference reads the atlas's top level where the shader samples its mip
chain. Judged pairwise on the pixels where two symbols' references disagree (as
polyhedral reads a number), the closest call is 1.3×.

**A second pull jumped the reels back.** The slot request always started from the
stops the last play rested on, so Play pressed while the reels still ran snapped them
back a lap or more. Every other game already carried on from its current state. The
request now starts a mid-spin pull from the reels' current positions, and `--state`
holds a frame's movement across the press under one stop.

**The rest, briefly.** `GLState`'s restore only re-enabled the depth test, never
disabled it, and the shower turns it on. The lip's constant was read from the shader
text at the first `vec2( LIP, `, which is the track's end; it is `LIP_TOP` now. A
space was 0.35 of a capital and ran "31 6" into "316". The wood's grain was polar and
had a seam where the angle wraps. `near` was an identifier in the GLSL and in
Lottery.cpp (a macro under windef.h). Puck was declared and read by nothing (the
sweep). The worktree guard keys on the session's starting directory, so a bare
`git add` after `cd ~/dev/jackpot` was blocked; `git -C <absolute path>` is not.

## Would this hold on another rasteriser, at another raster?

- `--slots`, `--nearmiss`, `--roulette`, `--wheel`, `--craps`, `--lottery`,
  `--shower`, `--pile`, `--duration`, `--defaults`, `--names`, `--determinism`: CPU,
  double, no rasteriser. **Trajectories are not promised identical across architectures**
  (clang fuses multiply-adds on arm64, x86_64 has another libm, MSVC differs again),
  so the same seed may spin differently on a Windows rig. The RESULTS are identical
  everywhere: they come from the integer draw or Fixed Number. The C37 and C54
  identities compare two runs of one build, so they hold on any.
- `--blur`: 320×180 and 640×360. The bound (0.08 mean per channel) is the reference
  reading the atlas's top level nearest where the GPU filters its mip chain —
  measured 0.034–0.055 at rest, where nothing is integrated; the discriminating
  part is relative (the shutter's reference at most half as far as the instant's).
- `--slots-readback`: 320×180 and 640×360, pairwise on disagreeing pixels (0.1
  apart); closest call 1.3×. Another driver's mip filtering moves both errors; the
  margin is what protects it.
- `--roulette-readback`, `--wheel-readback`: the id picture is flat colours, one ray
  a pixel, no blending, so a pixel reads exactly; the point sampled is the middle of
  a band about 4 px wide at 320×180. Raster-independent while the band is wider
  than a pixel.
- `--over-check`: 320×180 and 640×360; the only change is the round trip through
  linear (pow 2.2, then 1/2.2), measured 1.2×10⁻⁷, bound 10⁻⁵.
- `--resize`, `--fonts`, `--state`: raster-independent.
- The Samples-2 edge test uses `fwidth`; another driver's quads may see different
  edges, which changes antialiasing, not any check (the readbacks run at one ray).

## Mutation testing

`tools/mutate.sh`, seventeen one-character mutants, each caught by the named check:

| mutant | caught by |
| --- | --- |
| GLSL roulette pocket at a point: `phi - RingAngle` → `+` | `--roulette-readback` (40 of 40 wrong) |
| GLSL the stop at a reel's row: `floor( u + 0.5 )` → `- 0.5` | `--slots-readback` |
| GLSL the money wheel's segment at a point: `- WheelAngle` → `+` | `--wheel-readback` (6 of 7 wrong) |
| GLSL a wheel of Segments' segment at a point: `- WheelAngle` → `+` | `--wheel-readback` (9 of 12 file lines wrong) |
| C++ the ring's shift: `best.pocket - plan.wanted` → `+` | `--roulette` (36 of 37 wrong) |
| C++ the segment under the clapper: `- whole` → `+` | `--wheel` |
| C++ the segment the physics landed in: `best.gap / m` → `% m` | `--wheel-custom` (104 of 128 plans off target) |
| C++ a file's values laid clockwise: `( n - k ) % n` → `n + k` | `--wheel-draw` (22 of 24 misplaced) |
| C++ the jackpot marker: `U'*'` → `U'%'` | `--wheel-file` |
| C++ a stacked label's clearance from a divider: `x sn - w cs` → `+` | `--wheel-geometry` (300 spilt pixels on 54) |
| C++ a drawn ball marked dealt: `= 1` → `= 0` | `--lottery` |
| C++ the virtual stops beside the seven: 6 → 1 | `--nearmiss` (12.3 per jackpot falls to 2.1) |
| C++ the eased playback's start rate: `0.5 * k` → `0.6` | `--duration` (craps stops early) |
| C++ a disc's axial moment: `0.5` → `0.6` | `--shower` (the tumble, and Euler's disk) |
| C++ a shower piece's age: `+=` → `-=` | `--pile` (771 of 1,223 left 30 s after the last press) |
| C++ the cap's choice of a piece to fade: `<` → `>` | `--pile` (Always at Amount 1.0 throws 1,147 of 4,000) |
| C++ polyhedral's symmetry search: `a - b` → `+` | `--craps` (40 of 44 wrong) |

## The sweep's context table

Emptied (`tools/sweep.py --no-context`), 31 controls go dead: Land On, Fixed Number,
Interval, Strip, Blur, Reel Bounce, the roulette, money wheel (Clapper, Segments, Wheel
File, Lines, Short File), craps, lottery and shower controls, the felt colour, Display,
Light Angle, Tilt, and the effect's Clip symbols. Lines needs a file with more lines than
segments (`eighty.txt` on 12) and Short File one with fewer (`three.txt` on 12). Every one is a control that belongs to another game or acts at another
moment; none is live only by a side effect.

## In Arena (2026-10-09)

Loaded into Resolume Arena 7.27.1 on macOS and driven by hand: both plugins register
(JP01 category 3, JP02 category 1), SW Jackpot renders in a clip, a slots Jackpot and a
roulette spin land as asked, the shower falls, and the diag log is clean. One thing to
know when driving Arena from a session: **an option row's button can light without the
value reaching the plugin** (fleet memory: an accessibility press does that). The first
play here came up Random under a lit "Jackpot"; Arena then redrew the row as Random,
the value the plugin held. A real click sent it. Read the diag log's play line, not the
panel, for what was played.

On Windows (win-lab, llvmpipe, 2026-10-09) plugin-bench's gate passed 17 of 17 on a
CI build: registration, all 50/52 controls as declared, render, a clean log. Its
expectation (`plugin-bench/arena/expect/jackpot.json`) marks 16 controls inert because
they act only on a play and the gate never presses Play; which ones was measured with
jptest first (`--play 100000 --frames 60`: Play never pressed, the same frame both
sides). An idle slot machine is not still: the lamps chase at 3 Hz, so the faint
controls hold Lights 0 as a precondition. A change to what an idle game draws, or a
new control, must change that file in the same commit. **jackpot#1 did not** (the gate
is plugin-bench's, and its file is re-seeded from a running Arena, never by hand): the
expectation still describes v0.1.1's 50/52 controls in v0.1.1's order. The four new ones
change an idle picture only with Game on Money Wheel (and Lines/Short File only with a
file of more or fewer lines than Segments), so they want those as preconditions.

## What is verified, and what is assumed

Verified (numbers in README "Status"): every game shows the result asked for, read
from the plan as drawn and, for slots, roulette and the money wheel, out of the
pixels; the C37 and C54 identities to the bit; the ball's departure law; the
clapper's energy; fairness by chi-square; the virtual reel's near-miss rate; the
shutter's integral; the pass line; the lottery's deal; free fall under drag, the
tumble, energy at contact and Euler's disk; the pile clearing, staying on the table,
never stalling the pool and leaving the table flat; Spin Time and Land On; the clip
under the effect; fonts; the state between plays; Game and Play outside every group;
wheel files (above), harness only.

Assumed or chosen, not measured:

- Every physical constant: the roulette profile (track 14°, stator 20°), its
  frictions and bounces, the rotor's 40 s spin-down; the wheel's inertia, bearing,
  clapper stiffness range and its 0.35 unloading; the lottery's drag, bounces, jet
  and suction; the chips' and coins' drag, friction and bounce. Chosen to look
  right, checked against laws, never against filmed equipment.
- The look of everything, and that a 15° view suits the shower.
- That Resolume's `SetBeatInfo` bar phase is a usable downbeat (fleet-wide unknown).
- That Arena's FFGL clock behaves as boreal measured it.

## Not done

In Resolume only on this Mac, two plays (README "Status"); no OpenFX port, browser
demo or presets. The 0.1.1 fixes (the pile, jackpot#3; Play at the top, jackpot#2)
are measured by the harness. The pile was also run in Resolume Arena 7.27.1 on Windows (software rendering, a build of the 0.1.1 code made with MSVC on the lab machine, 2026-10-12):
Shower Only on Always, chips then coins, the chips gone about 14 s after the switch, the floor
clear 15 s after Shower Off, the shower still falling. Play outside the closed Game group has not
been looked at (a firewall prompt sat over the panel). The workflows exist and have never run (there is no GitHub repo); turn
them on only if the repo is public.
