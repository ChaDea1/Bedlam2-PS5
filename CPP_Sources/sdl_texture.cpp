#include "bedlam2_log.hpp"

#include "exported_func.h"
#include "sdl_texture.h"
#include "sdl_window.h"
#ifdef BEDLAM2_PS5_NATIVE
#include "native_debug.hpp"
#if defined(__x86_64__)
#include <immintrin.h>
#endif
#endif

namespace
{
#if defined(BEDLAM2_PS5_NATIVE) && defined(__x86_64__)
__attribute__((target("avx2")))
void expand_indexed_row_avx2(const uint8_t *source, SDL_Color *destination,
                             int width, const SDL_Color *palette)
{
    static_assert(sizeof(SDL_Color) == sizeof(uint32_t),
                  "AVX2 palette expansion expects packed SDL_Color values");
    const auto *packed_palette = reinterpret_cast<const int *>(palette);
    int x = 0;
    for (; x + 8 <= width; x += 8)
    {
        const __m128i indices8 = _mm_loadl_epi64(
            reinterpret_cast<const __m128i *>(source + x));
        const __m256i indices32 = _mm256_cvtepu8_epi32(indices8);
        const __m256i colors = _mm256_i32gather_epi32(packed_palette,
                                                       indices32, 4);
        _mm256_storeu_si256(reinterpret_cast<__m256i *>(destination + x),
                            colors);
    }
    for (; x < width; ++x)
    {
        destination[x] = palette[source[x]];
    }
}
#endif

int texture_error(const char *operation, int width, int height)
{
#ifdef BEDLAM2_PS5_NATIVE
    char message[768];
    SDL_snprintf(message, sizeof(message), "Bedlam2 texture: %s (%dx%d): %s\n",
                 operation, width, height, SDL_GetError());
    bedlam2_ps5_trace(message);
#endif
    BEDLAM_LOG << operation << ": " << SDL_GetError() << BEDLAM_ENDL;
    return -1;
}
} // namespace

Texture::Texture()
{
}

Texture::Texture(int img, const BIN_File &bin)
{
    create_texture(img, bin);
}

Texture::Texture(int img, const BIN_File &bin, const File &pal)
{
    create_texture(img, bin, pal);
}

Texture::Texture(SDL_Texture *texture, int width, int height)
    : width(width)
    , height(height)
{
    m_texture = texture;
    m_offset_x = 0;
    m_offset_y = 0;
}

Texture::~Texture()
{
    destroy();
}

Texture::Texture(const Texture &t)
    : width(t.width)
    , height(t.height)
    , m_offset_x(t.m_offset_x)
    , m_offset_y(t.m_offset_y)
{
    int error_val = 0;
    if (t.m_texture)
    {
        destroy();

        m_texture = SDL_CreateTexture(
            GAME_WINDOW.get_renderer(), SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, t.width, t.height);
        if (m_texture == nullptr)
        {
            BEDLAM_LOG << "ERROR: create SDL texture in copy constructor. " << SDL_GetError() << BEDLAM_ENDL;
            error_val |= -1;
        }

        uint8_t *dest_arr = nullptr;
        uint8_t *source_arr = nullptr;
        int dest_pitch = 0;
        int source_pitch = 0;

        error_val |= SDL_LockTexture(m_texture, nullptr, reinterpret_cast<void **>(&dest_arr), &dest_pitch);
        error_val |= SDL_LockTexture(t.m_texture, nullptr, reinterpret_cast<void **>(&source_arr), &source_pitch);

        int dest_width = t.width;
        int dest_height = t.height;

        if (error_val)
        {
            BEDLAM_LOG << "ERROR: texture copy initialization. " << SDL_GetError() << BEDLAM_ENDL;
        }
        else
        {
            memcpy(dest_arr, source_arr, source_pitch * t.height);
        }
        SDL_UnlockTexture(t.m_texture);
        SDL_UnlockTexture(m_texture);

        error_val |= SDL_SetTextureBlendMode(m_texture, SDL_BLENDMODE_BLEND);
        if (error_val)
        {
            BEDLAM_LOG << "ERROR: texture copy initialization. " << SDL_GetError() << BEDLAM_ENDL;
        }
    }
    else
    {
        m_texture = nullptr;
    }
}

