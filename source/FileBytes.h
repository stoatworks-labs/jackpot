#pragma once

#include <cstddef>
#include <string>
#include <vector>

/**
    Reading a file by the path a host hands over, which is UTF-8.

    Copied from radar (`source/SpriteImage.cpp`, `ReadFileBytes` and
    `Utf8ToUtf16`, radar#5), which found the trap first: on Windows a narrow
    `fopen` reads its path in the ANSI code page, so a folder with a non-ASCII
    name ("Glücksrad", "Мой проект") cannot be opened at all. The UTF-8 path is
    widened to UTF-16 for `_wfopen_s` instead, with a converter that refuses
    what is not UTF-8 rather than guessing at it; a path that is not UTF-8 at
    all (a host handing over its code page) is opened narrow, as that is
    exactly what it is.

    Used by the Wheel File (WheelFace.cpp) and by Font File (Typeface.cpp),
    which had the same bug.
*/
namespace jackpot::files
{
/// Read a file by its UTF-8 path, whole, into `out`. Anything past `maxBytes`
/// is refused rather than truncated. `why` says what went wrong, for the log.
bool ReadFileBytes( const std::string& utf8Path, std::vector< unsigned char >& out, std::string& why, size_t maxBytes );

/// A file's size and last write, to tell whether it changed since it was
/// read without reading it again. `modified` is the platform's own ticks
/// (nanoseconds here, 100 ns on Windows): compared, never interpreted.
struct FileStamp
{
	unsigned long long size = 0;
	long long modified      = 0;
	bool operator==( const FileStamp& o ) const
	{
		return size == o.size && modified == o.modified;
	}
};

/// The stamp of a file by its UTF-8 path (UTF-16 on Windows, as ReadFileBytes).
/// False when there is no such file.
bool StampFile( const std::string& utf8Path, FileStamp& out );

/// Strict UTF-8 to UTF-16 (what `_wfopen` takes on Windows): overlong forms,
/// surrogates, code points past U+10FFFF and truncated sequences are refused,
/// never guessed at. Portable, so the harness checks it on macOS
/// (`jptest --wheel-file`); only Windows calls it for real.
bool Utf8ToUtf16( const std::string& in, std::u16string& out );

/// The negative control's wrong model: every byte widened to one code unit,
/// which is what a narrow fopen amounts to for a path that is not ASCII.
std::u16string WidenBytesForTest( const std::string& in );

} // namespace jackpot::files
