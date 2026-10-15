#include "MoneyWheel.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace jackpot::wheel
{
namespace
{
constexpr double kDt          = 2.0e-5;
constexpr double kWheelInertia = 9.0;   ///< kg m^2: a 1.8 m wheel, rim-heavy
constexpr double kClapperInertia = 9.0e-5;///< kg m^2: 20 g of leather about its pivot
constexpr double kClapperZeta = 0.08;
constexpr double kUnload      = 0.35;  ///< the leather's stiffness straightening, of its stiffness bending
constexpr double kBearing     = 0.5;    ///< N m, Coulomb
constexpr double kViscous     = 0.03;   ///< N m s
constexpr double kContactK    = 2.0e5;  ///< N/m
constexpr double kContactC    = 40.0;   ///< N s/m
constexpr double kMaxSim      = 40.0;
constexpr double kPull        = 1.0;    ///< s: the operator's pull before letting go
constexpr double kSegment     = 2.0 * kPi / kSegments;///< one of the Big Six's 54

double Wrap( double a )
{
	a = std::fmod( a, 2.0 * kPi );
	return a < 0.0 ? a + 2.0 * kPi : a;
}

struct V2
{
	double x = 0.0, y = 0.0;
};

/// The closest point of the clapper's centreline to p, and the distance.
double ClapperDistance( double psi, V2 p, V2& q )
{
	const V2 c = { 0.0, kPivotY };
	const V2 t = { kClapperLength * std::sin( psi ), kPivotY - kClapperLength * std::cos( psi ) };
	const double ex = t.x - c.x, ey = t.y - c.y;
	const double s  = std::clamp( ( ( p.x - c.x ) * ex + ( p.y - c.y ) * ey ) / ( ex * ex + ey * ey ), 0.0, 1.0 );
	q               = { c.x + s * ex, c.y + s * ey };
	return std::hypot( p.x - q.x, p.y - q.y );
}

double Hermite( double a0, double w0, double a1, double w1, double T, double t )
{
	const double s   = std::clamp( t / T, 0.0, 1.0 );
	const double h00 = 2 * s * s * s - 3 * s * s + 1, h10 = s * s * s - 2 * s * s + s;
	const double h01 = -2 * s * s * s + 3 * s * s, h11 = s * s * s - s * s;
	return h00 * a0 + h10 * T * w0 + h01 * a1 + h11 * T * w1;
}

std::vector< int > MakeLayout()
{
	//Two halves of 27, a joker and a logo opposite each other; 24 ones,
	//15 twos, 7 fives, 4 tens, 2 twenties (the Big Six's counts).
	const int half[ 26 ] = { 1, 2, 1, 5, 1, 2, 1, 10, 1, 2, 1, 5, 1, 2, 1, 20, 1, 2, 1, 5, 1, 2, 1, 10, 1, 2 };
	std::vector< int > layout;
	layout.push_back( 40 );
	for( int v : half )
		layout.push_back( v );
	layout.push_back( 45 );
	for( int i = 0; i < 26; ++i )
	{
		int v = half[ i ];
		if( i == 4 )
			v = 5;
		if( i == 12 )
			v = 2;
		layout.push_back( v );
	}
	return layout;
}
} // namespace

const std::vector< int >& Layout()
{
	static const std::vector< int > layout = MakeLayout();
	return layout;
}

std::string Name( int value )
{
	if( value == 40 )
		return "JOKER";
	if( value == 45 )
		return "LOGO";
	return "$" + std::to_string( value );
}

double ClearingDeflection( double angle, double direction, int pegs )
{
	//The pegs near the top, and the smallest swing (in the direction the
	//pegs are travelling) that keeps the clapper off all of them.
	const double pitch = 2.0 * kPi / pegs;
	auto clear = [ & ]( double psi ) {
		for( int j = 0; j < pegs; ++j )
		{
			const double a = angle + j * pitch;
			if( std::fabs( std::remainder( a - 0.5 * kPi, 2.0 * kPi ) ) > 3.0 * pitch )
				continue;
			V2 q;
			if( ClapperDistance( psi, { kPegRadius * std::cos( a ), kPegRadius * std::sin( a ) }, q ) < kPegSize + kClapperHalf )
				return false;
		}
		return true;
	};
	if( clear( 0.0 ) )
		return 0.0;
	const double sign = direction >= 0.0 ? -1.0 : 1.0;
	double lo = 0.0, hi = 1.2;
	for( int i = 0; i < 30; ++i )
	{
		const double mid = 0.5 * ( lo + hi );
		( clear( sign * mid ) ? hi : lo ) = mid;
	}
	return sign * hi;
}

double BearingWorkPerSegment()
{
	return kBearing * kSegment;
}

Simulation Simulate( const Release& r, const std::atomic< bool >* cancel )
{
	Simulation sim;
	//One peg's angle. With 54 pegs this is the Big Six's segment, computed
	//as v0.1.1 computed it (the same division), so its paths are its paths.
	const double pitch = 2.0 * kPi / r.pegs;
	const int pegs     = r.pegs;
	//The angle in pegs: whole + phase, phase in [0, 1). Only the phase
	//places the pegs, so `turns` cannot change a single step.
	long whole   = r.turns;
	double phase = r.phase + r.pegTurn;
	while( phase >= 1.0 )
	{
		phase -= 1.0;
		++whole;
	}
	while( phase < 0.0 )
	{
		phase += 1.0;
		--whole;
	}
	double omega = r.speed;
	double psi = 0.0, psiRate = 0.0;
	const double k = r.stiffness;
	const double c = 2.0 * kClapperZeta * std::sqrt( k * kClapperInertia );
	sim.startEnergy = 0.5 * kWheelInertia * omega * omega;

	//The peg at the top line, counted in whole pegs from the phase.
	const double top = 0.5 * kPi / pitch;
	auto topPeg      = [ & ]() { return static_cast< long >( std::floor( top - phase ) ) - whole; };
	double t = 0.0, nextKey = 0.0, still = 0.0;
	double lastCrossEnergy = sim.startEnergy;
	long lastTopPeg        = topPeg();
	bool touching          = false;
	const long maxSteps    = static_cast< long >( kMaxSim / kDt );
	for( long step = 0; step < maxSteps; ++step )
	{
		if( cancel != nullptr && ( step & 8191 ) == 0 && cancel->load() )
			return {};
		while( t >= nextKey - 1e-12 )
		{
			sim.angle.push_back( ( static_cast< double >( whole ) + phase ) * pitch );
			sim.phase.push_back( phase );
			sim.whole.push_back( whole );
			sim.clapper.push_back( psi );
			nextKey += 1.0 / Simulation::kKeyRate;
		}

		const double theta = phase * pitch;
		//The leather: full stiffness while a peg bends it, a third of it while
		//it straightens against one (hysteresis). Elastic both ways, a wheel too
		//slow to clear a peg was handed back all it had put in and rocked back
		//over three or four pegs, several times (AGENTS.md).
		const bool unloading = touching && psi * psiRate < 0.0;
		double torqueWheel = 0.0, torqueClapper = -k * psi * ( unloading ? kUnload : 1.0 ) - c * psiRate;
		touching = false;
		//Only the pegs within three pitches of the top can reach the clapper.
		const int nearest = static_cast< int >( std::lround( ( 0.5 * kPi - theta ) / pitch ) );
		for( int dj = -3; dj <= 3; ++dj )
		{
			const double a = theta + ( nearest + dj ) * pitch;
			const V2 p     = { kPegRadius * std::cos( a ), kPegRadius * std::sin( a ) };
			V2 q;
			const double d     = ClapperDistance( psi, p, q );
			const double depth = kPegSize + kClapperHalf - d;
			if( depth <= 0.0 || d < 1e-12 )
				continue;
			const V2 n  = { ( p.x - q.x ) / d, ( p.y - q.y ) / d };
			const V2 vp = { -omega * p.y, omega * p.x };
			const V2 vq = { -psiRate * ( q.y - kPivotY ), psiRate * q.x };
			const double approach = -( ( vp.x - vq.x ) * n.x + ( vp.y - vq.y ) * n.y );
			const double f        = std::max( 0.0, kContactK * depth + kContactC * approach );
			touching              = true;
			torqueWheel += p.x * ( f * n.y ) - p.y * ( f * n.x );
			torqueClapper += q.x * ( -f * n.y ) - ( q.y - kPivotY ) * ( -f * n.x );
		}

		//The bearing: Coulomb with sticking, and a little viscous drag.
		const double bearing = kBearing;
		if( std::fabs( omega ) < 1e-4 && std::fabs( torqueWheel ) <= bearing )
			omega = 0.0;
		else
		{
			const double friction = ( omega > 0.0 ? -1.0 : omega < 0.0 ? 1.0 : ( torqueWheel > 0.0 ? -1.0 : 1.0 ) ) * bearing - kViscous * omega;
			const double before   = omega;
			omega += ( torqueWheel + friction ) / kWheelInertia * kDt;
			//Friction stops a wheel; it never turns it round.
			if( before != 0.0 && ( before > 0.0 ) != ( omega > 0.0 ) && std::fabs( torqueWheel ) <= bearing )
				omega = 0.0;
			if( before > 0.0 && omega < 0.0 )
				++sim.reversals;
		}
		psiRate += torqueClapper / kClapperInertia * kDt;
		phase += omega * kDt / pitch;
		//Exact for a phase in [1, 2) or [-1, 0) (Sterbenz): no rounding moves
		//between the phase and the whole pegs.
		if( phase >= 1.0 )
		{
			phase -= 1.0;
			++whole;
		}
		else if( phase < 0.0 )
		{
			phase += 1.0;
			--whole;
		}
		psi += psiRate * kDt;
		t += kDt;

		//A peg crossing the top line: the energy the passage took.
		const long peg = topPeg();
		if( peg != lastTopPeg )
		{
			const double energy = 0.5 * kWheelInertia * omega * omega;
			sim.pegLoss.push_back( lastCrossEnergy - energy );
			lastCrossEnergy = energy;
			lastTopPeg      = peg;
			++sim.pegsPassed;
		}

		if( std::fabs( omega ) < 0.01 && std::fabs( psiRate ) < 0.3 )
			still += kDt;
		else
			still = 0.0;
		if( still >= 0.4 )
		{
			sim.settled = true;
			sim.natural = t - 0.4;
			//The gap under the clapper in the paint's frame: the pegs' own
			//turn (the negative control's) taken back out.
			const long under = static_cast< long >( std::floor( top - phase + r.pegTurn ) ) - whole;
			sim.gap          = static_cast< int >( ( ( under % pegs ) + pegs ) % pegs );
			const size_t keep = static_cast< size_t >( std::ceil( sim.natural * Simulation::kKeyRate ) ) + 1;
			if( sim.angle.size() > keep )
			{
				sim.angle.resize( keep );
				sim.phase.resize( keep );
				sim.whole.resize( keep );
				sim.clapper.resize( keep );
			}
			return sim;
		}
	}
	sim.natural = t;
	return sim;
}

namespace
{
/// The bet snapped to the nearest value this wheel has (v0.1.1: of the
/// seven; a smaller Big Six can have fewer).
int SnapBet( const Face& face, int bet )
{
	const int values[] = { 1, 2, 5, 10, 20, 40, 45 };
	int best           = -1;
	for( int v : values )
	{
		bool there = false;
		for( const Segment& s : face.segment )
			there = there || s.value == v;
		if( there && ( best < 0 || std::abs( v - bet ) < std::abs( best - bet ) ) )
			best = v;
	}
	return best < 0 ? values[ 0 ] : best;
}

bool BigSixTop( int v )
{
	return v == 40 || v == 45;
}
} // namespace

int Wanted( const Request& r, bool* fellBack )
{
	const Face& face = r.face ? *r.face : *DefaultFace();
	const int n      = face.Count();
	Stream s( Hash( r.seed, r.play, 0x51c5u ) );
	bool fell  = false;
	auto pick  = [ & ]( auto accept ) {
		std::vector< int > options;
		for( int i = 0; i < n; ++i )
			if( accept( i ) )
				options.push_back( i );
		fell = options.empty();
		return options.empty() ? s.Below( n ) : options[ static_cast< size_t >( s.Below( static_cast< int >( options.size() ) ) ) ];
	};
	auto at = [ & ]( int i ) -> const Segment& { return face.segment[ static_cast< size_t >( ( ( i % n ) + n ) % n ) ]; };
	int wanted = 0;
	if( r.result == Result::Random )
		wanted = r.biasedDraw ? s.Below( n - 1 ) : s.Below( n );
	else if( !face.fromFile )
	{
		//The Big Six (v0.1.1's rules, against the values this wheel has).
		const int bet = SnapBet( face, r.bet );
		switch( r.result )
		{
		case Result::Jackpot: wanted = pick( [ & ]( int i ) { return BigSixTop( at( i ).value ); } ); break;
		case Result::NearMiss:
			wanted = pick( [ & ]( int i ) { return !BigSixTop( at( i ).value ) && ( BigSixTop( at( i + 1 ).value ) || BigSixTop( at( i - 1 ).value ) ); } );
			break;
		case Result::Lose: wanted = pick( [ & ]( int i ) { return at( i ).value != bet && !BigSixTop( at( i ).value ); } ); break;
		default: wanted = pick( [ & ]( int i ) { return at( i ).value == bet; } ); break;
		}
	}
	else
	{
		//A file: only the marked segments win, '*' is the top prize, and
		//Fixed Number is a line of the file.
		auto marked  = [ & ]( int i ) { return at( i ).mark != Mark::None; };
		auto jackpot = [ & ]( int i ) { return at( i ).mark == Mark::Jackpot; };
		switch( r.result )
		{
		case Result::Win: wanted = pick( marked ); break;
		case Result::Jackpot: wanted = pick( jackpot ); break;
		case Result::NearMiss: wanted = pick( [ & ]( int i ) { return !marked( i ) && ( jackpot( i + 1 ) || jackpot( i - 1 ) ); } ); break;
		case Result::Lose: wanted = pick( [ & ]( int i ) { return !marked( i ); } ); break;
		default: wanted = pick( [ & ]( int i ) { return at( i ).line == r.bet; } ); break;
		}
	}
	if( fellBack != nullptr )
		*fellBack = fell;
	return wanted;
}

Plan MakePlan( const Request& request, const std::atomic< bool >* cancel )
{
	const auto start = std::chrono::steady_clock::now();
	Request r = request;
	if( !r.face )
		r.face = DefaultFace();
	const Face& face = *r.face;
	const int n      = face.Count();
	const int m      = r.onePegPerSegment ? 1 : PegsPerSegment( n );
	Plan plan;
	plan.request  = r;
	plan.segments = n;
	plan.pegsPer  = m;
	plan.wanted   = Wanted( r, &plan.fellBack );
	plan.pull     = kPull;

	Stream s( Hash( r.seed, r.play, 0x77e1u ) );
	Release release;
	release.pegs      = n * m;
	release.phase     = s.Uniform();
	release.stiffness = r.stiffness;
	const double want = std::max( 1.0, r.duration - kPull );
	double speed      = std::clamp( 0.7 * want * std::sqrt( r.stiffness / 4.0 ), 1.0, 14.0 );
	Simulation best;
	double bestError = 1e30, bestSpeed = speed, lastSpeed = -1.0, lastTime = -1.0;
	for( int trial = 0; trial < 6; ++trial )
	{
		++plan.trials;
		release.speed        = speed;
		const Simulation sim = Simulate( release, cancel );
		if( cancel != nullptr && cancel->load() )
			return Plan {};
		if( !sim.settled )
		{
			speed *= 0.8;
			continue;
		}
		const double error = std::fabs( std::log( sim.natural / want ) );
		if( error < bestError )
		{
			bestError = error;
			best      = sim;
			bestSpeed = speed;
		}
		if( error < 0.05 )
			break;
		double next = speed * want / sim.natural;
		if( lastSpeed > 0.0 && std::fabs( sim.natural - lastTime ) > 1e-3 )
			next = speed + ( want - sim.natural ) * ( speed - lastSpeed ) / ( sim.natural - lastTime );
		lastSpeed = speed;
		lastTime  = sim.natural;
		speed     = std::clamp( next, 0.8, 16.0 );
	}
	release.speed = bestSpeed;
	plan.release  = release;
	plan.sim      = best;

	//The physics' gap is in pegs; the segment it is in is the gap divided by
	//m, and the paint turns by whole segments (m whole pegs each).
	const int landed = best.gap / m;
	const double segmentAngle = 2.0 * kPi / n;
	plan.shift       = r.noShift ? 0 : ( ( landed - plan.wanted ) % n + n ) % n;
	plan.paintOffset = plan.shift * segmentAngle;
	plan.paintFrom   = r.paintNow;
	//Where the paint must be when the wheel is let go, a pull of at least a
	//quarter turn on from where it stands.
	double delta = Wrap( release.Angle() + plan.paintOffset - r.paintNow );
	while( delta < 0.5 * release.speed * kPull )
		delta += 2.0 * kPi;
	plan.paintAtRelease = r.paintNow + delta;
	plan.playback       = MakePlayback( kPull + ( best.settled ? best.natural : 0.0 ), r.duration, r.noWarp );

	const int i          = plan.Shown();
	const Segment& shown = face.segment[ static_cast< size_t >( i ) ];
	const Segment& after = face.segment[ static_cast< size_t >( ( i + 1 ) % n ) ];
	const Segment& below = face.segment[ static_cast< size_t >( ( i + n - 1 ) % n ) ];
	if( !face.fromFile )
	{
		const int bet         = SnapBet( face, r.bet );
		plan.outcome.value    = shown.value;
		plan.outcome.jackpot  = BigSixTop( shown.value );
		plan.outcome.win      = shown.value == bet || plan.outcome.jackpot;
		plan.outcome.nearMiss = !plan.outcome.jackpot && ( BigSixTop( after.value ) || BigSixTop( below.value ) );
		plan.outcome.payout   = plan.outcome.win ? shown.value : 0;
		plan.outcome.text     = Name( shown.value );
	}
	else
	{
		//Only the marked segments win: a file with no markers never does.
		plan.outcome.value    = shown.line;
		plan.outcome.jackpot  = shown.mark == Mark::Jackpot;
		plan.outcome.win      = shown.mark != Mark::None || r.markedWinAny;
		plan.outcome.nearMiss = !plan.outcome.win && ( after.mark == Mark::Jackpot || below.mark == Mark::Jackpot );
		plan.outcome.payout   = plan.outcome.win ? 1 : 0;
		plan.outcome.text     = shown.board;
	}
	plan.ms = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count();
	return plan;
}

Plan MakeRest( const Request& request )
{
	Request r = request;
	if( !r.face )
		r.face = DefaultFace();
	Plan plan;
	plan.request   = r;
	plan.segments  = r.face->Count();
	plan.pegsPer   = PegsPerSegment( plan.segments );
	plan.release.pegs = plan.segments * plan.pegsPer;
	plan.idle      = true;
	plan.paintFrom = r.paintNow;
	plan.playback  = MakePlayback( 0.0, 0.0 );
	plan.playback.duration = 0.0;
	return plan;
}

double Plan::PaintAngle( double seconds ) const
{
	if( idle )
		return paintFrom;
	const double simT = playback.SimTime( seconds );
	if( simT < pull )
		return Hermite( paintFrom, 0.0, paintAtRelease, release.speed, pull, simT );
	if( sim.angle.empty() )
		return paintAtRelease;
	const double f    = ( simT - pull ) * Simulation::kKeyRate;
	const size_t last = sim.angle.size() - 1;
	double physical   = sim.angle[ last ];
	if( f < static_cast< double >( last ) )
	{
		const size_t k = static_cast< size_t >( std::floor( f ) );
		physical       = sim.angle[ k ] + ( sim.angle[ k + 1 ] - sim.angle[ k ] ) * ( f - static_cast< double >( k ) );
	}
	return physical + ( paintAtRelease - release.Angle() );
}

int Plan::Shown() const
{
	return ( ( sim.gap / pegsPer - shift ) % segments + segments ) % segments;
}

double Plan::Clapper( double seconds ) const
{
	if( idle )
		return 0.0;
	const double simT = playback.SimTime( seconds );
	if( simT < pull || sim.clapper.empty() )
		return ClearingDeflection( PaintAngle( seconds ) - paintOffset, 1.0, release.pegs );
	const double f    = ( simT - pull ) * Simulation::kKeyRate;
	const size_t last = sim.clapper.size() - 1;
	if( f >= static_cast< double >( last ) )
		return sim.clapper[ last ];
	const size_t k = static_cast< size_t >( std::floor( f ) );
	return sim.clapper[ k ] + ( sim.clapper[ k + 1 ] - sim.clapper[ k ] ) * ( f - static_cast< double >( k ) );
}

} // namespace jackpot::wheel
