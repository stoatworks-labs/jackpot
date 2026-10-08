#!/usr/bin/env bash
#
# Mutation testing: change ONE character of the shipped code and require that a
# check fails. A check that still passes against a mutant was not looking at
# the code it claims to cover.
#
# One copy of the tree in a temporary directory (the FFGL SDK is symlinked, not
# copied), built arm64-only once; each mutant is applied in place, the harness
# rebuilt (only the changed file recompiles), the named check run under a time
# limit -- a mutant can hang (an inverted loop test), and a hang counts as
# caught -- and the file put back.
#
#     tools/mutate.sh
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
LIMIT=600

# file | the exact text | its one-character mutant | the check that must fail | what it is
# (\n and \t in the texts are a newline and a tab, for a target that needs its
# next line to be unique.)
MUTANTS=(
	"source/Shaders.cpp|float u = ( phi - RingAngle ) / ( 2.0 * PI / float( Pockets ) );|float u = ( phi + RingAngle ) / ( 2.0 * PI / float( Pockets ) );|--roulette-readback|GLSL: the pocket under a point, the ring's angle - -> +"
	"source/Shaders.cpp|\t\tfloat k  = floor( u + 0.5 );|\t\tfloat k  = floor( u - 0.5 );|--slots-readback|GLSL: the stop at a reel's row, + 0.5 -> - 0.5"
	"source/Shaders.cpp|( atan( m.y, m.x ) - WheelAngle ) / seg;\n\t\tfloat k   = floor( u );|( atan( m.y, m.x ) + WheelAngle ) / seg;\n\t\tfloat k   = floor( u );|--wheel-readback|GLSL: the money wheel's segment at a point, - -> +"
	"source/Roulette.cpp|( ( best.pocket - plan.wanted ) % N + N ) % N;|( ( best.pocket + plan.wanted ) % N + N ) % N;|--roulette|C++: the ring's shift in pockets, - -> +"
	"source/MoneyWheel.cpp|std::floor( top - phase + r.pegTurn ) ) - whole;|std::floor( top - phase + r.pegTurn ) ) + whole;|--wheel|C++: the segment under the clapper, whole segments - -> +"
	"source/Lottery.cpp|plan.sim.captured[ k ] ) ]       = 1;|plan.sim.captured[ k ] ) ]       = 0;|--lottery|C++: a drawn ball marked dealt, 1 -> 0"
	"source/Slots.cpp|constexpr int kNextToSeven = 6;|constexpr int kNextToSeven = 1;|--nearmiss|C++: the virtual stops beside the seven, 6 -> 1"
	"source/Common.cpp|p.rateStart    = p.warp / ( 1.0 - 0.5 * k );|p.rateStart    = p.warp / ( 1.0 - 0.6 * k );|--duration|C++: the eased playback's start rate, 0.5 -> 0.6"
	"source/Shower.cpp|p.radius * p.radius : 0.5 * p.radius * p.radius;|p.radius * p.radius : 0.6 * p.radius * p.radius;|--shower|C++: a disc's axial moment of inertia, 0.5 -> 0.6"
	"source/Geometry.cpp|Length( solid.group[ g ] * a - b ) < 1e-9|Length( solid.group[ g ] * a + b ) < 1e-9|--craps|C++: the symmetry that takes the wanted face to the top, - -> +"
)

tree="$WORK/tree"
mkdir -p "$tree/external"
cp -R "$REPO/source" "$REPO/tools" "$REPO/cmake" "$REPO/CMakeLists.txt" "$tree/"
cp -R "$REPO/external/stb" "$tree/external/stb"
ln -s "$REPO/external/ffgl" "$tree/external/ffgl"
cmake -S "$tree" -B "$tree/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 >/dev/null
cmake --build "$tree/build" --target jptest -j"$(sysctl -n hw.ncpu)" >/dev/null 2>&1

caught=0
for entry in "${MUTANTS[@]}"; do
	IFS='|' read -r file original mutant check what <<<"$entry"
	cp "$tree/$file" "$WORK/original"
	python3 - "$tree/$file" "$original" "$mutant" <<'PY'
import sys, pathlib
path = pathlib.Path(sys.argv[1])
unescape = lambda s: s.replace("\\n", "\n").replace("\\t", "\t")
original, mutant = unescape(sys.argv[2]), unescape(sys.argv[3])
text = path.read_text()
if text.count(original) != 1:
    sys.exit(f"mutation target found {text.count(original)} times in {path}: '{original}'")
if len(original) != len(mutant) or sum(a != b for a, b in zip(original, mutant)) != 1:
    sys.exit("a mutant must differ by exactly one character")
path.write_text(text.replace(original, mutant))
PY

	printf '\n== mutant: %s\n' "$what"
	# The file's object and the harness go, so both ARE rebuilt: make compares
	# times to the second, and Shaders.cpp compiles in under one -- its new
	# object could share the old harness's second, the link was skipped, the
	# old binary ran and the mutant "survived" (twice, before this).
	rm -f "$tree/build/CMakeFiles/jackpot_core.dir/$file.o" "$tree/build/jptest"
	cmake --build "$tree/build" --target jptest -j"$(sysctl -n hw.ncpu)" >/dev/null 2>&1
	status=0
	perl -e 'alarm shift; exec @ARGV' "$LIMIT" "$tree/build/jptest" "$check" >"$WORK/log" 2>&1 || status=$?
	if [[ "$status" -eq 0 ]]; then
		printf '   FAIL  %s still PASSES -- the check does not cover this code\n' "$check"
	elif [[ "$status" -eq 142 ]]; then
		printf '   ok    %s hung past %d s against the mutant (counted as caught)\n' "$check" "$LIMIT"
		caught=$(( caught + 1 ))
	else
		printf '   ok    %s fails against the mutant:\n' "$check"
		grep -E '^  FAIL' "$WORK/log" | head -3 | cut -c1-150 | sed 's/^/        /'
		caught=$(( caught + 1 ))
	fi
	cp "$WORK/original" "$tree/$file"
	rm -f "$tree/build/CMakeFiles/jackpot_core.dir/$file.o"
done

printf '\nmutants: %d, caught: %d\n' "${#MUTANTS[@]}" "$caught"
[[ "$caught" -eq "${#MUTANTS[@]}" ]]
