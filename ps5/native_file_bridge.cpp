#include <array>
#include <climits>
#include <cerrno>
#include <cctype>
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
constexpr char kStorageRoot[] = "/data/Bedlam2PS5";
constexpr char kPersistentContentRoot[] = "/data/Bedlam2PS5/content";
constexpr char kPersistentSaveRoot[] = "/data/Bedlam2PS5/saves";
std::array<std::FILE *, kMaxOpenFiles> open_files{};
std::mutex open_files_mutex;
char game_data_root[kMaxPathLength]{};
char save_data_root[kMaxPathLength]{};

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

bool copy_absolute_root(char *destination, const char *path) noexcept
{
    const std::size_t length = string_length(path);
    if (path == nullptr || path[0] != '/' || length == 0 ||
        length >= kMaxPathLength)
    {
        return false;
    }
    copy_normalized_path(path, destination, 0);
    return true;
}

bool normalize_relative_path(const char *path, char *output,
                             std::size_t output_size) noexcept
{
    if (path == nullptr || output == nullptr || output_size == 0 ||
        path[0] == '/' || path[0] == '\\')
    {
        return false;
    }

    std::size_t output_length = 0;
    const char *segment = path;
    for (const char *cursor = path;; ++cursor)
    {
        const char character = *cursor;
        if (character != '/' && character != '\\' && character != '\0')
            continue;

        const std::size_t segment_length =
            static_cast<std::size_t>(cursor - segment);
        if (segment_length != 0)
        {
            if ((segment_length == 1 && segment[0] == '.') ||
                (segment_length == 2 && segment[0] == '.' && segment[1] == '.'))
            {
                return false;
            }
            const std::size_t separator_length = output_length == 0 ? 0 : 1;
            if (separator_length + segment_length >= output_size - output_length)
                return false;
            if (separator_length != 0)
                output[output_length++] = '/';
            for (std::size_t index = 0; index < segment_length; ++index)
                output[output_length++] = segment[index];
        }

        if (character == '\0')
            break;
        segment = cursor + 1;
    }

    if (output_length == 0)
        return false;
    output[output_length] = '\0';
    return true;
}

bool join_root_path(const char *root, const char *relative_path,
                    char *output, std::size_t output_size) noexcept
{
    const std::size_t root_length = string_length(root);
    const std::size_t relative_length = string_length(relative_path);
    if (root == nullptr || relative_path == nullptr || output == nullptr ||
        root_length == 0 || output_size == 0)
    {
        return false;
    }

    const bool needs_separator = root[root_length - 1] != '/';
    const std::size_t separator_length = relative_length != 0 && needs_separator ? 1 : 0;
    if (root_length + separator_length + relative_length + 1 > output_size)
        return false;

    for (std::size_t index = 0; index < root_length; ++index)
        output[index] = root[index] == '\\' ? '/' : root[index];
    std::size_t offset = root_length;
    if (separator_length != 0)
        output[offset++] = '/';
    copy_normalized_path(relative_path, output, offset);
    return true;
}

int ensure_directory(const char *path) noexcept
{
    const int status = sceKernelMkdir(path, 0777);
    if (status == 0)
        return 0;

    const unsigned int error = static_cast<unsigned int>(status) & 0xffffu;
    if (error == EEXIST)
        return 0;
    errno = static_cast<int>(error);
    return -1;
}

int ensure_directory_tree(const char *path) noexcept
{
    const std::size_t length = string_length(path);
    if (path == nullptr || path[0] != '/' || length == 0 ||
        length >= kMaxPathLength)
    {
        errno = EINVAL;
        return -1;
    }

    char partial[kMaxPathLength];
    for (std::size_t index = 0; index <= length; ++index)
    {
        if (path[index] != '/' && path[index] != '\0')
            continue;
        if (index == 0)
            continue;

        for (std::size_t copy_index = 0; copy_index < index; ++copy_index)
            partial[copy_index] = path[copy_index] == '\\' ? '/' : path[copy_index];
        partial[index] = '\0';
        if (ensure_directory(partial) != 0)
            return -1;
    }
    return 0;
}

