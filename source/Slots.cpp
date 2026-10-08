#include "Slots.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace jackpot::slots
{
namespace
{
constexpr double kNominalSpeed = 40.0;///< stops per second: 1.8 turns a second, a real reel's run
constexpr double kCreep        = 0.3; ///< of the run speed, when the detent catches
constexpr double kOmega        = 75.0;///< rad/s: the detent spring (12 Hz)
constexpr double kSettle       = 0.002;///< stops: closer than this to the stop is at rest
constexpr int kMaxRedraws      = 20000;

/// Strips: the eleven symbols on the even stops; every odd stop is a blank.
/// Stop 0 is the jackpot symbol on every reel.
using Eleven = std::array< int, 11 >;

const Eleven kFruit[ kMaxReels ] = {
	{ Seven, Cherry, Bar, Plum, Bell, Orange, Lemon, Cherry, Melon, Plum, Orange },
	{ Seven, Lemon, Bell, Cherry, Orange, Bar, Plum, Lemon, Melon, Bell, Orange },
	{ Seven, Orange, Plum, Bell, Lemon, Cherry, Bar, Melon, Plum, Lemon, Bell },
	{ Seven, Plum, Cherry, Orange, Bell, Lemon, Melon, Bar, Orange, Plum, Cherry },
	{ Seven, Bell, Lemon, Melon, Orange, Plum, Cherry, Bar, Lemon, Orange, Bell },
};

const Eleven kBars[ kMaxReels ] = {
	{ Seven, Bar, DoubleBar, Cherry, TripleBar, Bar, Diamond, DoubleBar, Bar, Cherry, DoubleBar },
	{ Seven, DoubleBar, Bar, TripleBar, Cherry, Bar, DoubleBar, Diamond, Bar, TripleBar, Bar },
	{ Seven, Bar, TripleBar, DoubleBar, Bar, Diamond, Cherry, Bar, DoubleBar, Bar, TripleBar },
	{ Seven, TripleBar, Bar, Cherry, DoubleBar, Bar, Diamond, Bar, TripleBar, DoubleBar, Bar },
	{ Seven, Bar, DoubleBar, Bar, Diamond, TripleBar, Bar, Cherry, DoubleBar, Bar, TripleBar },
};

/// Virtual stops per symbol stop, before the blanks take the rest. The seven
/// gets one; the blanks either side of it get kNextToSeven each.
int SymbolWeight( int symbol )
{
	switch( symbol )
	{
	case Cherry: return 4;
	case Lemon: return 4;
	case Orange: return 3;
	case Plum: return 3;
	case Bell: return 3;
	case Melon: return 2;
	case Bar: return 3;
	case DoubleBar: return 2;
	case TripleBar: return 2;
	case Diamond: return 1;
	case Seven:
	case ClipSymbol: return 1;
	default: return 1;
	}
}
constexpr int kNextToSeven = 6;

Reel MakeReel( const Eleven& eleven, bool clip )
{
	Reel reel;
	for( int i = 0; i < 11; ++i )
	{
		int s = eleven[ static_cast< size_t >( i ) ];
		if( clip && s == Seven )
			s = ClipSymbol;
		reel.symbol[ static_cast< size_t >( 2 * i ) ]     = s;
		reel.symbol[ static_cast< size_t >( 2 * i + 1 ) ] = Blank;
	}
	reel.seven = 0;

	int used = 0;
	for( int i = 0; i < kStops; i += 2 )
	{
		reel.weight[ static_cast< size_t >( i ) ] = SymbolWeight( reel.symbol[ static_cast< size_t >( i ) ] );
		used += reel.weight[ static_cast< size_t >( i ) ];
	}
	reel.weight[ 1 ]          = kNextToSeven;
	reel.weight[ kStops - 1 ] = kNextToSeven;
	used += 2 * kNextToSeven;
	//The other nine blanks share what is left of the 64, as evenly as it divides.
	const int left  = kVirtual - used;
	const int share = left / 9, extra = left % 9;
	int n = 0;
	for( int i = 3; i < kStops - 1; i += 2, ++n )
		reel.weight[ static_cast< size_t >( i ) ] = share + ( n < extra ? 1 : 0 );
	return reel;
}

std::array< Reel, kMaxReels > MakeReels( const Eleven* strips, bool clip )
{
	std::array< Reel, kMaxReels > reels;
	for( int i = 0; i < kMaxReels; ++i )
		reels[ static_cast< size_t >( i ) ] = MakeReel( strips[ i ], clip );
	return reels;
}

int ThreeOfAKind( int symbol )
{
	switch( symbol )
	{
	case Seven:
	case ClipSymbol: return 200;
	case Diamond: return 100;
	case TripleBar: return 60;
	case DoubleBar: return 40;
	case Bar: return 20;
	case Bell: return 18;
	case Melon: return 15;
	case Plum: return 14;
	case Orange: return 10;
	case Lemon: return 8;
	case Cherry: return 10;
	default: return 0;
	}
}

bool IsBar( int s )
{
	return s == Bar || s == DoubleBar || s == TripleBar;
}

int RunMultiplier( int run )
{
	return run >= 5 ? 20 : run == 4 ? 4 : 1;
}

/// The settling time of the detent: how long after it catches at `creep` the
/// reel is within kSettle of its stop for good.
double SettleTime( double creep, double zeta )
{
	if( zeta < 0.999 )
	{
		const double wd = kOmega * std::sqrt( 1.0 - zeta * zeta );
		return std::max( 0.03, std::log( std::max( 1.0001, creep / ( wd * kSettle ) ) ) / ( zeta * kOmega ) );
	}
	//Critically damped: creep tau e^(-w tau) falls below kSettle past its peak at 1/w.
	double tau = 1.0 / kOmega;
	while( creep * tau * std::exp( -kOmega * tau ) > kSettle )
		tau += 0.001;
	return tau;
}

double Smooth( double x )
{
	x = std::clamp( x, 0.0, 1.0 );
	return x * x * ( 3.0 - 2.0 * x );
}
} // namespace

const char* SymbolName( int symbol )
{
	static const char* const names[] = { "blank", "cherry", "lemon", "orange", "plum", "bell", "melon",
		                                 "bar", "double bar", "triple bar", "seven", "diamond", "clip" };
	return symbol >= 0 && symbol < kSymbolCount ? names[ symbol ] : "?";
}

const std::array< Reel, kMaxReels >& Reels( SymbolSet set )
{
	static const std::array< Reel, kMaxReels > fruit = MakeReels( kFruit, false );
	static const std::array< Reel, kMaxReels > bars  = MakeReels( kBars, false );
	static const std::array< Reel, kMaxReels > clip  = MakeReels( kFruit, true );
	switch( set )
	{
	case SymbolSet::SevensAndBars: return bars;
	case SymbolSet::Clip: return clip;
	default: return fruit;
	}
}

namespace
{
void LineSymbols( SymbolSet set, int reels, const int* stops, int* out )
{
	const auto& strips = Reels( set );
	for( int i = 0; i < reels; ++i )
		out[ i ] = strips[ static_cast< size_t >( i ) ].symbol[ static_cast< size_t >( ( ( stops[ i ] % kStops ) + kStops ) % kStops ) ];
}
} // namespace

int Pays( SymbolSet set, int reels, const int* stops )
{
	int sym[ kMaxReels ] = {};
	LineSymbols( set, reels, stops, sym );

	//A run of one symbol from the left, three or more long.
	int run = 1;
	while( run < reels && sym[ run ] == sym[ 0 ] )
		++run;
	if( run >= 3 && sym[ 0 ] != Blank )
		return ThreeOfAKind( sym[ 0 ] ) * RunMultiplier( run );

	//Any bars, mixed.
	int bars = 0;
	while( bars < reels && IsBar( sym[ bars ] ) )
		++bars;
	if( bars >= 3 )
		return 10 * RunMultiplier( bars );

	//Cherries from the left: one or two.
	if( sym[ 0 ] == Cherry )
		return sym[ 1 ] == Cherry ? 5 : 2;
	return 0;
}

bool IsJackpot( SymbolSet set, int reels, const int* stops )
{
	const auto& strips = Reels( set );
	for( int i = 0; i < reels; ++i )
		if( ( ( stops[ i ] % kStops ) + kStops ) % kStops != strips[ static_cast< size_t >( i ) ].seven )
			return false;
	return true;
}

bool IsNearMiss( SymbolSet set, int reels, const int* stops )
{
	if( reels < 3 )
		return false;
	const auto& strips = Reels( set );
	auto wrap          = []( int s ) { return ( ( s % kStops ) + kStops ) % kStops; };
	const int seven2   = strips[ 2 ].seven;
	return wrap( stops[ 0 ] ) == strips[ 0 ].seven && wrap( stops[ 1 ] ) == strips[ 1 ].seven
	       && ( wrap( stops[ 2 ] ) == wrap( seven2 + 1 ) || wrap( stops[ 2 ] ) == wrap( seven2 - 1 ) );
}

std::array< int, kMaxReels > DrawStops( const Request& r )
{
	const auto& strips = Reels( r.set );
	const int reels    = std::clamp( r.reels, 1, kMaxReels );
	std::array< int, kMaxReels > stops {};

	auto random = [ & ]( uint32_t attempt ) {
		for( int i = 0; i < kMaxReels; ++i )
		{
			Stream s( Hash( r.seed, r.play, attempt * 8u + static_cast< uint32_t >( i ), 0x5107u ) );
			const Reel& reel = strips[ static_cast< size_t >( i ) ];
			if( r.strip == Strip::Physical || r.uniformVirtual )
				stops[ static_cast< size_t >( i ) ] = s.Below( kStops );
			else
				stops[ static_cast< size_t >( i ) ] = s.Weighted( reel.weight );
		}
	};

	switch( r.result )
	{
	case Result::Random:
		random( 0 );
		break;
	case Result::Win:
		for( uint32_t a = 0; a < kMaxRedraws; ++a )
		{
			random( a );
			if( Pays( r.set, reels, stops.data() ) > 0 && !IsJackpot( r.set, reels, stops.data() ) )
				break;
		}
		break;
	case Result::Jackpot:
		for( int i = 0; i < kMaxReels; ++i )
			stops[ static_cast< size_t >( i ) ] = strips[ static_cast< size_t >( i ) ].seven;
		break;
	case Result::NearMiss:
	{
		random( 0 );
		Stream s( Hash( r.seed, r.play, 0x4ea2u ) );
		stops[ 0 ] = strips[ 0 ].seven;
		stops[ 1 ] = strips[ 1 ].seven;
		stops[ 2 ] = ( strips[ 2 ].seven + ( s.Below( 2 ) == 0 ? 1 : kStops - 1 ) ) % kStops;
		break;
	}
	case Result::Lose:
		for( uint32_t a = 0; a < kMaxRedraws; ++a )
		{
			random( a );
			if( Pays( r.set, reels, stops.data() ) == 0 && !IsNearMiss( r.set, reels, stops.data() ) )
				break;
		}
		break;
	case Result::Fixed:
	default:
	{
		//Fixed Number is a symbol: 0 blank, 1 cherry ... 10 seven, 11 diamond.
		int want = std::clamp( r.fixed, 0, static_cast< int >( Diamond ) );
		if( r.set == SymbolSet::Clip && want == Seven )
			want = ClipSymbol;
		for( int i = 0; i < kMaxReels; ++i )
		{
			const Reel& reel = strips[ static_cast< size_t >( i ) ];
			std::vector< int > options;
			for( int k = 0; k < kStops; ++k )
				if( reel.symbol[ static_cast< size_t >( k ) ] == want )
					options.push_back( k );
			Stream s( Hash( r.seed, r.play, static_cast< uint32_t >( i ), 0xf1ed5u ) );
			stops[ static_cast< size_t >( i ) ] = options.empty() ? s.Below( kStops ) : options[ static_cast< size_t >( s.Below( static_cast< int >( options.size() ) ) ) ];
		}
		break;
	}
	}
	return stops;
}

//---------------------------------------------------------------------------
// The motion.
//---------------------------------------------------------------------------
double ReelMotion::Position( double t ) const
{
	if( t <= start )
		return from;
	if( t >= restAt )
		return target;
	const double u = t - start;
	if( u < rampUp )
	{
		const double x = u / rampUp;
		//The run-up: the integral of smoothstep is x^3 - x^4 / 2; the kick
		//winds the reel back first and is gone by the end of the ramp.
		return from + speed * rampUp * ( x * x * x - 0.5 * x * x * x * x ) - kick * std::sin( kPi * x );
	}
	const double atRun = from + 0.5 * speed * rampUp;
	if( t < slowAt )
		return atRun + speed * ( u - rampUp );
	const double atSlow = atRun + speed * ( slowAt - start - rampUp );
	if( t < catchAt )
	{
		const double tau = t - slowAt, span = catchAt - slowAt;
		return atSlow + speed * tau - ( speed - creep ) * tau * tau / ( 2.0 * span );
	}
	const double tau = t - catchAt;
	if( zeta < 0.999 )
	{
		const double wd = omega * std::sqrt( 1.0 - zeta * zeta );
		return target + creep * std::exp( -zeta * omega * tau ) * std::sin( wd * tau ) / wd;
	}
	return target + creep * tau * std::exp( -omega * tau );
}

double Plan::Position( int i, double seconds ) const
{
	return reels[ static_cast< size_t >( std::clamp( i, 0, kMaxReels - 1 ) ) ].Position( seconds );
}

double Plan::Handle( double seconds ) const
{
	if( idle || seconds < 0.0 )
		return 0.0;
	if( seconds < pull )
		return std::sin( 0.5 * kPi * seconds / pull );
	return 1.0 - Smooth( ( seconds - pull ) / 0.45 );
}

Plan MakePlan( const Request& r )
{
	Plan plan;
	plan.request     = r;
	const int reels  = std::clamp( r.reels, 1, kMaxReels );
	const double D   = std::max( 0.5, r.duration );
	plan.stops       = DrawStops( r );
	plan.pull        = std::min( 0.3, 0.12 * D );
	plan.playback    = MakePlayback( D, D );

	const double zeta    = std::clamp( r.damping, 0.05, 1.0 );
	const double rampUp  = std::min( 0.22, 0.1 * D );
	const double slowFor = std::min( 0.2, 0.08 * D );
	const double creep   = kCreep * kNominalSpeed;
	const double settle  = SettleTime( creep, zeta );
	const double first   = plan.pull + rampUp + 0.08 + slowFor + settle;//the earliest the first reel can rest
	const double gap     = reels > 1 ? std::clamp( ( D - first ) / ( reels - 1 ), 0.0, 0.45 ) : 0.0;

	for( int i = 0; i < kMaxReels; ++i )
	{
		ReelMotion& m = plan.reels[ static_cast< size_t >( i ) ];
		m.from        = r.from[ static_cast< size_t >( i ) ];
		m.start       = plan.pull;
		m.rampUp      = rampUp;
		m.zeta        = zeta;
		m.omega       = kOmega;
		m.restAt      = i < reels ? D - ( reels - 1 - i ) * gap : D;
		m.catchAt     = m.restAt - settle;
		m.slowAt      = std::max( m.start + rampUp, m.catchAt - slowFor );
		if( i >= reels )
		{
			//Reels the machine does not have: still.
			m.target = m.from;
			m.restAt = m.start = m.catchAt = m.slowAt = 0.0;
			continue;
		}

		//The distance the reel must cover is fixed by where it must stop, modulo
		//whole laps; the laps are chosen so the run speed is near a real reel's,
		//and then the speed is solved exactly so the detent catches on the stop.
		const double bracket = 0.5 * rampUp + ( m.slowAt - m.start - rampUp ) + 0.5 * ( 1.0 + kCreep ) * ( m.catchAt - m.slowAt );
		const int fromStop   = static_cast< int >( std::lround( m.from ) );
		const int delta      = ( ( plan.stops[ static_cast< size_t >( i ) ] - fromStop ) % kStops + kStops ) % kStops;
		int laps             = static_cast< int >( std::lround( ( kNominalSpeed * bracket - delta ) / kStops ) );
		if( delta + laps * kStops < 6 )
			++laps;
		laps            = std::max( laps, 0 );
		const double distance = delta + laps * static_cast< double >( kStops ) + ( fromStop - m.from );
		m.speed         = distance / std::max( bracket, 1e-6 );
		m.creep         = kCreep * m.speed;
		m.target        = m.from + distance;
	}

	const int* stops = plan.stops.data();
	plan.outcome.payout   = Pays( r.set, reels, stops );
	plan.outcome.jackpot  = IsJackpot( r.set, reels, stops );
	plan.outcome.nearMiss = IsNearMiss( r.set, reels, stops );
	plan.outcome.win      = plan.outcome.payout > 0;
	const auto& strips    = Reels( r.set );
	plan.outcome.value    = strips[ 0 ].symbol[ static_cast< size_t >( stops[ 0 ] ) ];
	std::string text;
	for( int i = 0; i < reels; ++i )
		text += ( i ? " | " : "" ) + std::string( SymbolName( strips[ static_cast< size_t >( i ) ].symbol[ static_cast< size_t >( stops[ i ] ) ] ) );
	plan.outcome.text = text + ( plan.outcome.payout > 0 ? " -> " + std::to_string( plan.outcome.payout ) : "" );
	return plan;
}

Plan MakeRest( const Request& r )
{
	Plan plan;
	plan.request = r;
	plan.idle    = true;
	for( int i = 0; i < kMaxReels; ++i )
	{
		ReelMotion& m = plan.reels[ static_cast< size_t >( i ) ];
		m.from = m.target = r.from[ static_cast< size_t >( i ) ];
		plan.stops[ static_cast< size_t >( i ) ] = ( ( static_cast< int >( std::lround( m.from ) ) % kStops ) + kStops ) % kStops;
	}
	plan.playback = MakePlayback( 0.0, 0.0 );
	plan.playback.duration = 0.0;
	return plan;
}

} // namespace jackpot::slots
