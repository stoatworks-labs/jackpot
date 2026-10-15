#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jackpot
{
struct GlyphAtlas;
}

/**
    What is painted on the money wheel: the Big Six, or the lines of a Wheel
    File (jackpot#1, asked for by VJPandaAU).

    ## A wheel file

    A .txt, one value per line, read as UTF-8 (a BOM is fine; UTF-16 with a BOM
    is read too; a file that is not UTF-8 is read as Windows-1252, which is what
    an old Notepad saved). CRLF, LF and CR all end a line; blank lines are
    skipped but still counted, so a line's number is the one an editor shows.

    - A leading `*` makes the segment the jackpot style, a leading `%` the joker
      style, and the marker is not printed. Neither character is in the
      plugin's type, so there is no literal `*` or `%` to escape.
    - Lowercase is printed as capitals; accented Latin letters lose their
      accents (Café -> CAFE); curly quotes and dashes become ' and -; anything
      else outside the atlas (digits, A-Z, `$'+-.:/!` and the space) is
      dropped.
    - One of the slot machine's emoji alone on a line (🍒 🍋 🍊 🔔 🍉 💎 7️⃣)
      paints that reel symbol on the segment. Emoji inside text are dropped
      like any other character the type does not have.

    ## The wheel

    `Segments` segments (2 to 54). With more values than segments, Lines picks
    them: the first ones in order, or a draw. The draw shuffles every value by
    Seed and takes the first Segments, so it changes only when the file, Seed,
    Segments, Lines or Short File does -- never from spin to spin. With fewer
    values than segments, Short File repeats them round the wheel, or makes the
    wheel that many segments (at least 2).

    The values go round CLOCKWISE from segment 0, so a wheel turning
    anticlockwise (the way it is pulled) brings them past the clapper in order.
*/
namespace jackpot::wheel
{
constexpr int kSegments    = 54;///< the Big Six, and Segments' maximum
constexpr int kMinSegments = 2;
constexpr int kMaxValues   = 1000;           ///< lines past this many values are ignored
constexpr size_t kMaxFileBytes = 1024 * 1024;///< a wheel file is a short list
constexpr int kMaxLabel    = 32;             ///< characters a label can ever have (2 segments)

enum class Mark
{
	None = 0,
	Jackpot,///< a leading '*': the top prize
	Joker,  ///< a leading '%': a win
};

enum class Lines
{
	InOrder = 0,
	Random,
	Count
};

enum class ShortFile
{
	Repeat = 0,///< the values again round the wheel
	Fewer,     ///< a wheel of as many segments as there are values (at least 2)
	Count
};

/// The colours a segment can be painted. 0-4 are the Big Six's money colours
/// ($1 yellow, $2 blue, $5 purple, $10 green, $20 orange), which a file's
/// plain values cycle through. = mirrored in Shaders.cpp (kWheel's STYLE_).
enum Style : int
{
	StyleOne = 0,
	StyleTwo,
	StyleFive,
	StyleTen,
	StyleTwenty,
	StyleBigSixJoker,///< black, a gold star, gold print (v0.1.1's joker)
	StyleJackpot,    ///< black, a gold star, gold print (v0.1.1's logo, and '*')
	StyleJoker,      ///< red, a gold star, gold print ('%')
	StyleCount
};

/// One value from a file.
struct Entry
{
	std::string label;///< atlas characters only, before any cut for the segment count
	int symbol = 0;   ///< a reel symbol (slots::Symbol) in place of the text, 0 none
	Mark mark  = Mark::None;
	int line   = 0;   ///< the file's line, counted from 1 as an editor counts them
};

/// A file's values, and what was noticed reading it (for the log).
struct FileContents
{
	std::vector< Entry > entries;
	std::string encoding;
	std::vector< std::string > notes;
	int marked = 0;
	int blank  = 0;///< values with nothing the type can print (Cyrillic, say): blank segments
};

/// The wrong models `jptest --negative` puts in (never set in the plugin).
struct ReadTestFlags
{
	bool keepBomAndCr = false;///< the BOM and the CR of a CRLF kept as text
	bool noMarkers    = false;///< a leading '*' or '%' is just a character
};

/// Bytes to text: BOM, UTF-16, UTF-8, Windows-1252; a file with NUL bytes or
/// mostly control characters is refused. Never throws.
bool DecodeText( const unsigned char* bytes, size_t size, std::u32string& text, std::string& encoding, std::string& why,
                 const ReadTestFlags& flags = {} );

/// Text to values: lines, markers, emoji, the cleaned labels.
FileContents ParseText( const std::u32string& text, const ReadTestFlags& flags = {} );

/// Decode and parse. False (with `why`) when there is nothing to draw.
bool ReadWheelText( const unsigned char* bytes, size_t size, FileContents& out, std::string& why, const ReadTestFlags& flags = {} );

/// Read a file by its UTF-8 path (FileBytes.h), decode and parse it.
bool LoadWheelFile( const std::string& utf8Path, FileContents& out, std::string& why );

/// A raw label as the atlas can print it (see above), uncut: the face cuts it
/// for its segment count.
std::string CleanLabel( const std::u32string& raw );
/// The reel symbol when `raw`, trimmed, is exactly one mapped emoji; else 0.
int EmojiSymbol( const std::u32string& raw );
/// The symbol's name in capitals, for the result board ("CHERRY").
std::string SymbolLabel( int symbol );

//---------------------------------------------------------------------------
// The wheel as laid out.
//---------------------------------------------------------------------------
struct Segment
{
	int value = 0;     ///< the Big Six's value (40 the joker, 45 the logo); 0 on a file's wheel
	std::string label; ///< what is printed, cut for the segment count
	std::string board; ///< what the result board says (the whole line, to kMaxLabel)
	int symbol = 0;    ///< a reel symbol printed in place of the label
	Mark mark  = Mark::None;
	int line   = 0;    ///< the file's line, 0 on the Big Six
	int style  = 0;

