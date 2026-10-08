#include "Roulette.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace jackpot::roulette
{
namespace
{
constexpr double kDt           = 1.0 / 4000.0;
constexpr double kMaxSim       = 45.0;
constexpr double kDrag         = 0.012;        ///< 1/m: (1/2) rho Cd A / m for a 19 mm ball of 6 g
constexpr double kRollResist   = 0.006;        ///< on the wooden stator and track
constexpr double kLipFriction  = 0.010;        ///< against the lip: the ball rubs as it rolls
constexpr double kRotorGrip    = 0.12;         ///< on the turning rotor: the ball is carried
constexpr double kSurfaceBounce = 0.35;
constexpr double kFretBounce   = 0.50;
constexpr double kFretGrip     = 0.20;
constexpr double kDiamondBounce = 0.55;
constexpr double kRestSpeed    = 0.012;        ///< m/s relative to the pocket
constexpr double kRestFor      = 0.30;         ///< s
constexpr double kRotorTau     = 40.0;         ///< s: the rotor's spin-down, on its bearing
constexpr double kHandSpin     = 1.2;          ///< s: the croupier's spin before the release
constexpr double kFretHalf     = 0.001;        ///< m: half a fret's thickness
constexpr double kDiamondHalf  = 0.010, kDiamondRadius = 0.006;

/// The wheels as they are always written: clockwise from the zero, seen from
/// above.
const int kEuropeanClockwise[] = { 0, 32, 15, 19, 4, 21, 2, 25, 17, 34, 6, 27, 13, 36, 11, 30, 8, 23, 10,
	                               5, 24, 16, 33, 1, 20, 14, 31, 9, 22, 18, 29, 7, 28, 12, 35, 3, 26 };
const int kAmericanClockwise[] = { 0, 28, 9, 26, 30, 11, 7, 20, 32, 17, 5, 22, 34, 15, 3, 24, 36, 13, 1,
	                               37, 27, 10, 25, 29, 12, 8, 19, 31, 18, 6, 21, 33, 16, 4, 23, 35, 14, 2 };

/// Pocket order here runs ANTICLOCKWISE (angle increases from +x toward +y,
/// z up), so the lists are read backwards from the zero. Taken as written, the
/// wheel would be drawn in mirror image.
template< size_t N >
std::vector< int > Anticlockwise( const int ( &clockwise )[ N ] )
{
	std::vector< int > out = { clockwise[ 0 ] };
	for( size_t i = N - 1; i >= 1; --i )
		out.push_back( clockwise[ i ] );
	return out;
}
const std::vector< int > kEuropean = Anticlockwise( kEuropeanClockwise );
const std::vector< int > kAmerican = Anticlockwise( kAmericanClockwise );

/// The profile, (r, z), inward to outward. Segments 0..4 turn with the rotor.
struct Vertex
{
	double r, z;
};
const Vertex kProfile[] = {
	{ 0.120, 0.0249 },                          //the rotor's cone, toward the turret
	{ kPocketIn - kStep, 0.0 },
	{ kPocketIn, -kPocketDepth },
	{ kPocketOut - kStep, -kPocketDepth },
	{ kPocketOut, 0.0 },
	{ kRotorOut, kApronSlope * ( kRotorOut - kPocketOut ) },
	{ kTrackIn, kApronSlope * ( kRotorOut - kPocketOut ) + kStatorSlope * ( kTrackIn - kRotorOut ) },
	{ kLip, kApronSlope * ( kRotorOut - kPocketOut ) + kStatorSlope * ( kTrackIn - kRotorOut ) + kTrackSlope * ( kLip - kTrackIn ) },
	{ kLip, 0.15 },                             //the lip: a vertical wall
};
constexpr int kVertices    = 9;
constexpr int kRotorSegments = 5;
constexpr int kLipSegment  = 7;

double Wrap( double a )
{
	a = std::fmod( a, 2.0 * kPi );
	return a < 0.0 ? a + 2.0 * kPi : a;
}

double Frac( double x )
{
	return x - std::floor( x );
}

/// The rotor's turning since release, rad.
double Turned( double rotorSpeed, double t )
{
	return rotorSpeed * kRotorTau * ( 1.0 - std::exp( -t / kRotorTau ) );
}

double RotorRate( double rotorSpeed, double t )
{
	return rotorSpeed * std::exp( -t / kRotorTau );
}

struct Contact
{
	bool hit = false;
	V3 normal;
	double depth = 0.0;
	int segment  = -1;
};

/// The ball against the profile: the closest point of the polyline in the
/// ball's meridian plane, which for a surface of revolution IS the closest
/// point of the surface.
///
/// Which SIDE the ball is on comes from the profile as a height field (below
/// it, or beyond the lip, is solid), not from the side of the nearest
/// segment's line: past the end of a steep segment -- a pocket's step -- that
/// line runs through open air, and reading the side from it once threw the
/// ball four centimetres inward on its first step.
Contact Profile( V3 p )
{
	const double r = std::sqrt( p.x * p.x + p.y * p.y );
	Contact best;
	double nearest = 1e9;
	double cr = 0.0, cz = 0.0;
	for( int i = 0; i + 1 < kVertices; ++i )
	{
		const Vertex a = kProfile[ i ], b = kProfile[ i + 1 ];
		const double er = b.r - a.r, ez = b.z - a.z;
		const double t  = std::clamp( ( ( r - a.r ) * er + ( p.z - a.z ) * ez ) / ( er * er + ez * ez ), 0.0, 1.0 );
		const double qr = a.r + t * er, qz = a.z + t * ez;
		const double d  = std::sqrt( ( r - qr ) * ( r - qr ) + ( p.z - qz ) * ( p.z - qz ) );
		if( d < nearest )
		{
			nearest      = d;
			cr           = qr;
			cz           = qz;
			best.segment = i;
		}
	}
	const bool solid = r >= kLip || p.z < Height( r );
	double nr = r - cr, nz = p.z - cz;
	if( nearest > 1e-12 )
	{
		nr /= nearest;
		nz /= nearest;
		if( solid )
		{
			nr = -nr;
			nz = -nz;
		}
	}
	else
	{
		//On the surface: the segment's own normal, up and inward.
		const Vertex a = kProfile[ best.segment ], b = kProfile[ best.segment + 1 ];
		const double len = std::hypot( b.r - a.r, b.z - a.z );
		nr = -( b.z - a.z ) / len;
		nz = ( b.r - a.r ) / len;
	}
	const double inv = r > 1e-9 ? 1.0 / r : 0.0;
	best.normal      = { nr * p.x * inv, nr * p.y * inv, nz };
	best.depth       = kBallRadius - ( solid ? -nearest : nearest );
	best.hit         = best.depth > 0.0;
	return best;
}

V3 SurfaceVelocity( V3 p, double omega )
{
	return { -omega * p.y, omega * p.x, 0.0 };
}

/// Resolve one contact: push out, bounce or rest, and Coulomb friction from
/// the normal impulse. Returns the normal impulse (per unit mass).
double Resolve( V3& p, V3& v, V3 n, double depth, V3 surfaceVelocity, double bounce, double grip )
{
	p += n * depth;
	V3 rel          = v - surfaceVelocity;
	const double vn = Dot( rel, n );
	if( vn >= 0.0 )
		return 0.0;
	const double target = -vn > 0.08 ? -bounce * vn : 0.0;
	const double jn     = target - vn;
	v += n * jn;
	rel             = v - surfaceVelocity;
	V3 tangent      = rel - n * Dot( rel, n );
	const double vt = Length( tangent );
	if( vt > 1e-12 )
		v -= tangent * ( std::min( vt, grip * jn ) / vt );
	return jn;
}
} // namespace

const std::vector< int >& Numbers( RouletteWheel wheel )
{
	return wheel == RouletteWheel::American ? kAmerican : kEuropean;
}

int Colour( int number )
{
	static const int red[] = { 1, 3, 5, 7, 9, 12, 14, 16, 18, 19, 21, 23, 25, 27, 30, 32, 34, 36 };
	if( number == 0 || number == 37 )
		return 0;
	for( int n : red )
		if( n == number )
			return 1;
	return 2;
}

std::string Name( int number )
{
	const std::string n = number == 37 ? "00" : std::to_string( number );
	const int c         = Colour( number );
	return n + ( c == 0 ? " GREEN" : c == 1 ? " RED" : " BLACK" );
}

double Height( double r )
{
	if( r <= kProfile[ 0 ].r )
		return kProfile[ 0 ].z;
	for( int i = 0; i + 1 < kVertices; ++i )
		if( r <= kProfile[ i + 1 ].r && kProfile[ i + 1 ].r > kProfile[ i ].r )
			return kProfile[ i ].z + ( kProfile[ i + 1 ].z - kProfile[ i ].z ) * ( r - kProfile[ i ].r ) / ( kProfile[ i + 1 ].r - kProfile[ i ].r );
	return kProfile[ kVertices - 2 ].z;
}

double Slope( double r )
{
	for( int i = 0; i + 1 < kVertices; ++i )
		if( r <= kProfile[ i + 1 ].r && kProfile[ i + 1 ].r > kProfile[ i ].r )
			return ( kProfile[ i + 1 ].z - kProfile[ i ].z ) / ( kProfile[ i + 1 ].r - kProfile[ i ].r );
	return 0.0;
}

bool OnRotor( double r )
{
	return r < kRotorOut;
}

double RotorPockets( const Launch& launch, double seconds )
{
	const double pocket = 2.0 * kPi / launch.pockets;
	return launch.fretTurns + launch.fretPhase + Turned( launch.rotorSpeed, seconds ) / pocket;
}

Simulation Simulate( const Launch& L, const std::atomic< bool >* cancel )
{
	Simulation sim;
	const int N         = std::max( 1, L.pockets );
	const double pocket = 2.0 * kPi / N;
	const double fretTop = -kPocketDepth + kFretHeight;
	const double zDiamond = Height( kDiamondR ) + 0.003;

	//On the track, against the lip, going clockwise.
	const double r0 = kLip - kBallRadius - 1e-5;
	V3 p = { r0 * std::cos( L.ballAngle ), r0 * std::sin( L.ballAngle ), Height( r0 ) + kBallRadius * 1.03 };
	V3 v = { L.ballSpeed * std::sin( L.ballAngle ), -L.ballSpeed * std::cos( L.ballAngle ), 0.0 };

	double still = 0.0, t = 0.0, nextKey = 0.0;
	const long maxSteps = static_cast< long >( kMaxSim / kDt );
	for( long step = 0; step < maxSteps; ++step )
	{
		if( cancel != nullptr && ( step & 1023 ) == 0 && cancel->load() )
			return {};
		while( t >= nextKey - 1e-12 )
		{
			sim.keys.push_back( p );
			nextKey += 1.0 / Simulation::kKeyRate;
		}

		const double omega = RotorRate( L.rotorSpeed, t );
		//The frets' phase from the FRACTIONAL part only: whole pockets of rotor
		//angle are invisible to the ball by construction.
		const double fretFrac = Frac( L.fretPhase + Turned( L.rotorSpeed, t ) / pocket + L.fretNudge );

		//Gravity and air.
		v.z -= kGravity * kDt;
		const double speed = Length( v );
		v -= v * ( kDrag * speed * kDt );
		p += v * kDt;

		bool onLip = false, inPocket = false;
		double lipImpulse = 0.0;
		for( int pass = 0; pass < 2; ++pass )
		{
			const Contact c = Profile( p );
			if( c.hit )
			{
				const bool rotor = c.segment < kRotorSegments;
				const V3 sv      = rotor ? SurfaceVelocity( p, omega ) : V3 {};
				const double grip = rotor ? kRotorGrip : c.segment == kLipSegment ? kLipFriction : kRollResist;
				const double jn   = Resolve( p, v, c.normal, c.depth, sv, kSurfaceBounce, grip );
				if( c.segment == kLipSegment )
				{
					onLip = true;
					lipImpulse += jn;
				}
				if( c.segment >= 1 && c.segment <= 3 )
					inPocket = true;
			}
		}

		const double r   = std::sqrt( p.x * p.x + p.y * p.y );
		const double phi = std::atan2( p.y, p.x );

		//The frets: thin radial walls between the steps, up to fretTop.
		if( r > kPocketIn - kStep - kBallRadius && r < kPocketOut + kBallRadius && p.z - kBallRadius < fretTop )
		{
			const double psi  = Wrap( phi ) / pocket - fretFrac;
			const double fret = std::floor( psi ) + 0.5;
			const double ang  = ( psi - fret ) * pocket;
			const double dt   = r * std::sin( ang );             //across the fret
			const double along = r * std::cos( ang );            //along it, radially
			//The frets run between the steps' feet: their outer ends stop a
			//step short of the apron, as on a real rotor. (Run out to the apron's
			//edge, a fret's end stood 4 mm proud of it and the ball, carried
			//round with the rotor, could lean on one for ever.)
			const double cr   = std::clamp( along, kPocketIn - kStep, kPocketOut - kStep );
			const double cz   = std::clamp( p.z, -kPocketDepth, fretTop );
			const double across = std::fabs( dt ) - kFretHalf;
			const double d    = std::sqrt( std::max( across, 0.0 ) * std::max( across, 0.0 ) + ( along - cr ) * ( along - cr ) + ( p.z - cz ) * ( p.z - cz ) );
			if( d < kBallRadius && across > -kBallRadius )
			{
				//Back into world axes: across the fret is the tangential direction
				//at the fret's own angle.
				const double fa = ( fret + fretFrac ) * pocket;
				const V3 tan    = { -std::sin( fa ), std::cos( fa ), 0.0 };
				const V3 rad    = { std::cos( fa ), std::sin( fa ), 0.0 };
				V3 n            = tan * ( ( dt >= 0.0 ? 1.0 : -1.0 ) * std::max( across, 0.0 ) ) + rad * ( along - cr ) + V3 { 0, 0, p.z - cz };
				const double nl = Length( n );
				n               = nl > 1e-12 ? n * ( 1.0 / nl ) : tan * ( dt >= 0.0 ? 1.0 : -1.0 );
				const double jn = Resolve( p, v, n, kBallRadius - d, SurfaceVelocity( p, omega ), kFretBounce, kFretGrip );
				if( jn > 0.05 )
					++sim.fretHits;
			}
		}

		//The diamonds: capsules on the stator, alternately along and across.
		if( L.deflectors && std::fabs( r - kDiamondR ) < 0.03 && p.z < zDiamond + 0.03 )
		{
			for( int i = 0; i < kDiamonds; ++i )
			{
				const double a = ( i + 0.5 ) * 2.0 * kPi / kDiamonds;
				const V3 c     = { kDiamondR * std::cos( a ), kDiamondR * std::sin( a ), zDiamond };
				const V3 axis  = ( i % 2 == 0 ) ? V3 { std::cos( a ), std::sin( a ), 0.0 } : V3 { -std::sin( a ), std::cos( a ), 0.0 };
				const V3 rel   = p - c;
				const double s = std::clamp( Dot( rel, axis ), -kDiamondHalf, kDiamondHalf );
				const V3 off   = rel - axis * s;
				const double d = Length( off );
				if( d < kBallRadius + kDiamondRadius && d > 1e-9 )
				{
					const double jn = Resolve( p, v, off * ( 1.0 / d ), kBallRadius + kDiamondRadius - d, {}, kDiamondBounce, kRollResist );
					if( jn > 0.05 )
						++sim.diamondHits;
				}
			}
		}

		if( onLip && lipImpulse > 0.0 )
		{
			sim.departSpeed  = Length( V3 { v.x, v.y, 0.0 } );
			sim.departRadius = r;
			sim.departTime   = t;
		}

		t += kDt;

		//At rest in a pocket, turning with the rotor.
		const V3 rel = v - SurfaceVelocity( p, omega );
		if( inPocket && r > kPocketIn && r < kPocketOut && Length( rel ) < kRestSpeed )
			still += kDt;
		else
			still = 0.0;
		if( still >= kRestFor )
		{
			sim.settled = true;
			sim.natural = t - kRestFor;
			//The pocket in the ROTOR's frame: counted from where the rotor's own
			//pocket 0 is now, whole turns and all. (The frets' fractional phase
			//alone, which is all the physics uses, loses the whole pockets the
			//rotor turned during the spin, and the ring was turned that many
			//pockets wrong: a ball in 35's pocket reported 22.)
			const double rotorPockets = RotorPockets( L, t );
			const double psi = Wrap( phi ) / pocket - rotorPockets;
			sim.pocket       = static_cast< int >( ( ( static_cast< long >( std::floor( psi + 0.5 ) ) % N ) + N ) % N );
			const double rotorAngle = rotorPockets * pocket;
			sim.restAngleInRotor = phi - rotorAngle;
			sim.restRadius   = r;
			sim.restHeight   = p.z;
			//Trim the keys to the rest: the ball rides the pocket from here.
			const size_t keep = static_cast< size_t >( std::ceil( sim.natural * Simulation::kKeyRate ) ) + 1;
			if( sim.keys.size() > keep )
				sim.keys.resize( keep );
			return sim;
		}
	}
	sim.natural = t;
	return sim;
}

//---------------------------------------------------------------------------
// The plan.
//---------------------------------------------------------------------------
namespace
{
/// The croupier's spin: a cubic from (a0, w0) to (a1, w1) over T.
double Hermite( double a0, double w0, double a1, double w1, double T, double t, double* rate = nullptr )
{
	const double s = std::clamp( t / T, 0.0, 1.0 );
	const double h00 = 2 * s * s * s - 3 * s * s + 1, h10 = s * s * s - 2 * s * s + s;
	const double h01 = -2 * s * s * s + 3 * s * s, h11 = s * s * s - s * s;
	if( rate != nullptr )
	{
		const double d00 = 6 * s * s - 6 * s, d10 = 3 * s * s - 4 * s + 1, d01 = -6 * s * s + 6 * s, d11 = 3 * s * s - 2 * s;
		*rate = ( d00 * a0 + d10 * T * w0 + d01 * a1 + d11 * T * w1 ) / T;
	}
	return h00 * a0 + h10 * T * w0 + h01 * a1 + h11 * T * w1;
}

int WantedIndex( const Request& r, int N, const std::vector< int >& numbers )
{
	int bet = r.bet;
	if( r.wheel == RouletteWheel::European && bet == 37 )
		bet = 0;
	bet = std::clamp( bet, 0, r.wheel == RouletteWheel::American ? 37 : 36 );
	int betIndex = 0;
	for( int i = 0; i < N; ++i )
		if( numbers[ static_cast< size_t >( i ) ] == bet )
			betIndex = i;
	Stream s( Hash( r.seed, r.play, 0x4011u ) );
	switch( r.result )
	{
	case Result::Random: return s.Below( N );
	case Result::NearMiss: return ( betIndex + ( s.Below( 2 ) == 0 ? 1 : N - 1 ) ) % N;
	case Result::Lose: return ( betIndex + 1 + s.Below( N - 1 ) ) % N;
	default: return betIndex;
	}
}
} // namespace

Plan MakePlan( const Request& r, const std::atomic< bool >* cancel )
{
	const auto start          = std::chrono::steady_clock::now();
	const std::vector< int >& numbers = Numbers( r.wheel );
	Plan plan;
	plan.request = r;
	plan.pockets = static_cast< int >( numbers.size() );
	plan.wanted  = WantedIndex( r, plan.pockets, numbers );
	plan.spinUp  = kHandSpin;

	Stream s( Hash( r.seed, r.play, 0x2011u ) );
	Launch launch;
	launch.pockets    = plan.pockets;
	launch.rotorSpeed = r.rotorSpeed;
	launch.deflectors = r.deflectors;
	launch.ballAngle  = s.Range( 0.0, 2.0 * kPi );
	launch.fretPhase  = s.Uniform();
	const double wantBall = std::max( 1.0, r.duration - kHandSpin );

	//The ball's speed for a wanted time on the wheel: a first guess from the
	//fitted curve (AGENTS.md), then secant steps on log time.
	double speed = std::clamp( 0.9 + 0.22 * wantBall, 1.0, 5.5 );
	Simulation best;
	double bestError = 1e30, bestSpeed = speed;
	double lastSpeed = -1.0, lastTime = -1.0;
	for( int trial = 0; trial < 6; ++trial )
	{
		++plan.trials;
		launch.ballSpeed       = speed;
		const Simulation sim   = Simulate( launch, cancel );
		if( cancel != nullptr && cancel->load() )
			return Plan {};
		if( sim.settled )
		{
			const double error = std::fabs( std::log( sim.natural / wantBall ) );
			if( error < bestError )
			{
				bestError = error;
				best      = sim;
				bestSpeed = speed;
			}
			if( error < 0.06 )
				break;
			double next = speed * std::pow( wantBall / sim.natural, 0.7 );
			if( lastSpeed > 0.0 && std::fabs( sim.natural - lastTime ) > 1e-3 )
				next = speed + ( wantBall - sim.natural ) * ( speed - lastSpeed ) / ( sim.natural - lastTime );
			lastSpeed = speed;
			lastTime  = sim.natural;
			speed     = std::clamp( next, 0.8, 6.0 );
		}
		else
		{
			//Never came to rest in time: a different release, slower.
			launch.ballAngle = s.Range( 0.0, 2.0 * kPi );
			speed            = std::max( 0.8, speed * 0.8 );
		}
	}
	launch.ballSpeed = bestSpeed;
	plan.launch      = launch;
	plan.sim         = best;

	const int N         = plan.pockets;
	const double pocket = 2.0 * kPi / N;
	plan.shift          = r.noShift ? 0 : ( ( best.pocket - plan.wanted ) % N + N ) % N;
	plan.paintOffset    = plan.shift * pocket;

	//The ring at release must be the frets' physical angle plus the shift. The
	//croupier's hand takes it there from wherever it is, a turn or so on.
	plan.paintFrom      = r.paintNow;
	plan.paintSpeedFrom = r.paintSpeedNow;
	const double atRelease = launch.fretPhase * pocket + plan.paintOffset;
	const double cruise    = 0.5 * ( std::fabs( r.paintSpeedNow ) + std::max( r.rotorSpeed, 1.5 ) ) * kHandSpin;
	double delta           = Wrap( atRelease - r.paintNow );
	while( delta < cruise )
		delta += 2.0 * kPi;
	plan.paintAtRelease = r.paintNow + delta;

	const double natural = kHandSpin + ( best.settled ? best.natural : 0.0 );
	plan.playback        = MakePlayback( natural, r.duration, r.noWarp );

	const int shown        = numbers[ static_cast< size_t >( ( ( best.pocket - plan.shift ) % N + N ) % N ) ];
	int bet                = r.bet;
	if( r.wheel == RouletteWheel::European && bet == 37 )
		bet = 0;
	int betIndex = 0;
	for( int i = 0; i < N; ++i )
		if( numbers[ static_cast< size_t >( i ) ] == bet )
			betIndex = i;
	const int shownIndex     = ( ( best.pocket - plan.shift ) % N + N ) % N;
	plan.outcome.value       = shown;
	plan.outcome.win         = shown == bet;
	plan.outcome.jackpot     = plan.outcome.win;
	plan.outcome.nearMiss    = !plan.outcome.win && ( ( shownIndex + 1 ) % N == betIndex || ( betIndex + 1 ) % N == shownIndex );
	plan.outcome.payout      = plan.outcome.win ? 35 : 0;
	plan.outcome.text        = Name( shown );
	plan.ms = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count();
	return plan;
}

Plan MakeRest( const Request& r )
{
	Plan plan;
	plan.request        = r;
	plan.idle           = true;
	plan.pockets        = static_cast< int >( Numbers( r.wheel ).size() );
	plan.paintFrom      = r.paintNow;
	plan.paintSpeedFrom = r.paintSpeedNow;
	plan.spinUp         = 0.0;
	plan.playback       = MakePlayback( 0.0, 0.0 );
	plan.playback.duration = 0.0;
	return plan;
}

double Plan::PaintAngle( double seconds ) const
{
	if( idle )
	{
		//Coasting on from where it was.
		return paintFrom + paintSpeedFrom * kRotorTau * ( 1.0 - std::exp( -std::max( 0.0, seconds ) / kRotorTau ) );
	}
	const double simT = seconds >= playback.duration ? playback.natural + ( seconds - playback.duration ) : playback.SimTime( seconds );
	if( simT < spinUp )
		return Hermite( paintFrom, paintSpeedFrom, paintAtRelease, launch.rotorSpeed, spinUp, simT );
	return paintAtRelease + Turned( launch.rotorSpeed, simT - spinUp );
}

double Plan::PaintSpeed( double seconds ) const
{
	if( idle )
		return paintSpeedFrom * std::exp( -std::max( 0.0, seconds ) / kRotorTau );
	const double simT = seconds >= playback.duration ? playback.natural + ( seconds - playback.duration ) : playback.SimTime( seconds );
	if( simT < spinUp )
	{
		double rate = 0.0;
		Hermite( paintFrom, paintSpeedFrom, paintAtRelease, launch.rotorSpeed, spinUp, simT, &rate );
		return rate;
	}
	return RotorRate( launch.rotorSpeed, simT - spinUp );
}

bool Plan::Ball( double seconds, V3& at ) const
{
	if( idle || sim.keys.empty() )
		return false;
	const double simT = seconds >= playback.duration ? playback.natural + ( seconds - playback.duration ) : playback.SimTime( seconds );
	const double t    = simT - spinUp;
	if( t < 0.0 )
		return false;
	if( sim.settled && t >= sim.natural )
	{
		//Riding the pocket: the rest position, carried round by the ring.
		const double a = sim.restAngleInRotor + PaintAngle( seconds ) - paintOffset;
		at = { sim.restRadius * std::cos( a ), sim.restRadius * std::sin( a ), sim.restHeight };
		return true;
	}
	const double f    = t * Simulation::kKeyRate;
	const size_t last = sim.keys.size() - 1;
	if( f >= static_cast< double >( last ) )
	{
		at = sim.keys[ last ];
		return true;
	}
	const size_t k = static_cast< size_t >( std::floor( f ) );
	const double u = f - static_cast< double >( k );
	at             = sim.keys[ k ] + ( sim.keys[ k + 1 ] - sim.keys[ k ] ) * u;
	return true;
}

} // namespace jackpot::roulette
