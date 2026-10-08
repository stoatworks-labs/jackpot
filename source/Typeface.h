#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
    Fonts: finding them, and turning one into the characters a casino needs.

    The scan and the loader are polyhedral's (which are downpour's), unchanged:
    walk the OS font directories reading only each file's `name` table, list one
    entry per family, load a chosen file through stb_truetype. What changed is
    the atlas: polyhedral needed ten digits; a craps layout needs PASS LINE and
    DON'T COME, a slot machine BAR and CREDITS, a money wheel $20. So the atlas
    holds `kCharset` -- the digits, the capitals and a little punctuation.

    ## A signed-distance atlas, not a bitmap

    A number on a roulette pocket is seen at every size and slant, small and
    foreshortened, turning. A signed-distance field stores, per texel, how far
    that texel is from the glyph's outline, and a bilinear sample of it is still
    a good distance at any magnification, so the shader cuts a crisp edge at the
    pixel footprint it is actually drawn at. `stbtt_GetCodepointSDF` builds it
    from the outline directly.

    One 128-px cell per character, scaled so the union of the digits' and the
    capitals' ink fills a common height `H` (at most 88 px). Layout is in units of
    `H`: a capital sits in 0..1 vertically whatever the font's own metrics.

    ## The built-in face

    Stroked characters defined here as polylines, with an exact distance field
    computed on the CPU. It is what a composition gets when the font it names is
    not installed on this machine, so the text never vanishes -- and it is the
    default, because it is the same everywhere.
*/
namespace jackpot
{
struct FontFile
{
	std::string family;
	std::string path;
	int collectionIndex = 0;
};

/// Every font in the OS font directories, one per family, sorted. Scanned once
/// per process.
const std::vector< FontFile >& InstalledFonts();

/// Index of the font whose family matches (case-insensitive), or -1.
int FindFontByFamily( const std::string& family );

/// The characters the atlas holds, in cell order. Anything else is drawn as a
/// space. Mirrored in the shader only through the glyph table it is handed.
constexpr const char* kCharset = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ$'+-.:/!";
constexpr int kGlyphCount      = 44;

/// The cell of character `c`, or -1 (a space, or anything not in kCharset).
int GlyphIndex( char c );

struct GlyphAtlas
{
	static constexpr int kCell    = 128;
	static constexpr int kColumns = 8;
	static constexpr int kRows    = 6;
	static constexpr int kWidth   = kCell * kColumns;
	static constexpr int kHeight  = kCell * kRows;
	static constexpr float kOnEdge = 128.0f;///< the byte value on the outline
	static constexpr float kSpread = 12.0f; ///< px of distance either side the bytes cover

	struct Glyph
	{
		float originX = 0, originY = 0;   ///< atlas px (y up) of the glyph's local origin
		float x0 = 0, y0 = 0, x1 = 0, y1 = 0;///< the cell's usable rectangle, atlas px
		float advance = 0.7f;             ///< H units
		float inkX0 = 0.0f, inkX1 = 0.6f; ///< H units, from the pen
	};

	std::vector< uint8_t > pixels;///< kWidth x kHeight, row 0 at the bottom
	Glyph glyph[ kGlyphCount ];
	float pxPerUnit = 88.0f;      ///< atlas px per H
	bool builtin    = true;
	std::string family;           ///< empty for the built-in face

	/// Signed distance in H units at local point (x, y) of glyph g: positive
	/// inside. Bilinear, exactly as the shader samples it.
	float Distance( int g, float x, float y ) const;

	/// The width of `text` set solid, ink edge to ink edge, in H units.
	float Width( const std::string& text, float tracking = 0.0f ) const;
};

class Typeface
{
public:
	bool Load( const std::string& path, int collectionIndex = 0 );
	void UseBuiltin();
	bool HasFont() const
	{
		return loaded;
	}
	const std::string& Family() const
	{
		return family;
	}

	/// The charset from this face, or from the built-in face for any character
	/// it does not have (and for all of them when nothing is loaded).
	GlyphAtlas Build() const;

private:
	std::vector< unsigned char > data;
	std::string family;
	bool loaded = false;
	/// stbtt_fontinfo, kept opaque so stb_truetype.h stays out of this header.
	std::vector< unsigned char > info;
};

/// The built-in stroked character `g` as polylines in H units, for the atlas
/// and for the slot symbols' BAR. Each glyph is a list of strokes; each stroke
/// a list of points.
const std::vector< std::vector< std::pair< float, float > > >& BuiltinStrokes( int g );
constexpr float kBuiltinHalfWidth = 0.075f;///< H units

} // namespace jackpot
