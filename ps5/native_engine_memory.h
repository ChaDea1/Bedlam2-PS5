#pragma once

#include <cstddef>
#include <cstring>
#include <limits>
#include <new>
#include <type_traits>

extern "C" int bedlam2_ps5_engine_stacks_init() noexcept;

namespace bedlam2::native_memory
{

void *allocate_low(std::size_t size) noexcept;
void release_low(void *memory) noexcept;

template <typename T>
T *allocate_zeroed_array(std::size_t count) noexcept
{
    static_assert(std::is_trivially_copyable<T>::value,
                  "low-address game buffers must be trivially copyable");
    if (count > std::numeric_limits<std::size_t>::max() / sizeof(T))
    {
        return nullptr;
    }
    const std::size_t bytes = count * sizeof(T);
    auto *memory = static_cast<T *>(allocate_low(bytes));
    if (memory != nullptr)
    {
        std::memset(memory, 0, bytes);
    }
    return memory;
}

template <typename T>
void release_array(T *memory) noexcept
{
    release_low(memory);
}

} // namespace bedlam2::native_memory
