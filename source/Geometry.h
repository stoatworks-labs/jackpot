#pragma once

#include "Maths.h"

#include <array>
#include <string>
#include <vector>

/**
    The solids, their rotation groups, and how a real set numbers them.

    ## Built, not typed

    Each solid starts as a list of vertices (the cube's corners, the
    icosahedron's golden rectangles, the trapezohedron's two zigzag rings) and
    everything else is DERIVED: the faces are the convex hull's facets, found by
    brute force over every vertex triple (twenty vertices is 1140 triples); the
    edges are the facets' sides; the inertia tensor is summed over a fan of
    tetrahedra; and the rotation group is every frame-to-frame rotation that
    carries the vertex set onto itself. Nothing about a d12 is written down twice,
    so nothing can disagree.

    ## The rotation group is the plugin's one idea

    For any two faces of a die there is a rotation of the solid onto itself that
    carries one onto the other. The physics decides which face ends up on top;
    drawing the die as `R(t) * S`, with `S` the symmetry that carries the WANTED
    face onto that one, puts the wanted number on top without changing a single
    pixel of the motion. `ditest --symmetry` checks the group's order, that every
    element maps vertices onto vertices, and that the picture really is the same.

    ## Units

    Metres, kilograms, seconds. The sizes are a real set's: a 16 mm d6, a d20 20 mm
    across its faces, a d4 with 20 mm edges.
*/
namespace dice::geo
{
enum class Shape
{
	Tetrahedron = 0,
	Cube,
	Octahedron,
	Trapezohedron,///< pentagonal: the d10 and both halves of the d100
	Dodecahedron,
	Icosahedron,
	Count
};

struct Face
{
	V3 normal;              ///< outward, unit
	double offset = 0.0;    ///< the plane is dot( normal, p ) = offset; inside is below it
	std::vector< int > loop;///< vertex indices, counter-clockwise seen from outside
	V3 centroid;            ///< of the polygon's AREA
	V3 incentre;            ///< the centre of the largest circle that fits in the face
	double inradius = 0.0;  ///< that circle's radius
};

struct Solid
{
	Shape shape = Shape::Cube;
	std::vector< V3 > vertices;
	std::vector< Face > faces;
	std::vector< std::array< int, 2 > > edges;
	/// Vertices then edge midpoints: what is tested against another die's
	/// planes. Vertices alone miss two dice meeting edge to edge.
	std::vector< V3 > probes;
	std::vector< int > opposite;///< per face, the parallel face opposite (-1 on the tetrahedron)
	std::vector< M3 > group;    ///< the rotation group, identity first

	double inradius     = 0.0;
	double midradius    = 0.0;///< centre to edge midpoint
	double circumradius = 0.0;
	double volume       = 0.0;
	double mass         = 0.0;
	M3 inertia;               ///< body frame, kg m^2
	M3 inverseInertia;
};

/// The solid, built once on first use and immutable after.
const Solid& GetSolid( Shape shape );

//---------------------------------------------------------------------------
// Dice: a shape and a numbering.
//---------------------------------------------------------------------------
enum class DieType
{
	D4 = 0,
	D6,
	D8,
	D10,
	D12,
	D20,
	D100,
	Count
};

/// How many values a roll of this die takes: 4, 6, 8, 10, 12, 20 or 100.
int Sides( DieType die );

/// Bodies thrown per die: 2 for the d100 (tens and units), 1 otherwise.
int BodiesPerDie( DieType die );

/// The shape of body `part` of a die (0 for everything but the d100's units).
Shape ShapeOf( DieType die );

/// A numbering: which item (face, or the d4's vertices) carries which printed
/// number. `digit` is what is printed, as a number: 1..20 on most dice, 0..9 on
/// the d10 and on both d100 bodies (the tens die prints digit * 10).
struct Labelling
{
	bool atVertex = false;      ///< the d4: read at the top vertex
	std::vector< int > digit;   ///< per item
	std::vector< std::string > text;
	std::vector< bool > marked; ///< a 6 or a 9 on a die that has both
};

/// `part` 0 is the die (or the d100's tens), 1 the d100's units.
const Labelling& GetLabelling( DieType die, int part );

/// The item whose number reads as `result` (1..Sides) on body `part`: on a d10
/// a 10 is the face printed 0; on the d100 the tens body shows (result % 100) / 10
/// and the units body result % 10, so 100 is 00 + 0.
int ItemForResult( DieType die, int part, int result );

/// The item a die at rest with orientation `rotation` (body to world, the
/// RENDERED one, symmetry included) shows on top: the face with the highest
/// normal, or on the d4 the highest vertex.
int TopItem( const Solid& solid, bool atVertex, const M3& rotation );

/// Every group element that carries item `from` onto item `to`. Faces by
/// normal, or vertices on a d4.
std::vector< int > SymmetriesTaking( const Solid& solid, bool atVertex, int from, int to );

/// Where a number is printed: one slot per face, three per face on the d4
/// (each vertex's number near that vertex, on each face that has it).
struct Slot
{
	int face = 0;   ///< the face it is printed on
	int item = 0;   ///< whose number it is (the face, or a d4 vertex)
	V3 centre;      ///< body frame, on the face
	V3 up;          ///< unit, in the face's plane: the top of the number
	double room = 0;///< radius of the circle at `centre` that stays on the face
};

const std::vector< Slot >& GetSlots( Shape shape );

const char* DieName( DieType die );

} // namespace dice::geo
