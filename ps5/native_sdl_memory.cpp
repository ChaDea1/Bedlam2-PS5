#include "native_sdl_memory.h"
#include "native_debug.hpp"

#include <SDL.h>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

extern "C" int sceKernelMmap(void *, std::size_t, int, int, int, off_t, void **);
extern "C" int sceKernelMunmap(void *, std::size_t);

namespace
{
constexpr std::size_t kMappedThreshold = 1024u * 1024u;

// Every SDL allocation carries a header, including small libc blocks. This
// permits realloc to migrate between heaps without guessing the old size or
// ever passing a mapped pointer to libc's realloc/free.
struct alignas(std::max_align_t) Block
{
    std::size_t size;
    std::size_t mapped_size; // Zero denotes a libc allocation.
};
static_assert(sizeof(Block) % alignof(std::max_align_t) == 0);

SDL_malloc_func original_malloc;
SDL_realloc_func original_realloc;
SDL_free_func original_free;
std::size_t page_size;

void report(const char *operation, std::size_t size, const void *result,
            int status = 0) noexcept
{
    const int saved_errno = errno;
    if (result == nullptr || size >= kMappedThreshold || status != 0)
    {
        char message[256];
        std::snprintf(message, sizeof(message),
                      "Bedlam2 SDL memory: %s bytes=%zu result=%p status=0x%08x\n",
                      operation, size, result, static_cast<unsigned int>(status));
        bedlam2_ps5_trace(message);
    }
    errno = saved_errno;
}

void *SDLCALL native_malloc(std::size_t size)
{
    if (size > SIZE_MAX - sizeof(Block))
    {
        errno = ENOMEM;
        return nullptr;
    }
    const std::size_t total = size + sizeof(Block);
    if (size < kMappedThreshold)
    {
        if (auto *block = static_cast<Block *>(original_malloc(total)))
        {
            *block = {size, 0};
            return block + 1;
        }
        report("libc fallback", size, nullptr, errno);
    }
    if (total > SIZE_MAX - (page_size - 1))
    {
        errno = ENOMEM;
        return nullptr;
    }
    const std::size_t mapped_size = ((total + page_size - 1) / page_size) * page_size;
    void *mapping = nullptr;
    // No low-address constraint or fixed placement for SDL's native pointers.
    // Use libkernel rather than the payload SDK's inline-syscall mmap wrapper.
    const int status = sceKernelMmap(nullptr, mapped_size, PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANON, -1, 0, &mapping);
    if (status != 0)
    {
        report("map failed", size, nullptr, status);
        errno = ENOMEM;
        return nullptr;
    }
    auto *block = static_cast<Block *>(mapping);
    *block = {size, mapped_size};
    report("mapped", size, block + 1);
    return block + 1;
}

void SDLCALL native_free(void *memory)
{
    if (memory == nullptr)
        return;
    auto *block = static_cast<Block *>(memory) - 1;
    if (block->mapped_size == 0)
    {
        original_free(block);
        return;
    }
    const std::size_t size = block->size;
    const int status = sceKernelMunmap(block, block->mapped_size);
    report("unmap", size, memory, status);
}

void *SDLCALL native_calloc(std::size_t count, std::size_t size)
{
    if (size != 0 && count > SIZE_MAX / size)
    {
        errno = ENOMEM;
        return nullptr;
    }
    const std::size_t bytes = count * size;
    void *result = native_malloc(bytes);
    if (result != nullptr)
        std::memset(result, 0, bytes);
    return result;
}

void *SDLCALL native_realloc(void *memory, std::size_t size)
{
    if (memory == nullptr)
        return native_malloc(size);
    if (size == 0)
    {
        native_free(memory);
        return nullptr;
    }
    if (size > SIZE_MAX - sizeof(Block))
    {
        errno = ENOMEM;
        return nullptr;
    }
    auto *block = static_cast<Block *>(memory) - 1;
    const std::size_t old_size = block->size;
    if (block->mapped_size != 0 && size <= block->mapped_size - sizeof(Block))
    {
        block->size = size;
        return memory;
    }
    if (block->mapped_size == 0 && size < kMappedThreshold)
    {
        if (auto *resized = static_cast<Block *>(original_realloc(block, size + sizeof(Block))))
        {
            resized->size = size;
            return resized + 1;
        }
        report("libc realloc fallback", size, nullptr, errno);
    }
    void *replacement = native_malloc(size);
    if (replacement != nullptr)
    {
        std::memcpy(replacement, memory, old_size < size ? old_size : size);
        native_free(memory);
    }
    return replacement;
}
} // namespace

int bedlam2_ps5_sdl_memory_init() noexcept
{
    if (original_malloc != nullptr)
        return 0;
    if (SDL_GetNumAllocations() != 0)
        return SDL_SetError("Native SDL heap must be installed before any SDL allocation");
    const long system_page_size = sysconf(_SC_PAGESIZE);
    if (system_page_size <= 0)
        return SDL_SetError("Cannot determine native SDL mapping page size");
    page_size = static_cast<std::size_t>(system_page_size);
    SDL_GetMemoryFunctions(&original_malloc, nullptr, &original_realloc, &original_free);
    return SDL_SetMemoryFunctions(native_malloc, native_calloc, native_realloc, native_free);
}
