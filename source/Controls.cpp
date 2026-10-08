#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace jackpot
{
namespace
{
float Clamp01( float v )
{
	return std::clamp( v, 0.0f, 1.0f );
}

double Geometric( float v, double lo, double hi )
{
	return lo * std::pow( hi / lo, static_cast< double >( Clamp01( v ) ) );
}

float InverseGeometric( double x, double lo, double hi )
{
	return static_cast< float >( std::clamp( std::log( x / lo ) / std::log( hi / lo ), 0.0, 1.0 ) );
}
} // namespace

double SpinTimeFromParam( float v )
{
	return Geometric( v, 1.0, 20.0 );
}

float ParamFromSpinTime( double seconds )
{
	return InverseGeometric( seconds, 1.0, 20.0 );
}

double IntervalFromParam( float v )
{
	return Geometric( v, 2.0, 120.0 );
}

float ParamFromInterval( double seconds )
{
	return InverseGeometric( seconds, 2.0, 120.0 );
}

double BlurFromParam( float v )
{
	return Clamp01( v );
}

double ReelDampingFromParam( float v )
{
	return 1.0 - 0.82 * Clamp01( v );
}

double RotorSpeedFromParam( float v )
{
	return 4.0 * Clamp01( v );
}

double ClapperFromParam( float v )
{
	return Geometric( v, 2.0, 40.0 );
}

double AirFromParam( float v )
{
	return 0.5 + 3.5 * Clamp01( v );
}

double AmountFromParam( float v )
{
	const double x = Clamp01( v );
	return 400.0 * x * x;
}

double PieceSizeFromParam( float v )
{
	return Geometric( v, 0.02, 0.2 );
}

float ParamFromPieceSize( double fraction )
{
	return InverseGeometric( fraction, 0.02, 0.2 );
}

double LightAngleFromParam( float v )
{
	return -180.0 + 360.0 * Clamp01( v );
}

double TiltFromParam( float v )
{
	return 20.0 + 70.0 * Clamp01( v );
}

double ZoomFromParam( float v )
{
	return Geometric( v, 0.5, 2.5 );
}

int OptionIndex( float value, int count )
{
	if( count <= 1 )
		return 0;
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

} // namespace jackpot
