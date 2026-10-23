#include <array>
#include <climits>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <sys/types.h>
#include <unistd.h>

#include "native_data_path.hpp"
#include "native_debug.hpp"

extern "C" int sceKernelMkdir(const char *path, unsigned int mode);

namespace
{

constexpr std::size_t kMaxOpenFiles = 256;
constexpr std::uint32_t kFileHandleBase = 0x7F000000u;
constexpr std::uint32_t kFileHandleStride = 4;
constexpr std::size_t kMaxPathLength = 4096;
std::array<std::FILE *, kMaxOpenFiles> open_files{};
std::mutex open_files_mutex;
char game_data_root[kMaxPathLength]{};

std::size_t string_length(const char *text) noexcept
{
    std::size_t length = 0;
    if (text != nullptr)
    {
        while (text[length] != '\0')
            ++length;
    }
    return length;
}

void copy_normalized_path(const char *input, char *output, std::size_t offset) noexcept
{
    while (*input != '\0')
    {
        output[offset++] = *input == '\\' ? '/' : *input;
        ++input;
    }
    output[offset] = '\0';
}

bool file_slot_index(std::uint32_t handle, std::size_t &index) noexcept
{
    if (handle < kFileHandleBase)
    {
        return false;
    }
    const std::uint32_t offset = handle - kFileHandleBase;
    if (offset % kFileHandleStride != 0)
    {
        return false;
    }
    index = offset / kFileHandleStride;
    return index < open_files.size();
}

std::FILE *file_from_handle(std::uint32_t handle) noexcept
{
    std::size_t index = 0;
    if (!file_slot_index(handle, index))
    {
        return nullptr;
    }
    return open_files[index];
}

} // namespace

extern "C" void bedlam2_ps5_set_data_root(const char *path) noexcept
{
    // Native titles mount their application at /app0. Keep the engine's virtual
    // working directory absolute so its directory and file bridges agree even
    // when startup found the assets through a relative candidate.
    const std::size_t length = string_length(path);
    const bool absolute = path != nullptr && path[0] == '/';
    const char *prefix = absolute ? "" : length == 0 ? "/app0" : "/app0/";
    const std::size_t prefix_length = string_length(prefix);
    if (path == nullptr || prefix_length + length >= sizeof(game_data_root))
    {
        game_data_root[0] = '\0';
        return;
    }
    for (std::size_t index = 0; index < prefix_length; ++index)
        game_data_root[index] = prefix[index];
    copy_normalized_path(path, game_data_root, prefix_length);
    bedlam2_ps5_trace("Bedlam2 files: engine directory ");
    bedlam2_ps5_trace(game_data_root);
    bedlam2_ps5_trace("\n");
}

extern "C" char *bedlam2_ps5_getcwd_legacy(char *buffer, std::size_t size) noexcept
{
    // This is the engine's virtual directory, shared with fopen/mkdir. Calling
    // the system libc getcwd import currently resolves to an unmapped target
    // on the console, and the process directory need not be the asset root.
    const std::size_t length = string_length(game_data_root);
    if (buffer == nullptr || length == 0)
    {
        errno = EINVAL;
        return nullptr;
    }
    if (size <= length)
    {
        errno = ERANGE;
        return nullptr;
    }
    for (std::size_t index = 0; index <= length; ++index)
        buffer[index] = game_data_root[index];
    bedlam2_ps5_trace("Bedlam2 files: engine getcwd complete\n");
    return buffer;
}

extern "C" int bedlam2_ps5_mkdir_legacy(const char *path) noexcept
{
    char resolved_path[kMaxPathLength];
    if (!bedlam2_ps5_resolve_data_path(path, resolved_path, sizeof(resolved_path)))
    {
        errno = path == nullptr ? EINVAL : ENAMETOOLONG;
        return -1;
    }
    const int status = sceKernelMkdir(resolved_path, 0777);
    if (status != 0)
    {
        // Native kernel errors encode the POSIX errno in the low 16 bits.
        errno = static_cast<unsigned int>(status) & 0xffffu;
        return -1;
    }
    return 0;
}

extern "C" bool bedlam2_ps5_resolve_data_path(const char *path, char *output,
                                               std::size_t output_size) noexcept
{
    if (path == nullptr || output == nullptr || output_size == 0)
        return false;

    const std::size_t root_length = string_length(game_data_root);
    const std::size_t path_length = string_length(path);
    const bool absolute = path[0] == '/';
    const std::size_t prefix_length = root_length == 0 || absolute ? 0 : root_length + 1;
    if (prefix_length + path_length + 1 > output_size)
        return false;

    if (prefix_length != 0)
    {
        for (std::size_t index = 0; index < root_length; ++index)
            output[index] = game_data_root[index] == '\\' ? '/' : game_data_root[index];
        output[root_length] = '/';
    }
    copy_normalized_path(path, output, prefix_length);
    return true;
}

