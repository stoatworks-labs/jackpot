#include "Lottery.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>

namespace jackpot::lottery
{
namespace
{
constexpr double kDt        = 1.0 / 1500.0;
constexpr double kGravity   = 9.81;
constexpr double kDrag      = 0.13;   ///< 1/m: (1/2) rho Cd A / m, a 42 mm ball of 3 g
constexpr double kJetWidth  = 0.085;  ///< m: the jet's radius
constexpr double kJetDecay  = 1.0;    ///< m: the jet's strength falls by e over this height
constexpr double kJetGain   = 2.5;    ///< the jet's lift at its core, per unit of Air
constexpr double kSwirl     = 0.08;   ///< 1/m: the tangential push, per unit of lift, at the bottom (more spins the cloud up into a centrifuge)
constexpr double kTurbulence = 0.25;  ///< of the lift
constexpr double kSuction   = 1.5;    ///< the open tube's pull at its mouth, per unit of lift
constexpr double kSuctionReach = 0.10;///< m
constexpr double kBallBounce = 0.85;
constexpr double kGlassBounce = 0.70;
constexpr double kBallGrip  = 0.10;
constexpr double kGlassGrip = 0.20;
constexpr double kEnterEvery = 0.035; ///< s between balls dropping in
constexpr double kMaxSim    = 70.0;
constexpr double kDrainTime = 0.6;

double Smooth( double x )
{
	x = std::clamp( x, 0.0, 1.0 );
	return x * x * ( 3.0 - 2.0 * x );
}

Quat Integrate( Quat q, V3 w, double dt )
{
	const Quat dq = { 0.0, w.x, w.y, w.z };
	const Quat d  = dq * q;
	return dice::Normalise( Quat { q.w + 0.5 * dt * d.w, q.x + 0.5 * dt * d.x, q.y + 0.5 * dt * d.y, q.z + 0.5 * dt * d.z } );
}

V3 RackSlot( int k )
{
	return { kRackX0 + kRackPitch * k, 0.0, kRackZ };
}

/// A ball on the rack: its number (on the body's z) toward the viewer (-y),
/// turned by `roll` about the rail's normal (y), which keeps it facing us.
Quat OnRack( double roll )
{
	const Quat facing = { std::cos( 0.25 * kPi ), std::sin( 0.25 * kPi ), 0.0, 0.0 };//z -> -y about x
	const Quat turn   = { std::cos( 0.5 * roll ), 0.0, std::sin( 0.5 * roll ), 0.0 };
	return turn * facing;
}
} // namespace

int ColourBand( int number )
{
	return std::clamp( ( number ) / 10, 0, 6 );
}

std::vector< int > DrawNumbers( const Request& r )
{
	const int N = std::clamp( r.balls, kMinBalls, kMaxBalls );
	const int K = std::clamp( r.draw, 1, std::min( kMaxDraw, N ) );
	Stream s( Hash( r.seed, r.play, 0x10770u ) );
	const int bet = std::clamp( r.bet, 1, N );
	std::vector< int > pool( static_cast< size_t >( N ) );
	std::iota( pool.begin(), pool.end(), 1 );
	auto take = [ & ]( int value ) { pool.erase( std::remove( pool.begin(), pool.end(), value ), pool.end() ); };
	auto drawOne = [ & ]() {
		const int n = static_cast< int >( pool.size() );
		const int i = r.biasedDraw ? s.Below( std::max( 1, n - 1 ) ) : s.Below( n );
		const int v = pool[ static_cast< size_t >( i ) ];
		take( v );
		return v;
	};
	std::vector< int > out;
	switch( r.result )
	{
	case Result::Fixed:
		take( bet );
		out.push_back( bet );
		while( static_cast< int >( out.size() ) < K )
			out.push_back( drawOne() );
		break;
	case Result::Win:
	case Result::Jackpot:
		take( bet );
		while( static_cast< int >( out.size() ) < K - 1 )
			out.push_back( drawOne() );
		out.push_back( bet );
		break;
	case Result::NearMiss:
	{
		const int near = bet + 1 <= N ? bet + 1 : bet - 1;
		take( bet );
		take( near );
		while( static_cast< int >( out.size() ) < K - 1 )
			out.push_back( drawOne() );
		out.push_back( near );
		break;
	}
	case Result::Lose:
		take( bet );
		while( static_cast< int >( out.size() ) < K )
			out.push_back( drawOne() );
		break;
	default:
		while( static_cast< int >( out.size() ) < K )
			out.push_back( drawOne() );
		break;
	}
	return out;
}

Simulation Simulate( const Request& r, double mix, double gap, const std::atomic< bool >* cancel )
{
	Simulation sim;
	const int N = std::clamp( r.balls, kMinBalls, kMaxBalls );
	const int K = std::clamp( r.draw, 1, std::min( kMaxDraw, N ) );
	sim.balls   = N;
	Stream s( Hash( r.seed, r.play, 0xd2a3u ) );
	const double ballBounce  = r.restitution >= 0.0 ? r.restitution : kBallBounce;
	const double glassBounce = r.restitution >= 0.0 ? r.restitution : kGlassBounce;
	const double inertia     = 0.4 * kBall * kBall;//per unit mass, a solid ball

	std::vector< V3 > x( static_cast< size_t >( N ) ), v( static_cast< size_t >( N ) ), w( static_cast< size_t >( N ) );
	std::vector< Quat > q( static_cast< size_t >( N ) );
	std::vector< char > active( static_cast< size_t >( N ), 0 ), gone( static_cast< size_t >( N ), 0 );
	sim.enter.resize( static_cast< size_t >( N ) );
	for( int i = 0; i < N; ++i )
	{
		const size_t k = static_cast< size_t >( i );
		sim.enter[ k ] = i * kEnterEvery;
		x[ k ]         = { s.Range( -0.012, 0.012 ), s.Range( -0.012, 0.012 ), kDrum - kBall - 0.004 };
		v[ k ]         = { s.Range( -0.3, 0.3 ), s.Range( -0.3, 0.3 ), -0.8 };
		const double a = s.Range( 0.0, 2.0 * kPi ), b = s.Range( -1.0, 1.0 ), c = s.Range( 0.0, 2.0 * kPi );
		q[ k ]         = dice::Normalise( Quat { std::cos( c ), std::sin( c ) * std::sqrt( 1 - b * b ) * std::cos( a ), std::sin( c ) * std::sqrt( 1 - b * b ) * std::sin( a ), std::sin( c ) * b } );
	}
	const double airOn  = N * kEnterEvery + 0.4;
	const double phase1 = s.Range( 0.0, 6.28 ), phase2 = s.Range( 0.0, 6.28 ), phase3 = s.Range( 0.0, 6.28 );
	double nextOpen     = airOn + mix;
	double airOff       = 1e30;
	double t = 0.0, nextKey = 0.0;
	const double lift = r.air * kGravity;

	const long maxSteps = static_cast< long >( kMaxSim / kDt );
	for( long step = 0; step < maxSteps; ++step )
	{
		if( cancel != nullptr && ( step & 1023 ) == 0 && cancel->load() )
			return {};
		while( t >= nextKey - 1e-12 )
		{
			for( int i = 0; i < N; ++i )
			{
				const size_t k = static_cast< size_t >( i );
				sim.keys.insert( sim.keys.end(), { static_cast< float >( x[ k ].x ), static_cast< float >( x[ k ].y ), static_cast< float >( x[ k ].z ),
				                                   static_cast< float >( q[ k ].w ), static_cast< float >( q[ k ].x ), static_cast< float >( q[ k ].y ),
				                                   static_cast< float >( q[ k ].z ) } );
			}
			nextKey += 1.0 / Simulation::kKeyRate;
		}
		const bool done = static_cast< int >( sim.captured.size() ) >= K;
		if( done && t >= sim.natural )
			break;

		//The air: a jet up the middle, swirling, and turned up while the tube
		//waits for a ball so a draw always comes.
		double strength = 0.0;
		if( t >= airOn && t < airOff )
		{
			strength = Smooth( ( t - airOn ) / 0.6 );
			if( t >= nextOpen )
				strength *= 1.0 + 0.5 * ( t - nextOpen );
		}
		for( int i = 0; i < N; ++i )
		{
			const size_t k = static_cast< size_t >( i );
			if( gone[ k ] )
				continue;
			if( !active[ k ] )
			{
				if( t < sim.enter[ k ] )
					continue;
				active[ k ] = 1;
			}
			V3& p = x[ k ];
			v[ k ].z -= kGravity * kDt;
			if( strength > 0.0 )
			{
				//A fountain: the jet carries balls up the middle to the top, where
				//they spread and fall back down the glass. (A swirl several times
				//the lift, as first written, flung every ball into a ring round
				//the equator and none ever reached the mouth.)
				const double rho2  = p.x * p.x + p.y * p.y;
				const double core  = std::exp( -rho2 / ( kJetWidth * kJetWidth ) ) * std::exp( -( p.z + kDrum ) / kJetDecay );
				const double swirl = kSwirl * std::exp( -( p.z + kDrum ) / 0.5 );
				const V3 turb      = { std::sin( 9.0 * p.y + 1.3 * t + phase1 ), std::sin( 8.0 * p.z + 1.7 * t + phase2 ), 0.4 * std::sin( 7.0 * p.x + 1.1 * t + phase3 ) };
				v[ k ] += ( V3 { -p.y * swirl, p.x * swirl, kJetGain * core } + turb * kTurbulence ) * ( lift * strength * kDt );
				//The open tube: the air leaves through it, and draws in what is
				//near its mouth.
				if( t >= nextOpen && !done )
				{
					const V3 mouth   = { 0.0, 0.0, kDrum - kBall };
					const V3 toward  = mouth - p;
					const double d   = Length( toward );
					if( d < kSuctionReach && d > 1e-6 )
						v[ k ] += toward * ( kSuction * lift * strength * ( 1.0 - d / kSuctionReach ) / d * kDt );
				}
			}
			const double speed = Length( v[ k ] );
			v[ k ] -= v[ k ] * ( kDrag * speed * kDt );
			p += v[ k ] * kDt;
			q[ k ] = Integrate( q[ k ], w[ k ], kDt );
			w[ k ] = w[ k ] * ( 1.0 - 0.5 * kDt );
		}

		//Ball on ball, twice round for the pile at the bottom.
		for( int pass = 0; pass < 2; ++pass )
			for( int i = 0; i < N; ++i )
			{
				const size_t a = static_cast< size_t >( i );
				if( !active[ a ] || gone[ a ] )
					continue;
				for( int j = i + 1; j < N; ++j )
				{
					const size_t b = static_cast< size_t >( j );
					if( !active[ b ] || gone[ b ] )
						continue;
					V3 d          = x[ b ] - x[ a ];
					const double dd = Dot( d, d );
					if( dd >= 4.0 * kBall * kBall || dd < 1e-18 )
						continue;
					const double dist = std::sqrt( dd );
					const V3 n        = d * ( 1.0 / dist );
					const double over = 2.0 * kBall - dist;
					x[ a ] -= n * ( 0.5 * over );
					x[ b ] += n * ( 0.5 * over );
					const V3 rel = ( v[ b ] + Cross( w[ b ], n * -kBall ) ) - ( v[ a ] + Cross( w[ a ], n * kBall ) );
					const double vn = Dot( rel, n );
					if( vn >= 0.0 )
						continue;
					const double jn = -( 1.0 + ( -vn > 0.05 ? ballBounce : 0.0 ) ) * vn * 0.5;
					v[ b ] += n * jn;
					v[ a ] -= n * jn;
					V3 vt           = rel - n * vn;
					const double st = Length( vt );
					if( st > 1e-9 )
					{
						//Per unit mass: two spheres' tangential effective mass is m / 7.
						const double jt = std::min( kBallGrip * jn, st / 7.0 );
						const V3 J      = vt * ( -jt / st );
						v[ b ] += J;
						v[ a ] -= J;
						const V3 spin = Cross( n * kBall, J ) * ( 1.0 / inertia );
						w[ b ] += spin;
						w[ a ] += spin;
					}
				}
			}

		//The glass, and the tube.
		for( int i = 0; i < N; ++i )
		{
			const size_t k = static_cast< size_t >( i );
			if( !active[ k ] || gone[ k ] )
				continue;
			V3& p           = x[ k ];
			const double rr = Length( p );
			if( rr > kDrum - kBall )
			{
				const V3 n = p * ( -1.0 / rr );
				p          = p + n * ( rr - ( kDrum - kBall ) );
				const double vn = Dot( v[ k ], n );
				if( vn < 0.0 )
				{
					const double jn = -( 1.0 + ( -vn > 0.05 ? glassBounce : 0.0 ) ) * vn;
					v[ k ] += n * jn;
					const V3 rel    = v[ k ] + Cross( w[ k ], n * -kBall );
					V3 vt           = rel - n * Dot( rel, n );
					const double st = Length( vt );
					if( st > 1e-9 )
					{
						const double jt = std::min( kGlassGrip * jn, st / 3.5 );
						const V3 J      = vt * ( -jt / st );
						v[ k ] += J;
						w[ k ] += Cross( n * -kBall, J ) * ( 1.0 / inertia );
					}
				}
			}
			const double rho = std::sqrt( p.x * p.x + p.y * p.y );
			//The mouth is a hole in the glass: a ball under it while it is open
			//goes up the tube, whichever way it was moving. (Requiring it to be
			//rising caught almost nothing: a ball pressed against the glass
			//under the mouth is not rising.)
			if( !done && t >= nextOpen && rho < kMouth && p.z > kDrum - kBall - 0.015 )
			{
				gone[ k ] = 1;
				sim.captured.push_back( i );
				sim.caughtAt.push_back( t );
				sim.caughtFrom.push_back( p );
				nextOpen = std::max( nextOpen + gap, t + 0.6 );
				if( static_cast< int >( sim.captured.size() ) >= K )
				{
					airOff      = t + 0.3;
					sim.natural = t + Plan::kRise + Plan::kRoll * ( 1.0 + 0.15 * ( K - 1 ) ) + 0.1;
				}
			}
		}

		//What the checks hold the simulation to.
		for( int i = 0; i < N; ++i )
		{
			const size_t a = static_cast< size_t >( i );
			if( !active[ a ] || gone[ a ] )
				continue;
			sim.maxEscape = std::max( sim.maxEscape, Length( x[ a ] ) - ( kDrum - kBall ) );
			if( ( step & 15 ) == 0 )
				for( int j = i + 1; j < N; ++j )
				{
					const size_t b = static_cast< size_t >( j );
					if( active[ b ] && !gone[ b ] )
						sim.maxOverlap = std::max( sim.maxOverlap, 2.0 * kBall - Length( x[ b ] - x[ a ] ) );
				}
		}
		t += kDt;
	}
	if( sim.natural <= 0.0 )
		sim.natural = t;
	return sim;
}

Plan MakePlan( const Request& r, const std::atomic< bool >* cancel )
{
	const auto start = std::chrono::steady_clock::now();
	Plan plan;
	plan.request = r;
	plan.numbers = DrawNumbers( r );
	plan.drain   = ( !r.previousDrum.empty() || !r.previousRack.empty() ) ? kDrainTime : 0.0;
	const int N  = std::clamp( r.balls, kMinBalls, kMaxBalls );
	const int K  = static_cast< int >( plan.numbers.size() );
	const double loading = N * kEnterEvery + 0.4;
	const double avail   = r.duration - plan.drain - loading - Plan::kRise - Plan::kRoll;
	plan.mix = std::clamp( 0.4 * avail, 0.8, 8.0 );
	plan.gap = std::clamp( ( avail - plan.mix ) / std::max( 1, K ), 0.5, 5.0 );
	plan.sim = Simulate( r, plan.mix, plan.gap, cancel );
	if( cancel != nullptr && cancel->load() )
		return Plan {};
	plan.playback = MakePlayback( plan.drain + plan.sim.natural, r.duration, r.noWarp );

	//Deal the paint: the k-th ball caught carries the k-th number wanted.
	plan.paint.assign( static_cast< size_t >( N ), 0 );
	std::vector< int > rest;
	for( int n = 1; n <= N; ++n )
		if( std::find( plan.numbers.begin(), plan.numbers.end(), n ) == plan.numbers.end() )
			rest.push_back( n );
	Stream s( Hash( r.seed, r.play, 0x5a1eu ) );
	for( size_t i = rest.size(); i > 1; --i )
		std::swap( rest[ i - 1 ], rest[ static_cast< size_t >( s.Below( static_cast< int >( i ) ) ) ] );
	std::vector< char > dealt( static_cast< size_t >( N ), 0 );
	for( size_t k = 0; k < plan.sim.captured.size() && k < plan.numbers.size(); ++k )
	{
		plan.paint[ static_cast< size_t >( plan.sim.captured[ k ] ) ] = plan.numbers[ k ];
		dealt[ static_cast< size_t >( plan.sim.captured[ k ] ) ]       = 1;
	}
	size_t next = 0;
	for( int i = 0; i < N; ++i )
		if( !dealt[ static_cast< size_t >( i ) ] )
			plan.paint[ static_cast< size_t >( i ) ] = next < rest.size() ? rest[ next++ ] : 0;
	if( r.identityPermutation )
		for( int i = 0; i < N; ++i )
			plan.paint[ static_cast< size_t >( i ) ] = i + 1;

	//What is shown is what was caught, as painted.
	std::vector< int > shown;
	for( int c : plan.sim.captured )
		shown.push_back( plan.paint[ static_cast< size_t >( c ) ] );
	const int bet         = std::clamp( r.bet, 1, N );
	const bool drawn      = std::find( shown.begin(), shown.end(), bet ) != shown.end();
	plan.outcome.value    = shown.empty() ? 0 : shown.back();
	plan.outcome.win      = drawn;
	plan.outcome.jackpot  = drawn && !shown.empty() && shown.back() == bet;
	plan.outcome.nearMiss = !drawn && ( std::find( shown.begin(), shown.end(), bet + 1 ) != shown.end() || std::find( shown.begin(), shown.end(), bet - 1 ) != shown.end() );
	std::string text;
	for( int n : shown )
		text += ( text.empty() ? "" : " " ) + std::to_string( n );
	plan.outcome.text = text;
	plan.ms = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count();
	(void)K;
	return plan;
}

Plan MakeRest( const Request& r )
{
	Plan plan;
	plan.request = r;
	plan.idle    = true;
	plan.numbers = r.previousRack;
	plan.playback = MakePlayback( 0.0, 0.0 );
	plan.playback.duration = 0.0;
	return plan;
}

std::vector< BallState > Plan::Balls( double seconds ) const
{
	std::vector< BallState > out;
	if( idle )
	{
		for( size_t k = 0; k < numbers.size(); ++k )
			out.push_back( { RackSlot( static_cast< int >( k ) ), OnRack( 0.0 ), numbers[ k ], true } );
		for( const BallState& b : request.previousDrum )
			out.push_back( b );
		return out;
	}
	const double simT = playback.SimTime( seconds );
	if( simT < drain )
	{
		//Last draw's balls: out through the trapdoor, and off the end of the rack.
		const double tau = simT;
		for( const BallState& b : request.previousDrum )
		{
			BallState o = b;
			o.x.z -= 0.5 * 9.81 * tau * tau;
			out.push_back( o );
		}
		for( size_t k = 0; k < request.previousRack.size(); ++k )
		{
			V3 p = RackSlot( static_cast< int >( k ) );
			p.x += 0.9 * tau + 1.2 * tau * tau;
			out.push_back( { p, OnRack( -( 0.9 * tau + 1.2 * tau * tau ) / kBall ), request.previousRack[ k ], true } );
		}
		return out;
	}
	const double t = simT - drain;
	const int N    = sim.balls;
	if( N <= 0 || sim.keys.empty() )
		return out;
	const size_t keys = sim.keys.size() / ( static_cast< size_t >( N ) * 7 );
	const double f    = std::min( t * Simulation::kKeyRate, static_cast< double >( keys - 1 ) );
	const size_t k0   = static_cast< size_t >( std::floor( f ) );
	const size_t k1   = std::min( k0 + 1, keys - 1 );
	const double u    = f - static_cast< double >( k0 );
	auto at = [ & ]( size_t key, int i, V3& x, Quat& q ) {
		const float* p = &sim.keys[ ( key * static_cast< size_t >( N ) + static_cast< size_t >( i ) ) * 7 ];
		x              = { p[ 0 ], p[ 1 ], p[ 2 ] };
		q              = { p[ 3 ], p[ 4 ], p[ 5 ], p[ 6 ] };
	};
	for( int i = 0; i < N; ++i )
	{
		BallState b;
		b.number = paint.empty() ? i + 1 : paint[ static_cast< size_t >( i ) ];
		if( t < sim.enter[ static_cast< size_t >( i ) ] )
			continue;
		const auto c = std::find( sim.captured.begin(), sim.captured.end(), i );
		if( c != sim.captured.end() )
		{
			const size_t order = static_cast< size_t >( c - sim.captured.begin() );
			const double since = t - sim.caughtAt[ order ];
			if( since >= 0.0 )
			{
				//Up the tube, then along the rack to its slot.
				const V3 mouth = { 0.0, 0.0, kDrum - kBall };
				const V3 top   = { 0.0, 0.0, kRackZ };
				const V3 slot  = RackSlot( static_cast< int >( order ) );
				const double rollFor = kRoll * ( 1.0 + 0.15 * static_cast< double >( order ) );
				if( since < kRise )
				{
					const double s = Smooth( since / kRise );
					b.x = since < 0.3 * kRise ? sim.caughtFrom[ order ] + ( mouth - sim.caughtFrom[ order ] ) * ( since / ( 0.3 * kRise ) )
					                          : mouth + ( top - mouth ) * s;
				}
				else
					b.x = top + ( slot - top ) * Smooth( ( since - kRise ) / rollFor );
				//Rolling along the rack turns it about the rail's normal, and it
				//comes to rest with its number upright.
				const double along = Smooth( std::max( 0.0, since - kRise ) / rollFor );
				if( since < kRise )
				{
					//Up the tube turning as it was caught: its last key holds it.
					V3 caughtX;
					at( k0, i, caughtX, b.q );
				}
				else
					b.q = OnRack( -( 1.0 - along ) * Length( slot - top ) / kBall );
				b.shown = true;
				out.push_back( b );
				continue;
			}
		}
		V3 x0, x1;
		Quat q0, q1;
		at( k0, i, x0, q0 );
		at( k1, i, x1, q1 );
		b.x     = x0 + ( x1 - x0 ) * u;
		b.q     = dice::Slerp( q0, q1, u );
		b.shown = true;
		out.push_back( b );
	}
	return out;
}

double Plan::Air( double seconds ) const
{
	if( idle )
		return 0.0;
	const double t = playback.SimTime( seconds ) - drain;
	const int N    = std::max( 1, sim.balls );
	const double on = N * kEnterEvery + 0.4;
	if( t < on )
		return 0.0;
	const double off = sim.caughtAt.empty() ? 1e30 : sim.caughtAt.back() + 0.3;
	return t < off ? Smooth( ( t - on ) / 0.6 ) : std::max( 0.0, 1.0 - ( t - off ) / 0.5 );
}

} // namespace jackpot::lottery
