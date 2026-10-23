#include "native_engine_memory.h"
#include "keyboard.h"
#include "native_debug.hpp"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <mutex>

#include <sys/mman.h>
#include <unistd.h>

extern "C" int sceKernelMmap(void *address, std::size_t size, int protection,
                              int flags, int fd, off_t offset, void **result);
extern "C" int sceKernelMunmap(void *address, std::size_t size);
extern "C" int sceKernelMprotect(void *address, std::size_t size, int protection);

namespace bedlam2::native_memory
{
namespace
{

constexpr std::uintptr_t kLowAddressStart = 0x20000000u;
constexpr std::uintptr_t kLowAddressLimit = 0x70000000u;
constexpr std::size_t kAllocationPrefix = 16;

// Keep ownership outside the mappings: inspecting an in-band header after
// munmap cannot validate an unknown or already-released pointer safely.
struct Allocation
{
    std::uintptr_t address;
    std::size_t mapped_size;
    Allocation *next;
};

Allocation *allocations = nullptr; // Sorted by mapping address; process lifetime.
std::atomic_flag allocation_lock = ATOMIC_FLAG_INIT;

struct AllocationLock
{
    AllocationLock() noexcept
    {
        while (allocation_lock.test_and_set(std::memory_order_acquire)) {}
    }
    ~AllocationLock() { allocation_lock.clear(std::memory_order_release); }
};
// The allocation lock has no static destructor: Window releases its buffers
// during global teardown, potentially after other translation units' globals.
std::mutex engine_image_mutex;
void *engine_image_memory = nullptr;
std::mutex engine_stack_mutex;
void *main_stack_memory = nullptr;
void *timer_stack_memory = nullptr;

void trace_mapping(const char *operation, std::uintptr_t address,
                   std::size_t size, int status = 0) noexcept
{
    char message[256];
    std::snprintf(message, sizeof(message),
                  "Bedlam2 memory: %s base=%p bytes=%zu status=0x%08x\n",
                  operation, reinterpret_cast<void *>(address), size,
                  static_cast<unsigned int>(status));
    bedlam2_ps5_trace(message);
}

std::uintptr_t allocate_engine_stack(std::size_t bytes, void *&allocation) noexcept
{
    const long system_page_size = sysconf(_SC_PAGESIZE);
    if (system_page_size <= 0)
        return 0;
    const auto page = static_cast<std::size_t>(system_page_size);
    // allocate_low adds its prefix and rounds up. Align past that prefix,
    // leaving a guard page below and above the usable stack.
    void *memory = allocate_low(bytes + 2 * page);
    if (memory == nullptr)
        return 0;
    const auto low = ((reinterpret_cast<std::uintptr_t>(memory) + page - 1) / page) * page;
    const auto top = low + page + bytes;
    const int bottom_status = sceKernelMprotect(reinterpret_cast<void *>(low), page, PROT_NONE);
    const int top_status = bottom_status == 0
        ? sceKernelMprotect(reinterpret_cast<void *>(top), page, PROT_NONE) : bottom_status;
    if (bottom_status != 0 || top_status != 0)
    {
        trace_mapping("stack guard failed", low, bytes, top_status);
        release_low(memory);
        return 0;
    }
    allocation = memory;
    trace_mapping("guarded stack", top - bytes, bytes);
    return top;
}

#ifdef BEDLAM2_PS5_TRACE
void probe_map_excl(std::size_t page_size) noexcept
{
    // Use only an isolated scratch page, before publishing any allocation.
    // This proves the console's behavior without risking live game buffers.
    void *scratch = nullptr;
    const int status = sceKernelMmap(reinterpret_cast<void *>(kLowAddressStart),
                                     page_size, PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANON, -1, 0, &scratch);
    if (status != 0)
    {
        trace_mapping("EXCL probe unavailable", kLowAddressStart, page_size, status);
        return;
    }
    constexpr std::uint64_t marker = 0x424544324C4F574Du;
    auto *value = static_cast<volatile std::uint64_t *>(scratch);
    *value = marker;
    void *overlap = nullptr;
    const int overlap_status = sceKernelMmap(scratch, page_size, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_EXCL, -1, 0, &overlap);
    const char *result = "EXCL overlap refused";
    if (overlap_status == 0)
    {
        result = overlap == scratch
            ? (*value == marker ? "EXCL overlap accepted" : "EXCL replaced live scratch")
            : "EXCL probe moved";
        if (overlap != scratch)
            sceKernelMunmap(overlap, page_size);
    }
    trace_mapping(result, reinterpret_cast<std::uintptr_t>(scratch), page_size, overlap_status);
    sceKernelMunmap(scratch, page_size);
}
#endif

template <typename T>
T *map_engine_symbol(T *symbol, std::uintptr_t data_begin,
                     std::uintptr_t data_end, std::uintptr_t bss_begin,
                     std::uintptr_t bss_end, std::uintptr_t data_low,
                     std::uintptr_t bss_low) noexcept
{
    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(symbol);
    if (address >= data_begin && address <= data_end - sizeof(T))
    {
        return reinterpret_cast<T *>(data_low + (address - data_begin));
    }
    if (address >= bss_begin && address <= bss_end - sizeof(T))
    {
        return reinterpret_cast<T *>(bss_low + (address - bss_begin));
    }
    return nullptr;
}

} // namespace

void *allocate_low(std::size_t size) noexcept
{
    const long system_page_size = sysconf(_SC_PAGESIZE);
    if (system_page_size <= 0 || size > SIZE_MAX - kAllocationPrefix)
    {
        return nullptr;
    }

    const std::size_t page_size = static_cast<std::size_t>(system_page_size);
    const std::size_t requested_size = size + kAllocationPrefix;
    if (requested_size > SIZE_MAX - (page_size - 1))
    {
        return nullptr;
    }
    const std::size_t mapped_size =
        ((requested_size + page_size - 1) / page_size) * page_size;
    if (mapped_size > kLowAddressLimit - kLowAddressStart)
    {
        return nullptr;
    }

    AllocationLock lock;
#ifdef BEDLAM2_PS5_TRACE
    static bool mapping_probed = false;
    if (!mapping_probed)
    {
        mapping_probed = true;
        probe_map_excl(page_size);
    }
#endif
    auto *record = static_cast<Allocation *>(std::malloc(sizeof(Allocation)));
    if (record == nullptr)
    {
        return nullptr;
    }
    Allocation **slot = &allocations;
    bool hint_move_reported = false;
    for (std::uintptr_t address = kLowAddressStart;
         address <= kLowAddressLimit - mapped_size; address += page_size)
    {
        // Never ask the kernel to remap a live allocation. Console MAP_EXCL
        // behavior cannot be inferred from the payload SDK's FreeBSD headers.
        while (*slot && (*slot)->address + (*slot)->mapped_size <= address)
        {
            slot = &(*slot)->next;
        }
        if (*slot && address + mapped_size > (*slot)->address)
        {
            address = (*slot)->address + (*slot)->mapped_size - page_size;
            continue;
        }
        void *const requested_address = reinterpret_cast<void *>(address);
        // The payload SDK's POSIX mmap emits syscall instructions in the
        // application, which native-game processes reject. Enter through
        // libkernel's native API instead.
        void *mapping = nullptr;
        const int status = sceKernelMmap(
            requested_address, mapped_size, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANON, -1, 0, &mapping);
        if (status != 0)
        {
            // SceKernel error codes carry the BSD errno in their low 16 bits.
            if ((static_cast<std::uint32_t>(status) & 0xffffu) == EEXIST)
            {
                continue;
            }
            trace_mapping("map failed", address, mapped_size, status);
            std::free(record);
            return nullptr;
        }
        if (mapping != requested_address)
        {
            if (!hint_move_reported)
            {
                trace_mapping("hint moved", reinterpret_cast<std::uintptr_t>(mapping), mapped_size);
                hint_move_reported = true;
            }
            const int unmap_status = sceKernelMunmap(mapping, mapped_size);
            if (unmap_status != 0)
            {
                trace_mapping("hint unmap failed", reinterpret_cast<std::uintptr_t>(mapping),
                              mapped_size, unmap_status);
                break;
            }
            continue;
        }

        *record = {address, mapped_size, *slot};
        *slot = record;
        trace_mapping("allocate", address, mapped_size);
        return reinterpret_cast<void *>(address + kAllocationPrefix);
    }
    std::free(record);
    return nullptr;
}

void release_low(void *memory) noexcept
{
    if (memory == nullptr)
    {
        return;
    }

    AllocationLock lock;
    Allocation **slot = &allocations;
    const auto pointer = reinterpret_cast<std::uintptr_t>(memory);
    while (*slot && (*slot)->address + kAllocationPrefix != pointer)
    {
        slot = &(*slot)->next;
    }
    if (*slot == nullptr)
    {
        trace_mapping("unknown release", pointer, 0);
        return;
    }
    Allocation *record = *slot;
    const int status = sceKernelMunmap(reinterpret_cast<void *>(record->address),
                                      record->mapped_size);
    trace_mapping("release", record->address, record->mapped_size, status);
    if (status == 0)
    {
        *slot = record->next;
        std::free(record);
    }
}

} // namespace bedlam2::native_memory

