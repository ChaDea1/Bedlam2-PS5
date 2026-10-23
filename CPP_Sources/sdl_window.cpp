#include <algorithm>
#include <cstdint>
#include "bedlam2_log.hpp"

#include "bedlam2.h"
#include "bedlam2_draw.h"
#include "palette.h"
#include "sdl_event.h"
#include "sdl_window.h"
#include "timer.h"
#ifdef BEDLAM2_PS5_NATIVE
#include "native_engine_memory.h"
#include "native_input.h"
#include "native_debug.hpp"
#define BEDLAM2_ALLOC_ARRAY(type, count) \
    bedlam2::native_memory::allocate_zeroed_array<type>(count)
#define BEDLAM2_FREE_ARRAY(memory) bedlam2::native_memory::release_array(memory)
#else
#define BEDLAM2_ALLOC_ARRAY(type, count) new type[count]()
#define BEDLAM2_FREE_ARRAY(memory) delete[] memory
#endif

Window GAME_WINDOW;

#ifdef BEDLAM2_PS5_NATIVE
namespace
{
constexpr unsigned int kMaxZoomFilterDiagnostics = 8;
unsigned int g_zoom_filter_diagnostics_emitted = 0;

constexpr unsigned int kMaxShopFrameRateDiagnostics = 8;
unsigned int g_shop_frame_rate_diagnostics_emitted = 0;
unsigned int g_shop_setup_diagnostics_emitted = 0;
std::uint64_t g_shop_performance_frequency = 0;
std::uint64_t g_shop_entry_start = 0;
std::uint64_t g_shop_window_start = 0;
std::uint64_t g_shop_window_frames = 0;
bool g_shop_loop_started = false;

void trace_shop_frame_rate()
{
    if (!bedlam2_ps5_shop_room_active)
    {
        g_shop_entry_start = 0;
        g_shop_window_start = 0;
        g_shop_window_frames = 0;
        g_shop_loop_started = false;
        return;
    }
    if (!g_shop_performance_frequency)
    {
        g_shop_performance_frequency = SDL_GetPerformanceFrequency();
    }
    const std::uint64_t now = SDL_GetPerformanceCounter();
    if (!g_shop_performance_frequency || !now)
    {
        return;
    }
    if (!g_shop_entry_start)
    {
        g_shop_entry_start = now;
    }
    if (!g_shop_loop_started ||
        g_shop_frame_rate_diagnostics_emitted >= kMaxShopFrameRateDiagnostics)
    {
        return;
    }
    if (!g_shop_window_start)
    {
        g_shop_window_start = now;
        g_shop_window_frames = 0;
    }
    ++g_shop_window_frames;

    const std::uint64_t elapsed = now - g_shop_window_start;
    if (elapsed < g_shop_performance_frequency * 2)
    {
        return;
    }

    const double elapsed_seconds =
        static_cast<double>(elapsed) / g_shop_performance_frequency;
    const double fps = static_cast<double>(g_shop_window_frames) / elapsed_seconds;
    const double average_frame_ms = 1000.0 / fps;
    char message[160];
    SDL_snprintf(message, sizeof(message),
                 "Bedlam2 shop: %.1f fps average_frame_ms=%.2f frames=%llu\n",
                 fps, average_frame_ms,
                 static_cast<unsigned long long>(g_shop_window_frames));
    bedlam2_ps5_trace(message);
    ++g_shop_frame_rate_diagnostics_emitted;
    g_shop_window_start = now;
    g_shop_window_frames = 0;
}
} // namespace

