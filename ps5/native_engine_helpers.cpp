#include "native_engine_helpers.h"

namespace bedlam2::native_engine
{

std::uint32_t z_buffer_byte_offset(std::int32_t x, std::int32_t y, std::int32_t z,
                                   std::int32_t tiles_x, std::int32_t tiles_y) noexcept
{
    // The x86 routine uses 32-bit IMUL/ADD/SHL instructions, so preserve their
    // modulo-2^32 arithmetic even though this is compiled for a 64-bit target.
    const auto ux = static_cast<std::uint32_t>(x);
    const auto uy = static_cast<std::uint32_t>(y);
    const auto uz = static_cast<std::uint32_t>(z);
    const auto utx = static_cast<std::uint32_t>(tiles_x);
    const auto uty = static_cast<std::uint32_t>(tiles_y);
    const std::uint32_t tile_index = ux + (utx * uy) + (utx * uty * uz);
    return tile_index << 2;
}

void complete_missing_zone_levels(std::int32_t map_screen_number,
                                  std::uint32_t& passed_level_mask) noexcept
{
    switch (map_screen_number)
    {
    case 4:
        passed_level_mask |= 8;
        break;
    case 5:
        passed_level_mask |= 2;
        break;
    case 6:
        passed_level_mask |= 9;
        break;
    case 7:
        passed_level_mask |= 10;
        break;
    default:
        break;
    }
}

} // namespace bedlam2::native_engine