Texture::Texture(int width, int height)
    : width(width)
    , height(height)
    , m_offset_x(0)
    , m_offset_y(0)
{
    m_texture = SDL_CreateTexture(
        GAME_WINDOW.get_renderer(), SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, width, height);
    int error_val = SDL_SetTextureBlendMode(m_texture, SDL_BLENDMODE_BLEND);
    if (error_val || m_texture == nullptr)
    {
        BEDLAM_LOG << "ERROR: create SDL texture. " << SDL_GetError() << BEDLAM_ENDL;
    }
}

Texture &Texture::operator=(const Texture &t)
{
    int error_val = 0;
    if (this == &t)
    {
        return *this;
    }

    if (t.m_texture)
    {
        uint8_t *dest_arr = nullptr;
        uint8_t *source_arr = nullptr;
        int dest_pitch = 0;
        int source_pitch = 0;

        destroy();

        m_texture = SDL_CreateTexture(
            GAME_WINDOW.get_renderer(), SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, t.width, t.height);

        error_val |= SDL_LockTexture(m_texture, nullptr, reinterpret_cast<void **>(&dest_arr), &dest_pitch);
        error_val |= SDL_LockTexture(t.m_texture, nullptr, reinterpret_cast<void **>(&source_arr), &source_pitch);

        int dest_width = t.width;
        int dest_height = t.height;

        if (error_val || m_texture == nullptr)
        {
            BEDLAM_LOG << "ERROR: operator=. " << SDL_GetError() << BEDLAM_ENDL;
        }
        else
        {
            memcpy(dest_arr, source_arr, source_pitch * t.height);
        }
        SDL_UnlockTexture(t.m_texture);
        SDL_UnlockTexture(m_texture);

        error_val |= SDL_SetTextureBlendMode(m_texture, SDL_BLENDMODE_BLEND);
        if (error_val)
        {
            BEDLAM_LOG << "ERROR: texture operator=. " << SDL_GetError() << BEDLAM_ENDL;
        }
        width = t.width;
        height = t.height;
        m_offset_x = t.m_offset_x;
        m_offset_y = t.m_offset_y;
    }
    else
    {
        m_texture = nullptr;
        width = 0;
        height = 0;
        m_offset_x = 0;
        m_offset_y = 0;
    }

    return *this;
}

int Texture::create_texture(int img, const BIN_File &bin)
{
    const File pal;
    return create_texture(img, bin, pal);
}

int Texture::create_texture(int img, const BIN_File &bin, const File &pal)
{
    // Fill texture in buffer via standard bedlam function,
    // then create texture from buffer with palette colors
    if (m_texture)
    {
        destroy();
    }
    int ret_val = 0;
    width = bin.get_img_width(img);
    height = bin.get_img_height(img);

    m_offset_x = bin.get_img_x_offset(img);
    m_offset_y = bin.get_img_y_offset(img);

    std::vector<uint8_t> buffer(640 * 480, 0);

    const uint16_t img_header = *(uint16_t *)bin.get_img_header_ptr(img);
    const uint16_t bedlam_tile_code = 0x0007;
    if (img_header == bedlam_tile_code)
    {
        if (width && height)
        {
            draw_tile(img, 0, 0, buffer.data(), bin.get_ptr());
        }
        else
        {
            width = 64;
            height = 32;
        }
    }
    else
    {
        if (width && height)
        {
            draw_IMG_in_buffer(img, 1, 0, 0, buffer.data(), bin.get_ptr());
        }
        else
        {
            // create empty texture
            width = 1;
            height = 1;
        }
    }

    m_texture = SDL_CreateTexture(
        GAME_WINDOW.get_renderer(), SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, width, height);
    if (m_texture == nullptr)
    {
        ret_val |= 1;
        BEDLAM_LOG << "ERROR: create SDL texture. " << SDL_GetError() << ". " << bin.get_full_path() << " Img: " << img
                  << BEDLAM_ENDL;
    }
    else
    {
        ret_val |= SDL_SetTextureBlendMode(m_texture, SDL_BLENDMODE_BLEND);

        const uint8_t *palette = pal.get_ptr() + 2;
        uint8_t *bytes = nullptr;
        int pitch = 0;
        ret_val |= SDL_LockTexture(m_texture, nullptr, reinterpret_cast<void **>(&bytes), &pitch);
        for (int y = 0; y < height; y++)
        {
            for (int x = 0; x < width; x++)
            {
                int index = buffer[(m_offset_y + y) * 640 + x + m_offset_x];
                bytes[y * pitch + x * 4 + 0] = 4 * palette[index * 3 + 0];
                bytes[y * pitch + x * 4 + 1] = 4 * palette[index * 3 + 1];
                bytes[y * pitch + x * 4 + 2] = 4 * palette[index * 3 + 2];
                bytes[y * pitch + x * 4 + 3] = index ? 255 : 0;
            }
        }
        SDL_UnlockTexture(m_texture);
    }
    return ret_val;
}

