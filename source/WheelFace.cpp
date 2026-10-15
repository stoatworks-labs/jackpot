#include "WheelFace.h"

#include "Common.h"
#include "FileBytes.h"
#include "MoneyWheel.h"
#include "Slots.h"
#include "Typeface.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>

namespace jackpot::wheel
{
namespace
{
//---------------------------------------------------------------------------
// Characters.
//---------------------------------------------------------------------------
bool IsSpace( char32_t c )
{
	return c == U' ' || c == U'\t' || c == U'\r' || c == U'\v' || c == U'\f' || c == 0x00A0 || c == 0x1680 || ( c >= 0x2000 && c <= 0x200A ) || c == 0x202F
	       || c == 0x205F || c == 0x3000 || c == 0xFEFF;
}

/// The naive trim the negative control reads with: spaces and tabs only.
bool IsPlainSpace( char32_t c )
{
	return c == U' ' || c == U'\t';
}

std::u32string Trim( const std::u32string& s, bool plain )
{
	auto space = [ plain ]( char32_t c ) { return plain ? IsPlainSpace( c ) : IsSpace( c ); };
	size_t a = 0, b = s.size();
	while( a < b && space( s[ a ] ) )
		++a;
	while( b > a && space( s[ b - 1 ] ) )
		--b;
	return s.substr( a, b - a );
}

/// A character the atlas has no cell for, as the nearest thing it has: the
/// accented Latin letters as their base letters, curly quotes and dashes as
/// ' and -. Empty: dropped.
std::string Fold( char32_t c )
{
	if( c >= 0x00C0 && c <= 0x00FF )
	{
		//Latin-1, upper then lower case; '*' is a letter pair, '_' dropped.
		static const char* const row = "AAAAAA*CEEEEIIIIDNOOOOO_OUUUUY**AAAAAA*CEEEEIIIIDNOOOOO_OUUUUY*Y";
		const char base = row[ c - 0x00C0 ];
		if( base == '_' )
			return {};
		if( base != '*' )
			return std::string( 1, base );
		switch( c )
		{
		case 0x00C6:
		case 0x00E6: return "AE";
		case 0x00DE:
		case 0x00FE: return "TH";
		default: return "SS";//U+00DF
		}
	}
	if( c >= 0x0100 && c <= 0x017F )
	{
		//Latin Extended-A, sixteen to a row.
		static const char* const row = "AAAAAACCCCCCCCDD"
		                               "DDEEEEEEEEEEGGGG"
		                               "GGGGHHHHIIIIIIII"
		                               "II**JJKKKLLLLLLL"
		                               "LLLNNNNNNNNNOOOO"
		                               "OO**RRRRRRSSSSSS"
		                               "SSTTTTTTUUUUUUUU"
		                               "UUUUWWYYYZZZZZZS";
		const char base = row[ c - 0x0100 ];
		if( base != '*' )
			return std::string( 1, base );
		return c == 0x0132 || c == 0x0133 ? "IJ" : "OE";
	}
	switch( c )
	{
	case 0x2018:
	case 0x2019:
	case 0x2032:
	case 0x02BC:
	case 0x00B4:
	case U'`': return "'";
	case 0x2010:
	case 0x2011:
	case 0x2012:
	case 0x2013:
	case 0x2014:
	case 0x2015:
	case 0x2212: return "-";
	case 0x2026: return "...";
	default: return {};
	}
}

/// Windows-1252's 0x80-0x9F (the rest of its upper half is Latin-1).
char32_t FromCp1252( unsigned char b )
{
	static const char16_t high[ 32 ] = { 0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
		                                 0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178 };
	if( b >= 0x80 && b < 0xA0 )
		return high[ b - 0x80 ];
	return b;
}

/// Strict UTF-8 (as files::Utf8ToUtf16 is strict) to code points.
bool DecodeUtf8( const unsigned char* b, size_t n, std::u32string& out )
{
	out.clear();
	size_t i = 0;
	while( i < n )
	{
		const unsigned char c = b[ i ];
		char32_t cp = 0, minimum = 0;
		int extra = 0;
		if( c < 0x80 )
			cp = c;
		else if( ( c & 0xE0 ) == 0xC0 )
			cp = c & 0x1F, extra = 1, minimum = 0x80;
		else if( ( c & 0xF0 ) == 0xE0 )
			cp = c & 0x0F, extra = 2, minimum = 0x800;
		else if( ( c & 0xF8 ) == 0xF0 )
			cp = c & 0x07, extra = 3, minimum = 0x10000;
		else
			return false;
		if( i + static_cast< size_t >( extra ) >= n && extra > 0 )
			return false;
		for( int k = 1; k <= extra; ++k )
		{
			const unsigned char d = b[ i + static_cast< size_t >( k ) ];
			if( ( d & 0xC0 ) != 0x80 )
				return false;
			cp = ( cp << 6 ) | ( d & 0x3F );
		}
		if( ( extra > 0 && cp < minimum ) || cp > 0x10FFFF || ( cp >= 0xD800 && cp <= 0xDFFF ) )
			return false;
		out.push_back( cp );
		i += 1 + static_cast< size_t >( extra );
	}
	return true;
}

int StyleOf( int value )
{
	switch( value )
	{
	case 1: return StyleOne;
	case 2: return StyleTwo;
	case 5: return StyleFive;
	case 10: return StyleTen;
	case 20: return StyleTwenty;
	case 40: return StyleBigSixJoker;
	default: return StyleJackpot;
	}
}
} // namespace

//---------------------------------------------------------------------------
// Reading.
//---------------------------------------------------------------------------
bool DecodeText( const unsigned char* bytes, size_t size, std::u32string& text, std::string& encoding, std::string& why, const ReadTestFlags& flags )
{
	text.clear();
	if( bytes == nullptr || size == 0 )
	{
		why = "is empty";
		return false;
	}
	//UTF-16 with a byte order mark: Notepad's "Unicode".
	if( size >= 2 && ( ( bytes[ 0 ] == 0xFF && bytes[ 1 ] == 0xFE ) || ( bytes[ 0 ] == 0xFE && bytes[ 1 ] == 0xFF ) ) )
	{
		const bool little = bytes[ 0 ] == 0xFF;
		encoding          = little ? "UTF-16 LE" : "UTF-16 BE";
		size_t controls   = 0;
		for( size_t i = 2; i + 1 < size; i += 2 )
		{
			const char32_t unit = little ? static_cast< char32_t >( bytes[ i ] | ( bytes[ i + 1 ] << 8 ) ) : static_cast< char32_t >( ( bytes[ i ] << 8 ) | bytes[ i + 1 ] );
			if( unit >= 0xD800 && unit <= 0xDBFF && i + 3 < size )
			{
				const char32_t next = little ? static_cast< char32_t >( bytes[ i + 2 ] | ( bytes[ i + 3 ] << 8 ) ) : static_cast< char32_t >( ( bytes[ i + 2 ] << 8 ) | bytes[ i + 3 ] );
				if( next >= 0xDC00 && next <= 0xDFFF )
				{
					text.push_back( 0x10000 + ( ( unit - 0xD800 ) << 10 ) + ( next - 0xDC00 ) );
					i += 2;
					continue;
				}
			}
			if( unit == 0 )
			{
				why = "is not a text file (it has NUL characters)";
				text.clear();
				return false;
			}
			controls += ( unit < 0x20 && unit != U'\t' && unit != U'\n' && unit != U'\r' ) ? 1 : 0;
			text.push_back( unit >= 0xD800 && unit <= 0xDFFF ? 0xFFFD : unit );
		}
		if( controls * 20 > text.size() )
		{
			why = "is not a text file (it is mostly control characters)";
			text.clear();
			return false;
		}
		return true;
	}

	const bool bom    = size >= 3 && bytes[ 0 ] == 0xEF && bytes[ 1 ] == 0xBB && bytes[ 2 ] == 0xBF;
	const size_t from = bom && !flags.keepBomAndCr ? 3 : 0;
	size_t controls = 0;
	for( size_t i = from; i < size; ++i )
	{
		const unsigned char b = bytes[ i ];
		if( b == 0 )
		{
			why = "is not a text file (it has NUL bytes)";
			return false;
		}
		controls += ( ( b < 0x20 && b != '\t' && b != '\n' && b != '\r' ) || b == 0x7F ) ? 1 : 0;
	}
	if( controls * 20 > size - from )
	{
		why = "is not a text file (it is mostly control characters)";
		return false;
	}
	if( DecodeUtf8( bytes + from, size - from, text ) )
	{
		encoding = bom ? "UTF-8 with a BOM" : "UTF-8";
		return true;
	}
	//Not UTF-8: what an older Notepad saved as "ANSI" on a Western machine.
	text.clear();
	for( size_t i = from; i < size; ++i )
		text.push_back( FromCp1252( bytes[ i ] ) );
	encoding = "Windows-1252 (it is not UTF-8)";
	return true;
}

int EmojiSymbol( const std::u32string& raw )
{
	std::u32string s;
	for( char32_t c : raw )
		if( c != 0xFE0F && c != 0xFE0E )
			s.push_back( c );
	s = Trim( s, false );
	if( s.size() == 2 && s[ 0 ] == U'7' && s[ 1 ] == 0x20E3 )
		return slots::Seven;
	if( s.size() != 1 )
		return 0;
	switch( s[ 0 ] )
	{
	case 0x1F352: return slots::Cherry;
	case 0x1F34B: return slots::Lemon;
	case 0x1F34A: return slots::Orange;
	case 0x1F514: return slots::Bell;
	case 0x1F349: return slots::Melon;
	case 0x1F48E: return slots::Diamond;
	default: return 0;
	}
}

std::string SymbolLabel( int symbol )
{
	switch( symbol )
	{
	case slots::Cherry: return "CHERRY";
	case slots::Lemon: return "LEMON";
	case slots::Orange: return "ORANGE";
	case slots::Bell: return "BELL";
	case slots::Melon: return "MELON";
	case slots::Seven: return "SEVEN";
	case slots::Diamond: return "DIAMOND";
	default: return {};
	}
}

std::string CleanLabel( const std::u32string& raw )
{
	std::string out;
	bool space = false;
	for( char32_t c : raw )
	{
		std::string piece;
		if( c >= U'a' && c <= U'z' )
			piece = std::string( 1, static_cast< char >( c - U'a' + U'A' ) );
		else if( c < 128 && GlyphIndex( static_cast< char >( c ) ) >= 0 )
			piece = std::string( 1, static_cast< char >( c ) );
		else if( IsSpace( c ) )
		{
			space = true;
			continue;
		}
		else
			piece = Fold( c );
		if( piece.empty() )
			continue;
		if( space && !out.empty() )
			out += ' ';
		space = false;
		out += piece;
	}
	return out;
}

FileContents ParseText( const std::u32string& text, const ReadTestFlags& flags )
{
	FileContents file;
	int ignored = 0, line = 0;
	size_t i = 0;
	while( i <= text.size() )
	{
		//One physical line: LF, CRLF and a lone CR all end it (the naive
		//reader of the negative control ends a line at LF only).
		size_t end = i;
		while( end < text.size() && text[ end ] != U'\n' && ( flags.keepBomAndCr || text[ end ] != U'\r' ) )
			++end;
		const std::u32string raw = text.substr( i, end - i );
		size_t next = end + 1;
		if( end < text.size() && text[ end ] == U'\r' && end + 1 < text.size() && text[ end + 1 ] == U'\n' )
			next = end + 2;
		const bool last = end >= text.size();
		++line;
		i = next;

		std::u32string s = Trim( raw, flags.keepBomAndCr );
		if( !s.empty() )
		{
			if( static_cast< int >( file.entries.size() ) >= kMaxValues )
				++ignored;
			else
			{
				Entry e;
				e.line = line;
				if( !flags.noMarkers && ( s[ 0 ] == U'*' || s[ 0 ] == U'%' ) )
				{
					e.mark = s[ 0 ] == U'*' ? Mark::Jackpot : Mark::Joker;
					s      = Trim( s.substr( 1 ), flags.keepBomAndCr );
				}
				e.symbol = EmojiSymbol( s );
				if( e.symbol == 0 )
					e.label = CleanLabel( s );
				file.marked += e.mark != Mark::None ? 1 : 0;
				file.entries.push_back( e );
			}
		}
		if( last )
			break;
	}
	if( ignored > 0 )
		file.notes.push_back( std::to_string( ignored ) + " values past the first " + std::to_string( kMaxValues ) + " ignored" );
	return file;
}

bool ReadWheelText( const unsigned char* bytes, size_t size, FileContents& out, std::string& why, const ReadTestFlags& flags )
{
	std::u32string text;
	std::string encoding;
	out = FileContents {};
	if( !DecodeText( bytes, size, text, encoding, why, flags ) )
		return false;
	out          = ParseText( text, flags );
	out.encoding = encoding;
	if( out.entries.empty() )
	{
		why = "has no values (every line is blank)";
		return false;
	}
	return true;
}

bool LoadWheelFile( const std::string& utf8Path, FileContents& out, std::string& why )
{
	std::vector< unsigned char > bytes;
	out = FileContents {};
	if( !files::ReadFileBytes( utf8Path, bytes, why, kMaxFileBytes ) )
		return false;
	return ReadWheelText( bytes.data(), bytes.size(), out, why );
}

//---------------------------------------------------------------------------
// Faces.
//---------------------------------------------------------------------------
int MaxLabelChars( int segments )
{
	if( segments <= 2 )
		return 32;
	if( segments <= 4 )
		return 24;
	if( segments <= 8 )
		return 16;
	if( segments <= 12 )
		return 12;
	if( segments <= 40 )
		return 8;
	return 7;//JACKPOT, the Big Six's longest word
}

bool SetAcross( int segments )
{
	return segments <= 12;
}

int PegsPerSegment( int segments )
{
	return std::max( 1, static_cast< int >( std::lround( static_cast< double >( kSegments ) / std::max( 1, segments ) ) ) );
}

Face BigSix( int segments )
{
	segments = std::clamp( segments, kMinSegments, kSegments );
	//The joker first, the logo opposite it (from four segments up), and the
	//money values between them in the Big Six's own order. At 54 this IS
	//Layout(); sampling Layout at a stride instead aliased with its pattern
	//(13 segments came out six $2s in a row and no $1).
	const std::vector< int >& layout = Layout();
	std::vector< int > money;
	for( int v : layout )
		if( v != 40 && v != 45 )
			money.push_back( v );
	const int logoAt = segments >= 4 ? segments / 2 : -1;
	Face face;
	face.segment.resize( static_cast< size_t >( segments ) );
	size_t next = 0;
	for( int j = 0; j < segments; ++j )
	{
		Segment& s = face.segment[ static_cast< size_t >( j ) ];
		s.value    = j == 0 ? 40 : j == logoAt ? 45 : money[ next++ % money.size() ];
		s.label    = s.value == 40 ? "JOKER" : s.value == 45 ? "JACKPOT" : "$" + std::to_string( s.value );
		s.board    = Name( s.value );
		s.mark     = s.value == 40 ? Mark::Joker : s.value == 45 ? Mark::Jackpot : Mark::None;
		s.style    = StyleOf( s.value );
	}
	return face;
}

const std::shared_ptr< const Face >& DefaultFace()
{
	static const std::shared_ptr< const Face > face = std::make_shared< const Face >( BigSix( kSegments ) );
	return face;
}

Face FromFile( const FileContents& file, int segments, Lines lines, ShortFile shortFile, uint32_t seed, const FaceTestFlags& flags )
{
	segments    = std::clamp( segments, kMinSegments, kSegments );
	const int L = static_cast< int >( file.entries.size() );
	if( L == 0 )
		return BigSix( segments );
	//Random: every value shuffled by Seed (Fisher-Yates), the first Segments
	//taken. Keyed on Seed and the number of values only, so editing a line's
	//text leaves the draw where it was and a larger Segments adds to it.
	std::vector< int > order( static_cast< size_t >( L ) );
	std::iota( order.begin(), order.end(), 0 );
	if( lines == Lines::Random )
	{
		Stream s( Hash( seed, 0x11e5u, static_cast< uint32_t >( L ), flags.salt ) );
		for( int i = L - 1; i > 0; --i )
			std::swap( order[ static_cast< size_t >( i ) ], order[ static_cast< size_t >( flags.sattolo ? s.Below( i ) : s.Below( i + 1 ) ) ] );
	}
	const int n    = ( L >= segments || shortFile == ShortFile::Repeat ) ? segments : std::max( kMinSegments, L );
	const int keep = MaxLabelChars( n );
	Face face;
	face.fromFile = true;
	face.segment.resize( static_cast< size_t >( n ) );
	for( int k = 0; k < n; ++k )
	{
		const Entry& e = file.entries[ static_cast< size_t >( order[ static_cast< size_t >( k % L ) ] ) ];
		//Clockwise from segment 0: pulled anticlockwise, the wheel brings them
		//past the clapper in order.
		Segment& s = face.segment[ static_cast< size_t >( ( n - k ) % n ) ];
		s.symbol   = e.symbol;
		s.mark     = e.mark;
		s.line     = e.line;
		s.label    = e.label.substr( 0, static_cast< size_t >( keep ) );
		while( !s.label.empty() && s.label.back() == ' ' )
			s.label.pop_back();
		s.board = e.symbol != 0 ? SymbolLabel( e.symbol ) : e.label.substr( 0, static_cast< size_t >( kMaxLabel ) );
		while( !s.board.empty() && s.board.back() == ' ' )
			s.board.pop_back();
	}
	//Plain values cycle through the Big Six's money colours; the last takes a
	//third when it would match the first.
	for( int j = 0; j < n; ++j )
	{
		Segment& s = face.segment[ static_cast< size_t >( j ) ];
		if( s.mark == Mark::Jackpot )
			s.style = StyleJackpot;
		else if( s.mark == Mark::Joker )
			s.style = StyleJoker;
		else
			s.style = ( j == n - 1 && n % 5 == 1 ) ? StyleFive : j % 5;
	}
	return face;
}

//---------------------------------------------------------------------------
// Fitting.
//---------------------------------------------------------------------------
namespace
{
constexpr double kRimLimit  = kRimClear - 0.005;///< the outermost ink, metres
constexpr double kPreferred = 0.62;            ///< where a label set across sits, if it can
constexpr float kSpaceH     = 0.5f;            ///< a space's advance in heights, = kSpace (Jackpot.cpp), SPACE (Shaders.cpp)

double Bisect( const std::function< bool( double ) >& fits, double most )
{
	if( fits( most ) )
		return most;
	double lo = 0.0, hi = most;
	for( int i = 0; i < 48; ++i )
	{
		const double mid = 0.5 * ( lo + hi );
		( fits( mid ) ? lo : hi ) = mid;
	}
	return lo;
}
} // namespace

std::vector< LabelFit > FitLabels( const Face& face, const GlyphAtlas& atlas, const FitTestFlags& flags )
{
	const int n      = face.Count();
	const double sn  = std::sin( kPi / n ), cs = std::cos( kPi / n );
	const bool wide  = SetAcross( n );
	std::vector< LabelFit > fits( static_cast< size_t >( n ) );
	for( int j = 0; j < n; ++j )
	{
		const Segment& s = face.segment[ static_cast< size_t >( j ) ];
		LabelFit& fit    = fits[ static_cast< size_t >( j ) ];
		fit.across       = wide;
		const double floor = s.mark != Mark::None ? kStarClear : kTextFloor;
		//A point (x out along the segment's middle, y round) is inside when it
		//is kMargin clear of both dividers, off the hub's side and in the rim.
		auto clear = [ & ]( double x, double halfWidth ) { return x * sn - halfWidth * cs >= kMargin && x >= floor; };
		auto inRim = [ & ]( double x, double halfWidth ) { return x * x + halfWidth * halfWidth <= kRimLimit * kRimLimit; };

		if( s.symbol != 0 )
		{
			fit.symbol = s.symbol;
			auto range = [ & ]( double a, double& lo, double& hi ) {
				lo = std::max( floor + a, a + ( kMargin + a * cs ) / sn );
				hi = a < kRimLimit ? std::sqrt( kRimLimit * kRimLimit - a * a ) - a : -1.0;
				return lo <= hi;
			};
			double lo = 0.0, hi = 0.0;
			const double a = flags.noShrink ? kSymbolMax : Bisect( [ & ]( double x ) { return range( x, lo, hi ); }, kSymbolMax );
			range( a, lo, hi );
			fit.symbolHalf = static_cast< float >( a );
			fit.anchor     = static_cast< float >( flags.noShrink ? kPreferred : wide ? std::clamp( kPreferred, lo, hi ) : hi );
			continue;
		}
		if( s.label.empty() )
			continue;
		for( char c : s.label )
			fit.glyphs.push_back( GlyphIndex( c ) );

		if( wide )
		{
			//One line, level when the segment is at the top: the ink's extent
			//as SetTextUniforms measures a span.
			float pen = 0.0f, lo = 1e9f, hi = -1e9f;
			for( int g : fit.glyphs )
			{
				if( g < 0 )
				{
					pen += kSpaceH;
					continue;
				}
				lo = std::min( lo, pen + atlas.glyph[ g ].inkX0 );
				hi = std::max( hi, pen + atlas.glyph[ g ].inkX1 );
				pen += atlas.glyph[ g ].advance;
			}
			fit.inkLeft  = hi > lo ? lo : 0.0f;
			fit.inkWidth = hi > lo ? hi - lo : 0.0f;
			auto range   = [ & ]( double h, double& xlo, double& xhi ) {
				const double w = 0.5 * fit.inkWidth * h, v = ( 0.5 + kSlack ) * h;
				xlo            = std::max( floor + v, v + ( kMargin + w * cs ) / sn );
				xhi            = w < kRimLimit ? std::sqrt( kRimLimit * kRimLimit - w * w ) - v : -1.0;
				return xlo <= xhi;
			};
			double xlo = 0.0, xhi = 0.0;
			const double most = n <= 4 ? kAcrossMaxFew : kAcrossMax;
			const double h    = flags.noShrink ? most : Bisect( [ & ]( double x ) { return range( x, xlo, xhi ); }, most );
			range( h, xlo, xhi );
			fit.h      = static_cast< float >( h );
			fit.anchor = static_cast< float >( flags.noShrink ? kPreferred : std::clamp( kPreferred, xlo, xhi ) );
			continue;
		}

		//Stacked: row k's middle at anchor - ( k + 0.5 ) h kLead, each
		//character upright and centred across the segment. The anchor is
		//v0.1.1's kTextTop, brought in for type big enough to reach the rim.
		double firstHalf = 0.0;
		for( int g : fit.glyphs )
			if( g >= 0 )
			{
				firstHalf = 0.5 * ( atlas.glyph[ g ].inkX1 - atlas.glyph[ g ].inkX0 );
				break;
			}
		auto anchor = [ & ]( double h ) {
			const double w = firstHalf * h;
			return std::min( static_cast< double >( kTextTop ), std::sqrt( std::max( 0.0, kRimLimit * kRimLimit - w * w ) ) - 1e-6 - ( 0.5 + kSlack - 0.5 * kLead ) * h );
		};
		auto stacks = [ & ]( double h ) {
			for( size_t k = 0; k < fit.glyphs.size(); ++k )
			{
				const int g = fit.glyphs[ k ];
				if( g < 0 )
					continue;
				const double cy = anchor( h ) - ( static_cast< double >( k ) + 0.5 ) * h * kLead;
				const double w  = 0.5 * ( atlas.glyph[ g ].inkX1 - atlas.glyph[ g ].inkX0 ) * h;
				const double v  = ( 0.5 + kSlack ) * h;
				if( !clear( cy - v, w ) || !inRim( cy + v, w ) )
					return false;
			}
			return true;
		};
		fit.h      = static_cast< float >( flags.noShrink ? kStackedMax : Bisect( stacks, kStackedMax ) );
		fit.anchor = static_cast< float >( flags.noShrink ? kTextTop : anchor( fit.h ) );
	}
	return fits;
}

} // namespace jackpot::wheel
