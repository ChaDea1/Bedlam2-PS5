// Host regression with a kernel shim that ignores the SDK's MAP_EXCL flag.
// Build with native_engine_memory.cpp, function/data sections and --gc-sections.
#include "native_engine_memory.h"

#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include <sys/mman.h>

namespace
{
std::atomic<int> forced_error{0};
std::atomic<bool> move_hint{false};
std::atomic<int> unmap_error{0};
std::atomic<int> fixed_requests{0};
#ifdef BEDLAM2_PS5_TRACE
int probe_result = 0;
const bool honor_excl = std::getenv("BEDLAM_TEST_HONOR_EXCL") != nullptr;
#endif
}

#ifdef BEDLAM2_PS5_TRACE
extern "C" int sceKernelDebugOutText(int, const char *message)
{
    if (std::strstr(message, "EXCL replaced live scratch"))
        probe_result = 1;
    if (std::strstr(message, "EXCL overlap refused"))
        probe_result = 2;
    return 0;
}
#endif

extern "C" int sceKernelMmap(void *address, std::size_t size, int protection,
                              int flags, int, off_t, void **result)
{
    if (const int error = forced_error.exchange(0))
        return error;
    if (flags & MAP_FIXED)
    {
        ++fixed_requests;
#ifdef BEDLAM2_PS5_TRACE
        if (honor_excl)
            return EEXIST;
#endif
    }
    // Deliberately ignore MAP_EXCL, matching the suspected console behavior.
    const bool moved = move_hint.exchange(false);
    if (moved)
        forced_error = EINVAL; // End the retry after testing high-address cleanup.
    void *mapping = mmap(moved ? nullptr : address, size,
                         protection, MAP_PRIVATE | MAP_ANONYMOUS | (flags & MAP_FIXED), -1, 0);
    if (mapping == MAP_FAILED)
        return errno;
    *result = mapping;
    return 0;
}

extern "C" int sceKernelMunmap(void *address, std::size_t size)
{
    if (const int error = unmap_error.exchange(0))
        return error;
    return munmap(address, size);
}

int main()
{
    using namespace bedlam2::native_memory;
    // Startup order: map, sidebar, screen, game screen, engine, keyboard.
    const std::size_t sizes[] = {640 * 480, 640 * 480, 1920 * 1080,
                                2240 * 1720, 0xa7000, 257};
    std::vector<void *> buffers;
    for (std::size_t size : sizes)
    {
        void *buffer = allocate_low(size);
        assert(buffer);
        assert(reinterpret_cast<std::uintptr_t>(buffer) < 0x70000000);
        assert(reinterpret_cast<std::uintptr_t>(buffer) % 16 == 0);
        for (std::size_t index = 0; index < buffers.size(); ++index)
        {
            const auto earlier = reinterpret_cast<std::uintptr_t>(buffers[index]);
            const auto current = reinterpret_cast<std::uintptr_t>(buffer);
            assert(current >= earlier + sizes[index] || earlier >= current + size);
            assert(*static_cast<unsigned char *>(buffers[index]) == index + 1);
        }
        std::memset(buffer, buffers.size() + 1, size);
        buffers.push_back(buffer);
    }
#ifdef BEDLAM2_PS5_TRACE
    assert(fixed_requests == 1 && probe_result == (honor_excl ? 2 : 1));
#else
    assert(fixed_requests == 0);
#endif
    // Interior/foreign/stale pointers must never read unmapped metadata.
    release_low(static_cast<char *>(buffers[1]) + 1);
    release_low(reinterpret_cast<void *>(0x1234));
    release_low(buffers[2]);
    release_low(buffers[2]);
    void *replacement = allocate_low(sizes[2]);
    assert(replacement == buffers[2]);
    buffers[2] = replacement;
    unmap_error = EIO;
    release_low(buffers[0]);
    assert(*static_cast<unsigned char *>(buffers[0]) == 1);
    for (void *buffer : buffers)
        release_low(buffer);
    assert(allocate_low(SIZE_MAX) == nullptr);
    assert(allocate_low(0x70000000) == nullptr);
    forced_error = EINVAL;
    assert(allocate_low(16) == nullptr);
    // Reject and unmap a kernel-selected high address without adopting it.
    move_hint = true;
    assert(allocate_low(16) == nullptr);
    // A foreign mapping at the first hint must not be replaced.
    void *foreign = mmap(reinterpret_cast<void *>(0x20000000), 4096,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(foreign == reinterpret_cast<void *>(0x20000000));
    *static_cast<unsigned char *>(foreign) = 0xa5;
    void *after_foreign = allocate_low(16);
    assert(after_foreign && after_foreign != static_cast<char *>(foreign) + 16);
    assert(*static_cast<unsigned char *>(foreign) == 0xa5);
    release_low(after_foreign);
    assert(munmap(foreign, 4096) == 0);
    std::vector<std::thread> threads;
    for (int worker = 0; worker < 4; ++worker)
        threads.emplace_back([] {
            for (int count = 0; count < 100; ++count)
            {
                auto *buffer = allocate_zeroed_array<unsigned char>(8192);
                assert(buffer && buffer[0] == 0 && buffer[8191] == 0);
                std::memset(buffer, 0x5a, 8192);
                release_low(buffer);
            }
        });
    for (auto &thread : threads)
        thread.join();
    std::puts("Low-memory regressions passed: disjoint buffers, safe release, errors, foreign mappings, threads.");
}
