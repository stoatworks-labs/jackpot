#include "Geometry.h"

#include <algorithm>
#include <cmath>

namespace dice::geo
{
namespace
{
constexpr double kPhi     = 1.6180339887498948482;
constexpr double kDensity = 1200.0;///< kg m^-3: cast resin, which most dice are

/// atan2 with its cut moved just past -pi. A direction exactly opposite the
/// reference (a square's far corner, a kite's far point, a d10 face looking
/// down -x) lands on the cut, and which side it rounds to depends on fused
/// multiply-adds and on the libm: arm64, x86_64 and wasm builds disagreed, and
/// printed a d6's or d10's numbers a different way up. Moved, it sorts last.
double SortAngle( double y, double x )
{
	const double a = std::atan2( y, x );
#ifdef POLYHEDRAL_PLAIN_ATAN2
	return a;//tools/verify.sh's negative control: the cut where it was
#else
	return a < -kPi + 1e-9 ? a + 2.0 * kPi : a;
#endif
}

/// Inradius of each shape as made, metres: a real set's proportions.
double TargetInradius( Shape shape )
{
	switch( shape )
	{
	case Shape::Tetrahedron: return 0.020 / ( 2.0 * std::sqrt( 6.0 ) );//20 mm edges
	case Shape::Cube: return 0.008;                                     //16 mm
	case Shape::Octahedron: return 0.0078;
	case Shape::Trapezohedron: return 0.0080;
	case Shape::Dodecahedron: return 0.0090;
	case Shape::Icosahedron: return 0.0100;                             //20 mm across the faces
	default: return 0.008;
	}
}

std::vector< V3 > CanonicalVertices( Shape shape )
{
	std::vector< V3 > v;
	switch( shape )
	{
	case Shape::Tetrahedron:
		v = { { 1, 1, 1 }, { 1, -1, -1 }, { -1, 1, -1 }, { -1, -1, 1 } };
		break;
	case Shape::Cube:
		for( int i = 0; i < 8; ++i )
			v.push_back( { ( i & 1 ) ? 1.0 : -1.0, ( i & 2 ) ? 1.0 : -1.0, ( i & 4 ) ? 1.0 : -1.0 } );
		break;
	case Shape::Octahedron:
		v = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
		break;
	case Shape::Trapezohedron:
	{
		//The dual of the pentagonal antiprism. Rings of five at +-z, offset by a
		//tenth of a turn, and apexes at +-h. A kite (apex, U_k, L_k, U_k+1) is
		//planar only if the apex, the midpoint of U_k U_k+1 and L_k are
		//collinear in the plane of the axis and L_k, which fixes
		//h = z (1 + cos 36) / (1 - cos 36). z is chosen for a real d10's
		//proportions: about 1.1 times as tall as it is wide.
		const double c = std::cos( kPi / 5.0 );
		const double h = 1.1;
		const double z = h * ( 1.0 - c ) / ( 1.0 + c );
		v.push_back( { 0, h, 0 } );
		v.push_back( { 0, -h, 0 } );
		for( int k = 0; k < 5; ++k )
		{
			const double a = 2.0 * kPi * k / 5.0;
			const double b = a + kPi / 5.0;
			v.push_back( { std::cos( a ), z, std::sin( a ) } );
			v.push_back( { std::cos( b ), -z, std::sin( b ) } );
		}
		break;
	}
	case Shape::Dodecahedron:
	{
		for( int i = 0; i < 8; ++i )
			v.push_back( { ( i & 1 ) ? 1.0 : -1.0, ( i & 2 ) ? 1.0 : -1.0, ( i & 4 ) ? 1.0 : -1.0 } );
		const double a = 1.0 / kPhi, b = kPhi;
		for( int sy : { -1, 1 } )
			for( int sz : { -1, 1 } )
			{
				v.push_back( { 0, sy * a, sz * b } );
				v.push_back( { sy * a, sz * b, 0 } );
				v.push_back( { sz * b, 0, sy * a } );
			}
		break;
	}
	case Shape::Icosahedron:
		for( int s1 : { -1, 1 } )
			for( int s2 : { -1, 1 } )
			{
				v.push_back( { 0, s1 * 1.0, s2 * kPhi } );
				v.push_back( { s1 * 1.0, s2 * kPhi, 0 } );
				v.push_back( { s2 * kPhi, 0, s1 * 1.0 } );
			}
		break;
	default:
		break;
	}
	return v;
}

/// The face's own 2D frame: u along the first vertex from the centroid, w = n x u.
void FaceFrame( const Solid& s, const Face& f, V3 origin, V3& u, V3& w )
{
	u = Normalise( s.vertices[ static_cast< size_t >( f.loop[ 0 ] ) ] - origin );
	u = Normalise( u - f.normal * Dot( u, f.normal ) );
	w = Cross( f.normal, u );
}

/// Distance from `p` (on the face's plane) to the nearest side, positive inside.
double Room( const Solid& s, const Face& f, V3 p )
{
	double room = 1e30;
	const size_t n = f.loop.size();
	for( size_t i = 0; i < n; ++i )
	{
		const V3 a      = s.vertices[ static_cast< size_t >( f.loop[ i ] ) ];
		const V3 b      = s.vertices[ static_cast< size_t >( f.loop[ ( i + 1 ) % n ] ) ];
		const V3 inward = Normalise( Cross( f.normal, b - a ) );//CCW loop: n x edge points in
		room            = std::min( room, Dot( p - a, inward ) );
	}
	return room;
}

/// The largest inscribed circle: room() is concave (a min of linear functions)
/// so a grid that shrinks round its best point finds the maximum without the
/// stalls a pattern search has on min-of-linear ridges.
void Incentre( const Solid& s, Face& f )
{
	V3 u, w;
	FaceFrame( s, f, f.centroid, u, w );
	double span = 0.0;
	for( int index : f.loop )
		span = std::max( span, Length( s.vertices[ static_cast< size_t >( index ) ] - f.centroid ) );

	V3 best         = f.centroid;
	double bestRoom = Room( s, f, best );
	double step     = span / 8.0;
	for( int round = 0; round < 40; ++round )
	{
		const V3 centre = best;
		for( int i = -8; i <= 8; ++i )
			for( int j = -8; j <= 8; ++j )
			{
				const V3 p      = centre + u * ( i * step ) + w * ( j * step );
				const double r  = Room( s, f, p );
				if( r > bestRoom + 1e-15 )
				{
					bestRoom = r;
					best     = p;
				}
			}
		step *= 0.25;
	}
	f.incentre = best;
	f.inradius = bestRoom;
}

Solid Build( Shape shape )
{
	Solid s;
	s.shape                     = shape;
	const std::vector< V3 > raw = CanonicalVertices( shape );

	//-------------------------------------------------------------------
	// Facets of the hull: every triple's plane that has every vertex on one
	// side, merged by normal.
	//-------------------------------------------------------------------
	double extent = 0.0;
	for( const V3& p : raw )
		extent = std::max( extent, Length( p ) );
	const double eps = 1e-9 * extent;

	std::vector< Face > faces;
	const size_t n = raw.size();
	for( size_t i = 0; i < n; ++i )
		for( size_t j = i + 1; j < n; ++j )
			for( size_t k = j + 1; k < n; ++k )
			{
				V3 normal = Cross( raw[ j ] - raw[ i ], raw[ k ] - raw[ i ] );
				if( Length( normal ) < 1e-12 )
					continue;
				normal          = Normalise( normal );
				double offset   = Dot( normal, raw[ i ] );
				bool above = false, below = false;
				for( const V3& p : raw )
				{
					const double d = Dot( normal, p ) - offset;
					above          = above || d > eps;
					below          = below || d < -eps;
				}
				if( above && below )
					continue;
				if( above )
				{
					normal = -normal;
					offset = -offset;
				}
				bool known = false;
				for( const Face& f : faces )
					known = known || Dot( f.normal, normal ) > 1.0 - 1e-12;
				if( known )
					continue;
				Face f;
				f.normal = normal;
				f.offset = offset;
				for( size_t m = 0; m < n; ++m )
					if( std::fabs( Dot( normal, raw[ m ] ) - offset ) < eps )
						f.loop.push_back( static_cast< int >( m ) );
				faces.push_back( f );
			}

	//-------------------------------------------------------------------
	// Scale to the real size: the inradius is the planes' common offset.
	//-------------------------------------------------------------------
	double inradius = 1e30;
	for( const Face& f : faces )
		inradius = std::min( inradius, f.offset );
	const double scale = TargetInradius( shape ) / inradius;
	for( const V3& p : raw )
		s.vertices.push_back( p * scale );
	for( Face& f : faces )
		f.offset *= scale;

	//Each loop counter-clockwise about the outward normal.
	for( Face& f : faces )
	{
		V3 centre {};
		for( int index : f.loop )
			centre += s.vertices[ static_cast< size_t >( index ) ];
		centre = centre * ( 1.0 / static_cast< double >( f.loop.size() ) );
		const V3 u = Normalise( s.vertices[ static_cast< size_t >( f.loop[ 0 ] ) ] - centre );
		const V3 w = Cross( f.normal, u );
		std::sort( f.loop.begin(), f.loop.end(), [ & ]( int a, int b ) {
			const V3 pa = s.vertices[ static_cast< size_t >( a ) ] - centre;
			const V3 pb = s.vertices[ static_cast< size_t >( b ) ] - centre;
			return SortAngle( Dot( pa, w ), Dot( pa, u ) ) < SortAngle( Dot( pb, w ), Dot( pb, u ) );
		} );

		//Area centroid, by the fan from the first vertex.
		double area = 0.0;
		V3 weighted {};
		const V3 a0 = s.vertices[ static_cast< size_t >( f.loop[ 0 ] ) ];
		for( size_t i = 1; i + 1 < f.loop.size(); ++i )
		{
			const V3 a1 = s.vertices[ static_cast< size_t >( f.loop[ i ] ) ];
			const V3 a2 = s.vertices[ static_cast< size_t >( f.loop[ i + 1 ] ) ];
			const double t = 0.5 * Dot( Cross( a1 - a0, a2 - a0 ), f.normal );
			area += t;
			weighted += ( a0 + a1 + a2 ) * ( t / 3.0 );
		}
		f.centroid = weighted * ( 1.0 / area );
	}
	s.faces = faces;
	for( Face& f : s.faces )
		Incentre( s, f );

	//-------------------------------------------------------------------
	// Edges, probes, opposite faces.
	//-------------------------------------------------------------------
	for( const Face& f : s.faces )
		for( size_t i = 0; i < f.loop.size(); ++i )
		{
			int a = f.loop[ i ], b = f.loop[ ( i + 1 ) % f.loop.size() ];
			if( a > b )
				std::swap( a, b );
			const std::array< int, 2 > edge = { a, b };
			if( std::find( s.edges.begin(), s.edges.end(), edge ) == s.edges.end() )
				s.edges.push_back( edge );
		}
	s.probes = s.vertices;
	for( const auto& e : s.edges )
		s.probes.push_back( ( s.vertices[ static_cast< size_t >( e[ 0 ] ) ] + s.vertices[ static_cast< size_t >( e[ 1 ] ) ] ) * 0.5 );
	for( const Face& f : s.faces )
	{
		int found = -1;
		for( size_t g = 0; g < s.faces.size(); ++g )
			if( Dot( s.faces[ g ].normal, f.normal ) < -1.0 + 1e-9 )
				found = static_cast< int >( g );
		s.opposite.push_back( found );
	}

	//-------------------------------------------------------------------
	// Radii, volume, inertia (a fan of tetrahedra from the centre).
	//-------------------------------------------------------------------
	s.inradius = 1e30;
	for( const Face& f : s.faces )
		s.inradius = std::min( s.inradius, f.offset );
	for( const V3& p : s.vertices )
		s.circumradius = std::max( s.circumradius, Length( p ) );
	for( const auto& e : s.edges )
		s.midradius = std::max( s.midradius, Length( ( s.vertices[ static_cast< size_t >( e[ 0 ] ) ] + s.vertices[ static_cast< size_t >( e[ 1 ] ) ] ) * 0.5 ) );

	M3 second = M3::Zero();
	double volume = 0.0;
	auto outer = []( V3 a, V3 b ) {
		M3 r;
		r.m[ 0 ][ 0 ] = a.x * b.x; r.m[ 0 ][ 1 ] = a.x * b.y; r.m[ 0 ][ 2 ] = a.x * b.z;
		r.m[ 1 ][ 0 ] = a.y * b.x; r.m[ 1 ][ 1 ] = a.y * b.y; r.m[ 1 ][ 2 ] = a.y * b.z;
		r.m[ 2 ][ 0 ] = a.z * b.x; r.m[ 2 ][ 1 ] = a.z * b.y; r.m[ 2 ][ 2 ] = a.z * b.z;
		return r;
	};
	for( const Face& f : s.faces )
	{
		const V3 a = s.vertices[ static_cast< size_t >( f.loop[ 0 ] ) ];
		for( size_t i = 1; i + 1 < f.loop.size(); ++i )
		{
			const V3 b = s.vertices[ static_cast< size_t >( f.loop[ i ] ) ];
			const V3 c = s.vertices[ static_cast< size_t >( f.loop[ i + 1 ] ) ];
			const double v = Dot( a, Cross( b, c ) ) / 6.0;
			volume += v;
			const V3 sum = a + b + c;
			second = second + ( outer( a, a ) + outer( b, b ) + outer( c, c ) + outer( sum, sum ) ) * ( v / 20.0 );
		}
	}
	s.volume = volume;
	s.mass   = volume * kDensity;
	const double trace = second.m[ 0 ][ 0 ] + second.m[ 1 ][ 1 ] + second.m[ 2 ][ 2 ];
	M3 inertia = M3 {} * trace + second * -1.0;
	s.inertia        = inertia * kDensity;
	s.inverseInertia = Inverse( s.inertia );

	//-------------------------------------------------------------------
	// The rotation group: every rotation taking face 0's frame (normal, a
	// vertex direction) to another face's, kept if it carries the vertex set
	// onto itself.
	//-------------------------------------------------------------------
	const Face& f0 = s.faces[ 0 ];
	auto frame = [ & ]( const Face& f, int vertex ) {
		V3 e = s.vertices[ static_cast< size_t >( vertex ) ] - f.centroid;
		e    = Normalise( e - f.normal * Dot( e, f.normal ) );
		return M3::Columns( e, Cross( f.normal, e ), f.normal );
	};
	const M3 reference = Transpose( frame( f0, f0.loop[ 0 ] ) );
	const double tol   = 1e-9 * s.circumradius;
	s.group.push_back( M3 {} );
	for( const Face& f : s.faces )
		for( int vertex : f.loop )
		{
			const M3 r = frame( f, vertex ) * reference;
			bool closed = true;
			for( const V3& p : s.vertices )
			{
				const V3 q   = r * p;
				bool matched = false;
				for( const V3& o : s.vertices )
					matched = matched || Length( q - o ) < tol;
				if( !matched )
				{
					closed = false;
					break;
				}
			}
			if( !closed )
				continue;
			bool known = false;
			for( const M3& g : s.group )
				known = known || MaxDifference( g, r ) < 1e-9;
			if( !known )
				s.group.push_back( r );
		}
	return s;
}

//---------------------------------------------------------------------------
// Numberings.
//---------------------------------------------------------------------------

/// A balanced numbering for a die whose faces come in opposite pairs that sum
/// to n + 1: a local search over which pair gets which value and which way
/// round, minimising how far each vertex's sum of numbers is from the mean.
/// Without it the high numbers cluster on one side, which a player notices
/// (and which a real set's d20 is designed not to do).
std::vector< int > BalancedPairs( const Solid& s )
{
	const int faces = static_cast< int >( s.faces.size() );
	std::vector< std::array< int, 2 > > pairs;
	for( int f = 0; f < faces; ++f )
		if( s.opposite[ static_cast< size_t >( f ) ] > f )
			pairs.push_back( { f, s.opposite[ static_cast< size_t >( f ) ] } );

	std::vector< std::vector< int > > around( s.vertices.size() );
	for( int f = 0; f < faces; ++f )
		for( int v : s.faces[ static_cast< size_t >( f ) ].loop )
			around[ static_cast< size_t >( v ) ].push_back( f );

	std::vector< int > value( static_cast< size_t >( faces ) );
	auto assign = [ & ]( const std::vector< int >& order, const std::vector< int >& flip ) {
		for( size_t p = 0; p < pairs.size(); ++p )
		{
			const int low                                                  = order[ p ] + 1;
			value[ static_cast< size_t >( pairs[ p ][ flip[ p ] ] ) ]       = low;
			value[ static_cast< size_t >( pairs[ p ][ 1 - flip[ p ] ] ) ]   = faces + 1 - low;
		}
	};
	auto cost = [ & ]() {
		double total = 0.0;
		for( const auto& list : around )
		{
			double sum = 0.0;
			for( int f : list )
				sum += value[ static_cast< size_t >( f ) ];
			const double ideal = 0.5 * ( faces + 1 ) * static_cast< double >( list.size() );
			total += ( sum - ideal ) * ( sum - ideal );
		}
		return total;
	};

	std::vector< int > order( pairs.size() ), flip( pairs.size(), 0 );
	for( size_t p = 0; p < pairs.size(); ++p )
		order[ p ] = static_cast< int >( p );
	assign( order, flip );
	double best = cost();
	for( bool improved = true; improved; )
	{
		improved = false;
		for( size_t a = 0; a < pairs.size(); ++a )
		{
			for( size_t b = a + 1; b < pairs.size(); ++b )
			{
				std::swap( order[ a ], order[ b ] );
				assign( order, flip );
				const double c = cost();
				if( c < best - 1e-9 )
				{
					best     = c;
					improved = true;
				}
				else
					std::swap( order[ a ], order[ b ] );
			}
			flip[ a ] ^= 1;
			assign( order, flip );
			const double c = cost();
			if( c < best - 1e-9 )
			{
				best     = c;
				improved = true;
			}
			else
				flip[ a ] ^= 1;
		}
	}
	assign( order, flip );
	return value;
}

std::vector< int > NumberFaces( DieType die, const Solid& s )
{
	const size_t faces = s.faces.size();
	std::vector< int > digit( faces, 0 );
	switch( die )
	{
	case DieType::D6:
		//1, 2 and 3 meet at a corner and run counter-clockwise round it, as on
		//a western die: +x, +y, +z (a +120 degree turn about (1,1,1) carries x
		//to y to z, counter-clockwise seen from outside), opposites summing to 7.
		for( size_t f = 0; f < faces; ++f )
		{
			const V3 n = s.faces[ f ].normal;
			if( n.x > 0.5 ) digit[ f ] = 1;
			else if( n.y > 0.5 ) digit[ f ] = 2;
			else if( n.z > 0.5 ) digit[ f ] = 3;
			else if( n.z < -0.5 ) digit[ f ] = 4;
			else if( n.y < -0.5 ) digit[ f ] = 5;
			else digit[ f ] = 6;
		}
		return digit;
	case DieType::D10:
	case DieType::D100:
	{
		//Evens round the upper apex, 0 8 6 4 2 in turn; each lower face is
		//9 minus its opposite, so the belt reads 0 5 8 7 6 9 4 1 2 3.
		std::vector< std::pair< double, size_t > > upper;
		for( size_t f = 0; f < faces; ++f )
			if( s.faces[ f ].normal.y > 0.0 )
				upper.push_back( { SortAngle( s.faces[ f ].normal.z, s.faces[ f ].normal.x ), f } );
		std::sort( upper.begin(), upper.end() );
		const int evens[ 5 ] = { 0, 8, 6, 4, 2 };
		for( size_t k = 0; k < upper.size(); ++k )
		{
			const size_t f = upper[ k ].second;
			digit[ f ]     = evens[ k ];
			digit[ static_cast< size_t >( s.opposite[ f ] ) ] = 9 - evens[ k ];
		}
		return digit;
	}
	default:
		return BalancedPairs( s );
	}
}

Labelling MakeLabelling( DieType die, int part, const Solid& s )
{
	Labelling l;
	if( die == DieType::D4 )
	{
		l.atVertex = true;
		for( size_t v = 0; v < s.vertices.size(); ++v )
			l.digit.push_back( static_cast< int >( v ) + 1 );
	}
	else
		l.digit = NumberFaces( die, s );

	const bool tens = die == DieType::D100 && part == 0;
	//Only a die that has both a 6 and a 9 needs them told apart.
	const bool both = Sides( die ) >= 9 && !tens;
	for( int d : l.digit )
	{
		std::string text = std::to_string( tens ? d * 10 : d );
		if( tens && d == 0 )
			text = "00";
		l.text.push_back( text );
		l.marked.push_back( both && ( text == "6" || text == "9" ) );
	}
	return l;
}

std::vector< Slot > MakeSlots( Shape shape, const Solid& s )
{
	std::vector< Slot > slots;
	if( shape == Shape::Tetrahedron )
	{
		//Each vertex's number near that vertex, on each of its three faces,
		//its top toward the vertex: whichever face you look at, the number at
		//the top is the roll.
		for( size_t f = 0; f < s.faces.size(); ++f )
		{
			const Face& face = s.faces[ f ];
			for( int v : face.loop )
			{
				Slot slot;
				slot.face      = static_cast< int >( f );
				slot.item      = v;
				const V3 apex  = s.vertices[ static_cast< size_t >( v ) ];
				slot.centre    = face.centroid + ( apex - face.centroid ) * 0.5;
				slot.up        = Normalise( apex - face.centroid );
				slot.room      = Room( s, face, slot.centre );
				slots.push_back( slot );
			}
		}
		return slots;
	}

	for( size_t f = 0; f < s.faces.size(); ++f )
	{
		const Face& face = s.faces[ f ];
		Slot slot;
		slot.face   = static_cast< int >( f );
		slot.item   = static_cast< int >( f );
		slot.centre = face.incentre;
		slot.room   = face.inradius;
		V3 toward;
		if( shape == Shape::Cube )
		{
			//Square faces read toward an edge, not a corner.
			const V3 a = s.vertices[ static_cast< size_t >( face.loop[ 0 ] ) ];
			const V3 b = s.vertices[ static_cast< size_t >( face.loop[ 1 ] ) ];
			toward     = ( a + b ) * 0.5;
		}
		else if( shape == Shape::Trapezohedron )
		{
			//A d10's number stands with its top toward the kite's long point,
			//the die's apex: the face vertex farthest from the incentre.
			double far = -1.0;
			for( int v : face.loop )
			{
				const double d = Length( s.vertices[ static_cast< size_t >( v ) ] - face.incentre );
				if( d > far )
				{
					far    = d;
					toward = s.vertices[ static_cast< size_t >( v ) ];
				}
			}
		}
		else
			toward = s.vertices[ static_cast< size_t >( face.loop[ 0 ] ) ];
		V3 up   = toward - slot.centre;
		slot.up = Normalise( up - face.normal * Dot( up, face.normal ) );
		slots.push_back( slot );
	}
	return slots;
}

/// Everything, built once. A function-local static is initialised exactly once
/// even with several plugin instances racing for it (C++11 magic statics), and
/// is immutable after, so the planner's worker can read it without a lock.
struct Tables
{
	Solid solids[ static_cast< int >( Shape::Count ) ];
	Labelling labels[ static_cast< int >( DieType::Count ) ][ 2 ];
	std::vector< Slot > slots[ static_cast< int >( Shape::Count ) ];
};

const Tables& GetTables()
{
	static const Tables tables = [] {
		Tables t;
		for( int i = 0; i < static_cast< int >( Shape::Count ); ++i )
			t.solids[ i ] = Build( static_cast< Shape >( i ) );
		for( int i = 0; i < static_cast< int >( Shape::Count ); ++i )
			t.slots[ i ] = MakeSlots( static_cast< Shape >( i ), t.solids[ i ] );
		for( int d = 0; d < static_cast< int >( DieType::Count ); ++d )
			for( int part = 0; part < 2; ++part )
			{
				const DieType die = static_cast< DieType >( d );
				t.labels[ d ][ part ] = MakeLabelling( die, die == DieType::D100 ? part : 0,
				                                       t.solids[ static_cast< int >( ShapeOf( die ) ) ] );
			}
		return t;
	}();
	return tables;
}
} // namespace

const Solid& GetSolid( Shape shape )
{
	return GetTables().solids[ std::clamp( static_cast< int >( shape ), 0, static_cast< int >( Shape::Count ) - 1 ) ];
}

int Sides( DieType die )
{
	static const int sides[] = { 4, 6, 8, 10, 12, 20, 100 };
	return sides[ std::clamp( static_cast< int >( die ), 0, 6 ) ];
}

int BodiesPerDie( DieType die )
{
	return die == DieType::D100 ? 2 : 1;
}

Shape ShapeOf( DieType die )
{
	switch( die )
	{
	case DieType::D4: return Shape::Tetrahedron;
	case DieType::D6: return Shape::Cube;
	case DieType::D8: return Shape::Octahedron;
	case DieType::D10: return Shape::Trapezohedron;
	case DieType::D12: return Shape::Dodecahedron;
	case DieType::D20: return Shape::Icosahedron;
	case DieType::D100: return Shape::Trapezohedron;
	default: return Shape::Cube;
	}
}

const char* DieName( DieType die )
{
	static const char* const names[] = { "D4", "D6", "D8", "D10", "D12", "D20", "D100" };
	return names[ std::clamp( static_cast< int >( die ), 0, 6 ) ];
}

const Labelling& GetLabelling( DieType die, int part )
{
	const int d = std::clamp( static_cast< int >( die ), 0, static_cast< int >( DieType::Count ) - 1 );
	return GetTables().labels[ d ][ die == DieType::D100 ? std::clamp( part, 0, 1 ) : 0 ];
}

const std::vector< Slot >& GetSlots( Shape shape )
{
	return GetTables().slots[ std::clamp( static_cast< int >( shape ), 0, static_cast< int >( Shape::Count ) - 1 ) ];
}

int ItemForResult( DieType die, int part, int result )
{
	const Labelling& l = GetLabelling( die, part );
	int digit          = result;
	if( die == DieType::D10 )
		digit = ( ( result % 10 ) + 10 ) % 10;
	else if( die == DieType::D100 )
		digit = part == 0 ? ( ( ( result % 100 ) + 100 ) % 100 ) / 10 : ( ( result % 10 ) + 10 ) % 10;
	for( size_t i = 0; i < l.digit.size(); ++i )
		if( l.digit[ i ] == digit )
			return static_cast< int >( i );
	return 0;
}

int TopItem( const Solid& solid, bool atVertex, const M3& rotation )
{
	int best        = 0;
	double highest  = -1e30;
	if( atVertex )
	{
		for( size_t v = 0; v < solid.vertices.size(); ++v )
		{
			const double y = ( rotation * solid.vertices[ v ] ).y;
			if( y > highest )
			{
				highest = y;
				best    = static_cast< int >( v );
			}
		}
		return best;
	}
	for( size_t f = 0; f < solid.faces.size(); ++f )
	{
		const double y = ( rotation * solid.faces[ f ].normal ).y;
		if( y > highest )
		{
			highest = y;
			best    = static_cast< int >( f );
		}
	}
	return best;
}

std::vector< int > SymmetriesTaking( const Solid& solid, bool atVertex, int from, int to )
{
	std::vector< int > found;
	const V3 a = atVertex ? Normalise( solid.vertices[ static_cast< size_t >( from ) ] ) : solid.faces[ static_cast< size_t >( from ) ].normal;
	const V3 b = atVertex ? Normalise( solid.vertices[ static_cast< size_t >( to ) ] ) : solid.faces[ static_cast< size_t >( to ) ].normal;
	for( size_t g = 0; g < solid.group.size(); ++g )
		if( Length( solid.group[ g ] * a - b ) < 1e-9 )
			found.push_back( static_cast< int >( g ) );
	return found;
}

} // namespace dice::geo
