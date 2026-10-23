#pragma once

#ifdef BEDLAM2_PS5_TRACE
extern "C" int sceKernelDebugOutText(int channel, const char *text);

inline void bedlam2_ps5_trace(const char *text) noexcept
{
    // Native-game processes must enter syscalls through libkernel. The native
    // app target explicitly links libkernel.so rather than kernel_web.
    (void)sceKernelDebugOutText(0, text);
}
#else
inline void bedlam2_ps5_trace(const char *) noexcept {}
#endif
