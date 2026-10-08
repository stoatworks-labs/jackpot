#pragma once

#include <cmath>
#include <cstdint>

/**
    The small amount of linear algebra the dice need, in double.

    Everything the physics and the planner touch is double: a roll is integrated
    for several thousand steps, and the symmetry test compares rotated vertices
    against the originals to 1e-9 of a die's size. The renderer is handed floats
    at the very end.

    Rotations are 3x3 matrices that take BODY coordinates to WORLD coordinates
    (`world = R * body`); orientations integrate as unit quaternions and are
    turned into matrices only where something needs one.
*/
namespace dice
{
constexpr double kPi = 3.14159265358979323846;

struct V3
{
	double x = 0.0, y = 0.0, z = 0.0;
};

inline V3 operator+( V3 a, V3 b ) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline V3 operator-( V3 a, V3 b ) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline V3 operator-( V3 a ) { return { -a.x, -a.y, -a.z }; }
inline V3 operator*( V3 a, double s ) { return { a.x * s, a.y * s, a.z * s }; }
inline V3 operator*( double s, V3 a ) { return { a.x * s, a.y * s, a.z * s }; }
inline V3& operator+=( V3& a, V3 b ) { a = a + b; return a; }
inline V3& operator-=( V3& a, V3 b ) { a = a - b; return a; }
inline double Dot( V3 a, V3 b ) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross( V3 a, V3 b ) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline double Length( V3 a ) { return std::sqrt( Dot( a, a ) ); }
inline V3 Normalise( V3 a )
{
	const double l = Length( a );
	return l > 0.0 ? a * ( 1.0 / l ) : V3 {};
}

/// Row-major: m[ row ][ column ].
struct M3
{
	double m[ 3 ][ 3 ] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };

	static M3 Columns( V3 a, V3 b, V3 c )
	{
		M3 r;
		r.m[ 0 ][ 0 ] = a.x; r.m[ 0 ][ 1 ] = b.x; r.m[ 0 ][ 2 ] = c.x;
		r.m[ 1 ][ 0 ] = a.y; r.m[ 1 ][ 1 ] = b.y; r.m[ 1 ][ 2 ] = c.y;
		r.m[ 2 ][ 0 ] = a.z; r.m[ 2 ][ 1 ] = b.z; r.m[ 2 ][ 2 ] = c.z;
		return r;
	}
	static M3 Zero()
	{
		M3 r;
		for( auto& row : r.m )
			for( double& v : row )
				v = 0.0;
		return r;
	}
	V3 Column( int c ) const { return { m[ 0 ][ c ], m[ 1 ][ c ], m[ 2 ][ c ] }; }
};

inline V3 operator*( const M3& a, V3 v )
{
	return { a.m[ 0 ][ 0 ] * v.x + a.m[ 0 ][ 1 ] * v.y + a.m[ 0 ][ 2 ] * v.z,
	         a.m[ 1 ][ 0 ] * v.x + a.m[ 1 ][ 1 ] * v.y + a.m[ 1 ][ 2 ] * v.z,
	         a.m[ 2 ][ 0 ] * v.x + a.m[ 2 ][ 1 ] * v.y + a.m[ 2 ][ 2 ] * v.z };
}

inline M3 operator*( const M3& a, const M3& b )
{
	M3 r = M3::Zero();
	for( int i = 0; i < 3; ++i )
		for( int j = 0; j < 3; ++j )
			for( int k = 0; k < 3; ++k )
				r.m[ i ][ j ] += a.m[ i ][ k ] * b.m[ k ][ j ];
	return r;
}

inline M3 operator+( const M3& a, const M3& b )
{
	M3 r;
	for( int i = 0; i < 3; ++i )
		for( int j = 0; j < 3; ++j )
			r.m[ i ][ j ] = a.m[ i ][ j ] + b.m[ i ][ j ];
	return r;
}

