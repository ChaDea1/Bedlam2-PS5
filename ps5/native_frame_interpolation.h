#pragma once

#include <cstdint>

namespace bedlam2::native_frame_interpolation
{

class FrameInterpolator
{
public:
    static constexpr int kMaxWidth = 1920;
    static constexpr int kMaxHeight = 1080;
    static constexpr int kPitch = kMaxWidth;
    static constexpr int kPixelCount = kMaxWidth * kMaxHeight;

    void reset();
    void capture(const std::uint8_t *pixels, int width, int height, int pitch,
                 std::uint64_t now, std::uint64_t frequency);

    bool ready() const;
    const std::uint8_t *current_frame() const;
    std::uint8_t *output_buffer();
    int output_pitch() const;
    const std::uint8_t *interpolated_frame(std::uint64_t now) const;

private:
    struct Motion
    {
        std::int8_t x = 0;
        std::int8_t y = 0;
        bool valid = false;
    };

    static constexpr int kBlockSize = 32;
    static constexpr int kBlocksX = (kMaxWidth + kBlockSize - 1) / kBlockSize;
    static constexpr int kBlocksY = (kMaxHeight + kBlockSize - 1) / kBlockSize;
    static constexpr int kBlockCount = kBlocksX * kBlocksY;

    void estimate_motion();
    void estimate_local_motion();
    std::uint64_t interpolation_interval(std::uint64_t frequency) const;

    std::uint8_t m_frames[2][kPixelCount]{};
    mutable std::uint8_t m_output[kPixelCount]{};
    int m_width = 0;
    int m_height = 0;
    int m_previous_index = 0;
    int m_current_index = 0;
    bool m_has_current = false;
    bool m_has_pair = false;
    bool m_global_motion_confident = false;
    std::int8_t m_motion_x = 0;
    std::int8_t m_motion_y = 0;
    Motion m_local_motion[kBlockCount]{};
    std::uint64_t m_current_time = 0;
    std::uint64_t m_previous_interval = 0;
    std::uint64_t m_frequency = 0;
};

} // namespace bedlam2::native_frame_interpolation
