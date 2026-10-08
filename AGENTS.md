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
    Controls.*       parameter ids (append only) and every 0..1 -> unit mapping
    Transport.h      honeydew's beat and bar clock
    Slots.*          strips, the virtual reel, the pay table, the reel schedule
    SymbolArt.*      the reel symbols, painted on the CPU into an atlas
    Roulette.*       the ball on a profile of cones, frets, diamonds; the plan
    MoneyWheel.*     the wheel against its clapper; the plan
    Craps.*          the throw on polyhedral's engine; the pass line
    Lottery.*        spheres in a sphere, the air, the tube; the deal
    Shower.*         live rigid discs, the pile as a height field
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
- **The About block and ATTRIBUTIONS.md are provisional hand copies** (adapted from
  polyhedral's), because Jackpot is not in stoatworks-backend's projects.json. With
  no user guide, `guide` is empty and the block has three buttons, so the effect's
  Mix sits one id earlier than polyhedral's (nothing has shipped). Registering it
  replaces both files and adds FUNDING.yml and the issue forms.
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
  `--shower`, `--duration`, `--defaults`, `--names`, `--determinism`: CPU, double,
  no rasteriser. **Trajectories are not promised identical across architectures**
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

`tools/mutate.sh`, ten one-character mutants, each caught by the named check:

| mutant | caught by |
| --- | --- |
| GLSL roulette pocket at a point: `phi - RingAngle` → `+` | `--roulette-readback` (40 of 40 wrong) |
| GLSL the stop at a reel's row: `floor( u + 0.5 )` → `- 0.5` | `--slots-readback` |
| GLSL the money wheel's segment at a point: `- WheelAngle` → `+` | `--wheel-readback` (6 of 7 wrong) |
| C++ the ring's shift: `best.pocket - plan.wanted` → `+` | `--roulette` (36 of 37 wrong) |
| C++ the segment under the clapper: `- whole` → `+` | `--wheel` |
| C++ a drawn ball marked dealt: `= 1` → `= 0` | `--lottery` |
| C++ the virtual stops beside the seven: 6 → 1 | `--nearmiss` (12.3 per jackpot falls to 2.1) |
| C++ the eased playback's start rate: `0.5 * k` → `0.6` | `--duration` (craps stops early) |
| C++ a disc's axial moment: `0.5` → `0.6` | `--shower` (the tumble, and Euler's disk) |
| C++ polyhedral's symmetry search: `a - b` → `+` | `--craps` (40 of 44 wrong) |

## The sweep's context table

Emptied (`tools/sweep.py --no-context`), 27 controls go dead: Land On, Fixed Number,
Interval, Strip, Blur, Reel Bounce, the roulette, money wheel, craps, lottery and
shower controls, the felt colour, Display, Light Angle, Tilt, and the effect's Clip
symbols. Every one is a control that belongs to another game or acts at another
moment; none is live only by a side effect.

## What is verified, and what is assumed

Verified (numbers in README "Status"): every game shows the result asked for, read
from the plan as drawn and, for slots, roulette and the money wheel, out of the
pixels; the C37 and C54 identities to the bit; the ball's departure law; the
clapper's energy; fairness by chi-square; the virtual reel's near-miss rate; the
shutter's integral; the pass line; the lottery's deal; free fall under drag, the
tumble, energy at contact and Euler's disk; Spin Time and Land On; the clip under
the effect; fonts; the state between plays.

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

Never loaded into Resolume; no Arena gate, OpenFX port, browser demo, presets or
user guide. The workflows exist and have never run (there is no GitHub repo); turn
them on only if the repo is public.
