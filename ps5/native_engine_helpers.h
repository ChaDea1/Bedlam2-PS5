#pragma once

#include <cstdint>

namespace bedlam2::native_engine
{

// Equivalent to the legacy get_zbuff_pos routine. The result is a byte offset
// into the 32-bit z-buffer, not a host pointer.
std::uint32_t z_buffer_byte_offset(std::int32_t x, std::int32_t y, std::int32_t z,
                                   std::int32_t tiles_x, std::int32_t tiles_y) noexcept;

// Equivalent to complete_missing_levels. The mask uses the original game's
// four zone-completion bits.
void complete_missing_zone_levels(std::int32_t map_screen_number,
                                 std::uint32_t& passed_level_mask) noexcept;

} // namespace bedlam2::native_engine