extern "C" void bedlam2_ps5_shop_loop_started()
{
    if (!bedlam2_ps5_shop_room_active || g_shop_loop_started)
    {
        return;
    }
    if (!g_shop_performance_frequency)
    {
        g_shop_performance_frequency = SDL_GetPerformanceFrequency();
    }
    const std::uint64_t now = SDL_GetPerformanceCounter();
    if (!now || !g_shop_performance_frequency)
    {
        return;
    }
    if (!g_shop_entry_start)
    {
        g_shop_entry_start = now;
    }
    if (g_shop_setup_diagnostics_emitted < kMaxShopFrameRateDiagnostics)
    {
        const double setup_ms =
            1000.0 * static_cast<double>(now - g_shop_entry_start) /
            g_shop_performance_frequency;
        char message[128];
        SDL_snprintf(message, sizeof(message),
                     "Bedlam2 shop: setup_ms=%.2f before_selection_loop\n",
                     setup_ms);
        bedlam2_ps5_trace(message);
        ++g_shop_setup_diagnostics_emitted;
    }
    g_shop_loop_started = true;
    g_shop_window_start = now;
    g_shop_window_frames = 0;
}
#endif

Window::Window()
{
}

Window::~Window()
{
    // The renderer owns its SDL textures. Release our wrappers before SDL
    // destroys them, so the member destructors cannot use dangling handles.
    m_screen.destroy();
    m_map.destroy();
    m_sidebar.destroy();
    SDL_DestroyRenderer(m_renderer);
    m_renderer = nullptr;
    SDL_DestroyWindow(m_window);
    m_window = nullptr;
    BEDLAM2_FREE_ARRAY(SCREEN_BUFFER_PTR);
    BEDLAM2_FREE_ARRAY(SIDEBAR_BUFFER_PTR);
    BEDLAM2_FREE_ARRAY(MAP_BUFFER_PTR);
    BEDLAM2_FREE_ARRAY(GAME_SCREEN_PTR);
    SCREEN_BUFFER_PTR = nullptr;
    SIDEBAR_BUFFER_PTR = nullptr;
    MAP_BUFFER_PTR = nullptr;
    GAME_SCREEN_PTR = nullptr;
    SDL_VideoQuit();
}