extern "C" const unsigned char bedlam2_data_base[];
extern "C" const unsigned char bedlam2_data_end[];
extern "C" const unsigned char bedlam2_bss_base[];
extern "C" const unsigned char bedlam2_bss_end[];
extern "C" const unsigned char byte_14723[];
extern "C" const std::uint32_t bedlam2_ps5_data_ptr_fixups[];
extern "C" const std::uint32_t bedlam2_ps5_data_ptr_fixups_end[];
extern "C" volatile std::int32_t screen_x_pos;
extern "C" volatile std::int32_t screen_y_pos;
extern "C" volatile std::int32_t game_is_playing;
extern "C" volatile std::int32_t map_active;
extern "C" volatile std::int32_t dead_screen_scale;
extern "C" volatile std::int32_t num_menu_lines;

extern "C"
{
std::uint32_t bedlam2_ps5_data_base_low = 0;
std::uint32_t bedlam2_ps5_bss_base_low = 0;
std::uint32_t bedlam2_ps5_symbol_search_base_low = 0;
std::uintptr_t bedlam2_ps5_main_stack_top = 0;
std::uintptr_t bedlam2_ps5_timer_stack_top = 0;
volatile std::int32_t *bedlam2_ps5_screen_x_pos = &screen_x_pos;
volatile std::int32_t *bedlam2_ps5_screen_y_pos = &screen_y_pos;
volatile std::int32_t *bedlam2_ps5_game_is_playing = &game_is_playing;
volatile std::int32_t *bedlam2_ps5_map_active = &map_active;
volatile std::int32_t *bedlam2_ps5_dead_screen_scale = &dead_screen_scale;
volatile std::int32_t *bedlam2_ps5_num_menu_lines = &num_menu_lines;
}