	bool operator==( const Segment& o ) const
	{
		return value == o.value && label == o.label && board == o.board && symbol == o.symbol && mark == o.mark && line == o.line && style == o.style;
	}
};

struct Face
{
	std::vector< Segment > segment;///< index 0 at the wheel's angle, anticlockwise
	bool fromFile = false;

	int Count() const
	{
		return static_cast< int >( segment.size() );
	}
	/// v0.1.1's wheel: 54 Big Six segments, drawn by the shader's own layout.
	bool IsBigSix54() const
	{
		return !fromFile && Count() == kSegments;
	}
	bool operator==( const Face& o ) const
	{
		return fromFile == o.fromFile && segment == o.segment;
	}
};

/// The Big Six with `segments` segments: the joker at segment 0, the logo
/// opposite it (from 4 segments up), the money values between them in the
/// Big Six's order. 54 is v0.1.1's wheel exactly.
Face BigSix( int segments );
/// The shared v0.1.1 wheel, for requests that do not say.
const std::shared_ptr< const Face >& DefaultFace();

struct FaceTestFlags
{
	bool sattolo = false;///< the shuffle's off-by-one: j drawn below i, not i + 1
	uint32_t salt = 0;   ///< mixed into the draw's seed (--wheel-draw's per-spin negative)
};

/// A file's values on a wheel of `segments`, as described above. `file` must
/// have at least one entry.
Face FromFile( const FileContents& file, int segments, Lines lines, ShortFile shortFile, uint32_t seed, const FaceTestFlags& flags = {} );

/// The most characters a label keeps on a wheel of `segments`.
int MaxLabelChars( int segments );
/// Up to 12 segments the label is set across the segment, level when it is at
/// the top; from 13 on it is stacked, a character under another (as v0.1.1).
bool SetAcross( int segments );
/// The physics' pegs per segment: round( 54 / segments ), so 37 to 72 pegs.
int PegsPerSegment( int segments );

//---------------------------------------------------------------------------
// Fitting a label to its segment. Metres from the hub; = mirrored in
// Shaders.cpp (kWheel). The type shrinks until every character is inside the
// wedge, kMargin clear of the dividers, between the bands.
//---------------------------------------------------------------------------
constexpr float kTextTop    = 0.775f;///< a stacked label's first row starts here (or nearer the hub, for big type)
constexpr float kTextFloor  = 0.30f; ///< nothing nearer the hub than this...
constexpr float kStarClear  = 0.41f; ///< ... or, on a marked segment, this (the star is at 0.36)
constexpr float kRimClear   = 0.794f;///< the outer gold band's inside edge
constexpr float kLead       = 1.12f; ///< a stacked row, in character heights
constexpr float kMargin     = 0.010f;///< ink to a divider's centre line
constexpr float kStackedMax = 0.11f; ///< (v0.1.1's $1 is 0.07, on 54)
constexpr float kAcrossMax  = 0.10f;
constexpr float kAcrossMaxFew = 0.15f;///< up to four segments
constexpr float kSymbolMax  = 0.15f; ///< half a reel symbol's tile
constexpr float kSlack      = 0.25f; ///< character heights of ink allowed above and below a capital

struct LabelFit
{
	bool across  = false;
	float h      = 0.0f;  ///< a character's height (a capital's), metres
	float anchor = 0.0f;  ///< stacked: the first row's top; across: the line's middle; a symbol: its middle
	float inkLeft = 0.0f, inkWidth = 0.0f;///< across: the ink's extent from the pen's start, in heights
	std::vector< int > glyphs;            ///< atlas cells, -1 a space
	int symbol = 0;
	float symbolHalf = 0.0f;              ///< half the symbol's tile, metres
};

struct FitTestFlags
{
	bool noShrink = false;///< every label at its largest size: the negative control
};

std::vector< LabelFit > FitLabels( const Face& face, const GlyphAtlas& atlas, const FitTestFlags& flags = {} );

} // namespace jackpot::wheel