int Window::init()
{
    int ret_val = 0;

    if (SDL_Init(SDL_INIT_VIDEO))
    {
        BEDLAM_LOG << "ERROR: init SDL video. " << SDL_GetError() << BEDLAM_ENDL;
        ret_val |= 1;
        return ret_val;
    }

    const int display_index = 0;
    SDL_DisplayMode DM;
    if (SDL_GetCurrentDisplayMode(display_index, &DM))
    {
        BEDLAM_LOG << "ERROR: get display mode. " << SDL_GetError() << BEDLAM_ENDL;
        ret_val |= 1;
    }
    const int monitor_width = DM.w;
    const int monitor_height = DM.h;

#ifdef _DEBUG
    const Resolution_settings &resolution_settings = m_options.get_resolution_settings(Resolution(800, 600));
#else
    const Resolution_settings &resolution_settings = m_options.get_resolution_settings(
        Resolution(monitor_width, monitor_height));
#endif
    m_game_width = resolution_settings.m_resolution.get_width();
    m_game_height = resolution_settings.m_resolution.get_height();
    m_window_width = m_game_width;
    m_window_height = m_game_height;

    if (m_window_width > monitor_width)
    {
        m_window_width = monitor_width;
    }
    if (m_window_height > monitor_height)
    {
        m_window_height = monitor_height;
    }

    int window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_SHOWN;
    if (m_window_width == monitor_width && m_window_height == monitor_height)
    {
        window_flags = SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_SHOWN;
    }
    m_window = SDL_CreateWindow(
        "Bedlam 2", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, m_window_width, m_window_height, window_flags);
    if (!m_window)
    {
        BEDLAM_LOG << "ERROR: created window. " << SDL_GetError() << BEDLAM_ENDL;
        ret_val |= 1;
        return ret_val;
    }

#ifdef BEDLAM2_PS5_NATIVE
    // The PS5 SDL backend has no hardware cursor. Start its software cursor
    // somewhere useful instead of at the backend's default (0, 0).
    SDL_WarpMouseInWindow(m_window, m_window_width / 2, m_window_height / 2);
#endif

    m_renderer = SDL_CreateRenderer(m_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);
    if (!m_renderer)
    {
        BEDLAM_LOG << "ERROR: created accelerated renderer is invalid. " << SDL_GetError() << BEDLAM_ENDL;
        BEDLAM_LOG << "Try to create software renderer... ";
        m_renderer = SDL_CreateRenderer(m_window, -1, SDL_RENDERER_SOFTWARE | SDL_RENDERER_TARGETTEXTURE);
        if (!m_renderer)
        {
            BEDLAM_LOG << "ERROR: created software renderer is invalid. " << SDL_GetError() << BEDLAM_ENDL;
            ret_val |= 1;
            return ret_val;
        }
        BEDLAM_LOG << "Ok!" << BEDLAM_ENDL;
    }

#ifdef BEDLAM2_PS5_NATIVE
    SDL_RendererInfo renderer_info{};
    SDL_GetRendererInfo(m_renderer, &renderer_info);
    char video_message[256];
    SDL_snprintf(video_message, sizeof(video_message),
                 "Bedlam2 video: display %dx%d, game %dx%d, renderer %s\n",
                 monitor_width, monitor_height, m_game_width, m_game_height,
                 renderer_info.name ? renderer_info.name : "unknown");
    bedlam2_ps5_trace(video_message);
#endif

    SDL_SetWindowMinimumSize(m_window, ORIGINAL_GAME_WIDTH, ORIGINAL_GAME_HEIGHT);
    if (!SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "2"))
    {
        if (!SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1"))
        {
            BEDLAM_LOG << "ERROR: Could't set render scale quality. " << SDL_GetError() << BEDLAM_ENDL;
            ret_val |= 1;
        }
    }

    ret_val |= SDL_SetRenderTarget(m_renderer, NULL);
    ret_val |= SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);
    ret_val |= clear_render();
    SDL_RenderPresent(m_renderer);

    if (MAP_BUFFER_PTR == nullptr)
    {
        MAP_BUFFER_PTR = BEDLAM2_ALLOC_ARRAY(uint8_t, ORIGINAL_GAME_WIDTH * ORIGINAL_GAME_HEIGHT);
        if (MAP_BUFFER_PTR == nullptr)
        {
            return ret_val | 1;
        }
    }
    ret_val |= m_map.init(MAP_BUFFER_PTR, ORIGINAL_GAME_WIDTH, ORIGINAL_GAME_HEIGHT, MAP_WIDTH, MAP_HEIGHT);

    if (SIDEBAR_BUFFER_PTR == nullptr)
    {
        SIDEBAR_BUFFER_PTR =
            BEDLAM2_ALLOC_ARRAY(uint8_t, ORIGINAL_GAME_WIDTH * ORIGINAL_GAME_HEIGHT);
        if (SIDEBAR_BUFFER_PTR == nullptr)
        {
            return ret_val | 1;
        }
    }
    SIDEBAR_START_POS_X = m_game_width - SIDEBAR_WIDTH;
    ret_val |= m_sidebar.init(
        SIDEBAR_BUFFER_PTR, ORIGINAL_GAME_WIDTH, ORIGINAL_GAME_HEIGHT, SIDEBAR_WIDTH, SIDEBAR_HEIGHT,
        ORIGINAL_GAME_WIDTH - SIDEBAR_WIDTH);

    File game_pal("GAMEGFX/GAMEPAL.PAL");
    ret_val |= m_sidebar.set_palette(game_pal);
    ret_val |= m_map.set_palette(game_pal);

    ret_val |= reinit_screen_data(m_game_width, m_game_height);

    ret_val |= update_game_position();
    ret_val |= update_sidebar_position();
    ret_val |= update_map_position();
    ret_val |= update_menu_position();

    ret_val |= m_tiles.init_vars(resolution_settings);

    if (ret_val)
    {
        BEDLAM_LOG << "ERROR: init window." << BEDLAM_ENDL;
    }

    return ret_val;
}