extern "C" std::uint32_t bedlam2_ps5_fopen_legacy(
    const char *filename, const char *mode) noexcept
{
    char resolved_path[kMaxPathLength];
    if (!bedlam2_ps5_resolve_data_path(filename, resolved_path, sizeof(resolved_path)))
        return 0;

    std::FILE *const file = std::fopen(resolved_path, mode);
    if (file == nullptr)
    {
        return 0;
    }

    std::lock_guard<std::mutex> lock(open_files_mutex);
    for (std::size_t index = 0; index < open_files.size(); ++index)
    {
        if (open_files[index] == nullptr)
        {
            open_files[index] = file;
            return kFileHandleBase +
                   static_cast<std::uint32_t>(index) * kFileHandleStride;
        }
    }
    std::fclose(file);
    return 0;
}

extern "C" int bedlam2_ps5_fclose_legacy(std::uint32_t handle) noexcept
{
    std::lock_guard<std::mutex> lock(open_files_mutex);
    std::size_t index = 0;
    if (!file_slot_index(handle, index))
    {
        return EOF;
    }
    std::FILE *const file = file_from_handle(handle);
    if (file == nullptr)
    {
        return EOF;
    }
    open_files[index] = nullptr;
    const int result = std::fclose(file);
    return result;
}

extern "C" int bedlam2_ps5_fflush_legacy(std::uint32_t handle) noexcept
{
    if (handle == 0)
    {
        return std::fflush(nullptr);
    }
    std::lock_guard<std::mutex> lock(open_files_mutex);
    std::FILE *const file = file_from_handle(handle);
    return file == nullptr ? EOF : std::fflush(file);
}

extern "C" std::uint32_t bedlam2_ps5_fread_legacy(
    void *buffer, std::uint32_t element_size, std::uint32_t element_count,
    std::uint32_t handle) noexcept
{
    std::lock_guard<std::mutex> lock(open_files_mutex);
    std::FILE *const file = file_from_handle(handle);
    if (file == nullptr)
    {
        return 0;
    }
    return static_cast<std::uint32_t>(
        std::fread(buffer, element_size, element_count, file));
}

extern "C" std::uint32_t bedlam2_ps5_fwrite_legacy(
    const void *buffer, std::uint32_t element_size, std::uint32_t element_count,
    std::uint32_t handle) noexcept
{
    std::lock_guard<std::mutex> lock(open_files_mutex);
    std::FILE *const file = file_from_handle(handle);
    if (file == nullptr)
    {
        return 0;
    }
    return static_cast<std::uint32_t>(
        std::fwrite(buffer, element_size, element_count, file));
}

extern "C" int bedlam2_ps5_fseek_legacy(
    std::uint32_t handle, std::int32_t offset, int origin) noexcept
{
    std::lock_guard<std::mutex> lock(open_files_mutex);
    std::FILE *const file = file_from_handle(handle);
    return file == nullptr ? -1 : std::fseek(file, offset, origin);
}

extern "C" std::int32_t bedlam2_ps5_ftell_legacy(
    std::uint32_t handle) noexcept
{
    std::lock_guard<std::mutex> lock(open_files_mutex);
    std::FILE *const file = file_from_handle(handle);
    return file == nullptr ? -1 : static_cast<std::int32_t>(std::ftell(file));
}

extern "C" std::int32_t bedlam2_ps5_dos_lseek(
    std::uint32_t handle, std::uint32_t origin, std::int32_t offset) noexcept
{
    const int whence = origin == 0 ? SEEK_SET : origin == 1 ? SEEK_CUR :
                       origin == 2 ? SEEK_END : -1;
    if (whence < 0)
    {
        return -1;
    }

    std::lock_guard<std::mutex> lock(open_files_mutex);
    std::FILE *const file = file_from_handle(handle);
    if (file != nullptr)
    {
        if (std::fseek(file, offset, whence) != 0)
        {
            return -1;
        }
        const long position = std::ftell(file);
        return position < 0 ? -1 : static_cast<std::int32_t>(position);
    }
    if (handle > static_cast<std::uint32_t>(INT_MAX))
    {
        return -1;
    }
    const off_t position = ::lseek(static_cast<int>(handle), offset, whence);
    return position < 0 ? -1 : static_cast<std::int32_t>(position);
}

extern "C" std::int32_t bedlam2_ps5_dos_write(
    std::uint32_t handle, const void *buffer, std::uint32_t byte_count) noexcept
{
    std::lock_guard<std::mutex> lock(open_files_mutex);
    std::FILE *const file = file_from_handle(handle);
    if (file != nullptr)
    {
        const std::size_t written = std::fwrite(buffer, 1, byte_count, file);
        return written == 0 && std::ferror(file)
                   ? -1
                   : static_cast<std::int32_t>(written);
    }
    if (handle > static_cast<std::uint32_t>(INT_MAX))
    {
        return -1;
    }
    const ssize_t written = ::write(static_cast<int>(handle), buffer, byte_count);
    return written < 0 ? -1 : static_cast<std::int32_t>(written);
}

extern "C" std::int32_t bedlam2_ps5_dos_isatty(
    std::uint32_t handle) noexcept
{
    std::lock_guard<std::mutex> lock(open_files_mutex);
    if (file_from_handle(handle) != nullptr ||
        handle > static_cast<std::uint32_t>(INT_MAX))
    {
        return 0;
    }
    return ::isatty(static_cast<int>(handle)) != 0 ? 1 : 0;
}
