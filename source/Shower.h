#pragma once

#include "Common.h"
#include "Controls.h"

#include <vector>

/**
    Chips and coins, showered: thin rigid discs, live.

    Unlike the games the shower is not planned -- nobody needs it to land on
    anything -- so it is stepped every frame at 1.2 kHz from the plugin's own
    clock: gravity, quadratic air drag, torque-free tumbling with the
    gyroscopic term (an axisymmetric disc's spin axis wobbles round as a coin's
    does in the air), and contact with the table at the lowest point of each
    rim: an impulse with restitution and Coulomb friction, solved twice round.

    What falls out of the contact alone: a chip that lands on its edge rolls; a
    rolling disc leans, and as it leans its point of contact runs round the rim
    faster and faster -- Euler's disk, the coin's whirr -- until it slaps flat.
    `jptest --shower` measures that rate rising as the tilt falls.

    A disc in contact and still for a quarter of a second is taken out of the
    simulation and laid on a height field, which is what the pile is: later
    pieces land on it. Within 16 degrees of flat it is laid flat; steeper (it
    stopped leaning on a step of the pile) it is laid as it lies. That is the
    one place this is not rigid-body physics, said so in AGENTS.md.

    The pile does not last (jackpot#3): every piece fades kLinger seconds after
    it was thrown, oldest first, and when the pool is nearly full the oldest
    fade early (kCapPieces), so the shower never stops for want of room. A
    piece that rolls off the table, in x or in z, is gone. When a piece in the
    pile goes, the height field is rebuilt from the pieces still in it and they
    come down onto it, so nothing lands on, or lies on, a piece that is not
    there. `jptest --pile` measures all four.

    Units: the world is in frame heights at the stage (y up, the frame from
    y = -0.5 to 0.5, z toward the camera); physics scales by the piece's real
    size, so a big (close) chip falls slowly across the frame and a small one
    fast, as they would.
*/
namespace jackpot
{
enum class PieceKind : int
{
	Chip = 0,
	Coin,
};

struct Piece
{
	V3 x, v;         ///< frame heights, frame heights per second
	Quat q;          ///< body to world; the body's z is the disc's axis
	V3 w;            ///< rad/s, world
	PieceKind kind = PieceKind::Chip;
	int colour     = 0;///< a chip's denomination (0..5)
	double radius  = 0.02;
	double half    = 0.003;///< half the thickness
	double still   = 0.0;
	double age     = 0.0;///< seconds since it was thrown
	double fade    = 1.0;///< opacity: below 1 it is on its way out
	double fadeRate = 0.0;///< per second, 0 until it starts to fade
	long rested    = -1;///< the order it came to rest in, -1 until it does
	bool resting   = false;
};

struct ShowerSettings
{
	Pattern pattern  = Pattern::Rain;
	Pieces pieces    = Pieces::Mixed;
	double rate      = 60.0;  ///< pieces per second while showering
	double size      = 0.05;  ///< a chip's diameter, frame heights
	bool pile        = true;
	double aspect    = 16.0 / 9.0;
	double restitution = -1.0;///< < 0: the material's own (negative control: > 1)
	bool noDrag      = false;
	bool noGyro      = false;///< negative control: the torque-free tumble without its gyroscopic term
	bool ballInertia = false;///< negative control: a disc given a ball's moments of inertia
	bool noAgeing    = false;///< negative control: nothing fades for its age or a full pool, so the pile never drains
	bool noEdgeCull  = false;///< negative control: the table runs on for ever in z (0.1.0's)
	bool noRebuild   = false;///< negative control: the height field only ever rises (0.1.0's)
};

class Shower
{
public:
	static constexpr double kDt       = 1.0 / 1200.0;
	static constexpr int kMaxPieces   = 900;
	/**
	    How long a piece lasts, from the moment it is thrown: 12 s.

	    Most pieces fall and settle within two seconds, so they lie in the pile
	    for about ten. That outlasts the next play after a win at the defaults
	    (Auto Play every 10 s, Spin Time 4 s), so a run of wins keeps one pile
	    that renews itself; yet a single win's pile -- 2.5 s of shower, 3.5 s
	    for a jackpot -- is gone about 15 s after the shower began, and the
	    last of the old kind 12.5 s after a change of Pieces. Much shorter and
	    a win's pile is gone before the next result lands; much longer and the
	    old kind hangs about for half a minute after Pieces is changed, which
	    is what jackpot#3 saw (for ever, in 0.1.0). Fixed in 0.1.1, not a
	    control.
	*/
	static constexpr double kLinger   = 12.0;
	static constexpr double kFadeTime = 0.5;///< s: a piece's fade at the end of its linger, or when Pile goes off
	/// The cap: past 90% of the pool, counting every piece wherever it is, the
	/// oldest fade early, in kQuickFade. 90 places, each free again in 0.2 s,
	/// pass 450 pieces a second -- more than Amount's 400 -- so the pool never
	/// fills and the shower never stalls.
	static constexpr int kCapPieces   = kMaxPieces * 9 / 10;
	static constexpr double kQuickFade = 0.2;///< s: the cap's fade, and a piece's over the table's edge
	static constexpr double kFloor    = -0.47;///< frame heights: the table's edge, just above the frame's bottom
	/// The table's depth either side of the middle (z), frame heights: the
	/// height field's, and past it (plus a piece's radius) a piece is gone.
	static constexpr double kTableHalfZ = 0.6;
	static constexpr double kChipMetres = 0.039;
	/// Quadratic drag per metre of the piece's real size, face on and edge on.
	static constexpr double kCoinFace = 0.037, kChipFace = 0.079, kEdge = 0.009;

	ShowerSettings settings;

	/// Shower for `seconds` (from now), at the settings' rate.
	void Start( double seconds, double rateScale = 1.0 );
	void Stop();
	bool Showering() const
	{
		return remaining > 0.0;
	}
	/// Advance by dt seconds (substepped). `continuous` showers regardless.
	void Advance( double dt, bool continuous );
	void Clear();

	const std::vector< Piece >& Pieces() const
	{
		return pieces;
	}
	std::vector< Piece >& MutablePieces()
	{
		return pieces;
	}
	/// The table's height under (x, z), pile included.
	double FloorAt( double x, double z ) const;
	/// Physics metres per frame height for a piece of this radius.
	double MetresPerUnit( const Piece& p ) const;
	uint32_t seed = 0;
	long spawned  = 0;
	long culled   = 0; ///< pieces gone off the frame or the table
	long faded    = 0; ///< pieces faded out: their linger, the cap, Pile off, the table's edge
	long contactSteps = 0; ///< steps in which a piece touched the table or the pile
	long energyRises = 0;  ///< contacts that left a piece with more kinetic energy than they found
	double worstSnap = 0.0;///< degrees: the most a piece was turned to lie flat as it settled
	double worstRise = 0.0;

	/// Step one piece by kDt: the physics, for the checks.
	void StepPiece( Piece& p );
	Piece Spawn();

private:
	void Settle( Piece& p );
	/// Raise the height field under a piece at rest to its top.
	void Stamp( const Piece& p );
	/// The height field again from the pieces at rest, in the order they came
	/// to rest, each brought down onto what is under it now.
	void Rebuild();

	std::vector< Piece > pieces;
	double remaining = 0.0, rateScale = 1.0, owed = 0.0, carry = 0.0;
	long restings = 0;
	bool pileWas  = true;
	std::vector< float > heights;
	static constexpr int kGrid = 96;
	static constexpr double kGridHalfX = 1.2, kGridHalfZ = kTableHalfZ;
};

} // namespace jackpot