bool ensure_parent_directories(const char *path) noexcept
{
    const std::size_t length = string_length(path);
    if (path == nullptr || path[0] != '/' || length >= kMaxPathLength)
        return false;

    char partial[kMaxPathLength];
    for (std::size_t index = 0; index < length; ++index)
    {
        if (index == 0 || path[index] != '/')
            continue;
        for (std::size_t copy_index = 0; copy_index < index; ++copy_index)
            partial[copy_index] = path[copy_index] == '\\' ? '/' : path[copy_index];
        partial[index] = '\0';
        if (ensure_directory_tree(partial) != 0)
            return false;
    }
    return true;
}

bool mode_contains(const char *mode, char wanted) noexcept
{
    if (mode == nullptr)
        return false;
    for (std::size_t index = 0; mode[index] != '\0'; ++index)
    {
        if (mode[index] == wanted)
            return true;
    }
    return false;
}

bool is_legacy_save_path(const char *relative_path) noexcept
{
    static const char prefix[] = "MIRAGE/AB_BED/";
    constexpr std::size_t prefix_length = sizeof(prefix) - 1;
    if (relative_path == nullptr || string_length(relative_path) <= prefix_length)
        return false;
    for (std::size_t index = 0; index < prefix_length; ++index)
    {
        const unsigned char left = static_cast<unsigned char>(relative_path[index]);
        const unsigned char right = static_cast<unsigned char>(prefix[index]);
        if (std::tolower(left) != std::tolower(right))
            return false;
    }
    return true;
}

bool file_exists(const char *path) noexcept
{
    std::FILE *const file = std::fopen(path, "rb");
    if (file == nullptr)
        return false;
    std::fclose(file);
    return true;
}

bool copy_file_if_missing(const char *source_path, const char *destination_path) noexcept
{
    if (file_exists(destination_path))
        return true;

    std::FILE *const source = std::fopen(source_path, "rb");
    if (source == nullptr)
        return false;
    if (!ensure_parent_directories(destination_path))
    {
        std::fclose(source);
        return false;
    }

    std::FILE *const destination = std::fopen(destination_path, "wb");
    if (destination == nullptr)
    {
        std::fclose(source);
        return false;
    }

    char buffer[8192];
    bool success = true;
    for (;;)
    {
        const std::size_t count = std::fread(buffer, 1, sizeof(buffer), source);
        if (count != 0 && std::fwrite(buffer, 1, count, destination) != count)
        {
            success = false;
            break;
        }
        if (count < sizeof(buffer))
        {
            if (std::ferror(source))
                success = false;
            break;
        }
    }

    if (std::fclose(source) != 0)
        success = false;
    if (std::fclose(destination) != 0)
        success = false;
    return success;
}

std::FILE *open_legacy_app_file(const char *relative_path,
                               const char *mode) noexcept
{
    static const char *const legacy_roots[] = {
        "/app0/assets/bed2",
        "/app0/bed2",
        "/app0",
    };
    char path[kMaxPathLength];
    for (const char *root : legacy_roots)
    {
        if (!join_root_path(root, relative_path, path, sizeof(path)))
            continue;
        std::FILE *const file = std::fopen(path, mode);
        if (file != nullptr)
            return file;
    }
    return nullptr;
}