int Window::set_palette(uint8_t *pal_ptr, int offset, int num_entries)
{
    int ret_val = 0;
    ret_val |= m_screen.set_palette(pal_ptr, offset, num_entries);
    return ret_val;
}

int Window::clear_screen()
{
    int ret_val = 0;
    ret_val |= m_screen.clear();
    ret_val |= clear_render();
    ret_val |= redraw();
    return ret_val;
}

int Window::fill_screen_surface(uint8_t *buffer)
{
    int ret_val = 0;
    ret_val |= m_screen.fill_screen_surface(buffer);
    return ret_val;
}

int Window::unlock_screen_surface() const
{
    int ret_val = 0;
    ret_val |= m_screen.unlock_surface();
    return ret_val;
}

int Window::render_scene()
{
    int ret_val = 0;
    if (game_is_playing)
    {
        ret_val |= update_game_position();
        SDL_Texture *screen_texture = m_screen.get_texture();
        ret_val |= SDL_RenderCopy(
            m_renderer, screen_texture, m_game_pos.get_render_source(), m_game_pos.get_render_destination());
        ret_val |= SDL_RenderCopy(m_renderer, m_sidebar.get_texture(), NULL, m_sidebar_pos.get_render_destination());
        if (map_active)
        {
            ret_val |= SDL_RenderCopy(m_renderer, m_map.get_texture(), NULL, m_map_pos.get_render_destination());
        }
    }
    else
    {
        ret_val |= SDL_RenderCopy(
            m_renderer, m_screen.get_texture(), m_menu_pos.get_render_source(), m_menu_pos.get_render_destination());
    }
#ifdef BEDLAM2_PS5_NATIVE
    bedlam2_ps5_input_draw_overlay(m_renderer);
#endif
    return ret_val;
}

int Window::redraw()
{
    const bool death_screen = !game_is_playing && dead_screen_scale == 480;
    if (!death_screen)
    {
        DEATH_PALETTE_FINALIZED = 0;
    }

    int ret_val = 0;
    if (death_screen && !DEATH_PALETTE_FINALIZED)
    {
        // Game-over draws the indexed death image before installing its own
        // palette. Keep that intermediate frame hidden until the final palette
        // is ready, otherwise it flashes with the gameplay colors.
        ret_val |= clear_render();
        SDL_RenderPresent(m_renderer);
    }
    else
    {
        ret_val |= present_current_frame();
    }
#ifdef BEDLAM2_PS5_NATIVE
    trace_shop_frame_rate();
#endif
    SDL_events();
    return ret_val;
}

int Window::present_current_frame()
{
    int ret_val = render_scene();
    SDL_RenderPresent(m_renderer);
    return ret_val;
}

int Window::copy_screen_to_buffer(uint8_t *buffer_ptr)
{
    return m_screen.copy_surface_to_buffer(buffer_ptr);
}

uint8_t *Window::lock_and_get_surface_ptr()
{
    return m_screen.lock_and_get_surface_ptr();
}

uint8_t *Window::get_RGB_palette_ptr()
{
    return m_screen.get_RGB_palette_ptr();
}

int Window::get_window_height() const
{
    return m_window_height;
}

int Window::get_window_width() const
{
    return m_window_width;
}

int Window::get_game_height() const
{
    return m_game_height;
}

int Window::get_game_width() const
{
    return m_game_width;
}