int Texture::create_texture_from_surface(SDL_Surface *surface, SDL_Rect *texture_rect)
{
    if (surface == nullptr || texture_rect == nullptr)
    {
        return SDL_SetError("Texture source surface or rectangle is null");
    }
    if (texture_rect->x < 0 || texture_rect->y < 0 ||
        texture_rect->x >= surface->w || texture_rect->y >= surface->h)
    {
        return SDL_SetError("Texture source origin is outside the surface");
    }
    if (texture_rect->w <= 0 || texture_rect->w > surface->w - texture_rect->x)
    {
        texture_rect->w = surface->w - texture_rect->x;
    }
    if (texture_rect->h <= 0 || texture_rect->h > surface->h - texture_rect->y)
    {
        texture_rect->h = surface->h - texture_rect->y;
    }
    destroy();
    width = texture_rect->w;
    height = texture_rect->h;
    m_offset_x = texture_rect->x;
    m_offset_y = texture_rect->y;

    m_texture = SDL_CreateTexture(
        GAME_WINDOW.get_renderer(), SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, width, height);
    if (m_texture == nullptr)
    {
        return texture_error("create from surface", width, height);
    }
    if (SDL_SetTextureBlendMode(m_texture, SDL_BLENDMODE_NONE) < 0)
    {
        return texture_error("set surface texture blend mode", width, height);
    }
    return fill_texture_from_surface(surface, texture_rect);
}

int Texture::fill_texture_from_surface(SDL_Surface *surface, SDL_Rect *texture_rect)
{
    if (surface == nullptr || texture_rect == nullptr || surface->pixels == nullptr ||
        surface->format == nullptr || surface->format->BytesPerPixel != 1 ||
        surface->format->palette == nullptr)
    {
        return SDL_SetError("Texture source must be an indexed surface with pixels and a palette");
    }
    if (texture_rect->x < 0 || texture_rect->y < 0 ||
        texture_rect->w <= 0 || texture_rect->h <= 0 ||
        texture_rect->x > surface->w || texture_rect->y > surface->h ||
        texture_rect->w > surface->w - texture_rect->x ||
        texture_rect->h > surface->h - texture_rect->y ||
        texture_rect->w > width || texture_rect->h > height)
    {
        return SDL_SetError("Texture source rectangle exceeds source or destination bounds");
    }
    uint8_t *bytes = nullptr;
    int pitch = 0;
    if (SDL_LockTexture(m_texture, nullptr, reinterpret_cast<void **>(&bytes), &pitch) < 0)
    {
        return texture_error("lock surface texture", width, height);
    }
    if (bytes == nullptr || pitch / static_cast<int>(sizeof(SDL_Color)) < texture_rect->w)
    {
        SDL_UnlockTexture(m_texture);
        return SDL_SetError("Locked texture has no usable pixel storage");
    }
    const bool must_lock = SDL_MUSTLOCK(surface);
    if (must_lock)
    {
        if (SDL_LockSurface(surface) < 0)
        {
            SDL_UnlockTexture(m_texture);
            return texture_error("lock source surface", width, height);
        }
    }
    int ret_val = 0;
#if defined(BEDLAM2_PS5_NATIVE) && defined(__x86_64__)
    if (surface->format->palette->ncolors == 256)
    {
        for (int y = 0; y < texture_rect->h; ++y)
        {
            const auto *source_row = static_cast<const uint8_t *>(surface->pixels) +
                                     (y + texture_rect->y) * surface->pitch + texture_rect->x;
            auto *destination_row = reinterpret_cast<SDL_Color *>(bytes + y * pitch);
            expand_indexed_row_avx2(source_row, destination_row,
                                    texture_rect->w,
                                    surface->format->palette->colors);
        }
    }
    else
#endif
    {
        for (int y = 0; y < texture_rect->h; y++)
        {
            const auto *source_row = static_cast<const uint8_t *>(surface->pixels) +
                                     (y + texture_rect->y) * surface->pitch + texture_rect->x;
            auto *destination_row = reinterpret_cast<SDL_Color *>(bytes + y * pitch);
            for (int x = 0; x < texture_rect->w; x++)
            {
                const uint8_t index = source_row[x];
                if (index >= surface->format->palette->ncolors)
                {
                    ret_val = SDL_SetError("Surface pixel exceeds its palette");
                    break;
                }
                destination_row[x] = surface->format->palette->colors[index];
            }
            if (ret_val < 0)
                break;
        }
    }
    if (must_lock)
    {
        SDL_UnlockSurface(surface);
    }
    SDL_UnlockTexture(m_texture);
    return ret_val;
}

