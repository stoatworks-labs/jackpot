#include "Craps.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>

namespace jackpot::craps
{
using namespace dice;

namespace
{
constexpr double kMaxSim      = 10.0;
constexpr int kMaxTrials      = 10;
constexpr double kCockedDeg   = 1.0;
constexpr double kOnTable     = 3e-4;
constexpr double kBlendSeconds = 0.15;
constexpr double kSpeedLow    = 1.0;
constexpr double kSpeedHigh   = 3.2;
constexpr int kStepsPerKey    = 4;///< 960 Hz physics, 240 Hz keys

/// The face a body at rest lies on, and how far off flat it is (degrees).
int BottomFace( const geo::Solid& solid, const M3& rotation, double& offFlat )
{
	int best   = 0;
	double low = 2.0;
	for( size_t f = 0; f < solid.faces.size(); ++f )
	{
		const double y = ( rotation * solid.faces[ f ].normal ).y;
		if( y < low )
		{
			low  = y;
			best = static_cast< int >( f );
		}
	}
	offFlat = std::acos( std::clamp( -low, -1.0, 1.0 ) ) * 180.0 / kPi;
	return best;
}

double LowestVertex( const physics::Body& body )
{
	const M3 r    = body.Rotation();
	double lowest = 1e30;
	for( const V3& v : body.solid->vertices )
		lowest = std::min( lowest, ( body.x + r * v ).y );
	return lowest;
}

/// How far two dice overlap (m; negative: the gap), by separating axes
/// (polyhedral's Roll.cpp, unchanged).
double Overlap( const geo::Solid& a, const Pose& pa, const geo::Solid& b, const Pose& pb )
{
	const M3 ra = ToMatrix( pa.q ), rb = ToMatrix( pb.q );
	std::vector< V3 > va, vb, axes;
	for( const V3& v : a.vertices )
		va.push_back( pa.x + ra * v );
	for( const V3& v : b.vertices )
		vb.push_back( pb.x + rb * v );
	for( const geo::Face& f : a.faces )
		axes.push_back( ra * f.normal );
	for( const geo::Face& f : b.faces )
		axes.push_back( rb * f.normal );
	for( const auto& ea : a.edges )
		for( const auto& eb : b.edges )
		{
			const V3 c = Cross( va[ static_cast< size_t >( ea[ 1 ] ) ] - va[ static_cast< size_t >( ea[ 0 ] ) ],
			                    vb[ static_cast< size_t >( eb[ 1 ] ) ] - vb[ static_cast< size_t >( eb[ 0 ] ) ] );
			if( Length( c ) > 1e-12 )
				axes.push_back( Normalise( c ) );
		}
	double least = 1e30;
	for( const V3& axis : axes )
	{
		double a0 = 1e30, a1 = -1e30, b0 = 1e30, b1 = -1e30;
		for( const V3& p : va )
		{
			a0 = std::min( a0, Dot( p, axis ) );
			a1 = std::max( a1, Dot( p, axis ) );
		}
		for( const V3& p : vb )
		{
			b0 = std::min( b0, Dot( p, axis ) );
			b1 = std::max( b1, Dot( p, axis ) );
		}
		least = std::min( least, std::min( a1 - b0, b1 - a0 ) );
	}
	return least;
}

/// A uniformly random rotation (Shoemake).
Quat RandomRotation( Stream& s )
{
	const double u1 = s.Uniform(), u2 = s.Uniform(), u3 = s.Uniform();
	const double a = std::sqrt( 1.0 - u1 ), b = std::sqrt( u1 );
	return Normalise( Quat { a * std::sin( 2 * kPi * u2 ), a * std::cos( 2 * kPi * u2 ), b * std::sin( 2 * kPi * u3 ),
	                         b * std::cos( 2 * kPi * u3 ) } );
}

double Smooth( double x )
{
	x = std::clamp( x, 0.0, 1.0 );
	return x * x * ( 3.0 - 2.0 * x );
}

struct Attempt
{
	std::array< Track, 2 > tracks;
	double natural = 0.0;
	bool valid     = false;
	bool hitWall   = false;
	bool cocked    = false;
	bool cancelled = false;
	physics::Stats stats;
};

Attempt Throw( const Request& r, double speed, uint32_t sub, const std::atomic< bool >* cancel )
{
	Attempt out;
	physics::World world = MakeWorld( r );
	Stream s( sub );
	const geo::Solid& cube = geo::GetSolid( geo::Shape::Cube );
	const double side      = s.Range( -0.10, 0.10 );
	for( int i = 0; i < 2; ++i )
	{
		physics::Body body;
		body.solid = &cube;
		body.q     = RandomRotation( s );
		body.x     = { side + ( i == 0 ? -0.018 : 0.018 ) + s.Range( -0.004, 0.004 ), s.Range( 0.045, 0.075 ), kNearWall - 0.05 + s.Range( -0.01, 0.01 ) };
		//At the back wall, with a little spread either way and a lob.
		body.v = { s.Range( -0.12, 0.12 ) * speed - 0.3 * side, s.Range( 0.05, 0.25 ) * speed, -speed };
		const V3 axis = Normalise( V3 { s.Range( -1, 1 ), s.Range( -1, 1 ), s.Range( -1, 1 ) } );
		body.w        = axis * s.Range( 18.0, 42.0 );
		world.bodies.push_back( body );
	}

	std::array< long, 2 > asleepSince = { -1, -1 };
	auto record = [ & ]() {
		for( int i = 0; i < 2; ++i )
			out.tracks[ static_cast< size_t >( i ) ].keys.push_back( { world.bodies[ static_cast< size_t >( i ) ].x, world.bodies[ static_cast< size_t >( i ) ].q } );
	};
	record();
	const long maxSteps = static_cast< long >( kMaxSim / world.settings.dt );
	long step           = 0;
	for( ; step < maxSteps; ++step )
	{
		if( cancel != nullptr && ( step & 127 ) == 0 && cancel->load() )
		{
			out.cancelled = true;
			return out;
		}
		world.Step();
		for( size_t i = 0; i < 2; ++i )
		{
			const physics::Body& b = world.bodies[ i ];
			if( !b.asleep )
				asleepSince[ i ] = -1;
			else if( asleepSince[ i ] < 0 )
				asleepSince[ i ] = step + 1;
			//Did a corner reach the back wall's pyramids?
			const M3 rot = b.Rotation();
			for( const V3& v : b.solid->vertices )
				if( ( b.x + rot * v ).z < kBackWall + kPyramidHeight + 1e-3 )
					out.hitWall = true;
		}
		if( ( step + 1 ) % kStepsPerKey == 0 )
			record();
		if( world.AllAsleep() )
		{
			++step;
			break;
		}
	}
	if( step % kStepsPerKey != 0 )
		record();
	out.stats = world.stats;
	if( !world.AllAsleep() )
		return out;

	const double dt = world.settings.dt;
	std::array< Pose, 2 > finals;
	for( size_t i = 0; i < 2; ++i )
	{
		const physics::Body& body = world.bodies[ i ];
		double offFlat            = 0.0;
		const M3 rotation         = body.Rotation();
		const int bottom          = BottomFace( *body.solid, rotation, offFlat );
		if( offFlat > kCockedDeg || LowestVertex( body ) > kOnTable )
		{
			out.cocked = true;
			return out;
		}
		const double settle    = std::max( 0.0, asleepSince[ i ] * dt - world.settings.sleepTime );
		out.tracks[ i ].settle = std::floor( settle * Plan::kKeyRate ) / Plan::kKeyRate;
		out.natural            = std::max( out.natural, out.tracks[ i ].settle );
		const geo::Face& face  = body.solid->faces[ static_cast< size_t >( bottom ) ];
		const M3 flat          = Align( rotation * face.normal, { 0, -1, 0 } ) * rotation;
		const V3 footprint     = body.x + rotation * face.centroid;
		finals[ i ].q          = FromMatrix( flat );
		finals[ i ].x          = footprint - flat * face.centroid;
		finals[ i ].x.y        = face.offset;
	}
	if( Overlap( cube, finals[ 0 ], cube, finals[ 1 ] ) > 1e-4 )
	{
		out.cocked = true;
		return out;
	}
	for( size_t i = 0; i < 2; ++i )
	{
		Track& track       = out.tracks[ i ];
		const long settled = std::lround( track.settle * Plan::kKeyRate );
		const long from    = std::max( 0L, settled - static_cast< long >( kBlendSeconds * Plan::kKeyRate ) );
		for( long k = 0; k < static_cast< long >( track.keys.size() ); ++k )
		{
			Pose& key = track.keys[ static_cast< size_t >( k ) ];
			if( k >= settled )
				key = finals[ i ];
			else if( k > from )
			{
				const double w = static_cast< double >( k - from ) / static_cast< double >( settled - from );
				key.q          = Slerp( key.q, finals[ i ].q, w );
				key.x          = key.x + ( finals[ i ].x - key.x ) * w;
			}
		}
	}
	out.valid = true;
	return out;
}

std::vector< std::array< int, 2 > > Tuples( const std::function< bool( int, int ) >& accept )
{
	std::vector< std::array< int, 2 > > out;
	for( int a = 1; a <= 6; ++a )
		for( int b = 1; b <= 6; ++b )
			if( accept( a, b ) )
				out.push_back( { a, b } );
	return out;
}
} // namespace

physics::World MakeWorld( const Request& r )
{
	physics::World world;
	const double e             = r.restitution >= 0.0 ? r.restitution : 0.45;
	world.settings.restitution = e;
	world.settings.energyGuard = r.restitution < 0.0;
	world.settings.wallRestitution = r.restitution >= 0.0 ? r.restitution : 0.55;

	physics::Plane table;
	table.normal      = { 0, 1, 0 };
	table.offset      = 0.0;
	table.friction    = 0.35;
	table.restitution = e;
	table.table       = true;
	world.planes.push_back( table );

	auto wall = [ & ]( V3 n, double offset, double restitution ) {
		physics::Plane p;
		p.normal      = n;
		p.offset      = offset;
		p.friction    = world.settings.wallFriction;
		p.restitution = restitution;
		world.planes.push_back( p );
		return world.planes.size() - 1;
	};
	const size_t back = wall( { 0, 0, 1 }, kBackWall, r.restitution >= 0.0 ? r.restitution : 0.6 );
	if( r.pyramids )
	{
		world.planes[ back ].pyramidPitch  = kPyramidPitch;
		world.planes[ back ].pyramidHeight = kPyramidHeight;
		world.planes[ back ].u             = { 1, 0, 0 };
		world.planes[ back ].w             = { 0, 1, 0 };
		world.planes[ back ].friction      = 0.5;
	}
	wall( { 0, 0, -1 }, -kNearWall, world.settings.wallRestitution );
	wall( { 1, 0, 0 }, -kSideWall, world.settings.wallRestitution );
	wall( { -1, 0, 0 }, -kSideWall, world.settings.wallRestitution );
	return world;
}

int Decide( int point, int total )
{
	if( point == 0 )
		return ( total == 7 || total == 11 ) ? 1 : ( total == 2 || total == 3 || total == 12 ) ? -1 : 0;
	return total == point ? 1 : total == 7 ? -1 : 0;
}

int PointAfter( int point, int total )
{
	if( point == 0 )
		return ( total >= 4 && total <= 10 && total != 7 ) ? total : 0;
	return ( total == point || total == 7 ) ? 0 : point;
}

std::array< int, 2 > DrawDice( const Request& r )
{
	Stream s( Hash( r.seed, r.play, 0xc4a95u ) );
	const int p = r.point;
	std::function< bool( int, int ) > accept;
	switch( r.result )
	{
	case Result::Random: return { 1 + s.Below( 6 ), 1 + s.Below( 6 ) };
	case Result::Win:
		accept = [ p ]( int a, int b ) { return Decide( p, a + b ) > 0; };
		break;
	case Result::Lose:
		accept = [ p ]( int a, int b ) { return Decide( p, a + b ) < 0; };
		break;
	case Result::Jackpot:
		if( p == 0 )
			accept = []( int a, int b ) { return a + b == 11; };
		else if( p % 2 == 0 )
			accept = [ p ]( int a, int b ) { return a == b && a + b == p; };
		else
			accept = [ p ]( int a, int b ) { return a + b == p; };
		break;
	case Result::NearMiss:
		if( p == 0 )
			accept = []( int a, int b ) { return a + b == 6 || a + b == 8; };
		else
			accept = [ p ]( int a, int b ) { return ( a + b == p - 1 || a + b == p + 1 ) && a + b != 7; };
		break;
	case Result::Fixed:
	default:
	{
		const int total = std::clamp( r.fixed, 2, 12 );
		accept          = [ total ]( int a, int b ) { return a + b == total; };
		break;
	}
	}
	const auto options = Tuples( accept );
	if( options.empty() )
		return { 1 + s.Below( 6 ), 1 + s.Below( 6 ) };
	return options[ static_cast< size_t >( s.Below( static_cast< int >( options.size() ) ) ) ];
}

Plan MakePlan( const Request& r, const std::atomic< bool >* cancel )
{
	const auto start = std::chrono::steady_clock::now();
	Plan plan;
	plan.request = r;
	plan.values  = DrawDice( r );
	plan.total   = plan.values[ 0 ] + plan.values[ 1 ];
	plan.sweep   = r.havePrevious ? 0.35 : 0.0;

	const double want = std::max( 0.3, r.duration - plan.sweep );
	double speed      = std::clamp( 1.0 + 0.6 * want, kSpeedLow, kSpeedHigh );
	Attempt best;
	double bestError = 1e30;
	for( int trial = 0; trial < kMaxTrials; ++trial )
	{
		++plan.trials;
		const uint32_t sub = Hash( r.seed, r.play, static_cast< uint32_t >( trial ), 0x7417u );
		Attempt attempt    = Throw( r, speed, sub, cancel );
		if( attempt.cancelled )
			return Plan {};
		if( attempt.cocked )
			++plan.cocked;
		if( attempt.valid && !attempt.hitWall )
		{
			++plan.missedWall;
			speed = std::min( kSpeedHigh, speed * 1.25 );
			continue;
		}
		if( !attempt.valid || attempt.natural < 0.05 )
			continue;
		const double error = std::fabs( std::log( attempt.natural / want ) );
		if( error < bestError )
		{
			bestError  = error;
			best       = attempt;
			plan.speed = speed;
		}
		//Dice settle in about a second however hard they are thrown, so a long
		//Spin Time is slow motion; accept anything inside the warp's comfort.
		const double warp = attempt.natural / want;
		if( warp > 0.35 && warp < 1.6 )
			break;
	}
	if( !best.valid )
	{
		Plan rest   = MakeRest( r );
		rest.values = plan.values;
		rest.total  = plan.total;
		rest.trials = plan.trials;
		return rest;
	}

	plan.natural  = best.natural;
	plan.hitWall  = best.hitWall;
	plan.stats    = best.stats;
	plan.playback = MakePlayback( plan.sweep + best.natural, r.duration, r.noWarp );
	const geo::Solid& cube = geo::GetSolid( geo::Shape::Cube );
	const geo::Labelling& labels = geo::GetLabelling( geo::DieType::D6, 0 );
	for( int i = 0; i < 2; ++i )
	{
		Track& t   = plan.dice[ static_cast< size_t >( i ) ];
		t          = best.tracks[ static_cast< size_t >( i ) ];
		t.result   = plan.values[ static_cast< size_t >( i ) ];
		t.target   = geo::ItemForResult( geo::DieType::D6, 0, t.result );
		t.landed   = geo::TopItem( cube, labels.atVertex, ToMatrix( t.keys.back().q ) );
		const auto options = geo::SymmetriesTaking( cube, labels.atVertex, t.target, t.landed );
		const int choice   = options.empty() ? 0 : options[ Hash( r.seed, r.play, static_cast< uint32_t >( i ), 0x5e77u ) % options.size() ];
		t.S                = r.identitySymmetry ? M3 {} : cube.group[ static_cast< size_t >( choice ) ];
	}

	const int decision   = Decide( r.point, plan.total );
	plan.pointAfter      = PointAfter( r.point, plan.total );
	plan.outcome.value   = plan.total;
	plan.outcome.win     = decision > 0;
	const bool hard      = plan.values[ 0 ] == plan.values[ 1 ];
	plan.outcome.jackpot = decision > 0 && ( ( r.point == 0 && plan.total == 11 ) || ( r.point != 0 && hard && r.point % 2 == 0 ) );
	plan.outcome.nearMiss = decision == 0 && ( r.point == 0 ? ( plan.total == 6 || plan.total == 8 ) : std::abs( plan.total - r.point ) == 1 );
	plan.outcome.payout  = decision > 0 ? 1 : 0;
	std::string text;
	if( r.point == 0 )
		text = decision > 0 ? std::to_string( plan.total ) + ( plan.total == 11 ? " YO" : " WINNER" )
		     : decision < 0 ? "CRAPS " + std::to_string( plan.total )
		                    : "POINT " + std::to_string( plan.total );
	else
		text = decision > 0 ? std::to_string( plan.total ) + ( hard ? " HARD" : " WINNER" ) : decision < 0 ? "SEVEN OUT" : std::to_string( plan.total );
	plan.outcome.text = text;
	plan.ms = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count();
	return plan;
}

Plan MakeRest( const Request& r )
{
	Plan plan;
	plan.request = r;
	plan.idle    = true;
	plan.values  = DrawDice( r );
	plan.total   = plan.values[ 0 ] + plan.values[ 1 ];
	plan.pointAfter = r.point;
	plan.sweep   = 0.0;
	plan.playback = MakePlayback( 0.0, 0.0 );
	plan.playback.duration = 0.0;
	if( r.havePrevious )
	{
		//The last roll's dice, where they lay.
		for( int i = 0; i < 2; ++i )
		{
			plan.dice[ static_cast< size_t >( i ) ].keys = { r.previous[ static_cast< size_t >( i ) ] };
			plan.dice[ static_cast< size_t >( i ) ].S    = M3 {};
		}
		return plan;
	}
	//Nothing thrown yet: two dice by the near rail, showing what Result would.
	const geo::Solid& cube = geo::GetSolid( geo::Shape::Cube );
	for( int i = 0; i < 2; ++i )
	{
		Track& t      = plan.dice[ static_cast< size_t >( i ) ];
		t.result      = plan.values[ static_cast< size_t >( i ) ];
		t.target      = geo::ItemForResult( geo::DieType::D6, 0, t.result );
		const V3 up   = cube.faces[ static_cast< size_t >( t.target ) ].normal;
		const M3 rot  = AxisAngle( { 0, 1, 0 }, 0.3 + 0.5 * i ) * Align( up, { 0, 1, 0 } );
		Pose pose;
		pose.q = FromMatrix( rot );
		pose.x = { -0.025 + 0.05 * i, cube.inradius, 0.12 };
		t.keys = { pose };
		t.S    = M3 {};
	}
	return plan;
}

bool Plan::DieAt( int i, double seconds, V3& x, M3& r ) const
{
	const Track& t = dice[ static_cast< size_t >( std::clamp( i, 0, 1 ) ) ];
	if( t.keys.empty() )
		return false;
	if( idle )
	{
		x = t.keys.back().x;
		r = ToMatrix( t.keys.back().q ) * t.S;
		return true;
	}
	const double simT = playback.SimTime( seconds ) - sweep;
	if( simT < 0.0 )
		return false;
	const double f    = simT * kKeyRate;
	const size_t last = t.keys.size() - 1;
	if( f >= static_cast< double >( last ) )
	{
		x = t.keys[ last ].x;
		r = ToMatrix( t.keys[ last ].q ) * t.S;
		return true;
	}
	const size_t k = static_cast< size_t >( std::floor( f ) );
	const double u = f - static_cast< double >( k );
	x              = t.keys[ k ].x + ( t.keys[ k + 1 ].x - t.keys[ k ].x ) * u;
	r              = ToMatrix( Slerp( t.keys[ k ].q, t.keys[ k + 1 ].q, u ) ) * t.S;
	return true;
}

bool Plan::OldDieAt( int i, double seconds, V3& x, M3& r ) const
{
	if( idle || !request.havePrevious || sweep <= 0.0 )
		return false;
	const double simT = playback.SimTime( seconds );
	if( simT >= sweep )
		return false;
	const Pose& p = request.previous[ static_cast< size_t >( std::clamp( i, 0, 1 ) ) ];
	x             = p.x + V3 { 0.0, 0.0, 0.7 * Smooth( simT / sweep ) };
	r             = ToMatrix( p.q );
	return true;
}

} // namespace jackpot::craps
