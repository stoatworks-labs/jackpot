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

    A disc at rest and flat is taken out of the simulation and laid on a
    height field, which is what the pile is: later pieces land on it. That is
    the one place this is not rigid-body physics, said so in AGENTS.md.

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
	double fade    = 1.0;
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
};

class Shower
{
public:
	static constexpr double kDt       = 1.0 / 1200.0;
	static constexpr int kMaxPieces   = 900;
	static constexpr int kMaxResting  = 700;
	static constexpr double kFloor    = -0.47;///< frame heights: the table's edge, just above the frame's bottom
	static constexpr double kChipMetres = 0.039;

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
	long energyRises = 0;  ///< contacts that left a piece with more kinetic energy than they found
	double worstRise = 0.0;

	/// Step one piece by kDt: the physics, for the checks.
	void StepPiece( Piece& p );
	Piece Spawn();

private:
	void Settle( Piece& p );

	std::vector< Piece > pieces;
	double remaining = 0.0, rateScale = 1.0, owed = 0.0, carry = 0.0;
	std::vector< float > heights;
	static constexpr int kGrid = 96;
	static constexpr double kGridHalfX = 1.2, kGridHalfZ = 0.6;
};

} // namespace jackpot
