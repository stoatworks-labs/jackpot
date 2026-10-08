#!/usr/bin/env bash
#
# Every shader through glslc, the one shader check that needs no GL driver.
# Called by tools/verify.sh AND by CI, so the two cannot drift: a runner with
# no accelerated GL cannot compile a shader through a driver, and this is how
# CI covers the shaders instead (jptest --offline covers the physics; on a Mac,
# `jptest --shaders` compiles the same programs through the real driver).
#
#     tools/glslc.sh          exit 0 when every shader compiles, or glslc is absent
#     GLSLC_REQUIRED=1 tools/glslc.sh   ...and a missing glslc is a failure (CI)
#
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

#---------------------------------------------------------------------------
# Every program, assembled exactly as shaders::Programs() in Shaders.cpp
# assembles it -- the program list is READ from that function, not copied
# here, so a program the plugin compiles cannot be missed and a piece list
# cannot drift. The one list is the plugin's.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V, which
# demands an explicit layout( location ) on every uniform and varying. Those are
# Vulkan rules and not GLSL ones, and without the flag every shader "fails" for
# reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails.
#---------------------------------------------------------------------------
shaders_compile() {
	local dir bad=0 n=0 shader

	if ! command -v glslc >/dev/null 2>&1; then
		printf '   skipped: glslc not installed (brew install shaderc)\n'
		return 0
	fi

	dir="$( mktemp -d )"

	python3 - "$dir" <<'SHADERS_PY' || { rm -rf "$dir"; return 1; }
import re, sys, pathlib
out  = pathlib.Path( sys.argv[ 1 ] )
text = pathlib.Path( "source/Shaders.cpp" ).read_text()

named = {}
for m in re.finditer( r'const char\* const (\w+)\s*=\s*R"\((.*?)\)"', text, re.S ):
	named[ m.group( 1 ) ] = m.group( 2 )
for m in re.finditer( r'const char\* const (\w+)\s*=\s*((?:"(?:[^"\\\n]|\\.)*"\s*)+);', text ):
	named.setdefault( m.group( 1 ), "".join(
		s.encode().decode( "unicode_escape" ) for s in re.findall( r'"((?:[^"\\\n]|\\.)*)"', m.group( 2 ) ) ) )

def assemble( pieces ):
	# A name that has moved is a KeyError here, not a silent skip.
	return "".join( named[ p.strip() ] for p in pieces.split( "," ) if p.strip() )

body = text[ text.index( "std::vector< Program > Programs()" ): ]
body = body[ : body.index( "\n}\n" ) ]
quad = re.search( r'const std::string quad = Assemble\(\s*\{([^}]*)\}\s*\);', body )
programs = re.findall( r'\{\s*"(\w+)",\s*(quad|Assemble\(\s*\{[^}]*\}\s*\)),\s*Assemble\(\s*\{([^}]*)\}\s*\)\s*\}', body )
if not quad or not programs:
	sys.exit( "   could not read shaders::Programs() -- the extraction has gone stale" )

used = set()
for name, vertex, fragment in programs:
	vpieces = quad.group( 1 ) if vertex == "quad" else re.search( r'\{([^}]*)\}', vertex ).group( 1 )
	used |= { p.strip() for p in ( vpieces + "," + fragment ).split( "," ) if p.strip() }
	( out / ( name + ".vert" ) ).write_text( assemble( vpieces ) )
	( out / ( name + ".frag" ) ).write_text( assemble( fragment ) )

# A piece that no program uses is GLSL that is not being checked.
for name in named:
	if name not in used:
		sys.exit( f"   {name} is a shader piece no program in shaders::Programs() uses" )
SHADERS_PY

	for shader in "$dir"/*.vert "$dir"/*.frag; do
		[ -e "$shader" ] || continue
		n=$(( n + 1 ))
		if ! glslc --target-env=opengl4.5 -fauto-map-locations \
			   "$shader" -o /dev/null 2>"$dir/err"; then
			printf '   %s does not compile\n' "$( basename "$shader" )"
			sed "s|$dir/||; s|^|      |" "$dir/err"
			bad=$(( bad + 1 ))
		fi
	done

	if [ "$n" -eq 0 ]; then
		# No shaders at all is a FAILURE, not a pass: the extraction above has
		# lost track of where this repo keeps its GLSL.
		printf '   no shaders were extracted -- the extraction has gone stale\n'
		rm -rf "$dir"
		return 1
	fi

	if [ "$bad" -eq 0 ]; then
		printf '   %d shaders (%d programs), all compile\n' "$n" "$(( n / 2 ))"
	fi
	rm -rf "$dir"
	return "$bad"
}

if ! command -v glslc >/dev/null 2>&1 && [ "${GLSLC_REQUIRED:-0}" = 1 ]; then
	printf '   glslc is required here and not installed\n'
	exit 1
fi
shaders_compile
