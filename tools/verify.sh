#!/usr/bin/env bash
#
# Everything, in the order that fails fastest.
#
# The build is universal on purpose. An arm64-only bundle builds and tests
# perfectly well here and then fails to load in an Intel Resolume, and the
# build log calls it a success either way -- so the architecture is checked
# with lipo, never with the log.
#
#     tools/verify.sh [BUILD_DIR]
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-$REPO/build-verify}"

cd "$REPO"

step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
fail() { printf '\033[31mFAIL\033[0m %s\n' "$1"; exit 1; }

#---------------------------------------------------------------------------
# The GLSL reserved words, as identifiers -- and the built-ins a user function
# can be shadowed by (noise1..4: Apple's compiler says only "Incompatible types"
# at the line that uses one, and glslc accepts it). A declaration, not a
# mention, so the words may still appear in prose.
#---------------------------------------------------------------------------
reserved_words() {
	local words="patch sample input output filter common active half layout flat smooth noperspective packed near far noise1 noise2 noise3 noise4"
	local bad=0 word

	for word in $words; do
		if grep -nE "(float|int|uint|bool|vec[234]|ivec[234]|uvec[234]|mat[234])[[:space:]]+$word[[:space:]]*[;=,)(]" \
		            source/Shaders.cpp >/dev/null 2>&1; then
			printf '   "%s" is declared as an identifier and is a GLSL reserved word or built-in\n' "$word"
			grep -nE "(float|int|uint|bool|vec[234]|ivec[234]|uvec[234]|mat[234])[[:space:]]+$word[[:space:]]*[;=,)(]" source/Shaders.cpp \
				| sed 's/^/      /'
			bad=$(( bad + 1 ))
		fi
	done

	[ "$bad" -eq 0 ] && printf '   none of the reserved words or noise built-ins is declared\n'
	return "$bad"
}

#---------------------------------------------------------------------------
step "GLSL reserved words"
#---------------------------------------------------------------------------
reserved_words || fail "a GLSL reserved word is used as an identifier"

#---------------------------------------------------------------------------
step "GLSL smoothstep with its edges reversed"
#---------------------------------------------------------------------------
# Undefined in GLSL 4.10 (8.3) when edge0 >= edge1. Apple's driver does the
# obvious thing; another need not. Reversed ramps go through sstep(). A literal
# pair is all a grep can see, and it is the form that was written.
if grep -nE 'smoothstep\( *[0-9.]+, *-[0-9.]+' source/Shaders.cpp \
   || python3 - <<'PY'
import re, sys
text = open( "source/Shaders.cpp" ).read()
bad = [ m.group( 0 ) for m in re.finditer( r'smoothstep\(\s*([0-9.]+)\s*,\s*([0-9.]+)\s*,', text ) if float( m.group( 1 ) ) >= float( m.group( 2 ) ) ]
for b in bad:
	print( "   ", b )
sys.exit( 0 if bad else 1 )
PY
then
	fail "a smoothstep with edge0 >= edge1 (use sstep)"
fi
echo "   none"

#---------------------------------------------------------------------------
step "Shaders"
#---------------------------------------------------------------------------
tools/glslc.sh || fail "a shader does not compile"

#---------------------------------------------------------------------------
step "Submodule"
#---------------------------------------------------------------------------
if [[ ! -f external/ffgl/CMakeLists.txt ]]; then
	fail "FFGL SDK missing -- run: git submodule update --init --recursive"
fi
pin="$(git -C external/ffgl rev-parse --short=7 HEAD)"
[[ "$pin" == "b1afaf9" ]] || fail "FFGL SDK at $pin, not the fleet's b1afaf9"
echo "ok   FFGL SDK pinned at $pin"

#---------------------------------------------------------------------------
step "Build (universal)"
#---------------------------------------------------------------------------
cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD" -j"$(sysctl -n hw.ncpu)" >/dev/null
echo "ok   built"

