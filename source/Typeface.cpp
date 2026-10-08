#include "Typeface.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Diag.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#if defined( _WIN32 )
	// windows.h defines min and max as macros, which turns the std::min calls in
	// Build below into `std::(...)`. The error names neither min nor
	// windows.h -- it is `C2589: '(': illegal token on right side of '::'` --
	// and it only ever appears on the Windows build. CMakeLists sets this
	// globally too; it is repeated here so the file is correct on its own.
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#include <windows.h>
#else
	#include <dirent.h>
	#include <sys/stat.h>
#endif

namespace jackpot
{
namespace
{
//---------------------------------------------------------------------------
// A very small slice of the OpenType container, just enough to find the name
// table without reading the whole file. See Typeface.h for why.
//---------------------------------------------------------------------------
uint16_t ReadU16( const unsigned char* p )
{
	return static_cast< uint16_t >( ( p[ 0 ] << 8 ) | p[ 1 ] );
}

uint32_t ReadU32( const unsigned char* p )
{
	return ( static_cast< uint32_t >( p[ 0 ] ) << 24 ) | ( static_cast< uint32_t >( p[ 1 ] ) << 16 )
		| ( static_cast< uint32_t >( p[ 2 ] ) << 8 ) | static_cast< uint32_t >( p[ 3 ] );
}

bool ReadAt( std::FILE* file, long offset, void* into, size_t bytes )
{
	if( std::fseek( file, offset, SEEK_SET ) != 0 )
		return false;
	return std::fread( into, 1, bytes, file ) == bytes;
}

/// UTF-16BE, as the Windows platform stores names, reduced to ASCII. Family
/// names outside ASCII exist, but this is a dropdown label -- it has to survive
/// a round trip through an FFGL parameter name, which is a `char*`.
std::string DecodeNameUtf16( const std::vector< unsigned char >& raw )
{
	std::string out;
	for( size_t i = 0; i + 1 < raw.size(); i += 2 )
	{
		const uint16_t unit = ReadU16( raw.data() + i );
		out += ( unit >= 32 && unit < 127 ) ? static_cast< char >( unit ) : '?';
	}
	return out;
}

std::string DecodeNameAscii( const std::vector< unsigned char >& raw )
{
	std::string out;
	for( unsigned char c : raw )
		out += ( c >= 32 && c < 127 ) ? static_cast< char >( c ) : '?';
	return out;
}

/// Family name of the face at `faceOffset`, read from the name table alone.
std::string ReadFamilyName( std::FILE* file, uint32_t faceOffset )
{
	unsigned char header[ 12 ];
	if( !ReadAt( file, static_cast< long >( faceOffset ), header, sizeof( header ) ) )
		return {};

	const uint16_t tableCount = ReadU16( header + 4 );
	if( tableCount == 0 || tableCount > 512 )
		return {};

	std::vector< unsigned char > directory( static_cast< size_t >( tableCount ) * 16u );
	if( !ReadAt( file, static_cast< long >( faceOffset ) + 12, directory.data(), directory.size() ) )
		return {};

	uint32_t nameOffset = 0;
	uint32_t nameLength = 0;
	for( uint16_t i = 0; i < tableCount; ++i )
	{
		const unsigned char* record = directory.data() + static_cast< size_t >( i ) * 16u;
		if( std::memcmp( record, "name", 4 ) == 0 )
		{
			nameOffset = ReadU32( record + 8 );
			nameLength = ReadU32( record + 12 );
			break;
		}
	}

	if( nameOffset == 0 || nameLength < 6 || nameLength > 1u << 20 )
		return {};

	std::vector< unsigned char > table( nameLength );
	if( !ReadAt( file, static_cast< long >( nameOffset ), table.data(), table.size() ) )
		return {};

	const uint16_t recordCount = ReadU16( table.data() + 2 );
	const uint16_t storage     = ReadU16( table.data() + 4 );

	std::string best;
	int bestScore = -1;

	for( uint16_t i = 0; i < recordCount; ++i )
	{
		const size_t at = 6u + static_cast< size_t >( i ) * 12u;
		if( at + 12 > table.size() )
			break;

		const unsigned char* record = table.data() + at;
		const uint16_t platform     = ReadU16( record + 0 );
		const uint16_t language     = ReadU16( record + 4 );
		const uint16_t nameId       = ReadU16( record + 6 );
		const uint16_t length       = ReadU16( record + 8 );
		const uint16_t offset       = ReadU16( record + 10 );

		// 16 is the typographic family ("Helvetica Neue"); 1 is the legacy
		// family, which splits weights into separate families ("Helvetica Neue
		// Light"). Prefer 16 where a font has it so the dropdown lists a family
		// once rather than nine times.
		if( nameId != 1 && nameId != 16 )
			continue;

		const size_t start = static_cast< size_t >( storage ) + offset;
		if( start + length > table.size() )
			continue;

		std::vector< unsigned char > raw( table.begin() + start, table.begin() + start + length );
		const std::string decoded = ( platform == 3 || platform == 0 ) ? DecodeNameUtf16( raw )
		                                                              : DecodeNameAscii( raw );
		if( decoded.empty() )
			continue;

		// A name record can be in any script, and a font that ships an Arabic or
		// a Chinese family name usually ships an English one alongside it.
		// Taking whichever came first put a run of families called "????" at the
		// top of the dropdown -- the decoder is doing the only thing it can with
		// a script it cannot represent in a `char*` parameter name, so the fix
		// is to prefer the record that survives the trip.
		const size_t unknown = static_cast< size_t >( std::count( decoded.begin(), decoded.end(), '?' ) );
		if( unknown * 4 > decoded.size() )
			continue;

		// 0x409 is Windows US English; language 0 on the Mac platform is English.
		const bool english = ( platform == 3 && language == 0x409 ) || ( platform == 1 && language == 0 );

		const int score = ( nameId == 16 ? 4 : 0 ) + ( english ? 2 : 0 ) + ( platform == 3 ? 1 : 0 );
		if( score > bestScore )
		{
			bestScore = score;
			best      = decoded;
		}
	}

	return best;
}

/// Every face in one font file. A .ttc holds several.
void ScanFontFile( const std::string& path, std::vector< FontFile >& into )
{
	std::FILE* file = std::fopen( path.c_str(), "rb" );
	if( file == nullptr )
		return;

	unsigned char tag[ 12 ];
	if( std::fread( tag, 1, sizeof( tag ), file ) != sizeof( tag ) )
	{
		std::fclose( file );
		return;
	}

	std::vector< uint32_t > faceOffsets;

	if( std::memcmp( tag, "ttcf", 4 ) == 0 )
	{
		const uint32_t faceCount = ReadU32( tag + 8 );
		if( faceCount > 0 && faceCount < 4096 )
		{
			std::vector< unsigned char > offsets( static_cast< size_t >( faceCount ) * 4u );
			if( ReadAt( file, 12, offsets.data(), offsets.size() ) )
			{
				for( uint32_t i = 0; i < faceCount; ++i )
					faceOffsets.push_back( ReadU32( offsets.data() + static_cast< size_t >( i ) * 4u ) );
			}
		}
	}
	else
	{
		const uint32_t version = ReadU32( tag );
		// 0x00010000 is TrueType outlines, 'OTTO' is CFF, 'true' is an old Mac
		// TrueType. Anything else is not a font we can rasterise.
		if( version == 0x00010000u || version == 0x4F54544Fu || version == 0x74727565u )
			faceOffsets.push_back( 0 );
	}

	for( size_t i = 0; i < faceOffsets.size(); ++i )
	{
		const std::string family = ReadFamilyName( file, faceOffsets[ i ] );
		if( family.empty() )
			continue;

		// macOS ships around a hundred internal faces whose family name starts
		// with a dot -- ".Aqua Kana", ".Apple Color Emoji UI" and friends. They
		// are UI machinery rather than typefaces, several will not rasterise,
		// and listing them buries the real fonts. Apple's own font menus hide
		// them on exactly this rule.
		if( family[ 0 ] == '.' )
			continue;

		FontFile entry;
		entry.family          = family;
		entry.path            = path;
		entry.collectionIndex = static_cast< int >( i );
		into.push_back( entry );
	}

	std::fclose( file );
}

bool HasFontExtension( const std::string& name )
{
	if( name.size() < 5 )
		return false;
	std::string tail = name.substr( name.size() - 4 );
	for( char& c : tail )
		c = static_cast< char >( std::tolower( static_cast< unsigned char >( c ) ) );
	return tail == ".ttf" || tail == ".otf" || tail == ".ttc" || tail == ".otc";
}

void ScanDirectory( const std::string& directory, std::vector< FontFile >& into, int depth )
{
	// Two levels. macOS keeps a few fonts one folder down (Supplemental, and
	// per-family folders); nothing useful is deeper, and an unbounded walk over
	// a home directory is how a plugin constructor becomes a hang.
	if( depth > 2 )
		return;

#if defined( _WIN32 )
	const std::string pattern = directory + "\\*";
	WIN32_FIND_DATAA found {};
	HANDLE handle = FindFirstFileA( pattern.c_str(), &found );
	if( handle == INVALID_HANDLE_VALUE )
		return;

	do
	{
		const std::string name = found.cFileName;
		if( name == "." || name == ".." )
			continue;

		const std::string full = directory + "\\" + name;
		if( ( found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) != 0 )
			ScanDirectory( full, into, depth + 1 );
		else if( HasFontExtension( name ) )
			ScanFontFile( full, into );
	} while( FindNextFileA( handle, &found ) );

	FindClose( handle );
#else
	DIR* dir = opendir( directory.c_str() );
	if( dir == nullptr )
		return;

	while( struct dirent* entry = readdir( dir ) )
	{
		const std::string name = entry->d_name;
		if( name.empty() || name[ 0 ] == '.' )
			continue;

		const std::string full = directory + "/" + name;

		struct stat info {};
		if( stat( full.c_str(), &info ) != 0 )
			continue;

		if( S_ISDIR( info.st_mode ) )
			ScanDirectory( full, into, depth + 1 );
		else if( S_ISREG( info.st_mode ) && HasFontExtension( name ) )
			ScanFontFile( full, into );
	}

	closedir( dir );
#endif
}

std::vector< std::string > FontDirectories()
{
	std::vector< std::string > directories;

#if defined( _WIN32 )
	char windows[ MAX_PATH ] {};
	if( GetWindowsDirectoryA( windows, MAX_PATH ) != 0 )
		directories.push_back( std::string( windows ) + "\\Fonts" );

	const char* local = std::getenv( "LOCALAPPDATA" );
	if( local != nullptr )
		directories.push_back( std::string( local ) + "\\Microsoft\\Windows\\Fonts" );
#else
	const char* home = std::getenv( "HOME" );
	if( home != nullptr )
		directories.push_back( std::string( home ) + "/Library/Fonts" );
	directories.push_back( "/Library/Fonts" );
	directories.push_back( "/System/Library/Fonts" );
#endif

	return directories;
}

std::string Lowered( const std::string& text )
{
	std::string out = text;
	for( char& c : out )
		c = static_cast< char >( std::tolower( static_cast< unsigned char >( c ) ) );
	return out;
}

stbtt_fontinfo* AsFontInfo( std::vector< unsigned char >& storage )
{
	return storage.empty() ? nullptr : reinterpret_cast< stbtt_fontinfo* >( storage.data() );
}

const stbtt_fontinfo* AsFontInfo( const std::vector< unsigned char >& storage )
{
	return storage.empty() ? nullptr : reinterpret_cast< const stbtt_fontinfo* >( storage.data() );
}
} // namespace

const std::vector< FontFile >& InstalledFonts()
{
	static const std::vector< FontFile > fonts = [] {
		std::vector< FontFile > found;
		for( const std::string& directory : FontDirectories() )
			ScanDirectory( directory, found, 0 );

		//Within a family, the plain face first: the first face of a
		//collection, then the shortest file name ("Georgia.ttf" before
		//"Georgia Bold Italic.ttf", which sorts first by path alone because a
		//space is lower than a full stop -- downpour picks the bold italic).
		auto fileName = []( const std::string& path ) {
			const size_t slash = path.find_last_of( "/\\" );
			return slash == std::string::npos ? path : path.substr( slash + 1 );
		};
		std::sort( found.begin(), found.end(), [ & ]( const FontFile& a, const FontFile& b ) {
			const std::string left  = Lowered( a.family );
			const std::string right = Lowered( b.family );
			if( left != right )
				return left < right;
			if( a.collectionIndex != b.collectionIndex )
				return a.collectionIndex < b.collectionIndex;
			const size_t la = fileName( a.path ).size(), lb = fileName( b.path ).size();
			if( la != lb )
				return la < lb;
			return a.path < b.path;
		} );

		// One entry per family. A family with four weights is four files with
		// the same typographic name, and listing it four times makes the
		// dropdown useless without telling anyone anything.
		found.erase( std::unique( found.begin(), found.end(),
		                          []( const FontFile& a, const FontFile& b ) {
			                          return Lowered( a.family ) == Lowered( b.family );
		                          } ),
		             found.end() );

		diag::info( "font scan found " + std::to_string( found.size() ) + " families" );
		return found;
	}();

	return fonts;
}

int FindFontByFamily( const std::string& family )
{
	if( family.empty() )
		return -1;

	const std::string wanted        = Lowered( family );
	const std::vector< FontFile >& fonts = InstalledFonts();
	for( size_t i = 0; i < fonts.size(); ++i )
	{
		if( Lowered( fonts[ i ].family ) == wanted )
			return static_cast< int >( i );
	}
	return -1;
}

void Typeface::UseBuiltin()
{
	data.clear();
	info.clear();
	family.clear();
	loaded = false;
}

bool Typeface::Load( const std::string& path, int collectionIndex )
{
	UseBuiltin();

	if( path.empty() )
		return false;

	std::FILE* file = std::fopen( path.c_str(), "rb" );
	if( file == nullptr )
	{
		diag::warn( "font will not open: " + path );
		return false;
	}

	std::fseek( file, 0, SEEK_END );
	const long size = std::ftell( file );
	std::fseek( file, 0, SEEK_SET );

	// A font over 64 MB is a pan-CJK collection, and loading one to draw ninety
	// latin characters is not a trade worth making inside somebody else's
	// process.
	if( size <= 0 || size > 64L * 1024L * 1024L )
	{
		std::fclose( file );
		diag::warn( "font is an implausible size, ignoring: " + path );
		return false;
	}

	data.resize( static_cast< size_t >( size ) );
	const size_t read = std::fread( data.data(), 1, data.size(), file );
	std::fclose( file );

	if( read != data.size() )
	{
		UseBuiltin();
		return false;
	}

	info.resize( sizeof( stbtt_fontinfo ) );
	stbtt_fontinfo* font = AsFontInfo( info );

	const int offset = stbtt_GetFontOffsetForIndex( data.data(), collectionIndex );
	if( offset < 0 || stbtt_InitFont( font, data.data(), offset ) == 0 )
	{
		UseBuiltin();
		diag::warn( "font will not parse: " + path );
		return false;
	}

	loaded = true;

	// Re-open to read the name table rather than keeping the handle: the load
	// path above needs the file closed before it can be sure the whole thing is
	// in `data`, and a family name is worth one extra open.
	std::FILE* named = std::fopen( path.c_str(), "rb" );
	if( named != nullptr )
	{
		family = ReadFamilyName( named, static_cast< uint32_t >( offset ) );
		std::fclose( named );
	}

	diag::info( "font loaded: " + ( family.empty() ? path : family ) );
	return true;
}



//---------------------------------------------------------------------------
// The charset.
//---------------------------------------------------------------------------
int GlyphIndex( char c )
{
	if( c >= 'a' && c <= 'z' )
		c = static_cast< char >( c - 'a' + 'A' );
	for( int i = 0; i < kGlyphCount; ++i )
		if( kCharset[ i ] == c )
			return i;
	return -1;
}

//---------------------------------------------------------------------------
// The built-in face.
//---------------------------------------------------------------------------
namespace
{
using Stroke = std::vector< std::pair< float, float > >;

/// An arc as a polyline, angles in degrees, counter-clockwise when a1 > a0.
void Arc( Stroke& into, float cx, float cy, float rx, float ry, float a0, float a1, int segments = 24 )
{
	for( int i = 0; i <= segments; ++i )
	{
		const float a = ( a0 + ( a1 - a0 ) * static_cast< float >( i ) / static_cast< float >( segments ) ) * 3.14159265f / 180.0f;
		into.push_back( { cx + rx * std::cos( a ), cy + ry * std::sin( a ) } );
	}
}

Stroke Line( std::initializer_list< std::pair< float, float > > points )
{
	return Stroke( points );
}

/// Ink centrelines in 0..0.62 x 0.075..0.925 (the stroke adds kBuiltinHalfWidth,
/// so a capital's ink spans 0..1). The digits are polyhedral's.
std::vector< std::vector< Stroke > > MakeStrokes()
{
	std::vector< std::vector< Stroke > > g( kGlyphCount );
	auto at = [ & ]( char c ) -> std::vector< Stroke >& { return g[ static_cast< size_t >( GlyphIndex( c ) ) ]; };

	//-- digits ---------------------------------------------------------------
	{
		Stroke s;
		Arc( s, 0.3f, 0.5f, 0.24f, 0.425f, 0.0f, 360.0f, 48 );
		at( '0' ) = { s };
	}
	at( '1' ) = { Line( { { 0.13f, 0.80f }, { 0.36f, 0.925f }, { 0.36f, 0.075f } } ) };
	{
		Stroke s;
		Arc( s, 0.3f, 0.70f, 0.235f, 0.225f, 165.0f, -40.0f );
		s.push_back( { 0.06f, 0.075f } );
		s.push_back( { 0.56f, 0.075f } );
		at( '2' ) = { s };
	}
	{
		Stroke upper, lower;
		Arc( upper, 0.29f, 0.725f, 0.215f, 0.20f, 150.0f, -90.0f );
		Arc( lower, 0.29f, 0.29f, 0.245f, 0.235f, 90.0f, -150.0f );
		at( '3' ) = { upper, lower };
	}
	at( '4' ) = { Line( { { 0.44f, 0.075f }, { 0.44f, 0.925f }, { 0.04f, 0.30f }, { 0.58f, 0.30f } } ) };
	{
		Stroke s = { { 0.54f, 0.925f }, { 0.12f, 0.925f }, { 0.08f, 0.53f } };
		Arc( s, 0.30f, 0.315f, 0.25f, 0.245f, 120.0f, -150.0f );
		at( '5' ) = { s };
	}
	{
		Stroke bowl, stem;
		Arc( bowl, 0.30f, 0.31f, 0.245f, 0.235f, 0.0f, 360.0f, 40 );
		stem = { { 0.46f, 0.925f }, { 0.105f, 0.43f } };
		at( '6' ) = { bowl, stem };
	}
	at( '7' ) = { Line( { { 0.05f, 0.925f }, { 0.56f, 0.925f }, { 0.20f, 0.075f } } ) };
	{
		Stroke upper, lower;
		Arc( upper, 0.30f, 0.725f, 0.195f, 0.20f, 0.0f, 360.0f, 36 );
		Arc( lower, 0.30f, 0.285f, 0.245f, 0.21f, 0.0f, 360.0f, 40 );
		at( '8' ) = { upper, lower };
	}
	//Nine is six turned half round about the middle of its box.
	for( const Stroke& s : at( '6' ) )
	{
		Stroke turned;
		for( const auto& p : s )
			turned.push_back( { 0.6f - p.first, 1.0f - p.second } );
		at( '9' ).push_back( turned );
	}

	//-- capitals -------------------------------------------------------------
	at( 'A' ) = { Line( { { 0.04f, 0.075f }, { 0.30f, 0.925f }, { 0.56f, 0.075f } } ), Line( { { 0.13f, 0.36f }, { 0.47f, 0.36f } } ) };
	{
		Stroke s = { { 0.08f, 0.075f }, { 0.08f, 0.925f }, { 0.34f, 0.925f } };
		Arc( s, 0.34f, 0.71f, 0.215f, 0.215f, 90.0f, -90.0f, 16 );
		s.push_back( { 0.08f, 0.495f } );
		Stroke lower = { { 0.08f, 0.495f }, { 0.36f, 0.495f } };
		Arc( lower, 0.36f, 0.285f, 0.21f, 0.21f, 90.0f, -90.0f, 16 );
		lower.push_back( { 0.08f, 0.075f } );
		at( 'B' ) = { s, lower };
	}
	{
		Stroke s;
		Arc( s, 0.32f, 0.5f, 0.27f, 0.425f, 50.0f, 310.0f, 40 );
		at( 'C' ) = { s };
	}
	{
		Stroke s = { { 0.08f, 0.075f }, { 0.08f, 0.925f }, { 0.26f, 0.925f } };
		Arc( s, 0.26f, 0.5f, 0.30f, 0.425f, 90.0f, -90.0f, 28 );
		s.push_back( { 0.08f, 0.075f } );
		at( 'D' ) = { s };
	}
	at( 'E' ) = { Line( { { 0.54f, 0.925f }, { 0.08f, 0.925f }, { 0.08f, 0.075f }, { 0.54f, 0.075f } } ),
		          Line( { { 0.08f, 0.5f }, { 0.44f, 0.5f } } ) };
	at( 'F' ) = { Line( { { 0.54f, 0.925f }, { 0.08f, 0.925f }, { 0.08f, 0.075f } } ), Line( { { 0.08f, 0.5f }, { 0.42f, 0.5f } } ) };
	{
		Stroke s;
		Arc( s, 0.32f, 0.5f, 0.27f, 0.425f, 45.0f, 360.0f, 40 );
		s.push_back( { 0.36f, 0.5f } );
		at( 'G' ) = { s };
	}
	at( 'H' ) = { Line( { { 0.08f, 0.075f }, { 0.08f, 0.925f } } ), Line( { { 0.52f, 0.075f }, { 0.52f, 0.925f } } ),
		          Line( { { 0.08f, 0.5f }, { 0.52f, 0.5f } } ) };
	at( 'I' ) = { Line( { { 0.3f, 0.075f }, { 0.3f, 0.925f } } ), Line( { { 0.14f, 0.925f }, { 0.46f, 0.925f } } ),
		          Line( { { 0.14f, 0.075f }, { 0.46f, 0.075f } } ) };
	{
		Stroke s = { { 0.50f, 0.925f }, { 0.50f, 0.30f } };
		Arc( s, 0.29f, 0.30f, 0.21f, 0.225f, 0.0f, -180.0f, 20 );
		at( 'J' ) = { s };
	}
	at( 'K' ) = { Line( { { 0.08f, 0.075f }, { 0.08f, 0.925f } } ), Line( { { 0.54f, 0.925f }, { 0.08f, 0.36f } } ),
		          Line( { { 0.24f, 0.53f }, { 0.56f, 0.075f } } ) };
	at( 'L' ) = { Line( { { 0.08f, 0.925f }, { 0.08f, 0.075f }, { 0.54f, 0.075f } } ) };
	at( 'M' ) = { Line( { { 0.05f, 0.075f }, { 0.05f, 0.925f }, { 0.30f, 0.40f }, { 0.55f, 0.925f }, { 0.55f, 0.075f } } ) };
	at( 'N' ) = { Line( { { 0.08f, 0.075f }, { 0.08f, 0.925f }, { 0.52f, 0.075f }, { 0.52f, 0.925f } } ) };
	{
		Stroke s;
		Arc( s, 0.3f, 0.5f, 0.26f, 0.425f, 0.0f, 360.0f, 48 );
		at( 'O' ) = { s };
		at( 'Q' ) = { s, Line( { { 0.36f, 0.25f }, { 0.58f, 0.04f } } ) };
	}
	{
		Stroke s = { { 0.08f, 0.075f }, { 0.08f, 0.925f }, { 0.32f, 0.925f } };
		Arc( s, 0.32f, 0.70f, 0.225f, 0.225f, 90.0f, -90.0f, 16 );
		s.push_back( { 0.08f, 0.475f } );
		at( 'P' ) = { s };
		at( 'R' ) = { s, Line( { { 0.30f, 0.475f }, { 0.56f, 0.075f } } ) };
	}
	{
		Stroke s;
		Arc( s, 0.3f, 0.715f, 0.22f, 0.21f, 20.0f, 270.0f, 24 );
		Arc( s, 0.3f, 0.29f, 0.24f, 0.215f, 90.0f, -160.0f, 24 );
		at( 'S' ) = { s };
	}
	at( 'T' ) = { Line( { { 0.03f, 0.925f }, { 0.57f, 0.925f } } ), Line( { { 0.3f, 0.925f }, { 0.3f, 0.075f } } ) };
	{
		Stroke s = { { 0.08f, 0.925f }, { 0.08f, 0.30f } };
		Arc( s, 0.30f, 0.30f, 0.22f, 0.225f, 180.0f, 360.0f, 20 );
		s.push_back( { 0.52f, 0.925f } );
		at( 'U' ) = { s };
	}
	at( 'V' ) = { Line( { { 0.04f, 0.925f }, { 0.30f, 0.075f }, { 0.56f, 0.925f } } ) };
	at( 'W' ) = { Line( { { 0.02f, 0.925f }, { 0.16f, 0.075f }, { 0.30f, 0.60f }, { 0.44f, 0.075f }, { 0.58f, 0.925f } } ) };
	at( 'X' ) = { Line( { { 0.06f, 0.925f }, { 0.54f, 0.075f } } ), Line( { { 0.54f, 0.925f }, { 0.06f, 0.075f } } ) };
	at( 'Y' ) = { Line( { { 0.04f, 0.925f }, { 0.30f, 0.50f }, { 0.56f, 0.925f } } ), Line( { { 0.30f, 0.50f }, { 0.30f, 0.075f } } ) };
	at( 'Z' ) = { Line( { { 0.06f, 0.925f }, { 0.54f, 0.925f }, { 0.06f, 0.075f }, { 0.54f, 0.075f } } ) };

	//-- punctuation ----------------------------------------------------------
	{
		Stroke s;
		Arc( s, 0.3f, 0.68f, 0.19f, 0.17f, 20.0f, 270.0f, 20 );
		Arc( s, 0.3f, 0.34f, 0.20f, 0.17f, 90.0f, -160.0f, 20 );
		at( '$' ) = { s, Line( { { 0.3f, 0.02f }, { 0.3f, 0.98f } } ) };
	}
	at( '\'' ) = { Line( { { 0.30f, 0.925f }, { 0.27f, 0.70f } } ) };
	at( '+' )  = { Line( { { 0.3f, 0.25f }, { 0.3f, 0.75f } } ), Line( { { 0.05f, 0.5f }, { 0.55f, 0.5f } } ) };
	at( '-' )  = { Line( { { 0.1f, 0.5f }, { 0.5f, 0.5f } } ) };
	at( '.' )  = { Line( { { 0.3f, 0.075f }, { 0.3f, 0.09f } } ) };
	at( ':' )  = { Line( { { 0.3f, 0.25f }, { 0.3f, 0.26f } } ), Line( { { 0.3f, 0.70f }, { 0.3f, 0.71f } } ) };
	at( '/' )  = { Line( { { 0.08f, 0.075f }, { 0.52f, 0.925f } } ) };
	at( '!' )  = { Line( { { 0.3f, 0.925f }, { 0.3f, 0.32f } } ), Line( { { 0.3f, 0.075f }, { 0.3f, 0.09f } } ) };
	return g;
}

float SegmentDistance( float px, float py, float ax, float ay, float bx, float by )
{
	const float dx = bx - ax, dy = by - ay;
	const float l2 = dx * dx + dy * dy;
	float t        = l2 > 0.0f ? ( ( px - ax ) * dx + ( py - ay ) * dy ) / l2 : 0.0f;
	t              = std::clamp( t, 0.0f, 1.0f );
	const float x = ax + t * dx - px, y = ay + t * dy - py;
	return std::sqrt( x * x + y * y );
}

uint8_t Encode( float distancePx )
{
	const float v = GlyphAtlas::kOnEdge + distancePx * ( GlyphAtlas::kOnEdge / GlyphAtlas::kSpread );
	return static_cast< uint8_t >( std::clamp( std::lround( v ), 0L, 255L ) );
}

void CellOrigin( int g, int& x, int& y )
{
	x = ( g % GlyphAtlas::kColumns ) * GlyphAtlas::kCell;
	y = ( g / GlyphAtlas::kColumns ) * GlyphAtlas::kCell;
}

/// The built-in character into its cell, distance computed exactly from the strokes.
void DrawBuiltin( GlyphAtlas& atlas, int index, float pxPerUnit )
{
	int cx = 0, cy = 0;
	CellOrigin( index, cx, cy );
	//Local origin 20 px in from the cell's bottom-left: the ink (0..0.62 x 0..1,
	//plus the stroke) and the spread both fit in 128 px at 88 px per H.
	const float ox = static_cast< float >( cx ) + 20.0f;
	const float oy = static_cast< float >( cy ) + 20.0f;
	const auto& strokes = BuiltinStrokes( index );
	for( int y = 0; y < GlyphAtlas::kCell; ++y )
		for( int x = 0; x < GlyphAtlas::kCell; ++x )
		{
			const float lx = ( static_cast< float >( cx + x ) + 0.5f - ox ) / pxPerUnit;
			const float ly = ( static_cast< float >( cy + y ) + 0.5f - oy ) / pxPerUnit;
			float nearest  = 1e9f;
			for( const auto& stroke : strokes )
				for( size_t i = 0; i + 1 < stroke.size(); ++i )
					nearest = std::min( nearest, SegmentDistance( lx, ly, stroke[ i ].first, stroke[ i ].second,
					                                              stroke[ i + 1 ].first, stroke[ i + 1 ].second ) );
			const float inside = kBuiltinHalfWidth - nearest;//H units, positive inside
			atlas.pixels[ static_cast< size_t >( cy + y ) * GlyphAtlas::kWidth + static_cast< size_t >( cx + x ) ] =
				Encode( inside * pxPerUnit );
		}
	GlyphAtlas::Glyph& g = atlas.glyph[ index ];
	g.originX            = ox;
	g.originY            = oy;
	g.x0                 = static_cast< float >( cx );
	g.y0                 = static_cast< float >( cy );
	g.x1                 = static_cast< float >( cx + GlyphAtlas::kCell );
	g.y1                 = static_cast< float >( cy + GlyphAtlas::kCell );
	float inkLo = 1e9f, inkHi = -1e9f;
	for( const auto& stroke : strokes )
		for( const auto& p : stroke )
		{
			inkLo = std::min( inkLo, p.first );
			inkHi = std::max( inkHi, p.first );
		}
	g.inkX0   = inkLo - kBuiltinHalfWidth;
	g.inkX1   = inkHi + kBuiltinHalfWidth;
	//Proportional: the ink plus a constant side bearing, so I and 1 do not
	//stand in a W's width of space.
	g.advance = std::max( 0.3f, g.inkX1 + 0.14f );
}
} // namespace

const std::vector< std::vector< std::pair< float, float > > >& BuiltinStrokes( int g )
{
	static const std::vector< std::vector< Stroke > > glyphs = MakeStrokes();
	return glyphs[ static_cast< size_t >( std::clamp( g, 0, kGlyphCount - 1 ) ) ];
}

GlyphAtlas Typeface::Build() const
{
	GlyphAtlas atlas;
	atlas.pixels.assign( static_cast< size_t >( GlyphAtlas::kWidth ) * GlyphAtlas::kHeight, 0 );
	atlas.pxPerUnit = 88.0f;
	atlas.builtin   = !loaded;
	atlas.family    = loaded ? family : std::string();

	const stbtt_fontinfo* font = AsFontInfo( info );
	bool have[ kGlyphCount ]   = {};
	if( loaded && font != nullptr )
	{
		//The union of the digits' and capitals' ink, in font units: H is its
		//height, and its bottom is local y = 0. Width is checked too, so an
		//extended face still fits a cell.
		int unionY0 = 1 << 30, unionY1 = -( 1 << 30 ), widest = 0, tallest = 0;
		for( int i = 0; i < kGlyphCount; ++i )
		{
			const int glyph = stbtt_FindGlyphIndex( font, kCharset[ i ] );
			int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
			if( glyph == 0 || !stbtt_GetGlyphBox( font, glyph, &x0, &y0, &x1, &y1 ) )
				continue;
			have[ i ] = true;
			if( i < 36 )
			{
				unionY0 = std::min( unionY0, y0 );
				unionY1 = std::max( unionY1, y1 );
			}
			widest  = std::max( widest, x1 - x0 );
			tallest = std::max( tallest, y1 - y0 );
		}
		const int unitsH = unionY1 - unionY0;
		if( unitsH > 0 )
		{
			const float usable = static_cast< float >( GlyphAtlas::kCell ) - 2.0f * ( GlyphAtlas::kSpread + 4.0f );
			float px           = 88.0f;
			if( widest > 0 )
				px = std::min( px, usable * static_cast< float >( unitsH ) / static_cast< float >( widest ) );
			if( tallest > 0 )
				px = std::min( px, usable * static_cast< float >( unitsH ) / static_cast< float >( tallest ) );
			atlas.pxPerUnit   = px;
			const float scale = px / static_cast< float >( unitsH );

			for( int i = 0; i < kGlyphCount; ++i )
			{
				if( !have[ i ] )
					continue;
				const int glyph = stbtt_FindGlyphIndex( font, kCharset[ i ] );
				int w = 0, h = 0, xoff = 0, yoff = 0;
				unsigned char* sdf = stbtt_GetGlyphSDF( font, scale, glyph, static_cast< int >( GlyphAtlas::kSpread ),
				                                        static_cast< unsigned char >( GlyphAtlas::kOnEdge ),
				                                        GlyphAtlas::kOnEdge / GlyphAtlas::kSpread, &w, &h, &xoff, &yoff );
				if( sdf == nullptr || w <= 0 || h <= 0 || w > GlyphAtlas::kCell - 4 || h > GlyphAtlas::kCell - 4 )
				{
					if( sdf != nullptr )
						stbtt_FreeSDF( sdf, nullptr );
					have[ i ] = false;
					continue;
				}
				int cx = 0, cy = 0;
				CellOrigin( i, cx, cy );
				//Centred in the cell. Bitmap row 0 is the top; the atlas's row 0
				//is the bottom.
				const int ax0  = cx + ( GlyphAtlas::kCell - w ) / 2;
				const int aTop = cy + ( GlyphAtlas::kCell + h ) / 2 - 1;
				for( int by = 0; by < h; ++by )
					for( int bx = 0; bx < w; ++bx )
						atlas.pixels[ static_cast< size_t >( aTop - by ) * GlyphAtlas::kWidth + static_cast< size_t >( ax0 + bx ) ] =
							sdf[ by * w + bx ];
				stbtt_FreeSDF( sdf, nullptr );

				//stb samples bitmap pixel (bx, by) at glyph px (xoff + bx + 0.5,
				//down: yoff + by + 0.5); GL samples atlas texel (ax0 + bx,
				//aTop - by) at its centre. So the pen's origin is at atlas
				//(ax0 - xoff, aTop + yoff + 1), and local y = 0 is the union's
				//bottom, unionY0 * scale above the baseline.
				GlyphAtlas::Glyph& g = atlas.glyph[ i ];
				g.originX = static_cast< float >( ax0 - xoff );
				g.originY = static_cast< float >( aTop + yoff + 1 ) + static_cast< float >( unionY0 ) * scale;
				g.x0      = static_cast< float >( cx );
				g.y0      = static_cast< float >( cy );
				g.x1      = static_cast< float >( cx + GlyphAtlas::kCell );
				g.y1      = static_cast< float >( cy + GlyphAtlas::kCell );
				int advance = 0, bearing = 0;
				stbtt_GetGlyphHMetrics( font, glyph, &advance, &bearing );
				int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
				stbtt_GetGlyphBox( font, glyph, &x0, &y0, &x1, &y1 );
				g.advance = static_cast< float >( advance ) / static_cast< float >( unitsH );
				g.inkX0   = static_cast< float >( x0 ) / static_cast< float >( unitsH );
				g.inkX1   = static_cast< float >( x1 ) / static_cast< float >( unitsH );
			}
		}
	}

	//Whatever the font could not draw, the built-in face draws -- at the same
	//px per H, so a mixed set still lines up.
	int fromBuiltin = 0;
	for( int i = 0; i < kGlyphCount; ++i )
		if( !have[ i ] )
		{
			DrawBuiltin( atlas, i, atlas.pxPerUnit );
			++fromBuiltin;
		}
	if( fromBuiltin == kGlyphCount )
	{
		atlas.builtin = true;
		atlas.family.clear();
	}
	return atlas;
}

float GlyphAtlas::Distance( int index, float x, float y ) const
{
	const Glyph& g = glyph[ std::clamp( index, 0, kGlyphCount - 1 ) ];
	//Texel centres at +0.5, as GL samples them.
	const float ax = g.originX + x * pxPerUnit - 0.5f;
	const float ay = g.originY + y * pxPerUnit - 0.5f;
	if( ax < g.x0 || ay < g.y0 || ax > g.x1 - 1.0f || ay > g.y1 - 1.0f )
		return -kSpread / pxPerUnit;
	const int ix = static_cast< int >( std::floor( ax ) ), iy = static_cast< int >( std::floor( ay ) );
	const float fx = ax - static_cast< float >( ix ), fy = ay - static_cast< float >( iy );
	auto at = [ & ]( int px, int py ) {
		px = std::clamp( px, 0, kWidth - 1 );
		py = std::clamp( py, 0, kHeight - 1 );
		return static_cast< float >( pixels[ static_cast< size_t >( py ) * kWidth + static_cast< size_t >( px ) ] );
	};
	const float v = ( at( ix, iy ) * ( 1 - fx ) + at( ix + 1, iy ) * fx ) * ( 1 - fy )
	              + ( at( ix, iy + 1 ) * ( 1 - fx ) + at( ix + 1, iy + 1 ) * fx ) * fy;
	return ( v - kOnEdge ) / ( kOnEdge / kSpread ) / pxPerUnit;
}

float GlyphAtlas::Width( const std::string& text, float tracking ) const
{
	float pen = 0.0f, lo = 1e9f, hi = -1e9f;
	for( char c : text )
	{
		const int g = GlyphIndex( c );
		if( g >= 0 )
		{
			lo = std::min( lo, pen + glyph[ g ].inkX0 );
			hi = std::max( hi, pen + glyph[ g ].inkX1 );
			pen += glyph[ g ].advance + tracking;
		}
		else
			pen += 0.35f + tracking;
	}
	return hi > lo ? hi - lo : 0.0f;
}

} // namespace jackpot
