#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

#include "bedlam2_log.hpp"
#include "file.h"
#ifdef BEDLAM2_PS5_NATIVE
#include "native_data_path.hpp"
#endif

namespace
{
std::size_t last_separator(const std::string &path)
{
    return path.find_last_of("/\\");
}

std::FILE *open_file(const std::string &path)
{
#ifdef BEDLAM2_PS5_NATIVE
    char resolved_path[4096];
    if (!bedlam2_ps5_resolve_data_path(path.c_str(), resolved_path, sizeof(resolved_path)))
        return nullptr;
    return std::fopen(resolved_path, "rb");
#else
    return std::fopen(path.c_str(), "rb");
#endif
}
} // namespace

File::File()
    : m_file_size(0)
{
}

File::File(const std::string &path)
    : m_file(path), m_file_size(0)
{
    load_data();
}

int File::load(const std::string &path)
{
    if (path == m_file && m_file_size)
        return 0;
    return set_path(path);
}

int File::set_path(const std::string &path)
{
    std::FILE *file = open_file(path);
    if (file == nullptr)
        return 1;

    std::fclose(file);
    m_file = path;
    return load_data();
}

std::string File::get_full_path() const
{
    return m_file;
}

std::string File::get_directory() const
{
    const std::size_t separator = last_separator(m_file);
    if (separator == std::string::npos)
        return {};
    if (separator == 0)
        return m_file.substr(0, 1);
    return m_file.substr(0, separator);
}

std::string File::get_filename() const
{
    const std::size_t separator = last_separator(m_file);
    return separator == std::string::npos ? m_file : m_file.substr(separator + 1);
}

std::string File::get_extension() const
{
    const std::string filename = get_filename();
    const std::size_t dot = filename.find_last_of('.');
    if (dot == std::string::npos || dot == 0)
        return {};
    return filename.substr(dot);
}

int File::load_data()
{
    std::FILE *file = open_file(m_file);
    if (file == nullptr)
    {
        std::string uppercase_path = m_file;
        std::transform(uppercase_path.begin(), uppercase_path.end(), uppercase_path.begin(),
                       [](unsigned char character) {
                           return static_cast<char>(std::toupper(character));
                       });
        file = open_file(uppercase_path);
        if (file != nullptr)
            m_file = uppercase_path;
    }

    if (file == nullptr)
    {
        BEDLAM_LOG << "Unable to open file " << m_file << BEDLAM_ENDL;
        std::exit(404);
    }

    if (std::fseek(file, 0, SEEK_END) != 0)
    {
        std::fclose(file);
        return 1;
    }
    const long file_end = std::ftell(file);
    if (file_end < 0 || std::fseek(file, 0, SEEK_SET) != 0)
    {
        std::fclose(file);
        return 1;
    }

    m_file_size = static_cast<std::size_t>(file_end);
    m_data.resize(m_file_size);
    if (m_file_size != 0 && std::fread(m_data.data(), 1, m_file_size, file) != m_file_size)
    {
        std::fclose(file);
        m_data.clear();
        m_file_size = 0;
        return 1;
    }

    std::fclose(file);
    return 0;
}

uint16_t File::get_file_header() const
{
    return *((uint16_t *)m_data.data());
}

const uint8_t *File::get_ptr() const
{
    return m_data.data();
}

uint8_t *File::get_ptr()
{
    return m_data.data();
}

size_t File::get_size() const
{
    return m_file_size;
}
