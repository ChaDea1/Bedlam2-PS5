// Native titles terminate through SystemService, as in the Support Kit's
// runtime_ps5.cpp. libc exit() reaches SIGSYS in this launch context.
#include <cstdio>
#include "native_debug.hpp"

extern "C" void SDL_Quit();
extern "C" int sceSystemServiceLoadExec(const char *, const char **);
extern "C" int sceKernelUsleep(unsigned int);

extern "C" [[noreturn]] void __wrap_exit(int status)
{
    char message[160];
    std::snprintf(message, sizeof(message), "Bedlam2 trace: returning to dashboard, status=%d\n", status);
    bedlam2_ps5_trace(message);
    SDL_Quit();
    std::fflush(nullptr);
    const int result = sceSystemServiceLoadExec("exit", nullptr);
    std::snprintf(message, sizeof(message), "Bedlam2 trace: dashboard exit returned 0x%08x\n", result);
    bedlam2_ps5_trace(message);
    for (;;)
        sceKernelUsleep(1000000);
}
