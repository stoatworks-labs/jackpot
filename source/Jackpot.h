#pragma once

#include "Controls.h"
#include "Craps.h"
#include "Lottery.h"
#include "MoneyWheel.h"
#include "Roulette.h"
#include "Shower.h"
#include "Slots.h"
#include "SymbolArt.h"
#include "Transport.h"
#include "Typeface.h"

#include <FFGLSDK.h>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

#include <array>
#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace jackpot
{
/// Everything one play is: the game it was, its plan, and when it began.
struct Play
{
	Game game = Game::Slots;
	slots::Plan slots;
	roulette::Plan roulette;
	wheel::Plan wheel;
	craps::Plan craps;
	lottery::Plan lottery;

	const Playback& Clock() const;
	const Outcome& Result() const;
	bool Idle() const;
};

/**
    The plugin: the source (a casino game on its own, or on nothing) and, with
    `isEffect`, the Over effect (the same, over the clip, which can also be a
    reel symbol). One class, two registrations.
*/
class JackpotPlugin : public CFFGLPlugin
{
public:
	explicit JackpotPlugin( bool isEffect );
	~JackpotPlugin() override;

	FFResult InitGL( const FFGLViewportStruct* viewport ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* input ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	char* GetTextParameter( unsigned int index ) override;
	/// LOAD-BEARING: the base class's stub fails, and a failed default deletes
	/// the instance -- so without this no real host can load the plugin.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;
	FFResult SetTime( double time ) override;
	void SetBeatInfo( float bpm, float barPhase ) override;

	bool IsEffect() const
	{
		return isEffect;
	}
	unsigned int ParamCount() const
	{
		return isEffect ? PT_COUNT_ALL : PT_SOURCE_COUNT;
	}

	//-------------------------------------------------------------------
	// For the harness. Nothing in the plugin's own operation calls these.
	//-------------------------------------------------------------------
	/// Plan on the render thread, so a play starts on the frame it is asked
	/// for and every run is the same run.
	void SetSynchronousForTest( bool on )
	{
		synchronous = on;
	}
	void SetClockScaleForTest( double scale )
	{
		clockScale = scale;
	}
	/// Draw without lighting, glass or lamps (the readbacks).
	void SetFlatForTest( bool on )
	{
		flat = on;
	}
	const Play& CurrentPlay() const
	{
		return play;
	}
	double PlaybackSeconds() const
	{
		return clock - playStart;
	}
	int PlaysStarted() const
	{
		return playsStarted;
	}
	int RestsSeen() const
	{
		return restsSeen;
	}
	const Transport& CurrentTransport() const
	{
		return transport;
	}
	Game CurrentGame() const;
	/// The wrong models, copied into every request (`--negative`).
	struct TestFlags
	{
		bool uniformVirtual = false;
		bool noWarp         = false;
		bool noShift        = false;
		bool shiftFrets     = false;
		bool biasedDraw     = false;
		double restitution  = -1.0;
	};
	TestFlags& Flags()
	{
		return flags;
	}
	const Shower& CurrentShower() const
	{
		return shower;
	}
	Shower& MutableShower()
	{
		return shower;
	}
	const GlyphAtlas& CurrentAtlas() const
	{
		return atlas;
	}
	const SymbolAtlas& CurrentSymbols() const
	{
		return symbols;
	}
	std::string FontFamily() const
	{
		return resolvedFamily;
	}
	int Credits() const
	{
		return credits;
	}
	int CrapsPoint() const
	{
		return crapsPoint;
	}
	/// The duration the next play would be given, as it would be given now.
	double DurationForTest() const
	{
		return PlayDuration();
	}
	/// Fixed per-frame transport for the harness: bpm and bar phase.
	void SetTransportForTest( double bpm, double barPhase )
	{
		transport.SetBeatInfo( bpm, barPhase );
	}

private:
	void UpdateClock();
	void Tick( double dt );
	double PlayDuration() const;
	void StartPlay();
	void Relayout();
	void OnRest();
	void ResolveFont();
	void UploadAtlas();
	void UploadSymbols();
	void ReapAbandoned( bool wait );
	bool CompileGame( Game game, const std::string& fragment );

	slots::Request SlotsRequest() const;
	roulette::Request RouletteRequest() const;
	wheel::Request WheelRequest() const;
	craps::Request CrapsRequest() const;
	lottery::Request LotteryRequest() const;
	Play MakePlayFor( Game game, bool rest ) const;

	void SetCommonUniforms( ffglex::FFGLShader& shader, int width, int height, const FFGLTextureStruct* input );
	void SetTextUniforms( ffglex::FFGLShader& shader, const std::vector< std::string >& spans );
	void DrawSlots( ffglex::FFGLShader& shader, double t );
	void DrawRoulette( ffglex::FFGLShader& shader, double t, int width, int height );
	void DrawWheel( ffglex::FFGLShader& shader, double t );
	void DrawCraps( ffglex::FFGLShader& shader, double t, int width, int height );
	void DrawLottery( ffglex::FFGLShader& shader, double t, int width, int height );
	void DrawShower( int width, int height, GLint hostFbo );

	const bool isEffect;
	float params[ PT_COUNT_ALL ] = {};

	ffglex::FFGLShader programs[ static_cast< int >( Game::Count ) ];
	ffglex::FFGLShader showerProgram, compositeProgram;
	ffglex::FFGLScreenQuad quad;
	GLuint atlasTexture = 0, symbolTexture = 0, blankTexture = 0;
	GLuint showerFbo = 0, showerColour = 0, showerDepth = 0, showerVao = 0, showerVbo = 0, quadVbo = 0;
	int showerWidth = 0, showerHeight = 0;

	//Time. Resolume has sent both seconds and milliseconds (see boreal).
	double hostTime = -1.0, lastRawTime = -1.0, lastWallTime = -1.0, wallStart = -1.0;
	double clockScale = 0.0;
	int secondsVotes = 0, millisVotes = 0;
	double now = 0.0, lastNow = -1.0;
	double clock = 0.0;///< seconds of plugin time, monotonic, frame-relative
	double frameDt = 1.0 / 60.0;
	Transport transport;

	//Playing.
	Play play;
	double playStart       = 0.0;
	double pendingPressed  = 0.0;///< clock when the pending plan's Play was pressed
	double lastPlayRequest = -1e30;
	uint32_t playIndex     = 0;
	int playPresses        = 0;
	bool playHeld          = false;
	bool autoWas           = false;
	int playsStarted       = 0;
	int restsSeen          = 0;
	bool restHandled       = true;
	bool synchronous       = false;
	bool laidOut           = false;
	int layoutGame         = -1;
	float layoutKey[ 6 ]   = {};
	std::future< Play > pending;
	std::shared_ptr< std::atomic< bool > > pendingCancel;
	std::vector< std::future< Play > > abandoned;
	std::vector< std::shared_ptr< std::atomic< bool > > > abandonedCancel;
	TestFlags flags;
	bool flat = false;

	//What carries from play to play.
	std::array< double, slots::kMaxReels > reelStops {};
	int credits         = 100;
	int winFrom         = 0;
	double winSince     = -1e30;///< clock at the last win, for the meter's count-up and the banner
	int crapsPoint      = 0;
	double rotorAngle   = 0.0, rotorSpeed = 0.0;///< the roulette rotor between plays
	double wheelAngle   = 0.0;
	std::vector< int > lotteryShown;

	//The shower.
	Shower shower;
	int showerPresses = 0;
	bool showerHeld   = false;

	//Text.
	Typeface typeface;
	GlyphAtlas atlas;
	SymbolAtlas symbols;
	bool fontDirty = true, atlasUploadDirty = true, symbolsUploadDirty = true;
	bool fontNameSet = false;
	std::string fontFilePath, fontName, resolvedFamily;

	mutable std::mutex textMutex;
	char textReturn[ 1024 ] = {};
};

} // namespace jackpot
