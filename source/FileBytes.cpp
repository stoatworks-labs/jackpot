#include "FileBytes.h"

#include <cerrno>
#include <cstdio>
#include <system_error>

#if defined( _WIN32 )
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#include <windows.h>
#else
	#include <sys/stat.h>
#endif

namespace jackpot::files
{
// From radar's SpriteImage.cpp (radar#5), unchanged but for the size cap,
// which is the caller's here.
bool Utf8ToUtf16( const std::string& in, std::u16string& out )
{
	out.clear();
	const size_t n = in.size();
	size_t i       = 0;
	while( i < n )
	{
		const unsigned char c = static_cast< unsigned char >( in[ i ] );
		char32_t cp           = 0;
		int extra             = 0;
		char32_t minimum      = 0;
		if( c < 0x80 )
		{
			cp = c;
		}
		else if( ( c & 0xE0 ) == 0xC0 )
		{
			cp      = c & 0x1F;
			extra   = 1;
			minimum = 0x80;
		}
		else if( ( c & 0xF0 ) == 0xE0 )
		{
			cp      = c & 0x0F;
			extra   = 2;
			minimum = 0x800;
		}
		else if( ( c & 0xF8 ) == 0xF0 )
		{
			cp      = c & 0x07;
			extra   = 3;
			minimum = 0x10000;
		}
		else
			return false;//a continuation byte where a lead was due, or 0xF8+
		if( i + static_cast< size_t >( extra ) >= n && extra > 0 )
			return false;//truncated: the sequence runs past the end
		for( int k = 1; k <= extra; ++k )
		{
			const unsigned char d = static_cast< unsigned char >( in[ i + static_cast< size_t >( k ) ] );
			if( ( d & 0xC0 ) != 0x80 )
				return false;
			cp = ( cp << 6 ) | ( d & 0x3F );
		}
		if( extra > 0 && cp < minimum )
			return false;//overlong
		if( cp > 0x10FFFF || ( cp >= 0xD800 && cp <= 0xDFFF ) )
			return false;
		if( cp >= 0x10000 )
		{
			const char32_t v = cp - 0x10000;
			out.push_back( static_cast< char16_t >( 0xD800 + ( v >> 10 ) ) );
			out.push_back( static_cast< char16_t >( 0xDC00 + ( v & 0x3FF ) ) );
		}
		else
			out.push_back( static_cast< char16_t >( cp ) );
		i += 1 + static_cast< size_t >( extra );
	}
	return true;
}

std::u16string WidenBytesForTest( const std::string& in )
{
	std::u16string out;
	for( char c : in )
		out.push_back( static_cast< char16_t >( static_cast< unsigned char >( c ) ) );
	return out;
}

bool StampFile( const std::string& utf8Path, FileStamp& out )
{
	out = FileStamp {};
#if defined( _WIN32 )
	WIN32_FILE_ATTRIBUTE_DATA data {};
	std::u16string wide16;
	BOOL found = FALSE;
	if( Utf8ToUtf16( utf8Path, wide16 ) )
	{
		const std::wstring wide( wide16.begin(), wide16.end() );
		found = GetFileAttributesExW( wide.c_str(), GetFileExInfoStandard, &data );
	}
	else
		found = GetFileAttributesExA( utf8Path.c_str(), GetFileExInfoStandard, &data );
	if( !found )
		return false;
	out.size     = ( static_cast< unsigned long long >( data.nFileSizeHigh ) << 32 ) | data.nFileSizeLow;
	out.modified = static_cast< long long >( ( static_cast< unsigned long long >( data.ftLastWriteTime.dwHighDateTime ) << 32 ) | data.ftLastWriteTime.dwLowDateTime );
	return true;
#else
	struct stat info {};
	if( stat( utf8Path.c_str(), &info ) != 0 )
		return false;
	out.size = static_cast< unsigned long long >( info.st_size );
	#if defined( __APPLE__ )
	out.modified = static_cast< long long >( info.st_mtimespec.tv_sec ) * 1000000000LL + info.st_mtimespec.tv_nsec;
	#else
	out.modified = static_cast< long long >( info.st_mtim.tv_sec ) * 1000000000LL + info.st_mtim.tv_nsec;
	#endif
	return true;
#endif
}

bool ReadFileBytes( const std::string& utf8Path, std::vector< unsigned char >& out, std::string& why, size_t maxBytes )
{
	out.clear();
	std::FILE* file = nullptr;
#if defined( _WIN32 )
	std::u16string wide16;
	if( Utf8ToUtf16( utf8Path, wide16 ) )
	{
		const std::wstring wide( wide16.begin(), wide16.end() );
	#if defined( _MSC_VER )
		if( _wfopen_s( &file, wide.c_str(), L"rb" ) != 0 )
			file = nullptr;
	#else
		file = _wfopen( wide.c_str(), L"rb" );
	#endif
	}
	else
	{
		//Not UTF-8 at all: a host handing over its ANSI code page would look
		//like this, and the narrow open reads exactly that. Never tried for a
		//path that IS UTF-8, so it cannot hide the wide open's error.
	#if defined( _MSC_VER )
		if( fopen_s( &file, utf8Path.c_str(), "rb" ) != 0 )
			file = nullptr;
	#else
		file = std::fopen( utf8Path.c_str(), "rb" );
	#endif
		if( file == nullptr )
		{
			why = "is not UTF-8, and cannot be opened in the system code page either";
			return false;
		}
	}
#else
	file = std::fopen( utf8Path.c_str(), "rb" );
#endif
	if( file == nullptr )
	{
		why = "cannot be opened (" + std::generic_category().message( errno ) + ")";
		return false;
	}
	//On the heap: this runs on the host's render thread, whose stack is its own.
	std::vector< unsigned char > chunk( 65536 );
	size_t got = 0;
	while( ( got = std::fread( chunk.data(), 1, chunk.size(), file ) ) > 0 )
	{
		out.insert( out.end(), chunk.begin(), chunk.begin() + static_cast< std::ptrdiff_t >( got ) );
		if( out.size() > maxBytes )
		{
			std::fclose( file );
			out.clear();
			why = maxBytes >= 1024 * 1024 ? "is larger than " + std::to_string( maxBytes / ( 1024 * 1024 ) ) + " MB"
			                              : "is larger than " + std::to_string( maxBytes / 1024 ) + " KB";
			return false;
		}
	}
	const bool failed = std::ferror( file ) != 0;
	std::fclose( file );
	if( failed )
	{
		out.clear();
		why = "could not be read to the end";
		return false;
	}
	if( out.empty() )
	{
		why = "is empty";
		return false;
	}
	return true;
}

} // namespace jackpot::files