#---------------------------------------------------------------------------
step "Bundles"
#---------------------------------------------------------------------------
# Two bundles, one per registration: FFGL resolves one plugMain per binary.
check_bundle() {
	local bundle="$1" executable="$2" identifier="$3" id="$4" name="$5" kind="$6"
	local binary="$bundle/Contents/MacOS/$executable"
	[[ -f "$binary" ]] || fail "no binary at $binary"

	local arches
	arches="$(lipo -archs "$binary")"
	[[ "$arches" == *arm64* ]]  || fail "$executable: no arm64 slice (got: $arches)"
	[[ "$arches" == *x86_64* ]] || fail "$executable: no x86_64 slice (got: $arches)"

	# Captured, then matched from a herestring -- never `nm ... | grep -q`:
	# under pipefail the grep's early exit SIGPIPEs nm and fails the pipeline.
	local symbols
	symbols=$( nm -gU "$binary" 2>/dev/null || true )
	grep -q '_plugMain' <<<"$symbols" || fail "$executable: plugMain not exported"
	echo "ok   $executable: $arches, plugMain exported"

	local plist="$bundle/Contents/Info.plist"
	[[ -f "$plist" ]] || fail "no Info.plist in $bundle"
	read_plist() { /usr/libexec/PlistBuddy -c "Print :$1" "$plist" 2>/dev/null || true; }
	local declared
	declared="$( sed -n 's/^[[:space:]]*VERSION \([0-9.]*\)$/\1/p' CMakeLists.txt | head -1 )"
	[[ "$( read_plist CFBundleIdentifier )" == "$identifier" ]] || fail "$executable: bundle id is '$( read_plist CFBundleIdentifier )'"
	[[ "$( read_plist CFBundleExecutable )" == "$executable" ]] || fail "$executable: CFBundleExecutable is wrong"
	[[ "$( read_plist CFBundlePackageType )" == "BNDL" ]] || fail "$executable: CFBundlePackageType is not BNDL"
	[[ "$( read_plist CFBundleVersion )" == "$declared" ]] || fail "$executable: plist version != CMakeLists $declared"
	grep -q "versionFallback = \"v$declared\"" source/StoatworksAbout.h \
		|| fail "StoatworksAbout.h's versionFallback is not v$declared"
	echo "ok   $identifier, BNDL, v$declared -- plist, CMakeLists and About agree"

	codesign --force --sign - --timestamp=none "$bundle" >/dev/null 2>&1 || fail "$bundle could not be ad-hoc signed"
	codesign --verify --deep --strict "$bundle" >/dev/null 2>&1 || fail "$bundle's ad-hoc signature does not verify"
	echo "ok   ad-hoc signed and verified"

	# What a host reads: id, name, type. The FFGL name field is char[16] and
	# not null-terminated, so only something reading it back as a host does
	# notices a truncation.
	local OXBOW="${OXBOW:-$HOME/Projects/resolume/oxbow/build/oxbow}"
	if [[ -x "$OXBOW" ]]; then
		local probe
		probe="$( "$OXBOW" probe "$bundle" 2>&1 || true )"
		printf '%s\n' "$probe" | sed -n '1,4p' | sed 's/^/   /'
		grep -q "id:          $id" <<<"$probe" || fail "oxbow did not read the id $id"
		grep -q "name:        $name\$" <<<"$probe" || fail "oxbow did not read the name '$name'"
		grep -q "type:        $kind" <<<"$probe" || fail "oxbow did not read the type as $kind"
		local selftest
		selftest="$( "$OXBOW" selftest "$bundle" 2>&1 || true )"
		grep -q "selftest:    PASS" <<<"$selftest" || { printf '%s\n' "$selftest" | tail -5; fail "oxbow selftest failed on $bundle"; }
		echo "ok   a host reads $name $id $kind, and oxbow selftest renders through it"
	else
		echo "   skipped: no oxbow at $OXBOW (set OXBOW=...)"
	fi
}

check_bundle "$BUILD/Jackpot.bundle" "Jackpot" "com.stoatworks.ffgl.jackpot" JP01 "SW Jackpot" source
check_bundle "$BUILD/Jackpot Over.bundle" "Jackpot Over" "com.stoatworks.ffgl.jackpot.over" JP02 "SW Jackpot Over" effect

#---------------------------------------------------------------------------
step "Shaders through this Mac's driver"
#---------------------------------------------------------------------------
"$BUILD/jptest" --shaders --out "$( mktemp -d )" | tail -n +2 || fail "a program does not compile or link here"

#---------------------------------------------------------------------------
step "Checks"
#---------------------------------------------------------------------------
# Every claim the README makes, in the order the README makes them.
for check in slots nearmiss blur slots-readback roulette roulette-readback wheel wheel-readback craps lottery \
             shower duration defaults names determinism over-check resize fonts state; do
	"$BUILD/jptest" --$check || fail "jptest --$check"
done

#---------------------------------------------------------------------------
step "Offline (what CI runs)"
#---------------------------------------------------------------------------
# The no-GL subset, exactly as CI runs it, so the selector cannot rot here
# while CI goes on passing.
"$BUILD/jptest" --offline >/dev/null || fail "jptest --offline"
echo "ok   jptest --offline"