extern "C" int bedlam2_ps5_engine_stacks_init() noexcept
{
    using namespace bedlam2::native_memory;
    std::lock_guard<std::mutex> lock(engine_stack_mutex);
    if (bedlam2_ps5_main_stack_top != 0)
        return 0;
    const auto main_top = allocate_engine_stack(8u * 1024u * 1024u, main_stack_memory);
    if (main_top == 0)
        return -1;
    const auto timer_top = allocate_engine_stack(1024u * 1024u, timer_stack_memory);
    if (timer_top == 0)
    {
        release_low(main_stack_memory);
        main_stack_memory = nullptr;
        return -1;
    }
    bedlam2_ps5_main_stack_top = main_top;
    bedlam2_ps5_timer_stack_top = timer_top;
    return 0;
}

extern "C" int bedlam2_ps5_engine_memory_init() noexcept
{
    using namespace bedlam2::native_memory;

    std::lock_guard<std::mutex> lock(engine_image_mutex);
    if (engine_image_memory != nullptr)
    {
        return 0;
    }

    const auto data_begin = reinterpret_cast<std::uintptr_t>(bedlam2_data_base);
    const auto data_end = reinterpret_cast<std::uintptr_t>(bedlam2_data_end);
    const auto bss_begin = reinterpret_cast<std::uintptr_t>(bedlam2_bss_base);
    const auto bss_end = reinterpret_cast<std::uintptr_t>(bedlam2_bss_end);
    if (data_end < data_begin || bss_end < bss_begin)
    {
        return -1;
    }

    const std::size_t data_size = static_cast<std::size_t>(data_end - data_begin);
    const std::size_t bss_size = static_cast<std::size_t>(bss_end - bss_begin);
    constexpr std::size_t kBaseAlignment = 16;
    if (data_size > SIZE_MAX - (kBaseAlignment - 1))
    {
        return -1;
    }
    const std::size_t bss_offset =
        (data_size + kBaseAlignment - 1) & ~(kBaseAlignment - 1);
    if (bss_size > SIZE_MAX - bss_offset)
    {
        return -1;
    }

    void *const memory = allocate_low(bss_offset + bss_size);
    if (memory == nullptr)
    {
        return -1;
    }
    std::memset(memory, 0, bss_offset + bss_size);
    std::memcpy(memory, bedlam2_data_base, data_size);

    const auto data_address = reinterpret_cast<std::uintptr_t>(memory);
    const auto bss_address = data_address + bss_offset;
    if (data_address > UINT32_MAX || bss_address > UINT32_MAX)
    {
        release_low(memory);
        return -1;
    }

    const auto fixup_begin = reinterpret_cast<std::uintptr_t>(bedlam2_ps5_data_ptr_fixups);
    const auto fixup_end = reinterpret_cast<std::uintptr_t>(bedlam2_ps5_data_ptr_fixups_end);
    if (fixup_end < fixup_begin || (fixup_end - fixup_begin) % (3 * sizeof(std::uint32_t)))
    {
        release_low(memory);
        return -1;
    }
    const std::size_t fixup_count =
        (fixup_end - fixup_begin) / (3 * sizeof(std::uint32_t));
    for (std::size_t index = 0; index < fixup_count; ++index)
    {
        const std::uint32_t *const fixup = bedlam2_ps5_data_ptr_fixups + index * 3;
        const std::size_t slot_offset = fixup[0];
        const std::size_t target_offset = fixup[1];
        const std::uint32_t target_section = fixup[2];
        if (slot_offset > data_size || data_size - slot_offset < sizeof(std::uint32_t))
        {
            release_low(memory);
            return -1;
        }

        std::uintptr_t target_address = 0;
        if (target_section == 0 && target_offset <= data_size)
        {
            target_address = data_address + target_offset;
        }
        else if (target_section == 1 && target_offset <= bss_size)
        {
            target_address = bss_address + target_offset;
        }
        else if (target_section == 2)
        {
            // Text pointers stay section-relative for now; no native caller
            // consumes the DOS startup callback tables on the PS5 entry path.
            continue;
        }
        else
        {
            release_low(memory);
            return -1;
        }
        if (target_address > UINT32_MAX)
        {
            release_low(memory);
            return -1;
        }
        *reinterpret_cast<std::uint32_t *>(
            static_cast<unsigned char *>(memory) + slot_offset) =
            static_cast<std::uint32_t>(target_address);
    }

    auto *const pressed_keys = static_cast<std::uint8_t *>(allocate_low(257));
    if (pressed_keys == nullptr)
    {
        release_low(memory);
        return -1;
    }
    std::memset(pressed_keys, 0, 257);

    auto *const symbol_search = static_cast<std::uint8_t *>(allocate_low(32));
    if (symbol_search == nullptr)
    {
        release_low(pressed_keys);
        release_low(memory);
        return -1;
    }
    std::memcpy(symbol_search, byte_14723, 32);

    auto *const mapped_screen_x = map_engine_symbol(
        &screen_x_pos, data_begin, data_end, bss_begin, bss_end,
        data_address, bss_address);
    auto *const mapped_screen_y = map_engine_symbol(
        &screen_y_pos, data_begin, data_end, bss_begin, bss_end,
        data_address, bss_address);
    auto *const mapped_game_is_playing = map_engine_symbol(
        &game_is_playing, data_begin, data_end, bss_begin, bss_end,
        data_address, bss_address);
    auto *const mapped_map_active = map_engine_symbol(
        &map_active, data_begin, data_end, bss_begin, bss_end,
        data_address, bss_address);
    auto *const mapped_dead_screen_scale = map_engine_symbol(
        &dead_screen_scale, data_begin, data_end, bss_begin, bss_end,
        data_address, bss_address);
    auto *const mapped_num_menu_lines = map_engine_symbol(
        &num_menu_lines, data_begin, data_end, bss_begin, bss_end,
        data_address, bss_address);
    if (mapped_screen_x == nullptr || mapped_screen_y == nullptr ||
        mapped_game_is_playing == nullptr || mapped_map_active == nullptr ||
        mapped_dead_screen_scale == nullptr || mapped_num_menu_lines == nullptr)
    {
        release_low(symbol_search);
        release_low(pressed_keys);
        release_low(memory);
        return -1;
    }

    engine_image_memory = memory;
    PRESSED_KEY_ARR = pressed_keys;
    bedlam2_ps5_screen_x_pos = mapped_screen_x;
    bedlam2_ps5_screen_y_pos = mapped_screen_y;
    bedlam2_ps5_game_is_playing = mapped_game_is_playing;
    bedlam2_ps5_map_active = mapped_map_active;
    bedlam2_ps5_dead_screen_scale = mapped_dead_screen_scale;
    bedlam2_ps5_num_menu_lines = mapped_num_menu_lines;
    bedlam2_ps5_data_base_low = static_cast<std::uint32_t>(data_address);
    bedlam2_ps5_bss_base_low = static_cast<std::uint32_t>(bss_address);
    bedlam2_ps5_symbol_search_base_low =
        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(symbol_search));
    return 0;
}

extern "C" void *bedlam2_ps5_malloc(std::uint32_t size) noexcept
{
    return bedlam2::native_memory::allocate_low(size);
}

extern "C" void bedlam2_ps5_free(std::uint32_t address) noexcept
{
    bedlam2::native_memory::release_low(
        reinterpret_cast<void *>(static_cast<std::uintptr_t>(address)));
}
