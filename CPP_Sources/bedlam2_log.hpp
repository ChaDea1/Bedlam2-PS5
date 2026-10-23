#pragma once

#ifdef BEDLAM2_PS5_NATIVE
namespace bedlam2
{
class NullLogStream
{
public:
    template <typename T> NullLogStream &operator<<(const T &) noexcept
    {
        return *this;
    }

    NullLogStream &operator<<(NullLogStream &(*)(NullLogStream &)) noexcept
    {
        return *this;
    }
};

inline NullLogStream log_stream;

inline NullLogStream &log_endline(NullLogStream &stream) noexcept
{
    return stream;
}
} // namespace bedlam2

#define BEDLAM_LOG ::bedlam2::log_stream
#define BEDLAM_ENDL ::bedlam2::log_endline
#else
#include <iostream>

#define BEDLAM_LOG std::cout
#define BEDLAM_ENDL std::endl
#endif