inline M3 operator*( const M3& a, double s )
{
	M3 r;
	for( int i = 0; i < 3; ++i )
		for( int j = 0; j < 3; ++j )
			r.m[ i ][ j ] = a.m[ i ][ j ] * s;
	return r;
}

inline M3 Transpose( const M3& a )
{
	M3 r;
	for( int i = 0; i < 3; ++i )
		for( int j = 0; j < 3; ++j )
			r.m[ i ][ j ] = a.m[ j ][ i ];
	return r;
}

inline double Determinant( const M3& a )
{
	return a.m[ 0 ][ 0 ] * ( a.m[ 1 ][ 1 ] * a.m[ 2 ][ 2 ] - a.m[ 1 ][ 2 ] * a.m[ 2 ][ 1 ] )
	     - a.m[ 0 ][ 1 ] * ( a.m[ 1 ][ 0 ] * a.m[ 2 ][ 2 ] - a.m[ 1 ][ 2 ] * a.m[ 2 ][ 0 ] )
	     + a.m[ 0 ][ 2 ] * ( a.m[ 1 ][ 0 ] * a.m[ 2 ][ 1 ] - a.m[ 1 ][ 1 ] * a.m[ 2 ][ 0 ] );
}

inline M3 Inverse( const M3& a )
{
	const double det = Determinant( a );
	M3 r;
	const double s = det != 0.0 ? 1.0 / det : 0.0;
	r.m[ 0 ][ 0 ] = ( a.m[ 1 ][ 1 ] * a.m[ 2 ][ 2 ] - a.m[ 1 ][ 2 ] * a.m[ 2 ][ 1 ] ) * s;
	r.m[ 0 ][ 1 ] = ( a.m[ 0 ][ 2 ] * a.m[ 2 ][ 1 ] - a.m[ 0 ][ 1 ] * a.m[ 2 ][ 2 ] ) * s;
	r.m[ 0 ][ 2 ] = ( a.m[ 0 ][ 1 ] * a.m[ 1 ][ 2 ] - a.m[ 0 ][ 2 ] * a.m[ 1 ][ 1 ] ) * s;
	r.m[ 1 ][ 0 ] = ( a.m[ 1 ][ 2 ] * a.m[ 2 ][ 0 ] - a.m[ 1 ][ 0 ] * a.m[ 2 ][ 2 ] ) * s;
	r.m[ 1 ][ 1 ] = ( a.m[ 0 ][ 0 ] * a.m[ 2 ][ 2 ] - a.m[ 0 ][ 2 ] * a.m[ 2 ][ 0 ] ) * s;
	r.m[ 1 ][ 2 ] = ( a.m[ 0 ][ 2 ] * a.m[ 1 ][ 0 ] - a.m[ 0 ][ 0 ] * a.m[ 1 ][ 2 ] ) * s;
	r.m[ 2 ][ 0 ] = ( a.m[ 1 ][ 0 ] * a.m[ 2 ][ 1 ] - a.m[ 1 ][ 1 ] * a.m[ 2 ][ 0 ] ) * s;
	r.m[ 2 ][ 1 ] = ( a.m[ 0 ][ 1 ] * a.m[ 2 ][ 0 ] - a.m[ 0 ][ 0 ] * a.m[ 2 ][ 1 ] ) * s;
	r.m[ 2 ][ 2 ] = ( a.m[ 0 ][ 0 ] * a.m[ 1 ][ 1 ] - a.m[ 0 ][ 1 ] * a.m[ 1 ][ 0 ] ) * s;
	return r;
}

/// Rotation by `angle` radians about the unit `axis` (right-handed).
inline M3 AxisAngle( V3 axis, double angle )
{
	const V3 a     = Normalise( axis );
	const double c = std::cos( angle ), s = std::sin( angle ), t = 1.0 - c;
	M3 r;
	r.m[ 0 ][ 0 ] = t * a.x * a.x + c;       r.m[ 0 ][ 1 ] = t * a.x * a.y - s * a.z; r.m[ 0 ][ 2 ] = t * a.x * a.z + s * a.y;
	r.m[ 1 ][ 0 ] = t * a.x * a.y + s * a.z; r.m[ 1 ][ 1 ] = t * a.y * a.y + c;       r.m[ 1 ][ 2 ] = t * a.y * a.z - s * a.x;
	r.m[ 2 ][ 0 ] = t * a.x * a.z - s * a.y; r.m[ 2 ][ 1 ] = t * a.y * a.z + s * a.x; r.m[ 2 ][ 2 ] = t * a.z * a.z + c;
	return r;
}