int Texture::draw(int x, int y) const
{
    int ret_val = 0;
    SDL_Rect destination = { m_offset_x + x, m_offset_y + y, width, height };
    ret_val |= SDL_RenderCopy(GAME_WINDOW.get_renderer(), m_texture, NULL, &destination);
    return ret_val;
}

// Not tested in this project
int Texture::stretch_draw(int x, int y) const
{
    int ret_val = 0;
    int w = GAME_WINDOW.get_game_width();
    int h = GAME_WINDOW.get_game_height();
    if (w * 3 > h * 4)
    {
        w = w * 4 / 3;
    }
    else
    {
        h = h * 3 / 4;
    }
    SDL_Rect destination = { (m_offset_x + x) * w / 640, (m_offset_y + y) * h / 480, width * w / 640, height * h / 480 };
    ret_val |= SDL_RenderCopy(GAME_WINDOW.get_renderer(), m_texture, NULL, &destination);
    return ret_val;
}

int Texture::merge_with(const Texture &t, int x, int y)
{
    int ret_val = 0;

    uint8_t *dest_arr = nullptr;
    uint8_t *source_arr = nullptr;
    int dest_pitch = 0;
    int source_pitch = 0;

    ret_val |= SDL_LockTexture(m_texture, nullptr, reinterpret_cast<void **>(&dest_arr), &dest_pitch);
    ret_val |= SDL_LockTexture(t.m_texture, nullptr, reinterpret_cast<void **>(&source_arr), &source_pitch);

    int i = 0;
    uint8_t alpha = 0;
    int dest_width = x + t.width;
    int dest_height = y + t.height;
    if (dest_width > width)
    {
        dest_width = width;
    }
    if (dest_height > height)
    {
        dest_height = height;
    }

    for (int h = y; h < dest_height; h++)
    {
        for (int w = x; w < dest_width; w++)
        {
            alpha = source_arr[i + 3];
            if (alpha)
            {
                dest_arr[h * dest_pitch + w * 4 + 0] = source_arr[i + 0];
                dest_arr[h * dest_pitch + w * 4 + 1] = source_arr[i + 1];
                dest_arr[h * dest_pitch + w * 4 + 2] = source_arr[i + 2];
                dest_arr[h * dest_pitch + w * 4 + 3] = alpha;
            }
            i += 4;
        }
    }

    SDL_UnlockTexture(t.m_texture);
    SDL_UnlockTexture(m_texture);

    if (ret_val)
    {
        BEDLAM_LOG << "ERROR: texture merge. " << SDL_GetError() << BEDLAM_ENDL;
    }
    return ret_val;
}

int Texture::destroy()
{
    if (m_texture)
    {
        SDL_DestroyTexture(m_texture);
        m_texture = nullptr;
    }
    return 0;
}

SDL_Texture *Texture::get()
{
    return m_texture;
}

SDL_Texture *const Texture::get() const
{
    return m_texture;
}

SDL_Rect *const Texture::get_rect() const
{
    static SDL_Rect ret_rect;
    ret_rect.x = m_offset_x;
    ret_rect.y = m_offset_y;
    ret_rect.w = width;
    ret_rect.h = height;
    return &ret_rect;
}
