#pragma once

#include <string>

/**
    The GLSL, as text.

    One full-screen pass per game, each assembled as kVersion + kCommon + the
    game's piece, and the shower's instanced pass. Every game draws in a 2D
    "canvas" (y up, the frame's height is 1 at Zoom 1) or casts rays into its
    3D scene from a camera the CPU hands it.

      kCommon     hashing, 2D distance helpers, text from the SDF atlas, the
                  backdrop, the Over composite
      kSlots      the cabinet, the reels (cylinders, integrated over the
                  shutter), the handle, the meters, the lights
      kRoulette   the wheel: a surface of revolution, ray-marched, with
                  angular domain repetition for the pockets and frets
      kWheel      the money wheel, face on
      kCraps      the table's layout and the two dice
      kLottery    the drum and its balls
      kShowerVS/FS   one quad per piece; exact ray/cylinder in the fragment
      kComposite  the shower's buffer over the frame

    Each piece is kept under MSVC's ~16 KB string-literal cap; tools/glslc.sh
    reassembles them in this order.
*/
namespace jackpot::shaders
{
extern const char* const kVersion;
extern const char* const kQuadVertex;
extern const char* const kCommon;
extern const char* const kText;
extern const char* const kSlots;
extern const char* const kSlotsMain;
extern const char* const kRoulette;
extern const char* const kRouletteMain;
extern const char* const kWheel;
extern const char* const kCraps;
extern const char* const kCrapsMain;
extern const char* const kLottery;
extern const char* const kBlank;
extern const char* const kShowerVertex;
extern const char* const kShowerFragment;
extern const char* const kComposite;

std::string Assemble( std::initializer_list< const char* > pieces );

} // namespace jackpot::shaders