int Window::draw_game_to_screen_buffer(uint8_t *game_screen_ptr, int32_t dead_scale)
{
    uint8_t *source_ptr;
    uint8_t *destination_ptr;
    if (dead_scale)
    {
        clear_render();
    }
    else
    {
        const int surf_width = m_screen.get_surface_width();
        source_ptr = &game_screen_ptr
                         [GAME_SCREEN_WIDTH * (((screen_y_pos & 31) + (screen_x_pos & 31u)) >> 1) // y
                          + 64 * GAME_SCREEN_WIDTH                                                // y offset (64 is tile size)
                          + 64                                                                    // x offset
                          + (((screen_x_pos & 31) - (screen_y_pos & 31) + 32) & 63)];             // x
        destination_ptr = m_screen.lock_and_get_surface_ptr();
        int screen_line = m_game_height;
        do
        {
            memcpy(destination_ptr, source_ptr, m_game_width);
            source_ptr += GAME_SCREEN_WIDTH;
            destination_ptr += surf_width;
            --screen_line;
        } while (screen_line);
    }
    return 0;
}

int Window::resize_window(int new_width, int new_height)
{
    int ret_val = 0;
    // size decreases
    if (new_height < m_window_height || new_width < m_window_width)
    {
        // keep aspect ratio
        if (new_width * m_window_height > new_height * m_window_width)
        {
            new_width = new_height * m_window_width / m_window_height;
        }
        else
        {
            new_height = new_width * m_window_height / m_window_width;
        }
    }
    // size increases
    else
    {
        // keep aspect ratio
        if (new_width * m_window_height > new_height * m_window_width)
        {
            new_height = new_width * m_window_height / m_window_width;
        }
        else
        {
            new_width = new_height * m_window_width / m_window_height;
        }
    }
    if (new_width < ORIGINAL_GAME_WIDTH || new_height < ORIGINAL_GAME_HEIGHT)
    {
        new_height = ORIGINAL_GAME_HEIGHT;
        new_width = ORIGINAL_GAME_WIDTH;
    }
    m_window_width = new_width;
    m_window_height = new_height;
    SDL_SetWindowSize(m_window, m_window_width, m_window_height);
    ret_val |= update_game_position();
    ret_val |= update_sidebar_position();
    ret_val |= update_map_position();
    ret_val |= update_menu_position();
    ret_val |= redraw();
    return ret_val;
}

int Window::reinit_screen_data(int new_width, int new_height)
{
    int ret_val = 0;
    m_game_width = new_width;
    m_game_height = new_height;

    ret_val |= m_screen.destroy();

    if (SCREEN_BUFFER_PTR)
    {
        BEDLAM2_FREE_ARRAY(SCREEN_BUFFER_PTR);
        SCREEN_BUFFER_PTR = nullptr;
    }
    SCREEN_BUFFER_PTR = BEDLAM2_ALLOC_ARRAY(uint8_t, m_game_width * m_game_height);
    if (SCREEN_BUFFER_PTR == nullptr)
    {
        return ret_val | 1;
    }

    SCREEN_SURFACE_WIDTH = m_game_width;
    SCREEN_SURFACE_HEIGHT = m_game_height;

    ret_val |= m_screen.init(SCREEN_BUFFER_PTR, m_game_width, m_game_height, m_game_width, m_game_height);
    m_screen_scale_mode_captured = false;
    m_screen_scale_mode_valid = false;
    m_zoom_nearest_filter_active = false;
    ret_val |= reinit_game_screen_buffer(m_game_width, m_game_height);
    return ret_val;
}

int Window::reinit_game_screen_buffer(int new_width, int new_height)
{
    // Additional tile width needs to avoid black holes
    GAME_SCREEN_WIDTH = new_width + TILE_WIDTH * 5;

    const int game_screen_height = new_height + TILE_HEIGHT * 20;
    // Additional tile height needs to avoid black holes and draw all Z levels in screen bottom
    GAME_SCREEN_SIZE = GAME_SCREEN_WIDTH * game_screen_height;

    // Limits to avoid copy sprite that exceeds screen size
    LIMIT_GAME_SCREEN_WIDTH = GAME_SCREEN_WIDTH - 2 * TILE_WIDTH;
    LIMIT_GAME_SCREEN_HEIGHT = game_screen_height - 2 * TILE_HEIGHT;
    if (GAME_SCREEN_PTR)
    {
        BEDLAM2_FREE_ARRAY(GAME_SCREEN_PTR);
        GAME_SCREEN_PTR = nullptr;
    }
    GAME_SCREEN_PTR = BEDLAM2_ALLOC_ARRAY(uint8_t, GAME_SCREEN_SIZE);
    if (GAME_SCREEN_PTR == nullptr)
    {
        return 1;
    }
    return 0;
}

