// Link against the privately patched host SDL and native_sdl_memory.cpp.
#include "native_sdl_memory.h"
#include <SDL.h>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <sys/mman.h>

namespace
{
constexpr std::size_t pixels_size = 1920u * 1080u * 4u;
bool deny_heap = false;
bool deny_realloc = false;
bool deny_maps = false;
std::size_t large_heap_requests = 0;
std::size_t mapping_calls = 0;
std::unordered_map<void *, std::size_t> mappings;

void *SDLCALL test_malloc(std::size_t size)
{
    if (size >= 1024u * 1024u)
        ++large_heap_requests;
    if (deny_heap || size >= 1024u * 1024u)
    {
        errno = EAGAIN;
        return nullptr;
    }
    return std::malloc(size);
}

void *SDLCALL test_realloc(void *memory, std::size_t size)
{
    if (deny_realloc)
        return nullptr;
    return std::realloc(memory, size);
}
}

extern "C" int sceKernelDebugOutText(int, const char *message)
{
    std::fputs(message, stdout);
    return 0;
}

extern "C" int sceKernelMmap(void *address, std::size_t size, int protection,
                              int flags, int fd, off_t offset, void **result)
{
    assert(address == nullptr && !(flags & MAP_FIXED) && fd == -1 && offset == 0);
    ++mapping_calls;
    if (deny_maps)
        return static_cast<int>(0x80020023u); // Native EAGAIN, as seen on console.
    void *memory = mmap(nullptr, size, protection, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(memory != MAP_FAILED);
    assert(mappings.emplace(memory, size).second);
    *result = memory;
    return 0;
}

extern "C" int sceKernelMunmap(void *memory, std::size_t size)
{
    const auto found = mappings.find(memory);
    assert(found != mappings.end() && found->second == size);
    mappings.erase(found);
    return munmap(memory, size);
}

int main()
{
    assert(SDL_SetMemoryFunctions(test_malloc, std::calloc, test_realloc, std::free) == 0);
    assert(bedlam2_ps5_sdl_memory_init() == 0);
    assert(bedlam2_ps5_sdl_memory_init() == 0);
    SDL_malloc_func allocate;
    SDL_calloc_func zeroed;
    SDL_realloc_func resize;
    SDL_free_func release;
    SDL_GetMemoryFunctions(&allocate, &zeroed, &resize, &release);
    // Raw callbacks isolate allocator contracts from SDL's allocation counter.
    auto *small = static_cast<unsigned char *>(allocate(73));
    assert(small && reinterpret_cast<std::uintptr_t>(small) % alignof(std::max_align_t) == 0);
    std::memset(small, 0x5a, 73);
    auto *large = static_cast<unsigned char *>(resize(small, pixels_size));
    assert(large && large[0] == 0x5a && large[72] == 0x5a);
    assert(reinterpret_cast<std::uintptr_t>(large) % alignof(std::max_align_t) == 0);
    deny_maps = true;
    assert(resize(large, 2 * pixels_size) == nullptr && large[72] == 0x5a);
    deny_maps = false;
    large = static_cast<unsigned char *>(resize(large, 2 * pixels_size));
    assert(large && large[72] == 0x5a);
    assert(resize(large, 64) == large && large[63] == 0x5a);
    assert(resize(large, 0) == nullptr);
    release(nullptr);
    auto *zeros = static_cast<unsigned char *>(zeroed(1, pixels_size));
    assert(zeros && zeros[0] == 0 && zeros[pixels_size - 1] == 0);
    release(zeros);
    const auto before_overflow = mapping_calls;
    assert(allocate(SIZE_MAX) == nullptr);
    assert(allocate(SIZE_MAX - 128) == nullptr);
    assert(zeroed(SIZE_MAX, 2) == nullptr);
    assert(mapping_calls == before_overflow);
    small = static_cast<unsigned char *>(allocate(73));
    std::memset(small, 0x4b, 73);
    deny_heap = deny_realloc = true;
    auto *fallback = static_cast<unsigned char *>(resize(small, 128));
    assert(fallback && fallback[0] == 0x4b && fallback[72] == 0x4b);
    release(fallback);
    deny_heap = deny_realloc = false;
    assert(mappings.empty());

    assert(SDL_Init(0) == 0);
    SDL_Surface *output = SDL_CreateRGBSurfaceWithFormat(0, 1920, 1080, 32, SDL_PIXELFORMAT_RGBA32);
    SDL_Renderer *renderer = SDL_CreateSoftwareRenderer(output);
    assert(output && renderer && mappings.size() == 1);
    deny_maps = true;
    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                            SDL_TEXTUREACCESS_STREAMING, 1920, 1080);
    assert(texture == nullptr && std::strcmp(SDL_GetError(), "Out of memory") == 0);
    deny_maps = false;
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                SDL_TEXTUREACCESS_STREAMING, 1920, 1080);
    // The console's two simultaneous 1080p allocations bypass the limited heap.
    assert(texture && mappings.size() == 2 && large_heap_requests == 0);
    void *pixels = nullptr;
    int pitch = 0;
    assert(SDL_LockTexture(texture, nullptr, &pixels, &pitch) == 0 && pitch >= 1920 * 4);
    std::memset(pixels, 0x7f, pitch * 1080);
    SDL_UnlockTexture(texture);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_FreeSurface(output);
    SDL_Quit();
    assert(mappings.empty());
    std::puts("SDL memory regressions passed: two 1080p buffers, heap migration, failure preservation, zeroing, overflow, cleanup.");
}