#---------------------------------------------------------------------------
step "Tables across builds"
#---------------------------------------------------------------------------
# The dice's tables (polyhedral's Geometry.cpp, for craps) must not depend on
# how the compiler rounds: arm64 clang fuses multiply-adds, x86_64 has another
# libm, and sorting by atan2 once put a square's far corner at +pi in one and
# -pi in the other. The negative control puts that cut back.
geodump() { # geodump <name> <flags...>
	local name=$1; shift
	/usr/bin/clang++ -std=c++17 -O2 "$@" -Isource tools/geodump/main.cpp source/Geometry.cpp \
		-o "$BUILD/geodump-$name" || fail "geodump $name did not build"
	"$BUILD/geodump-$name" > "$BUILD/geodump-$name.txt" || fail "geodump $name did not run"
}
geodump fused -arch arm64
geodump nofma -arch arm64 -ffp-contract=off
geodump x86 -arch x86_64
cmp -s "$BUILD/geodump-fused.txt" "$BUILD/geodump-nofma.txt" || fail "tables differ with and without fused multiply-adds"
cmp -s "$BUILD/geodump-fused.txt" "$BUILD/geodump-x86.txt" || fail "tables differ between arm64 and x86_64"
echo "ok   the same tables from arm64 fused, arm64 unfused and x86_64 ($(wc -l < "$BUILD/geodump-fused.txt" | tr -d ' ') lines)"
geodump plain-fused -arch arm64 -DPOLYHEDRAL_PLAIN_ATAN2
geodump plain-nofma -arch arm64 -ffp-contract=off -DPOLYHEDRAL_PLAIN_ATAN2
if cmp -s "$BUILD/geodump-plain-fused.txt" "$BUILD/geodump-plain-nofma.txt"; then
	fail "negative control: with the cut at -pi the builds should disagree, and the check cannot see it"
fi
echo "ok   negative control: with the cut back at -pi, the builds disagree"

#---------------------------------------------------------------------------
step "Pipe"
#---------------------------------------------------------------------------
# The fleet's --pipe frame format, which the video renders through. Two and a
# half frames in must be exactly two out and a clean exit, a cue naming no
# parameter must be refused, and a reader that hangs up must end the run with
# exit 1, not SIGPIPE's silent 141.
frame=$(( 64 * 36 * 4 ))
raw=$( mktemp ); cues=$( mktemp )
head -c $(( frame * 5 / 2 )) /dev/zero > "$raw"
out=$( mktemp ); status=0
"$BUILD/jptest" --over --pipe --size 64x36 < "$raw" > "$out" 2>/dev/null || status=$?
got=$( wc -c < "$out" | tr -d ' ' ); rm -f "$out"
[[ "$status" -eq 0 && "$got" == "$(( frame * 2 ))" ]] \
	|| fail "2.5 frames in gave $got bytes out (want $(( frame * 2 ))), exit $status"
echo "ok   2.5 frames in, exactly 2 frames out, clean exit"
printf '0 No Such Control 0.5\n' > "$cues"
status=0
"$BUILD/jptest" --over --pipe --size 64x36 --script "$cues" < "$raw" >/dev/null 2>&1 || status=$?
[[ "$status" -eq 2 ]] || fail "a cue naming no parameter gave exit $status, not 2"
echo "ok   a cue naming no parameter is refused (exit 2)"
status=0
"$BUILD/jptest" --size 64x36 --set "Result=Sometimes" --out /dev/null >/dev/null 2>&1 || status=$?
[[ "$status" -eq 2 ]] || fail "--set with an option that does not exist gave exit $status, not 2"
echo "ok   --set refuses an option it does not know (exit 2)"
head -c $(( frame * 20 )) /dev/zero > "$raw"
set +e
"$BUILD/jptest" --over --pipe --size 64x36 < "$raw" 2>/dev/null | head -c 1 >/dev/null
status=${PIPESTATUS[0]}
set -e
[[ "$status" -eq 1 ]] || fail "a closed stdout gave exit $status, not 1"
set +e
"$BUILD/jptest" --film 20 --size 64x36 2>/dev/null | head -c 1 >/dev/null
status=${PIPESTATUS[0]}
set -e
[[ "$status" -eq 1 ]] || fail "--film into a closed stdout gave exit $status, not 1"
echo "ok   a closed stdout ends --pipe and --film with exit 1, not SIGPIPE"
rm -f "$raw" "$cues"

#---------------------------------------------------------------------------
step "Negative controls"
#---------------------------------------------------------------------------
# Every check above, against a model that is deliberately wrong, required to
# FAIL. A check that cannot fail is not a check.
"$BUILD/jptest" --negative > "$BUILD/negative.log" 2>&1 || { grep -E 'PASSED against|undetected' "$BUILD/negative.log"; fail "a negative control went undetected"; }
grep -E 'negative controls:' "$BUILD/negative.log" | sed 's/^/   /'

#---------------------------------------------------------------------------
step "Mutants"
#---------------------------------------------------------------------------
# One character of the shipped GLSL and of the engine, changed: a check must fail.
tools/mutate.sh > "$BUILD/mutants.log" 2>&1 || { grep -E 'PASSES|mutation target' "$BUILD/mutants.log"; fail "a mutant survived"; }
tail -1 "$BUILD/mutants.log" | sed 's/^/   /'

#---------------------------------------------------------------------------
step "Dead controls"
#---------------------------------------------------------------------------
# The only thing that catches a uniform whose name does not match the C++.
python3 tools/sweep.py --build "$(basename "$BUILD")" || fail "a dead control"

#---------------------------------------------------------------------------
step "Cost"
#---------------------------------------------------------------------------
"$BUILD/jptest" --bench

printf '\n\033[32mall green\033[0m\n'
