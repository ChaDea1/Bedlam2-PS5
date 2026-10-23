#pragma once

// Install before the first SDL allocation. Small blocks use the existing
// libc heap; large blocks use native libkernel mappings, with matching frees.
int bedlam2_ps5_sdl_memory_init() noexcept;
