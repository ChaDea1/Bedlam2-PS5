#include "native_engine_memory.h"
#include "native_data_path.hpp"
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <initializer_list>
#include <thread>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" std::uintptr_t bedlam2_ps5_main_stack_top;
extern "C" std::uintptr_t bedlam2_ps5_timer_stack_top;
extern "C" int check_native_main_abi();
extern "C" void bedlam2_native_level_clock();

namespace
{
std::atomic<int> main_calls{0}, timer_calls{0};
std::atomic<bool> fail_protection{true};
const char *expected_directory;
int mkdir_status;
}

extern "C" int sceKernelMkdir(const char *path, unsigned int mode)
{
    assert(expected_directory != nullptr);
    assert(std::strcmp(path, expected_directory) == 0);
    assert(mode == 0777);
    return mkdir_status;
}

void check_directory_bridge()
{
    char buffer[128];
    bedlam2_ps5_set_data_root("");
    assert(bedlam2_ps5_getcwd_legacy(buffer, sizeof(buffer)) == buffer);
    assert(std::strcmp(buffer, "/app0") == 0);
    bedlam2_ps5_set_data_root("assets\\bed2");
    assert(bedlam2_ps5_getcwd_legacy(buffer, sizeof(buffer)) == buffer);
    assert(std::strcmp(buffer, "/app0/assets/bed2") == 0);
    const auto length = std::strlen(buffer);
    buffer[0] = '!';
    assert(bedlam2_ps5_getcwd_legacy(buffer, length) == nullptr && errno == ERANGE);
    assert(buffer[0] == '!');
    assert(bedlam2_ps5_getcwd_legacy(buffer, length + 1) == buffer);
    assert(bedlam2_ps5_getcwd_legacy(nullptr, 128) == nullptr && errno == EINVAL);
    expected_directory = "/app0/assets/bed2/MIRAGE/AB_BED";
    assert(bedlam2_ps5_mkdir_legacy("MIRAGE\\AB_BED") == 0);
    mkdir_status = static_cast<int>(0x80020000u | EACCES);
    assert(bedlam2_ps5_mkdir_legacy("MIRAGE\\AB_BED") == -1 && errno == EACCES);
    mkdir_status = 0;
    expected_directory = "/data/bedlam-test";
    assert(bedlam2_ps5_mkdir_legacy(expected_directory) == 0);
    assert(bedlam2_ps5_mkdir_legacy(nullptr) == -1 && errno == EINVAL);
    char too_long[4097];
    std::memset(too_long, 'a', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = 0;
    assert(bedlam2_ps5_mkdir_legacy(too_long) == -1 && errno == ENAMETOOLONG);
    bedlam2_ps5_set_data_root("/app0/assets/bed2");
    assert(bedlam2_ps5_getcwd_legacy(buffer, sizeof(buffer)) == buffer);
    assert(std::strcmp(buffer, "/app0/assets/bed2") == 0);
    assert(bedlam2_ps5_resolve_data_path("GAMEGFX\\GAMEPAL.PAL", buffer, sizeof(buffer)));
    assert(std::strcmp(buffer, "/app0/assets/bed2/GAMEGFX/GAMEPAL.PAL") == 0);
    expected_directory = "/app0/assets/bed2";
}

extern "C" int sceKernelMmap(void *address, std::size_t size, int protection,
                              int flags, int fd, off_t offset, void **result)
{
    assert(!(flags & MAP_FIXED));
    *result = mmap(address, size, protection, MAP_PRIVATE | MAP_ANONYMOUS, fd, offset);
    return *result == MAP_FAILED ? errno : 0;
}
extern "C" int sceKernelMunmap(void *address, std::size_t size)
{
    return munmap(address, size);
}
extern "C" int sceKernelMprotect(void *address, std::size_t size, int protection)
{
    if (fail_protection.exchange(false))
        return EINVAL;
    return mprotect(address, size, protection);
}
extern "C" void check_engine_stack(int timer, std::uintptr_t stack)
{
    const auto top = timer ? bedlam2_ps5_timer_stack_top : bedlam2_ps5_main_stack_top;
    const auto bytes = timer ? 1024u * 1024u : 8u * 1024u * 1024u;
    assert(stack >= top - bytes && stack < top && stack < 0x70000000);
    if (timer)
        ++timer_calls;
    else
        ++main_calls;
}

int main()
{
    check_directory_bridge();
    assert(bedlam2_ps5_engine_stacks_init() == -1);
    assert(bedlam2_ps5_main_stack_top == 0 && bedlam2_ps5_timer_stack_top == 0);
    assert(bedlam2_ps5_engine_stacks_init() == 0);
    assert(bedlam2_ps5_engine_stacks_init() == 0);
    assert(bedlam2_ps5_main_stack_top % 16 == 0 && bedlam2_ps5_timer_stack_top % 16 == 0);
    assert(bedlam2_ps5_main_stack_top < bedlam2_ps5_timer_stack_top - 1024u * 1024u);
    std::thread timer([] {
        for (int index = 0; index < 100; ++index)
            bedlam2_native_level_clock();
    });
    for (int index = 0; index < 100; ++index)
        assert(check_native_main_abi() == 0);
    timer.join();
    assert(main_calls == 100 && timer_calls == 100);
    rlimit no_core{0, 0};
    assert(setrlimit(RLIMIT_CORE, &no_core) == 0);
    const auto page = static_cast<std::uintptr_t>(sysconf(_SC_PAGESIZE));
    for (const auto address : {bedlam2_ps5_main_stack_top,
                              bedlam2_ps5_main_stack_top - 8u * 1024u * 1024u - page,
                              bedlam2_ps5_timer_stack_top,
                              bedlam2_ps5_timer_stack_top - 1024u * 1024u - page})
    {
        const pid_t child = fork();
        assert(child >= 0);
        if (child == 0)
        {
            *reinterpret_cast<volatile unsigned char *>(address) = 1;
            _exit(99);
        }
        int status = 0;
        assert(waitpid(child, &status, 0) == child);
        assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
    }
    std::puts("Engine shim tests passed: directory bridge, low stacks, concurrent timer, getcwd thunk, argc slots, callee-saved state, guard faults, init retry.");
}