bool migrate_legacy_save_if_present(const char *filename,
                                    const char *relative_path,
                                    const char *destination_path) noexcept
{
    char source_path[kMaxPathLength];
    if (bedlam2_ps5_resolve_data_path(filename, source_path, sizeof(source_path)) &&
        copy_file_if_missing(source_path, destination_path))
    {
        return true;
    }

    static const char *const legacy_roots[] = {
        "/app0/assets/bed2",
        "/app0/bed2",
        "/app0",
    };
    for (const char *root : legacy_roots)
    {
        if (join_root_path(root, relative_path, source_path, sizeof(source_path)) &&
            copy_file_if_missing(source_path, destination_path))
        {
            return true;
        }
    }
    return false;
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

extern "C" int bedlam2_ps5_init_storage() noexcept
{
    if (!copy_absolute_root(save_data_root, kPersistentSaveRoot))
    {
        errno = EINVAL;
        return -1;
    }
    bedlam2_ps5_set_data_root(kPersistentContentRoot);

    if (ensure_directory_tree(kStorageRoot) != 0 ||
        ensure_directory_tree(kPersistentContentRoot) != 0 ||
        ensure_directory_tree(kPersistentSaveRoot) != 0)
    {
        bedlam2_ps5_trace("Bedlam2 files: persistent storage setup failed\n");
        return -1;
    }

    bedlam2_ps5_trace("Bedlam2 files: persistent content and saves ready\n");
    return 0;
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
    bool resolved = false;
    if (save_data_root[0] != '\0' && path != nullptr && path[0] != '/' && path[0] != '\\')
        resolved = bedlam2_ps5_resolve_save_path(path, resolved_path,
                                                 sizeof(resolved_path));
    else
        resolved = bedlam2_ps5_resolve_data_path(path, resolved_path,
                                                 sizeof(resolved_path));
    if (!resolved)
    {
        errno = path == nullptr ? EINVAL : ENAMETOOLONG;
        return -1;
    }
    return ensure_directory_tree(resolved_path);
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

extern "C" bool bedlam2_ps5_resolve_save_path(const char *path, char *output,
                                                std::size_t output_size) noexcept
{
    if (save_data_root[0] == '\0' || output == nullptr || output_size == 0)
        return false;

    char relative_path[kMaxPathLength];
    if (!normalize_relative_path(path, relative_path, sizeof(relative_path)))
        return false;
    return join_root_path(save_data_root, relative_path, output, output_size);
}

extern "C" std::uint32_t bedlam2_ps5_fopen_legacy(
    const char *filename, const char *mode) noexcept
{
    if (filename == nullptr || mode == nullptr)
        return 0;

    // Keep the previous path behavior for host shims and any early startup
    // opens. Native main initializes the persistent roots before the engine.
    if (save_data_root[0] == '\0')
    {
        char resolved_path[kMaxPathLength];
        if (!bedlam2_ps5_resolve_data_path(filename, resolved_path,
                                           sizeof(resolved_path)))
        {
            return 0;
        }
        std::FILE *const file = std::fopen(resolved_path, mode);
        if (file == nullptr)
            return 0;

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

    char relative_path[kMaxPathLength];
    const bool has_relative_path =
        normalize_relative_path(filename, relative_path, sizeof(relative_path));
    const bool legacy_save_file =
        has_relative_path && is_legacy_save_path(relative_path);
    const bool writable = mode_contains(mode, 'w') || mode_contains(mode, 'a') ||
                          mode_contains(mode, '+');
    std::FILE *file = nullptr;

    if (writable)
    {
        char save_path[kMaxPathLength];
        if (!has_relative_path ||
            !bedlam2_ps5_resolve_save_path(relative_path, save_path,
                                           sizeof(save_path)))
        {
            return 0;
        }

        const bool preserves_existing = !mode_contains(mode, 'w') &&
                                        (mode_contains(mode, '+') ||
                                         mode_contains(mode, 'a'));
        if (legacy_save_file && preserves_existing && !file_exists(save_path))
        {
            (void)migrate_legacy_save_if_present(filename, relative_path, save_path);
        }
        if (!ensure_parent_directories(save_path))
            return 0;
        file = std::fopen(save_path, mode);
    }
    else
    {
        if (legacy_save_file)
        {
            char save_path[kMaxPathLength];
            if (bedlam2_ps5_resolve_save_path(relative_path, save_path,
                                              sizeof(save_path)))
            {
                file = std::fopen(save_path, mode);
            }
        }

        if (file == nullptr)
        {
            char data_path[kMaxPathLength];
            if (bedlam2_ps5_resolve_data_path(filename, data_path,
                                              sizeof(data_path)))
            {
                file = std::fopen(data_path, mode);
            }
        }
        if (file == nullptr && legacy_save_file)
            file = open_legacy_app_file(relative_path, mode);
    }

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
