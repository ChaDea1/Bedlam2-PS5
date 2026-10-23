#include "native_frame_interpolation.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace bedlam2::native_frame_interpolation
{
namespace
{

// A single scene translation is intentionally conservative. It avoids the
// block seams and per-block texture deformation caused by local matching.
constexpr int kSearchRadius = 20;
constexpr int kSearchStep = 2;
constexpr int kSampleSpacing = 48;
constexpr int kMinimumImprovementDivisor = 16;
constexpr int kLocalSearchRadius = 12;
constexpr int kLocalSearchStep = 4;
constexpr int kLocalSamplesPerAxis = 6;
constexpr int kLocalMinimumImprovement = 4;
constexpr std::uint64_t kMinimumIntervalDenominator = 120;
constexpr std::uint64_t kMaximumFrameGapDenominator = 4;

} // namespace

void FrameInterpolator::reset()
{
    m_has_current = false;
    m_has_pair = false;
    m_width = 0;
    m_height = 0;
    m_current_time = 0;
    m_previous_interval = 0;
    m_frequency = 0;
    m_motion_x = 0;
    m_motion_y = 0;
    m_global_motion_confident = false;
    std::fill_n(m_local_motion, kBlockCount, Motion{});
}

void FrameInterpolator::capture(const std::uint8_t *pixels, int width, int height,
                                int pitch, std::uint64_t now,
                                std::uint64_t frequency)
{
    if (!pixels || width < 1 || height < 1 || width > kMaxWidth ||
        height > kMaxHeight || pitch < width || frequency == 0)
    {
        reset();
        return;
    }

    if (width != m_width || height != m_height)
    {
        reset();
        m_width = width;
        m_height = height;
    }

    m_frequency = frequency;
    if (m_has_current)
    {
        bool unchanged = true;
        for (int y = 0; y < m_height; ++y)
        {
            const std::uint8_t *source = pixels + y * pitch;
            const std::uint8_t *current =
                m_frames[m_current_index] + y * kPitch;
            if (std::memcmp(source, current, m_width) != 0)
            {
                unchanged = false;
                break;
            }
        }
        if (unchanged)
        {
            return;
        }
    }

    const int next_index = m_has_current ? 1 - m_current_index : 0;
    for (int y = 0; y < m_height; ++y)
    {
        std::memcpy(m_frames[next_index] + y * kPitch, pixels + y * pitch,
                    m_width);
    }

    if (!m_has_current)
    {
        m_current_index = next_index;
        m_current_time = now;
        m_has_current = true;
        m_has_pair = false;
        return;
    }

    const std::uint64_t elapsed = now >= m_current_time ? now - m_current_time : 0;
    const std::uint64_t maximum_gap =
        std::max<std::uint64_t>(1, frequency / kMaximumFrameGapDenominator);
    if (elapsed > maximum_gap)
    {
        // A pause, loading stall, or screen transition should not animate an
        // old scene into the first frame after the interruption.
        m_current_index = next_index;
        m_current_time = now;
        m_has_pair = false;
        m_motion_x = 0;
        m_motion_y = 0;
        m_global_motion_confident = false;
        std::fill_n(m_local_motion, kBlockCount, Motion{});
        return;
    }

    m_previous_index = m_current_index;
    m_current_index = next_index;
    m_previous_interval = std::max<std::uint64_t>(
        elapsed, std::max<std::uint64_t>(1, frequency / kMinimumIntervalDenominator));
    m_current_time = now;
    m_has_pair = true;
    estimate_motion();
}

bool FrameInterpolator::ready() const
{
    return m_has_current && m_has_pair;
}

const std::uint8_t *FrameInterpolator::current_frame() const
{
    return m_has_current ? m_frames[m_current_index] : nullptr;
}

std::uint8_t *FrameInterpolator::output_buffer()
{
    return m_output;
}

int FrameInterpolator::output_pitch() const
{
    return m_width;
}

std::uint64_t FrameInterpolator::interpolation_interval(std::uint64_t frequency) const
{
    if (m_previous_interval != 0)
    {
        return m_previous_interval;
    }
    return std::max<std::uint64_t>(1, frequency / 22);
}

const std::uint8_t *FrameInterpolator::interpolated_frame(std::uint64_t now) const
{
    if (!m_has_current)
    {
        return nullptr;
    }

    const std::uint8_t *previous = m_frames[m_previous_index];
    const std::uint8_t *current = m_frames[m_current_index];
    if (!m_has_pair || m_frequency == 0 || m_previous_interval == 0)
    {
        previous = current;
    }

    double alpha = 1.0;
    if (m_has_pair && m_frequency != 0 && m_previous_interval != 0)
    {
        const std::uint64_t elapsed = now >= m_current_time ? now - m_current_time : 0;
        const std::uint64_t interval = interpolation_interval(m_frequency);
        alpha = std::min(1.0, static_cast<double>(elapsed) /
                                   static_cast<double>(interval));
    }

    const bool use_current = alpha >= 0.5;
    const double shift_fraction = use_current ? 1.0 - alpha : alpha;
    const std::uint8_t *source = use_current ? current : previous;
    const int direction = use_current ? 1 : -1;
    const int shift_x = static_cast<int>(
        std::lround(shift_fraction * m_motion_x)) * direction;
    const int shift_y = static_cast<int>(
        std::lround(shift_fraction * m_motion_y)) * direction;

    for (int y = 0; y < m_height; ++y)
    {
        const int source_y = y + shift_y;
        const std::uint8_t *source_row =
            source + std::clamp(source_y, 0, m_height - 1) * kPitch;
        const std::uint8_t *current_row = current + y * kPitch;
        std::uint8_t *output_row = m_output + y * m_width;

        // Motion is a whole-scene integer translation, so each row is one
        // contiguous copy plus a small newly exposed strip. Avoid branching
        // for every output pixel on the 60 Hz presentation path.
        if (shift_x > 0)
        {
            const int copy_width = m_width - shift_x;
            std::memcpy(output_row, source_row + shift_x, copy_width);
            std::memcpy(output_row + copy_width, current_row + copy_width,
                        shift_x);
        }
        else if (shift_x < 0)
        {
            const int edge_width = -shift_x;
            std::memcpy(output_row, current_row, edge_width);
            std::memcpy(output_row + edge_width, source_row,
                        m_width - edge_width);
        }
        else
        {
            std::memcpy(output_row, source_row, m_width);
        }
    }

    // A moving robot or projectile can be too small to move the whole-scene
    // match away from the stationary background. Overlay only local vectors
    // that have a strong pixel match and support from a neighboring block.
    // The scene-wide translation remains the fallback for camera scrolling.
    const int blocks_x = (m_width + kBlockSize - 1) / kBlockSize;
    const int blocks_y = (m_height + kBlockSize - 1) / kBlockSize;
    for (int block_y = 0; block_y < blocks_y; ++block_y)
    {
        for (int block_x = 0; block_x < blocks_x; ++block_x)
        {
            const Motion &motion = m_local_motion[block_y * kBlocksX + block_x];
            if (!motion.valid ||
                (motion.x == m_motion_x && motion.y == m_motion_y))
            {
                continue;
            }

            const int local_shift_x = direction * static_cast<int>(
                std::lround(shift_fraction * motion.x));
            const int local_shift_y = direction * static_cast<int>(
                std::lround(shift_fraction * motion.y));
            const int left = block_x * kBlockSize;
            const int top = block_y * kBlockSize;
            const int right = std::min(m_width, left + kBlockSize);
            const int bottom = std::min(m_height, top + kBlockSize);
            for (int y = top; y < bottom; ++y)
            {
                const int source_y = y + local_shift_y;
                if (source_y < 0 || source_y >= m_height)
                {
                    continue;
                }

                const int copy_left = std::max(left, -local_shift_x);
                const int copy_right = std::min(right, m_width - local_shift_x);
                if (copy_right <= copy_left)
                {
                    continue;
                }
                const std::uint8_t *source_row =
                    source + source_y * kPitch + copy_left + local_shift_x;
                std::uint8_t *output_row = m_output + y * m_width;
                std::memcpy(output_row + copy_left, source_row,
                            copy_right - copy_left);
            }
        }
    }
    return m_output;
}

void FrameInterpolator::estimate_motion()
{
    const std::uint8_t *previous = m_frames[m_previous_index];
    const std::uint8_t *current = m_frames[m_current_index];
    m_global_motion_confident = false;
    m_motion_x = 0;
    m_motion_y = 0;
    std::fill_n(m_local_motion, kBlockCount, Motion{});
    if (m_width <= 2 * kSearchRadius + 1 ||
        m_height <= 2 * kSearchRadius + 1)
    {
        estimate_local_motion();
        return;
    }

    int sample_count = 0;
    int stationary_mismatches = 0;
    int best_mismatches = 0;
    int best_x = 0;
    int best_y = 0;

    auto score_candidate = [&](int dx, int dy) {
        int mismatches = 0;
        int count = 0;
        for (int y = kSearchRadius + 1; y < m_height - kSearchRadius - 1;
             y += kSampleSpacing)
        {
            for (int x = kSearchRadius + 1; x < m_width - kSearchRadius - 1;
                 x += kSampleSpacing)
            {
                ++count;
                mismatches += previous[y * kPitch + x] !=
                              current[(y + dy) * kPitch + x + dx];
            }
        }
        return std::pair<int, int>{mismatches, count};
    };

    const auto stationary = score_candidate(0, 0);
    stationary_mismatches = stationary.first;
    sample_count = stationary.second;
    best_mismatches = stationary_mismatches;

    auto consider = [&](int dx, int dy) {
        const auto result = score_candidate(dx, dy);
        const int score = result.first * 64 + std::abs(dx) + std::abs(dy);
        const int best_score = best_mismatches * 64 +
                               std::abs(best_x) + std::abs(best_y);
        if (score < best_score)
        {
            best_mismatches = result.first;
            best_x = dx;
            best_y = dy;
        }
    };

    for (int dy = -kSearchRadius; dy <= kSearchRadius; dy += kSearchStep)
    {
        for (int dx = -kSearchRadius; dx <= kSearchRadius; dx += kSearchStep)
        {
            consider(dx, dy);
        }
    }
    for (int dy = best_y - 1; dy <= best_y + 1; ++dy)
    {
        for (int dx = best_x - 1; dx <= best_x + 1; ++dx)
        {
            if (std::abs(dx) <= kSearchRadius && std::abs(dy) <= kSearchRadius)
            {
                consider(dx, dy);
            }
        }
    }

    const int required_improvement =
        std::max(1, sample_count / kMinimumImprovementDivisor);
    const bool confident = sample_count > 0 &&
                           stationary_mismatches - best_mismatches >= required_improvement &&
                           best_mismatches * 4 <= sample_count * 3;
    if (confident)
    {
        m_motion_x = static_cast<std::int8_t>(best_x);
        m_motion_y = static_cast<std::int8_t>(best_y);
        m_global_motion_confident = true;
    }
    estimate_local_motion();
}

void FrameInterpolator::estimate_local_motion()
{
    const std::uint8_t *previous = m_frames[m_previous_index];
    const std::uint8_t *current = m_frames[m_current_index];
    Motion candidates[kBlockCount]{};
    constexpr int sample_count = kLocalSamplesPerAxis * kLocalSamplesPerAxis;

    for (int block_y = 0; block_y < (m_height + kBlockSize - 1) / kBlockSize;
         ++block_y)
    {
        for (int block_x = 0; block_x < (m_width + kBlockSize - 1) / kBlockSize;
             ++block_x)
        {
            const int left = block_x * kBlockSize;
            const int top = block_y * kBlockSize;
            const int block_width = std::min(kBlockSize, m_width - left);
            const int block_height = std::min(kBlockSize, m_height - top);
            if (block_width < kLocalSamplesPerAxis ||
                block_height < kLocalSamplesPerAxis)
            {
                continue;
            }

            int sample_x[kLocalSamplesPerAxis];
            int sample_y[kLocalSamplesPerAxis];
            for (int sample = 0; sample < kLocalSamplesPerAxis; ++sample)
            {
                sample_x[sample] = (2 * sample + 1) * block_width /
                                   (2 * kLocalSamplesPerAxis);
                sample_y[sample] = (2 * sample + 1) * block_height /
                                   (2 * kLocalSamplesPerAxis);
            }

            int stationary_mismatches = 0;
            for (int sy : sample_y)
            {
                const int row = (top + sy) * kPitch + left;
                for (int sx : sample_x)
                {
                    stationary_mismatches += previous[row + sx] !=
                                             current[row + sx];
                }
            }
            if (stationary_mismatches < kLocalMinimumImprovement)
            {
                continue;
            }

            int best_x = 0;
            int best_y = 0;
            int best_mismatches = stationary_mismatches;
            int best_score = best_mismatches * 64;
            auto score_candidate = [&](int dx, int dy) {
                if (left + dx < 0 || top + dy < 0 ||
                    left + dx + block_width > m_width ||
                    top + dy + block_height > m_height)
                {
                    return;
                }

                int mismatches = 0;
                for (int sy : sample_y)
                {
                    const int previous_row = (top + sy) * kPitch + left;
                    const int current_row = (top + dy + sy) * kPitch + left + dx;
                    for (int sx : sample_x)
                    {
                        mismatches += previous[previous_row + sx] !=
                                      current[current_row + sx];
                    }
                }
                const int score = mismatches * 64 + std::abs(dx) + std::abs(dy);
                if (score < best_score)
                {
                    best_score = score;
                    best_mismatches = mismatches;
                    best_x = dx;
                    best_y = dy;
                }
            };

            for (int dy = -kLocalSearchRadius; dy <= kLocalSearchRadius;
                 dy += kLocalSearchStep)
            {
                for (int dx = -kLocalSearchRadius; dx <= kLocalSearchRadius;
                     dx += kLocalSearchStep)
                {
                    score_candidate(dx, dy);
                }
            }
            for (int dy = best_y - kLocalSearchStep + 1;
                 dy <= best_y + kLocalSearchStep - 1; ++dy)
            {
                for (int dx = best_x - kLocalSearchStep + 1;
                     dx <= best_x + kLocalSearchStep - 1; ++dx)
                {
                    score_candidate(dx, dy);
                }
            }

            if (stationary_mismatches - best_mismatches >=
                    kLocalMinimumImprovement &&
                best_mismatches * 3 <= sample_count * 2)
            {
                Motion &motion = candidates[block_y * kBlocksX + block_x];
                motion.x = static_cast<std::int8_t>(best_x);
                motion.y = static_cast<std::int8_t>(best_y);
                motion.valid = true;
            }
        }
    }

    const int blocks_x = (m_width + kBlockSize - 1) / kBlockSize;
    const int blocks_y = (m_height + kBlockSize - 1) / kBlockSize;
    for (int block_y = 0; block_y < blocks_y; ++block_y)
    {
        for (int block_x = 0; block_x < blocks_x; ++block_x)
        {
            const int index = block_y * kBlocksX + block_x;
            Motion &motion = candidates[index];
            if (!motion.valid)
            {
                continue;
            }

            int support = 0;
            for (int neighbour_y = std::max(0, block_y - 1);
                 neighbour_y <= std::min(blocks_y - 1, block_y + 1);
                 ++neighbour_y)
            {
                for (int neighbour_x = std::max(0, block_x - 1);
                     neighbour_x <= std::min(blocks_x - 1, block_x + 1);
                     ++neighbour_x)
                {
                    if (neighbour_x == block_x && neighbour_y == block_y)
                    {
                        continue;
                    }
                    const Motion &neighbour =
                        candidates[neighbour_y * kBlocksX + neighbour_x];
                    if (neighbour.valid &&
                        std::abs(static_cast<int>(neighbour.x) - motion.x) <= 2 &&
                        std::abs(static_cast<int>(neighbour.y) - motion.y) <= 2)
                    {
                        ++support;
                    }
                }
            }

            const bool matches_global = m_global_motion_confident &&
                std::abs(static_cast<int>(motion.x) - m_motion_x) <= 2 &&
                std::abs(static_cast<int>(motion.y) - m_motion_y) <= 2;
            if (support > 0 || matches_global)
            {
                m_local_motion[index] = motion;
            }
        }
    }
}

} // namespace bedlam2::native_frame_interpolation