int Window::set_window_pos(int pos_x, int pos_y)
{
    if (pos_x < 0 && pos_y < 0)
    {
        return -1;
    }
    SDL_SetWindowPosition(m_window, pos_x, pos_y);
    return 0;
}

int Window::set_window_pos_center()
{
    SDL_SetWindowPosition(m_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    return 0;
}

int Window::move_mouse_by(int delta_x, int delta_y)
{
    if (!m_window)
    {
        return -1;
    }
    int x = 0;
    int y = 0;
    SDL_GetMouseState(&x, &y);
    x = std::max(0, std::min(x + delta_x, m_window_width - 1));
    y = std::max(0, std::min(y + delta_y, m_window_height - 1));
    SDL_WarpMouseInWindow(m_window, x, y);
    return 0;
}

SDL_Renderer *Window::get_renderer()
{
    return m_renderer;
}

int Window::increase_viewport_scale()
{
    if (!game_is_playing)
    {
        return 0;
    }
    m_viewport_scale_x += m_screen.get_surface_width() >> 6;
    m_viewport_scale_y = m_viewport_scale_x * m_game_height / m_game_width;
    const int max_scale_x = m_game_width / 2;
    const int max_scale_y = m_game_height / 2;
    if (m_viewport_scale_x > max_scale_x || m_viewport_scale_y > max_scale_y)
    {
        m_viewport_scale_x = m_game_width / 2;
        m_viewport_scale_y = m_game_height / 2;
    }
    return update_game_position();
}

int Window::decrease_viewport_scale()
{
    if (!game_is_playing)
    {
        return 0;
    }
    m_viewport_scale_x -= m_screen.get_surface_width() >> 6;
    m_viewport_scale_y = m_viewport_scale_x * m_game_height / m_game_width;
    if (m_viewport_scale_x < 0 || m_viewport_scale_y < 0)
    {
        m_viewport_scale_x = 0;
        m_viewport_scale_y = 0;
    }
    return update_game_position();
}

int Window::cycle_hud_scale()
{
    if (m_hud_scale_percent == 100)
    {
        m_hud_scale_percent = 150;
    }
    else if (m_hud_scale_percent == 150)
    {
        m_hud_scale_percent = 200;
    }
    else
    {
        m_hud_scale_percent = 100;
    }
    update_sidebar_position();
    return m_hud_scale_percent;
}

int Window::clear_render()
{
    // Similar SDL_RenderClear()
    // Use to avoid SSE4 bug on virtual linux machine due to SDL_RenderClear()
    int ret_val = 0;
    ret_val |= SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 255);
    ret_val |= SDL_RenderFillRect(m_renderer, NULL);
    return ret_val;
}

int Window::update_map_position()
{
    int ret_val = 0;
    if (m_game_height > SIDEBAR_HEIGHT + MAP_HEIGHT)
    {
        // draw map bottom right
        ret_val |= m_map_pos.set_render_destination(
            (m_game_width - MAP_WIDTH) * m_window_width / m_game_width,
            (m_game_height - MAP_HEIGHT) * m_window_height / m_game_height, MAP_WIDTH * m_window_width / m_game_width,
            MAP_HEIGHT * m_window_height / m_game_height);
    }
    else
    {
        // draw map top left
        ret_val |= m_map_pos.set_render_destination(
            0, 0, MAP_WIDTH * m_window_width / m_game_width, MAP_HEIGHT * m_window_height / m_game_height);
    }
    return ret_val;
}

