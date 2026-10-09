# Attributions

Jackpot is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### The dice engine, the font atlas and the harness — Stoatworks polyhedral

<https://github.com/stoatworks-labs/polyhedral>  
Licence: MIT  
Copyright: Stoatworks Labs

The craps dice are polyhedral's, engine and all: source/Geometry.cpp, source/Physics.cpp (with a pyramid-studded wall plane added), Maths.h, and the one idea that a die is drawn as R(t)·S with S the symmetry that puts the wanted face up — here applied to every game, as the pocket shift, the segment shift and the ball permutation. Typeface.cpp's signed-distance atlas and built-in stroked face are polyhedral's, widened from ten digits to the capitals. The source-plus-Over shape, GLState.h, Diag, the host-clock unit vote, the playback time map, the harness's rig, --pipe, --film and cue sheets, tools/verify.sh, tools/sweep.py, tools/mutate.sh, tools/glslc.sh and tools/geodump are polyhedral's (which are boreal's, downpour's and flyback's before it).

### Landing on the beat — Stoatworks honeydew

<https://github.com/stoatworks-labs/honeydew>  
Licence: MIT  
Copyright: Stoatworks Labs

source/Transport.h — the bar phase kept from Resolume's BPM and re-aligned to the host's bar phase, and the next beat or bar at least a given time away — is honeydew's.

### Ray–primitive intersections — Inigo Quilez

<https://iquilezles.org/articles/intersectors/>  
Licence: MIT  
Copyright: Inigo Quilez

The capsule, box and capped-cylinder intersections, the five-pointed star's distance, and the sphere's soft shadow and ambient occlusion in source/Shaders.cpp follow Quilez's published functions, rewritten into the house style.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

### stb_truetype

<https://github.com/nothings/stb>  
Licence: MIT or Public Domain (Unlicense), at your choice  
Copyright: Sean Barrett

Single header vendored at external/stb/stb_truetype.h.

Rasterises glyphs into the atlas the plugin samples. A whole font stack would be a large dependency for one job this header already does.

## Work we checked ourselves against

No code was taken from these — but they were how we knew we had it right, and that is worth saying out loud.

### The virtual reel — Inge Telnaes, US patent 4,448,419 (1984)

The slot machine draws its stops on a 64-stop virtual reel mapped unevenly onto 22 physical stops, the blanks beside the seven over-weighted, as Telnaes's patent made every machine since. jptest --nearmiss measures the near-miss rate against the table.

### The roulette ball leaving the track — M. Small and C. K. Tse, Chaos 22 (2012)

On a track banked at α the ball can only stay up while v² ≥ g r tan α: the banked curve of any mechanics text, and the moment Small and Tse model in "Predicting the outcome of roulette". jptest --roulette measures it.

### Euler's disk — H. K. Moffatt, Nature 404 (2000)

A disc rolling on its edge turns its contact round at Ω² = 4 g / (r sin α), faster as it flattens. Nothing in the shower is written to do that; jptest --shower measures that the contact model does it.

### Sequential impulses — Erin Catto (Box2D)

<https://box2d.org/publications/>

The dice's contact solver (polyhedral's) and the shower's are the sequential-impulse method: accumulated normal impulses clamped at zero, Coulomb friction clamped to the normal impulse, restitution from the approach speed. Implemented from the method; no Box2D code is used.

### Uniform random rotations — Ken Shoemake, Graphics Gems III (1992)

Each die's and chip's starting orientation is Shoemake's construction from three uniform numbers.

### Fast random integer generation in an interval — Daniel Lemire (2019)

<https://arxiv.org/abs/1805.10941>

Every Random result is drawn by Lemire's multiply-shift with rejection, so no pocket, segment, stop or ball is favoured by modulo bias; the chi-squares in jptest measure it.

### Signed distance fields for glyphs — Chris Green, Valve, SIGGRAPH 2007

<https://steamcdn-a.akamaihd.net/apps/valve/2007/SIGGRAPH2007_AlphaTestedMagnification.pdf>

The text — the pockets' numbers, the money wheel's values, the craps layout, the marquee — is cut from a signed-distance atlas at the footprint it is drawn at.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### The casino floor

A three- and five-reel fruit machine; the European and American wheels in their real order; the Big Six with its 24 ones, 15 twos, 7 fives, 4 tens, 2 twenties, joker and logo; a craps table's layout, puck and pyramid wall; an air-mix lottery drum in the UK lotto's ball colours. Built from those conventions; nothing is copied from anyone's artwork, model or source.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
