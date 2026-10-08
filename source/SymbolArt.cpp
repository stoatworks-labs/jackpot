#include "SymbolArt.h"

#include "Slots.h"
#include "Typeface.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace jackpot
{
namespace
{
struct P
{
	double x = 0.0, y = 0.0;
};
P operator-( P a, P b ) { return { a.x - b.x, a.y - b.y }; }
P operator+( P a, P b ) { return { a.x + b.x, a.y + b.y }; }
P operator*( P a, double s ) { return { a.x * s, a.y * s }; }
double Len( P a ) { return std::sqrt( a.x * a.x + a.y * a.y ); }
double DotP( P a, P b ) { return a.x * b.x + a.y * b.y; }

struct Rgb
{
	double r = 0.0, g = 0.0, b = 0.0;
};
Rgb operator*( Rgb c, double s ) { return { c.r * s, c.g * s, c.b * s }; }
Rgb operator+( Rgb a, Rgb b ) { return { a.r + b.r, a.g + b.g, a.b + b.b }; }
Rgb Mix( Rgb a, Rgb b, double t ) { return a * ( 1.0 - t ) + b * t; }
Rgb Hex( uint32_t h )
{
	auto lin = []( uint32_t v ) { return std::pow( static_cast< double >( v ) / 255.0, 2.2 ); };
	return { lin( ( h >> 16 ) & 255 ), lin( ( h >> 8 ) & 255 ), lin( h & 255 ) };
}

//---------------------------------------------------------------------------
// Distances. Tile coordinates run -1..1 both ways, y up.
//---------------------------------------------------------------------------
double Circle( P p, P c, double r ) { return Len( p - c ) - r; }

double Ellipse( P p, P c, double rx, double ry, double turn = 0.0 )
{
	P d = p - c;
	const double cs = std::cos( turn ), sn = std::sin( turn );
	d = { cs * d.x + sn * d.y, -sn * d.x + cs * d.y };
	//Scaled-circle distance, corrected by the gradient: good near the edge,
	//which is all an outline needs.
	const double k  = Len( { d.x / rx, d.y / ry } );
	const double gx = d.x / ( rx * rx ), gy = d.y / ( ry * ry );
	const double g  = std::max( 1e-9, Len( { gx, gy } ) );
	return k * ( k - 1.0 ) / std::max( 1e-9, k * g );
}

double Box( P p, P c, double hx, double hy, double radius )
{
	const P d = { std::fabs( p.x - c.x ) - hx + radius, std::fabs( p.y - c.y ) - hy + radius };
	return Len( { std::max( d.x, 0.0 ), std::max( d.y, 0.0 ) } ) + std::min( std::max( d.x, d.y ), 0.0 ) - radius;
}

double Segment( P p, P a, P b, double r )
{
	const P pa = p - a, ba = b - a;
	const double h = std::clamp( DotP( pa, ba ) / std::max( 1e-12, DotP( ba, ba ) ), 0.0, 1.0 );
	return Len( pa - ba * h ) - r;
}

/// A closed polygon's signed distance (negative inside), any winding.
double Polygon( P p, const std::vector< P >& v )
{
	double d    = DotP( p - v[ 0 ], p - v[ 0 ] );
	double sign = 1.0;
	for( size_t i = 0, j = v.size() - 1; i < v.size(); j = i, ++i )
	{
		const P e = v[ j ] - v[ i ], w = p - v[ i ];
		const P b = w - e * std::clamp( DotP( w, e ) / DotP( e, e ), 0.0, 1.0 );
		d         = std::min( d, DotP( b, b ) );
		const bool c1 = p.y >= v[ i ].y, c2 = p.y < v[ j ].y, c3 = e.x * w.y > e.y * w.x;
		if( ( c1 && c2 && c3 ) || ( !c1 && !c2 && !c3 ) )
			sign = -sign;
	}
	return sign * std::sqrt( d );
}

/// The built-in face's strokes, set as text centred on c, `height` tall.
double Text( P p, P c, double height, const char* text )
{
	//Lay out by the built-in advances (ink + bearing, as Typeface does).
	struct G
	{
		int index;
		double pen;
	};
	std::vector< G > glyphs;
	double pen = 0.0, lo = 1e9, hi = -1e9;
	for( const char* t = text; *t; ++t )
	{
		const int g = GlyphIndex( *t );
		if( g < 0 )
		{
			pen += 0.35;
			continue;
		}
		double inkLo = 1e9, inkHi = -1e9;
		for( const auto& stroke : BuiltinStrokes( g ) )
			for( const auto& q : stroke )
			{
				inkLo = std::min( inkLo, static_cast< double >( q.first ) );
				inkHi = std::max( inkHi, static_cast< double >( q.first ) );
			}
		glyphs.push_back( { g, pen } );
		lo = std::min( lo, pen + inkLo );
		hi = std::max( hi, pen + inkHi );
		pen += std::max( 0.3, inkHi + kBuiltinHalfWidth + 0.14 );
	}
	const double width = hi - lo;
	//Local H units: x from the ink's left, y from the cap's bottom.
	const double lx = ( p.x - c.x ) / height + 0.5 * width + lo;
	const double ly = ( p.y - c.y ) / height + 0.5;
	double best = 1e9;
	for( const G& g : glyphs )
		for( const auto& stroke : BuiltinStrokes( g.index ) )
			for( size_t i = 0; i + 1 < stroke.size(); ++i )
				best = std::min( best, Segment( { lx, ly }, { g.pen + stroke[ i ].first, stroke[ i ].second },
				                                { g.pen + stroke[ i + 1 ].first, stroke[ i + 1 ].second }, 0.0 ) );
	return ( best - kBuiltinHalfWidth * 1.35 ) * height;
}

//---------------------------------------------------------------------------
// The canvas: premultiplied linear RGBA, painted over with coverage from a
// distance and a one-pixel ramp.
//---------------------------------------------------------------------------
struct Tile
{
	int size;
	double pixel;///< tile units per pixel
	std::vector< double > rgba;

	explicit Tile( int n ) : size( n ), pixel( 2.0 / n ), rgba( static_cast< size_t >( n ) * n * 4, 0.0 ) {}

	void Paint( const std::function< double( P ) >& distance, const std::function< Rgb( P ) >& colour, double alpha = 1.0 )
	{
		for( int y = 0; y < size; ++y )
			for( int x = 0; x < size; ++x )
			{
				const P p          = { -1.0 + ( x + 0.5 ) * pixel, -1.0 + ( y + 0.5 ) * pixel };
				const double d     = distance( p );
				const double cover = std::clamp( 0.5 - d / pixel, 0.0, 1.0 ) * alpha;
				if( cover <= 0.0 )
					continue;
				const Rgb c = colour( p );
				double* o   = &rgba[ ( static_cast< size_t >( y ) * size + x ) * 4 ];
				o[ 0 ]      = c.r * cover + o[ 0 ] * ( 1.0 - cover );
				o[ 1 ]      = c.g * cover + o[ 1 ] * ( 1.0 - cover );
				o[ 2 ]      = c.b * cover + o[ 2 ] * ( 1.0 - cover );
				o[ 3 ]      = cover + o[ 3 ] * ( 1.0 - cover );
			}
	}
	/// The shape, outlined: ink dilated by `outline` under the fill.
	void Shape( const std::function< double( P ) >& distance, const std::function< Rgb( P ) >& colour, double outline = 0.045,
	            Rgb ink = Hex( 0x1a1008 ) )
	{
		Paint( [ & ]( P p ) { return distance( p ) - outline; }, [ & ]( P ) { return ink; } );
		Paint( distance, colour );
	}
};

/// A ball's lighting: centre c, radius r, base colour.
std::function< Rgb( P ) > Sphere( P c, double r, Rgb base, double shine = 1.0 )
{
	return [ = ]( P p ) {
		const double nx = ( p.x - c.x ) / r, ny = ( p.y - c.y ) / r;
		const double nz = std::sqrt( std::max( 0.0, 1.0 - nx * nx - ny * ny ) );
		const double lx = -0.45, ly = 0.55, lz = 0.70;
		const double diffuse = std::max( 0.0, nx * lx + ny * ly + nz * lz );
		//Blinn half-vector toward a viewer on +z.
		const double hx = lx, hy = ly, hz = lz + 1.0, hl = std::sqrt( hx * hx + hy * hy + hz * hz );
		const double spec    = std::pow( std::max( 0.0, ( nx * hx + ny * hy + nz * hz ) / hl ), 40.0 ) * shine;
		const double rim     = std::pow( 1.0 - nz, 3.0 ) * 0.25;
		return base * ( 0.35 + 0.8 * diffuse ) + Rgb { spec, spec, spec } * 0.9 + base * rim;
	};
}

std::function< Rgb( P ) > Flat( Rgb c )
{
	return [ = ]( P ) { return c; };
}

void Cherry( Tile& t )
{
	const Rgb red = Hex( 0xd0101c ), stem = Hex( 0x3d7a1c ), leaf = Hex( 0x4ea52a );
	//Stems: from each cherry up to a shared knot, slightly bowed.
	auto stems = [ & ]( P p ) {
		return std::min( { Segment( p, { -0.36, -0.12 }, { -0.20, 0.30 }, 0.035 ), Segment( p, { -0.20, 0.30 }, { 0.06, 0.62 }, 0.035 ),
		                   Segment( p, { 0.30, -0.20 }, { 0.22, 0.25 }, 0.035 ), Segment( p, { 0.22, 0.25 }, { 0.06, 0.62 }, 0.035 ) } );
	};
	t.Shape( stems, Flat( stem ), 0.03 );
	t.Shape( [ & ]( P p ) { return Ellipse( p, { 0.30, 0.66 }, 0.30, 0.13, 0.35 ); },
	         [ & ]( P p ) { return Mix( leaf, Hex( 0x2c6a12 ), std::clamp( 0.5 - 1.5 * ( p.y - 0.66 ), 0.0, 1.0 ) ); } );
	t.Paint( [ & ]( P p ) { return Segment( p, { 0.08, 0.62 }, { 0.52, 0.72 }, 0.012 ); }, Flat( Hex( 0x2c6a12 ) ) );
	t.Shape( [ & ]( P p ) { return Circle( p, { -0.36, -0.40 }, 0.32 ); }, Sphere( { -0.36, -0.40 }, 0.32, red ) );
	t.Shape( [ & ]( P p ) { return Circle( p, { 0.32, -0.48 }, 0.32 ); }, Sphere( { 0.32, -0.48 }, 0.32, red ) );
}

void Lemon( Tile& t )
{
	const Rgb yellow = Hex( 0xf5d81c );
	auto shape = [ & ]( P p ) {
		return std::min( { Ellipse( p, { 0.0, 0.0 }, 0.64, 0.46 ), Circle( p, { 0.70, 0.06 }, 0.10 ), Circle( p, { -0.70, -0.06 }, 0.10 ) } );
	};
	auto lit = Sphere( { 0.0, 0.0 }, 0.70, yellow, 0.6 );
	t.Shape( shape, [ & ]( P p ) {
		//Peel: a fine dimpling from an integer hash, as the shader does it.
		const int ix = static_cast< int >( std::floor( ( p.x + 1.0 ) * 40.0 ) ), iy = static_cast< int >( std::floor( ( p.y + 1.0 ) * 40.0 ) );
		const double n = dice::Unit( dice::Hash( static_cast< uint32_t >( ix ), static_cast< uint32_t >( iy ), 7u ) );
		return lit( p ) * ( 0.94 + 0.08 * n );
	} );
}

void Orange( Tile& t )
{
	const Rgb orange = Hex( 0xf07a10 );
	auto lit         = Sphere( { 0.0, -0.08 }, 0.62, orange, 0.5 );
	t.Shape( [ & ]( P p ) { return Circle( p, { 0.0, -0.08 }, 0.62 ); }, [ & ]( P p ) {
		const int ix = static_cast< int >( std::floor( ( p.x + 1.0 ) * 36.0 ) ), iy = static_cast< int >( std::floor( ( p.y + 1.0 ) * 36.0 ) );
		const double n = dice::Unit( dice::Hash( static_cast< uint32_t >( ix ), static_cast< uint32_t >( iy ), 11u ) );
		return lit( p ) * ( 0.92 + 0.1 * n );
	} );
	t.Shape( [ & ]( P p ) { return Ellipse( p, { 0.22, 0.58 }, 0.26, 0.10, 0.5 ); }, Flat( Hex( 0x3f8f22 ) ), 0.03 );
	t.Paint( [ & ]( P p ) { return Circle( p, { 0.0, 0.52 }, 0.05 ); }, Flat( Hex( 0x5a3a10 ) ) );
}

void Plum( Tile& t )
{
	const Rgb plum = Hex( 0x6a1f8a );
	t.Shape( [ & ]( P p ) { return Segment( p, { 0.02, 0.48 }, { 0.12, 0.78 }, 0.035 ); }, Flat( Hex( 0x5a3a10 ) ), 0.03 );
	t.Shape( [ & ]( P p ) { return Ellipse( p, { 0.0, -0.08 }, 0.52, 0.62, 0.15 ); }, Sphere( { -0.05, -0.05 }, 0.66, plum, 0.8 ) );
	//The cleft down one side.
	t.Paint( [ & ]( P p ) { return Segment( p, { 0.10, 0.45 }, { 0.22, -0.55 }, 0.012 ); }, Flat( Hex( 0x3a0c50 ) ), 0.7 );
}

void Bell( Tile& t )
{
	const Rgb gold = Hex( 0xf2b51c ), dark = Hex( 0x9a6a08 );
	auto body = [ & ]( P p ) {
		const std::vector< P > flare = { { -0.40, 0.20 }, { 0.40, 0.20 }, { 0.50, -0.20 }, { 0.70, -0.46 }, { -0.70, -0.46 }, { -0.50, -0.20 } };
		return std::min( { Circle( p, { 0.0, 0.18 }, 0.42 ), Polygon( p, flare ), Box( p, { 0.0, -0.50 }, 0.74, 0.08, 0.07 ) } );
	};
	t.Shape( [ & ]( P p ) { return Circle( p, { 0.0, -0.66 }, 0.13 ); }, Sphere( { 0.0, -0.66 }, 0.13, gold ) );
	t.Shape( [ & ]( P p ) { return Box( p, { 0.0, 0.68 }, 0.08, 0.10, 0.04 ); }, Flat( dark ) );
	t.Shape( body, [ & ]( P p ) {
		//Turned metal: bright bands running down the bell.
		const double band = 0.5 + 0.5 * std::cos( p.x * 5.5 + 0.8 );
		return Mix( dark, gold, 0.35 + 0.65 * band ) * ( 0.9 + 0.25 * std::clamp( p.y, -1.0, 1.0 ) );
	} );
	t.Paint( [ & ]( P p ) { return Ellipse( p, { -0.20, 0.30 }, 0.06, 0.20, -0.3 ); }, Flat( Hex( 0xfff2c0 ) ), 0.8 );
}

void Melon( Tile& t )
{
	const Rgb green = Hex( 0x3c9a2a ), stripe = Hex( 0x1d5a14 );
	auto lit = Sphere( { 0.0, -0.04 }, 0.64, green, 0.5 );
	t.Shape( [ & ]( P p ) { return Ellipse( p, { 0.0, -0.04 }, 0.68, 0.58 ); }, [ & ]( P p ) {
		//Stripes run pole to pole: curved lines in x, bowed by the sphere.
		const double u     = p.x / std::max( 0.2, std::sqrt( std::max( 0.0, 1.0 - ( p.y / 0.62 ) * ( p.y / 0.62 ) ) ) );
		const double s     = 0.5 + 0.5 * std::cos( u * 9.0 );
		const double shade = std::clamp( ( s - 0.55 ) * 4.0, 0.0, 1.0 );
		return Mix( lit( p ), lit( p ) * 0.0 + stripe * 1.2, shade * 0.75 );
	} );
}

void Bars( Tile& t, int count )
{
	const double h     = count == 1 ? 0.36 : count == 2 ? 0.27 : 0.21;
	const double pitch = count == 1 ? 0.0 : count == 2 ? 0.62 : 0.50;
	for( int i = 0; i < count; ++i )
	{
		const double y = ( i - 0.5 * ( count - 1 ) ) * pitch;
		t.Shape( [ & ]( P p ) { return Box( p, { 0.0, y }, 0.86, h, 0.08 ); }, Flat( Hex( 0x0c0c0c ) ), 0.05, Hex( 0xf0f0f0 ) );
		t.Paint( [ & ]( P p ) { return Text( p, { 0.0, y }, h * 1.25, "BAR" ); }, Flat( Hex( 0xf8f8f8 ) ) );
	}
}

void Seven( Tile& t )
{
	const Rgb red = Hex( 0xe0141c ), gold = Hex( 0xffd23a );
	const std::vector< P > seven = { { -0.62, 0.80 }, { 0.66, 0.80 }, { 0.66, 0.54 }, { -0.04, -0.82 }, { -0.42, -0.82 },
		                             { 0.24, 0.48 },  { -0.36, 0.48 }, { -0.36, 0.38 }, { -0.62, 0.38 } };
	t.Paint( [ & ]( P p ) { return Polygon( p, seven ) - 0.11; }, Flat( Hex( 0x2a0a00 ) ) );
	t.Paint( [ & ]( P p ) { return Polygon( p, seven ) - 0.075; }, [ & ]( P p ) { return Mix( gold, Hex( 0xb07a08 ), std::clamp( 0.5 - 0.6 * p.y, 0.0, 1.0 ) ); } );
	t.Paint( [ & ]( P p ) { return Polygon( p, seven ); }, [ & ]( P p ) {
		return red * ( 0.75 + 0.35 * std::clamp( 0.5 + 0.6 * p.y - 0.3 * p.x, 0.0, 1.0 ) );
	} );
	t.Paint( [ & ]( P p ) { return Segment( p, { -0.48, 0.69 }, { 0.48, 0.69 }, 0.025 ); }, Flat( Hex( 0xffb0a0 ) ), 0.7 );
}

void Diamond( Tile& t )
{
	const std::vector< P > gem = { { 0.0, -0.80 }, { 0.78, 0.18 }, { 0.50, 0.52 }, { -0.50, 0.52 }, { -0.78, 0.18 } };
	const Rgb light = Hex( 0x9fdcff ), mid = Hex( 0x2f8fe0 ), deep = Hex( 0x0b3f9a );
	t.Shape( [ & ]( P p ) { return Polygon( p, gem ); }, [ & ]( P p ) {
		//Facets: the crown (above the girdle at y = 0.18) and the pavilion,
		//split into wedges that take the light differently.
		if( p.y > 0.18 )
		{
			if( std::fabs( p.x ) < 0.28 )
				return light;
			return p.x < 0.0 ? Mix( light, mid, 0.4 ) : mid;
		}
		const double a = std::atan2( p.x, p.y + 0.80 );
		const int wedge = static_cast< int >( std::floor( ( a + 1.0 ) * 3.0 ) );
		return wedge % 2 == 0 ? Mix( mid, deep, 0.3 ) : Mix( mid, light, 0.35 );
	}, 0.05 );
	t.Paint( [ & ]( P p ) { return Segment( p, { -0.78, 0.18 }, { 0.78, 0.18 }, 0.012 ); }, Flat( Hex( 0x082a66 ) ) );
	t.Paint( [ & ]( P p ) { return Circle( p, { -0.30, 0.36 }, 0.06 ); }, Flat( Hex( 0xffffff ) ) );
}

void ClipFrame( Tile& t )
{
	//What the source shows for the Clip symbol (the effect draws its input
	//there): a framed card, so the strip never shows a hole.
	t.Shape( [ & ]( P p ) { return Box( p, { 0.0, 0.0 }, 0.78, 0.62, 0.10 ); }, Flat( Hex( 0x202838 ) ), 0.05, Hex( 0xffd23a ) );
}
} // namespace

SymbolAtlas PaintSymbols()
{
	SymbolAtlas atlas;
	atlas.rgba.assign( static_cast< size_t >( SymbolAtlas::kWidth ) * SymbolAtlas::kHeight * 4, 0 );
	for( int s = 0; s < slots::kSymbolCount; ++s )
	{
		Tile t( SymbolAtlas::kTile );
		switch( s )
		{
		case slots::Cherry: jackpot::Cherry( t ); break;
		case slots::Lemon: jackpot::Lemon( t ); break;
		case slots::Orange: jackpot::Orange( t ); break;
		case slots::Plum: jackpot::Plum( t ); break;
		case slots::Bell: jackpot::Bell( t ); break;
		case slots::Melon: jackpot::Melon( t ); break;
		case slots::Bar: Bars( t, 1 ); break;
		case slots::DoubleBar: Bars( t, 2 ); break;
		case slots::TripleBar: Bars( t, 3 ); break;
		case slots::Seven: jackpot::Seven( t ); break;
		case slots::Diamond: jackpot::Diamond( t ); break;
		case slots::ClipSymbol: ClipFrame( t ); break;
		default: break;
		}
		const int tx = ( s % SymbolAtlas::kColumns ) * SymbolAtlas::kTile;
		const int ty = ( s / SymbolAtlas::kColumns ) * SymbolAtlas::kTile;
		for( int y = 0; y < SymbolAtlas::kTile; ++y )
			for( int x = 0; x < SymbolAtlas::kTile; ++x )
			{
				const double* in = &t.rgba[ ( static_cast< size_t >( y ) * SymbolAtlas::kTile + x ) * 4 ];
				uint8_t* out     = &atlas.rgba[ ( static_cast< size_t >( ty + y ) * SymbolAtlas::kWidth + static_cast< size_t >( tx + x ) ) * 4 ];
				//Linear premultiplied in the tile; the atlas stores display
				//values (an sRGB-ish 1/2.2), premultiplied, as RGBA8.
				const double a = std::clamp( in[ 3 ], 0.0, 1.0 );
				for( int c = 0; c < 3; ++c )
				{
					const double straight = a > 0.0 ? std::clamp( in[ c ] / a, 0.0, 1.0 ) : 0.0;
					out[ c ] = static_cast< uint8_t >( std::lround( std::pow( straight, 1.0 / 2.2 ) * a * 255.0 ) );
				}
				out[ 3 ] = static_cast< uint8_t >( std::lround( a * 255.0 ) );
			}
	}
	return atlas;
}

void SymbolAtlas::At( int s, double u, double v, double out[ 4 ] ) const
{
	s            = std::clamp( s, 0, kColumns * kRows - 1 );
	const int tx = ( s % kColumns ) * kTile, ty = ( s / kColumns ) * kTile;
	const int x  = std::clamp( static_cast< int >( u * kTile ), 0, kTile - 1 );
	const int y  = std::clamp( static_cast< int >( v * kTile ), 0, kTile - 1 );
	const uint8_t* p = &rgba[ ( static_cast< size_t >( ty + y ) * kWidth + static_cast< size_t >( tx + x ) ) * 4 ];
	for( int c = 0; c < 4; ++c )
		out[ c ] = p[ c ] / 255.0;
}

} // namespace jackpot