int Window::update_sidebar_position()
{
    const int width = SIDEBAR_WIDTH * m_window_width / m_game_width * m_hud_scale_percent / 100;
    const int height = SIDEBAR_HEIGHT * m_window_height / m_game_height * m_hud_scale_percent / 100;
    return m_sidebar_pos.set_render_destination(m_window_width - width, 0, width, height);
}

int Window::update_menu_position()
{
    int ret_val = 0;
    // scale original 640*480 resolution to user resolution and place in center
    const int menu_width = m_window_height * 4 / 3;
    const int menu_start_pos_x = (m_window_width - menu_width) / 2;
    ret_val |= m_menu_pos.set_render_source(0, 0, ORIGINAL_GAME_WIDTH, ORIGINAL_GAME_HEIGHT);
    ret_val |= m_menu_pos.set_render_destination(menu_start_pos_x, 0, menu_width, m_window_height);
    return ret_val;
}

int Window::update_game_position()
{
    int ret_val = 0;
#ifdef BEDLAM2_PS5_NATIVE
    SDL_Texture *screen_texture = m_screen.get_texture();
    if (screen_texture)
    {
        if (!m_screen_scale_mode_captured)
        {
            m_screen_scale_mode_captured = true;
            m_screen_scale_mode_valid =
                SDL_GetTextureScaleMode(screen_texture, &m_screen_unzoomed_scale_mode) == 0;
        }

        const bool zoomed = m_viewport_scale_x > 0 || m_viewport_scale_y > 0;
        if (m_screen_scale_mode_valid && zoomed != m_zoom_nearest_filter_active)
        {
            const SDL_ScaleMode scale_mode = zoomed
                                                 ? SDL_ScaleModeNearest
                                                 : m_screen_unzoomed_scale_mode;
            const int scale_result = SDL_SetTextureScaleMode(screen_texture, scale_mode);
            m_zoom_nearest_filter_active = zoomed;
            ret_val |= scale_result;

            if (g_zoom_filter_diagnostics_emitted < kMaxZoomFilterDiagnostics)
            {
                ++g_zoom_filter_diagnostics_emitted;
                char message[192];
                SDL_snprintf(message, sizeof(message),
                             "Bedlam2 video: zoom=%d scale_mode=%s result=%d source=%dx%d\n",
                             zoomed, zoomed ? "nearest" : "restored",
                             scale_result,
                             m_screen.get_surface_width() - m_viewport_scale_x,
                             m_screen.get_surface_height() - m_viewport_scale_y);
                bedlam2_ps5_trace(message);
            }
        }
    }
#endif
    const int viewport_start_x = 0;
    const int viewport_start_y = 0;

    const int max_scale_x = m_game_width / 2;
    // needs for always draw robot in one place when scaling
    const int scale_start_x = (m_viewport_scale_x - SIDEBAR_WIDTH / 2 * m_viewport_scale_x / max_scale_x) / 2;
    const int scale_start_y = m_viewport_scale_y / 2;

    const int viewport_width = m_screen.get_surface_width();
    const int viewport_height = m_screen.get_surface_height();
    const int scale_width = -m_viewport_scale_x;
    const int scale_height = -m_viewport_scale_y;

    ret_val |= m_game_pos.set_render_source(
        viewport_start_x + scale_start_x, viewport_start_y + scale_start_y, viewport_width + scale_width,
        viewport_height + scale_height);

    // max value of dead_screen_scale is 480, so we need to scale it to window res
    const int dead_screen_scale_x = dead_screen_scale * m_window_width / ORIGINAL_GAME_HEIGHT;
    const int dead_screen_scale_y = dead_screen_scale_x * m_window_height / m_window_width;
    ret_val |= m_game_pos.set_render_destination(
        dead_screen_scale_x / 2, dead_screen_scale_y / 2, m_window_width - dead_screen_scale_x,
        m_window_height - dead_screen_scale_y);
    return ret_val;
}
