#!/usr/bin/env bash
#
# Mutation testing: change ONE character of the shipped code and require that a
# check fails. A check that still passes against a mutant was not looking at
# the code it claims to cover.
#
# Each mutant is a copy of the tree in a temporary directory (the FFGL SDK is
# symlinked, not copied), built arm64-only, with only the named check run.
#
#     tools/mutate.sh
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# file | the exact text | its one-character mutant | the check that must fail | what it is
MUTANTS=(
	"source/Shaders.cpp|		if( den < 0.0 )|		if( den > 0.0 )|--readback|GLSL: the slab test's entering plane, < -> >"
	"source/Shaders.cpp|	vec2 p  = q + r3.zw;|	vec2 p  = q - r3.zw;|--readback|GLSL: the number's block offset, + -> -"
	"source/Geometry.cpp|Length( solid.group[ g ] * a - b ) < 1e-9|Length( solid.group[ g ] * a + b ) < 1e-9|--outcome|C++: the symmetry that takes the wanted face to the top, - -> +"
	"source/Physics.cpp|? -c.restitution * vn0|? +c.restitution * vn0|--physics|C++: the restitution target's sign, - -> +"
	"source/Roll.cpp|results.push_back( 1 + static_cast< int >( Below(|results.push_back( 0 + static_cast< int >( Below(|--outcome|C++: a random draw's offset, 1 -> 0"
	"source/Roll.cpp|return t * ( rateStart + 0.5 * ( rateEnd - rateStart ) * t / duration );|return t * ( rateStart + 0.6 * ( rateEnd - rateStart ) * t / duration );|--duration|C++: the eased playback's integral, 0.5 -> 0.6"
)

caught=0
for entry in "${MUTANTS[@]}"; do
	IFS='|' read -r file original mutant check what <<<"$entry"
	tree="$WORK/tree"
	rm -rf "$tree"
	mkdir -p "$tree/external"
	cp -R "$REPO/source" "$REPO/tools" "$REPO/cmake" "$REPO/CMakeLists.txt" "$tree/"
	cp -R "$REPO/external/stb" "$tree/external/stb"
	ln -s "$REPO/external/ffgl" "$tree/external/ffgl"

	python3 - "$tree/$file" "$original" "$mutant" <<'PY'
import sys, pathlib
path, original, mutant = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
text = path.read_text()
if text.count(original) != 1:
    sys.exit(f"mutation target found {text.count(original)} times in {path}: '{original}'")
if sum(a != b for a, b in zip(original, mutant)) != 1 or len(original) != len(mutant):
    sys.exit("a mutant must differ by exactly one character")
path.write_text(text.replace(original, mutant))
PY

	printf '\n== mutant: %s\n' "$what"
	cmake -S "$tree" -B "$tree/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 >/dev/null
	cmake --build "$tree/build" --target polytest -j"$(sysctl -n hw.ncpu)" >/dev/null 2>&1
	if "$tree/build/polytest" "$check" >"$WORK/log" 2>&1; then
		printf '   FAIL  %s still PASSES -- the check does not cover this code\n' "$check"
	else
		printf '   ok    %s fails against the mutant:\n' "$check"
		grep -E '^  FAIL' "$WORK/log" | head -3 | cut -c1-150 | sed 's/^/        /'
		caught=$(( caught + 1 ))
	fi
done

printf '\nmutants: %d, caught: %d\n' "${#MUTANTS[@]}" "$caught"
[[ "$caught" -eq "${#MUTANTS[@]}" ]]
