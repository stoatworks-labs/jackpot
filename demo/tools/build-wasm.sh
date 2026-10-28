#!/usr/bin/env bash
#
# Build demo/jackpot-core.{js,wasm}: the plugin's own C++, compiled to
# WebAssembly for the browser demo.
#
#     demo/tools/build-wasm.sh          (needs emscripten: em++ on PATH, or EMXX=...)
#
# The output is COMMITTED, because the Cloudflare deploy has no build step:
# what is in demo/ is what is served. So is demo/wasm/inputs.sha256, the hash
# of every input below and of both outputs; demo/tools/check_shaders.py (run
# by tools/verify.sh) fails when a source file has changed since this last ran,
# which is the only way a stale .wasm can be noticed without emscripten.
#
# What goes in, and none of it is edited for the browser:
#
#   source/   every .cpp of jackpot_core (CMakeLists.txt's OBJECT library):
#             Jackpot.cpp -- the plugin class, with its constructor, clock,
#             Play and Auto Play, the requests it makes, font resolution, the
#             wheel file, the uploads and every uniform -- and everything it
#             calls: the five planners (Slots, Roulette, MoneyWheel, Craps,
#             Lottery), the shower, polyhedral's dice engine (Geometry,
#             Physics), WheelFace, FileBytes, SymbolArt, Typeface, Controls,
#             Common, Shaders and Diag. Everything but the two registration
#             files (SourcePlugin.cpp, EffectPlugin.cpp), whose job the page does.
#   external/ffgl   the FFGL SDK's CFFGLPluginManager / CFFGLPlugin (parameter
#             bookkeeping, read back by the page as a host reads it), FFGLLog,
#             and ffglex's FFGLShader, FFGLScreenQuad and scoped bindings.
#   demo/wasm glue.cpp (the host's calls, as C functions) and gl_shim.cpp (the
#             four GL entry points WebGL2 cannot take as they are).
#
# GL itself is emscripten's WebGL2 library, bound by the page to its own
# context. `-D__linux__` is for the SDK's FFGLPlatform.h, which knows Windows,
# Linux and macOS and stops the build on anything else: told it is Linux, the
# SDK includes <GL/glew.h>, which emscripten provides. It is given only to the
# files that include the SDK.
#
# Flags follow CMakeLists.txt's Release build: -O3 -DNDEBUG, with Physics,
# Craps, Roulette, MoneyWheel, Lottery and Shower at -O2 as the plugin measures
# them. There are no threads: the plugin plans on a worker (std::async), and
# the page switches on its synchronous planning instead (glue.cpp says why).
# WebAssembly has no fused multiply-add, so where clang fuses one on arm64 the
# browser rounds twice: the same Seed draws the same RESULTS everywhere (an
# integer PRNG), but a spin's path can differ from the plugin's on a Mac -- as
# AGENTS.md says it can between any two compilers.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO"

EMXX="${EMXX:-em++}"
command -v "$EMXX" >/dev/null 2>&1 || { echo "em++ not found (install emscripten, or set EMXX=)" >&2; exit 1; }
[[ -f external/ffgl/CMakeLists.txt ]] || { echo "FFGL SDK missing -- git submodule update --init --recursive" >&2; exit 1; }

version="$( sed -n 's/^[[:space:]]*VERSION \([0-9.]*\)$/\1/p' CMakeLists.txt | sed -n 1p )"
[[ -n "$version" ]] || { echo "no VERSION in CMakeLists.txt" >&2; exit 1; }

SDK=external/ffgl/source/lib
PLUGIN_SOURCES="Common Controls Craps Diag FileBytes Geometry Jackpot Lottery MoneyWheel Physics Roulette Shaders Shower Slots SymbolArt Typeface WheelFace"
# CMakeLists.txt's -O2 list: the planners and the shower, as they are measured.
O2_SOURCES=" Physics Craps Roulette MoneyWheel Lottery Shower "
# The files that include the FFGL SDK (through Jackpot.h).
SDK_INCLUDERS=" Jackpot "
SDK_SOURCES="ffgl/FFGLPluginManager ffgl/FFGLPluginSDK ffgl/FFGLLog ffglex/FFGLShader ffglex/FFGLScreenQuad
             ffglex/FFGLScopedShaderBinding ffglex/FFGLScopedVAOBinding ffglex/FFGLScopedBufferBinding ffglex/FFGLUtilities"

OBJ="$( mktemp -d )"
trap 'rm -rf "$OBJ"' EXIT

INCLUDES=( -Isource -Iexternal/stb -I"$SDK" )
BASE=( -std=c++17 -DNDEBUG -DJACKPOT_VERSION="\"$version\"" "${INCLUDES[@]}" )
SDK_PLATFORM=( -D__linux__=1 )

objects=()
compile() {
	local src="$1" opt="$2"; shift 2
	local obj="$OBJ/$( echo "$src" | tr '/' '_' ).o"
	"$EMXX" "${BASE[@]}" "$opt" "$@" -c "$src" -o "$obj"
	objects+=( "$obj" )
}

for name in $PLUGIN_SOURCES; do
	opt=-O3
	[[ "$O2_SOURCES" == *" $name "* ]] && opt=-O2
	if [[ "$SDK_INCLUDERS" == *" $name "* ]]; then
		compile "source/$name.cpp" "$opt" "${SDK_PLATFORM[@]}"
	else
		compile "source/$name.cpp" "$opt"
	fi
done
for name in $SDK_SOURCES; do
	compile "$SDK/$name.cpp" -O3 "${SDK_PLATFORM[@]}"
done
compile demo/wasm/glue.cpp -O3 "${SDK_PLATFORM[@]}"
compile demo/wasm/gl_shim.cpp -O3

LINK=(
	-O3
	-sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createJackpot
	-sENVIRONMENT=web
	-sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=1048576
	-sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2
	-sDYNAMIC_EXECUTION=0
	-sFILESYSTEM=1
	-sEXPORTED_RUNTIME_METHODS=GL,FS,UTF8ToString,stringToNewUTF8
	-sEXPORTED_FUNCTIONS=_malloc,_free
)
"$EMXX" "${objects[@]}" "${LINK[@]}" -o demo/jackpot-core.js

#---------------------------------------------------------------------------
# The manifest check_shaders.py reads. Inputs first, then outputs.
#---------------------------------------------------------------------------
{
	echo "# demo/tools/build-wasm.sh, $( "$EMXX" --version | sed -n 1p )"
	echo "# FFGL SDK $( git -C external/ffgl rev-parse HEAD )"
	for name in $PLUGIN_SOURCES; do shasum -a 256 "source/$name.cpp"; done
	for header in source/*.h external/stb/*.h; do shasum -a 256 "$header"; done
	for name in $SDK_SOURCES; do shasum -a 256 "$SDK/$name.cpp"; done
	shasum -a 256 demo/wasm/glue.cpp demo/wasm/gl_shim.cpp demo/tools/build-wasm.sh
	shasum -a 256 demo/jackpot-core.js demo/jackpot-core.wasm
} > demo/wasm/inputs.sha256

ls -l demo/jackpot-core.js demo/jackpot-core.wasm | awk '{ print "wrote " $NF ", " $5 " bytes" }'