/// The rotation that carries unit `from` onto unit `to` by the shortest arc.
inline M3 Align( V3 from, V3 to )
{
	const V3 axis  = Cross( from, to );
	const double s = Length( axis );
	const double c = Dot( from, to );
	if( s < 1e-15 )
	{
		if( c > 0.0 )
			return M3 {};
		//Antiparallel: any perpendicular axis, half a turn.
		const V3 any = std::fabs( from.x ) < 0.9 ? V3 { 1, 0, 0 } : V3 { 0, 1, 0 };
		return AxisAngle( Cross( from, any ), kPi );
	}
	return AxisAngle( axis, std::atan2( s, c ) );
}

/// Largest absolute element of a - b.
inline double MaxDifference( const M3& a, const M3& b )
{
	double worst = 0.0;
	for( int i = 0; i < 3; ++i )
		for( int j = 0; j < 3; ++j )
			worst = std::fmax( worst, std::fabs( a.m[ i ][ j ] - b.m[ i ][ j ] ) );
	return worst;
}

struct Quat
{
	double w = 1.0, x = 0.0, y = 0.0, z = 0.0;
};

inline Quat operator*( Quat a, Quat b )
{
	return { a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
	         a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w };
}

inline Quat Normalise( Quat q )
{
	const double l = std::sqrt( q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z );
	return l > 0.0 ? Quat { q.w / l, q.x / l, q.y / l, q.z / l } : Quat {};
}

inline M3 ToMatrix( Quat q )
{
	M3 r;
	const double xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
	const double xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
	const double wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
	r.m[ 0 ][ 0 ] = 1 - 2 * ( yy + zz ); r.m[ 0 ][ 1 ] = 2 * ( xy - wz );     r.m[ 0 ][ 2 ] = 2 * ( xz + wy );
	r.m[ 1 ][ 0 ] = 2 * ( xy + wz );     r.m[ 1 ][ 1 ] = 1 - 2 * ( xx + zz ); r.m[ 1 ][ 2 ] = 2 * ( yz - wx );
	r.m[ 2 ][ 0 ] = 2 * ( xz - wy );     r.m[ 2 ][ 1 ] = 2 * ( yz + wx );     r.m[ 2 ][ 2 ] = 1 - 2 * ( xx + yy );
	return r;
}

