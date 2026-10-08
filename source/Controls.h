#pragma once

/**
    The host's parameters, and what they mean in physical units.

    Every ranged parameter the host sees is 0..1, because `SetParamInfo` clamps
    an `FF_TYPE_STANDARD` default into 0..1 before `SetParamRange` could widen
    it. The conversions live in Controls.cpp, one function per control. Option,
    boolean, event and integer parameters hold the value itself.

    Units: metres, seconds, radians unless a name says degrees.
*/
namespace jackpot
{
/**
    Parameter ids. **Append only**: FFGL's ABI is by index, and other hosts than
    Arena may store compositions that way.

    The Over group sits AFTER the About block, on purpose: the source declares
    ids 0 .. PT_SOURCE_COUNT-1 and the effect all of them, so both share every id
    they have in common and the About static_assert holds for both.
*/
enum ParamId : unsigned int
{
	// -- Game ----------------------------------------------------------------
	PT_GAME = 0,
	PT_PLAY,
	PT_SPIN_TIME,
	PT_LAND_ON,
	PT_RESULT,
	PT_FIXED_NUMBER,
	PT_SEED,
	PT_AUTO_PLAY,
	PT_INTERVAL,

	// -- Slots ---------------------------------------------------------------
	PT_REELS,
	PT_SYMBOLS,
	PT_STRIP,
	PT_BLUR,
	PT_REEL_BOUNCE,

	// -- Roulette ------------------------------------------------------------
	PT_WHEEL,
	PT_ROTOR_SPEED,
	PT_DEFLECTORS,

	// -- Money Wheel ---------------------------------------------------------
	PT_CLAPPER,

	// -- Craps ---------------------------------------------------------------
	PT_PYRAMIDS,
	PT_PUCK,

	// -- Lottery -------------------------------------------------------------
	PT_BALLS,
	PT_DRAW,
	PT_AIR,

	// -- Shower --------------------------------------------------------------
	PT_SHOWER,
	PT_SHOWER_NOW,
	PT_PATTERN,
	PT_PIECES,
	PT_AMOUNT,
	PT_PIECE_SIZE,
	PT_PILE,

	// -- Look ----------------------------------------------------------------
	PT_BACKDROP,
	PT_FELT_R,
	PT_FELT_G,
	PT_FELT_B,
	PT_ACCENT_R,
	PT_ACCENT_G,
	PT_ACCENT_B,
	PT_LIGHTS,
	PT_DISPLAY,
	PT_FONT,
	PT_FONT_FILE,
	PT_FONT_NAME,
	PT_LIGHT_ANGLE,
	PT_TILT,
	PT_ZOOM,

	// -- The Stoatworks About block --------------------------------------------
	PT_ABOUT_TEXT,
	PT_ABOUT_BUTTON_1,
	PT_ABOUT_BUTTON_2,
	PT_ABOUT_BUTTON_3,
	PT_SOURCE_COUNT,

	// -- Over (the effect only) ------------------------------------------------
	PT_MIX = PT_SOURCE_COUNT,

	PT_COUNT_ALL
};

enum class Game
{
	Slots = 0,
	Roulette,
	MoneyWheel,
	Craps,
	Lottery,
	ShowerOnly,
	Count
};

enum class LandOn
{
	Time = 0,
	Beat,
	Bar,
	Count
};

/// What the next play shows. Each game reads these against "the player's bet"
/// -- see README "Results" for the table.
enum class Result
{
	Random = 0,
	Win,
	Jackpot,
	NearMiss,
	Lose,
	Fixed,
	Count
};

/// `Clip` exists on the effect only (it is the input), so it is last and the
/// source declares one element fewer.
enum class SymbolSet
{
	Fruit = 0,
	SevensAndBars,
	Clip,
	Count
};

enum class Strip
{
	Virtual = 0,///< a 64-stop virtual reel over the 22 physical stops (Telnaes)
	Physical,   ///< every physical stop equally likely
	Count
};

enum class RouletteWheel
{
	European = 0,///< 37 pockets, single zero
	American,    ///< 38 pockets, 0 and 00
	Count
};

enum class ShowerWhen
{
	Off = 0,
	OnWin,
	OnJackpot,
	Always,
	Count
};

enum class Pattern
{
	Rain = 0,
	Fountain,
	Burst,
	Pour,
	Count
};

enum class Pieces
{
	Chips = 0,
	Coins,
	Mixed,
	Count
};

enum class Backdrop
{
	None = 0,///< transparent on the source; the clip, on the effect
	Felt,
	Dark,
	Count
};

constexpr int kMaxFixed = 99;
constexpr int kMinBalls = 10, kMaxBalls = 75;
constexpr int kMaxDraw  = 7;

//---------------------------------------------------------------------------
// The mappings.
//---------------------------------------------------------------------------

/// Seconds from Play to rest, 1 to 20, geometric.
double SpinTimeFromParam( float v );
float ParamFromSpinTime( double seconds );
/// Seconds between automatic plays, 2 to 120, geometric.
double IntervalFromParam( float v );
float ParamFromInterval( double seconds );

/// The camera's exposure as a fraction of the frame, 0 (sharp) to 1 (a 360 shutter).
double BlurFromParam( float v );
/// The reel's damping ratio at the stop: 1 (dead) at 0, 0.18 (lively) at 1.
double ReelDampingFromParam( float v );

/// The rotor's initial speed, rad/s, 0 to 4 (about 38 rpm).
double RotorSpeedFromParam( float v );
/// The clapper's torsion stiffness, N m / rad, 2 to 40, geometric.
double ClapperFromParam( float v );
/// The blower's lift as a multiple of g at the jet's core, 0.5 to 4.
double AirFromParam( float v );

/// Pieces launched per second, 0 to 400, quadratic (the low end is where it is set).
double AmountFromParam( float v );
/// A chip's diameter as a fraction of the frame's height, 0.02 to 0.2, geometric.
double PieceSizeFromParam( float v );
float ParamFromPieceSize( double fraction );

/// The key light's azimuth, -180 to 180 degrees.
double LightAngleFromParam( float v );
/// The camera's elevation, 20 (low) to 90 (straight down) degrees, for the
/// games seen from above (roulette, craps, lottery, the shower's table).
double TiltFromParam( float v );
/// Framing, 0.5 (wide) to 2.5 (close), geometric; 1 at 0.4.
double ZoomFromParam( float v );

/// Option parameters arrive as the element's value; the count is how many
/// elements, and anything outside rounds to the nearest end.
int OptionIndex( float value, int count );

} // namespace jackpot
