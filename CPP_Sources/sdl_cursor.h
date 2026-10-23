#pragma once
#include <SDL.h>
#include <vector>

#include "bin_file.h"
#include "sdl_texture.h"

class cursor
{
public:
    cursor();
    ~cursor();

    int init();
    int hide_cursor();
    int show_cursor();
    int set_cursor_icon(int icon);
    int set_waiting_cursor();
    int update_waiting_cursor();
    int set_crosshair_cursor();
    int set_default_cursor();
    int get_cursor_img() const;
    SDL_Texture *get_crosshair_texture() const;
    int get_crosshair_width() const;
    int get_crosshair_height() const;
    void release_crosshair_texture();

private:
    int load_cursors(const BIN_File &general_bin, const File &pal_file);
    SDL_Cursor *load_cursor(const Texture &cursor_texture);

    SDL_Cursor *m_default = nullptr;
    SDL_Cursor *m_crosshair = nullptr;
    Texture m_crosshair_texture;
    std::vector<SDL_Cursor *> m_waiting_cursor;
    int m_waiting_timer = 0;
    bool m_cursor_is_waiting = false;
    int m_img = 0;
};
