// geodump (polyhedral's, unchanged): every table Geometry.cpp builds, printed at 1e-6, so that builds of
// it can be diffed. tools/verify.sh builds it three ways (arm64 with fused
// multiply-adds, arm64 without, x86_64) and requires the same tables from all
// three: the face loops, the rotation group's order, each number's slot and up
// vector, and every die's labels. Until v0.1.0 they differed, and the universal
// bundle's two halves printed a d6's or d10's numbers a different way up.
#include "Geometry.h"
#include <cmath>
#include <cstdio>
using namespace dice;
using namespace dice::geo;
static double r6( double v ) { double x = std::round( v * 1e6 ) / 1e6; return x == 0.0 ? 0.0 : x; }
int main()
{
	for( int sh = 0; sh < static_cast< int >( Shape::Count ); ++sh )
	{
		const Solid& s = GetSolid( static_cast< Shape >( sh ) );
		std::printf( "shape %d: %zu faces, group %zu\n", sh, s.faces.size(), s.group.size() );
		for( size_t f = 0; f < s.faces.size(); ++f )
		{
			std::printf( "  face %zu loop", f );
			for( int v : s.faces[ f ].loop ) std::printf( " %d", v );
			std::printf( "\n" );
		}
		for( size_t g = 0; g < s.group.size(); ++g )
		{
			std::printf( "  g%zu", g );
			for( const V3& p : s.vertices )
			{
				const V3 q = s.group[ g ] * p;
				int best = -1; double bd = 1e30;
				for( size_t o = 0; o < s.vertices.size(); ++o )
				{
					const V3 d = q - s.vertices[ o ];
					const double l = std::sqrt( d.x * d.x + d.y * d.y + d.z * d.z );
					if( l < bd ) { bd = l; best = static_cast< int >( o ); }
				}
				std::printf( " %d", best );
			}
			std::printf( "\n" );
		}
		for( const Slot& sl : GetSlots( static_cast< Shape >( sh ) ) )
			std::printf( "  slot face %d item %d up %.6f %.6f %.6f\n", sl.face, sl.item, r6( sl.up.x ), r6( sl.up.y ), r6( sl.up.z ) );
	}
	for( int d = 0; d < static_cast< int >( DieType::Count ); ++d )
		for( int part = 0; part < BodiesPerDie( static_cast< DieType >( d ) ); ++part )
		{
			const Labelling& l = GetLabelling( static_cast< DieType >( d ), part );
			std::printf( "die %d part %d:", d, part );
			for( size_t i = 0; i < l.text.size(); ++i ) std::printf( " %s%s", l.text[ i ].c_str(), l.marked[ i ] ? "_" : "" );
			std::printf( "\n" );
		}
}
