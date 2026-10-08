#pragma once

#include <cstdint>
#include <vector>

/**
    The reel symbols, painted once on the CPU into an RGBA atlas.

    Cherries, lemon, orange, plum, bell, melon, BAR (one, two and three), seven
    and diamond, each drawn from signed-distance shapes with an analytic
    one-pixel edge, lit as a sphere or a bevel where that is what the thing is,
    and outlined the way a reel strip's print is. Painted rather than shaded in
    GLSL so the harness has the exact picture to read a payline back against
    (`jptest --slots-readback`).

    Tiles are kTile px square, kColumns across, premultiplied, row 0 at the
    BOTTOM (GL's order). Tile `s` is symbol `s` of slots::Symbol; tile 0 (the
    blank) is empty.
*/
namespace jackpot
{
struct SymbolAtlas
{
	static constexpr int kTile    = 192;
	static constexpr int kColumns = 4;
	static constexpr int kRows    = 4;
	static constexpr int kWidth   = kTile * kColumns;
	static constexpr int kHeight  = kTile * kRows;

	std::vector< uint8_t > rgba;///< kWidth x kHeight x 4, premultiplied

	/// The premultiplied colour at tile-local (u, v) in 0..1 of symbol `s`,
	/// nearest texel.
	void At( int s, double u, double v, double out[ 4 ] ) const;
};

/// Paint every symbol. Deterministic: the same bytes on every machine.
SymbolAtlas PaintSymbols();

} // namespace jackpot