/// Shepperd's method: the branch on the largest diagonal term keeps it exact
/// for every rotation, including the half-turns the symmetry groups are full of.
inline Quat FromMatrix( const M3& r )
{
	const double t = r.m[ 0 ][ 0 ] + r.m[ 1 ][ 1 ] + r.m[ 2 ][ 2 ];
	Quat q;
	if( t > 0.0 )
	{
		const double s = std::sqrt( t + 1.0 ) * 2.0;
		q = { 0.25 * s, ( r.m[ 2 ][ 1 ] - r.m[ 1 ][ 2 ] ) / s, ( r.m[ 0 ][ 2 ] - r.m[ 2 ][ 0 ] ) / s, ( r.m[ 1 ][ 0 ] - r.m[ 0 ][ 1 ] ) / s };
	}
	else if( r.m[ 0 ][ 0 ] > r.m[ 1 ][ 1 ] && r.m[ 0 ][ 0 ] > r.m[ 2 ][ 2 ] )
	{
		const double s = std::sqrt( 1.0 + r.m[ 0 ][ 0 ] - r.m[ 1 ][ 1 ] - r.m[ 2 ][ 2 ] ) * 2.0;
		q = { ( r.m[ 2 ][ 1 ] - r.m[ 1 ][ 2 ] ) / s, 0.25 * s, ( r.m[ 0 ][ 1 ] + r.m[ 1 ][ 0 ] ) / s, ( r.m[ 0 ][ 2 ] + r.m[ 2 ][ 0 ] ) / s };
	}
	else if( r.m[ 1 ][ 1 ] > r.m[ 2 ][ 2 ] )
	{
		const double s = std::sqrt( 1.0 + r.m[ 1 ][ 1 ] - r.m[ 0 ][ 0 ] - r.m[ 2 ][ 2 ] ) * 2.0;
		q = { ( r.m[ 0 ][ 2 ] - r.m[ 2 ][ 0 ] ) / s, ( r.m[ 0 ][ 1 ] + r.m[ 1 ][ 0 ] ) / s, 0.25 * s, ( r.m[ 1 ][ 2 ] + r.m[ 2 ][ 1 ] ) / s };
	}
	else
	{
		const double s = std::sqrt( 1.0 + r.m[ 2 ][ 2 ] - r.m[ 0 ][ 0 ] - r.m[ 1 ][ 1 ] ) * 2.0;
		q = { ( r.m[ 1 ][ 0 ] - r.m[ 0 ][ 1 ] ) / s, ( r.m[ 0 ][ 2 ] + r.m[ 2 ][ 0 ] ) / s, ( r.m[ 1 ][ 2 ] + r.m[ 2 ][ 1 ] ) / s, 0.25 * s };
	}
	return Normalise( q );
}

/// Spherical interpolation along the short way round.
inline Quat Slerp( Quat a, Quat b, double t )
{
	double c = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
	if( c < 0.0 )
	{
		b = { -b.w, -b.x, -b.y, -b.z };
		c = -c;
	}
	if( c > 0.9995 )
		return Normalise( Quat { a.w + ( b.w - a.w ) * t, a.x + ( b.x - a.x ) * t, a.y + ( b.y - a.y ) * t, a.z + ( b.z - a.z ) * t } );
	const double angle = std::acos( c );
	const double s     = std::sin( angle );
	const double wa    = std::sin( ( 1.0 - t ) * angle ) / s;
	const double wb    = std::sin( t * angle ) / s;
	return { a.w * wa + b.w * wb, a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb };
}

//---------------------------------------------------------------------------
// Integer hashing. The plugin's whole idea of chance: PCG's output mix, exact in
// 32 bits, the same on every machine (and mirrored in the GLSL for textures).
//---------------------------------------------------------------------------
inline uint32_t Pcg( uint32_t v )
{
	const uint32_t state = v * 747796405u + 2891336453u;
	const uint32_t word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

inline uint32_t Hash( uint32_t a, uint32_t b = 0, uint32_t c = 0, uint32_t d = 0 )
{
	return Pcg( a ^ Pcg( b ^ Pcg( c ^ Pcg( d + 0x9e3779b9u ) ) ) );
}

/// [0, 1), 32 bits of it.
inline double Unit( uint32_t h )
{
	return static_cast< double >( h ) * ( 1.0 / 4294967296.0 );
}

/// A uniform integer in [0, n), without modulo bias: Lemire's multiply-shift
/// with rejection. `next` is called again whenever a draw lands in the biased
/// sliver, which for n <= 100 is about one draw in forty million.
template< typename Next >
uint32_t Below( uint32_t n, Next next )
{
	uint64_t m        = static_cast< uint64_t >( next() ) * n;
	uint32_t low      = static_cast< uint32_t >( m );
	if( low < n )
	{
		const uint32_t threshold = static_cast< uint32_t >( -n ) % n;
		while( low < threshold )
		{
			m   = static_cast< uint64_t >( next() ) * n;
			low = static_cast< uint32_t >( m );
		}
	}
	return static_cast< uint32_t >( m >> 32 );
}

} // namespace dice
