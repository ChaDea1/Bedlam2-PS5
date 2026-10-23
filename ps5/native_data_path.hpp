#pragma once

#include <cstddef>

extern "C" void bedlam2_ps5_set_data_root(const char *path) noexcept;
extern "C" char *bedlam2_ps5_getcwd_legacy(char *buffer, std::size_t size) noexcept;
extern "C" int bedlam2_ps5_mkdir_legacy(const char *path) noexcept;
extern "C" bool bedlam2_ps5_resolve_data_path(const char *path, char *output,
                                                std::size_t output_size) noexcept;
