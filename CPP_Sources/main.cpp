#include <SDL.h>
#include <cstdio>
#include "bedlam2_log.hpp"
#ifdef BEDLAM2_PS5_NATIVE
#include <unistd.h>
#include "native_data_path.hpp"
#endif

#include "bedlam2.h"
#include "main.h"
#include "mouse.h"
#include "sdl_sound.h"
#include "sdl_timer.h"
#include "sdl_window.h"
#ifdef BEDLAM2_PS5_NATIVE
#include "native_debug.hpp"
#include "native_input.h"
#include "native_sdl_memory.h"
#include "native_engine_memory.h"
#endif

int main(int argc, char *argv[])
{
#ifdef _DEBUG
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    atexit([] { system("PAUSE"); });
#endif

#ifdef BEDLAM2_PS5_NATIVE
    bedlam2_ps5_trace("Bedlam2 trace: main entry\n");
    if (bedlam2_ps5_sdl_memory_init() != 0)
    {
        bedlam2_ps5_trace("Bedlam2 trace: native SDL memory init failed\n");
        return 3;
    }
    if (bedlam2_ps5_init_storage() != 0)
    {
        bedlam2_ps5_trace("Bedlam2 trace: persistent storage init failed\n");
        if (SDL_Init(SDL_INIT_VIDEO) == 0)
        {
            SDL_ShowSimpleMessageBox(
                SDL_MESSAGEBOX_ERROR, "Bedlam 2",
                "Unable to create /data/Bedlam2PS5/content and /data/Bedlam2PS5/saves. "
                "Check that the PS5 data partition is available and writable.",
                nullptr);
            SDL_Quit();
        }
        return 4;
    }

    const char *game_data_dir = "/data/Bedlam2PS5/content";
    bool game_data_found = false;
    bool using_legacy_app_data = false;
    const char *game_data_dirs[] = {
        "/data/Bedlam2PS5/content",
        "/app0/assets/bed2",
        "/app0/bed2",
        "/app0",
    };
    for (const char *candidate : game_data_dirs)
    {
        char palette_path[512];
        bedlam2_ps5_set_data_root(candidate);
        if (bedlam2_ps5_resolve_data_path("GAMEGFX/GAMEPAL.PAL", palette_path,
                                          sizeof(palette_path)))
        {
            std::FILE *palette = std::fopen(palette_path, "rb");
            if (palette != nullptr)
            {
                std::fclose(palette);
                game_data_dir = candidate;
                using_legacy_app_data = candidate != game_data_dirs[0];
                game_data_found = true;
                break;
            }
        }
    }
    if (!game_data_found)
    {
        bedlam2_ps5_set_data_root("/data/Bedlam2PS5/content");
        bedlam2_ps5_trace("Bedlam2 trace: game data missing\n");
        if (SDL_Init(SDL_INIT_VIDEO) == 0)
        {
            SDL_ShowSimpleMessageBox(
                SDL_MESSAGEBOX_ERROR, "Bedlam 2",
                "Game data is missing. Copy the original files into "
                "/data/Bedlam2PS5/content (for example, GAMEGFX, MIRAGE, and SOUND). "
                "The saves folder is created at /data/Bedlam2PS5/saves.",
                nullptr);
            SDL_Quit();
        }
        return 4;
    }
    bedlam2_ps5_set_data_root(game_data_dir);
    if (using_legacy_app_data)
    {
        bedlam2_ps5_trace(
            "Bedlam2 trace: using legacy app-folder data; move it to /data/Bedlam2PS5/content before future app updates\n");
    }
    bedlam2_ps5_trace("Bedlam2 trace: game data ready\n");
#endif

    if (GAME_WINDOW.init())
    {
#ifdef BEDLAM2_PS5_NATIVE
        bedlam2_ps5_trace("Bedlam2 trace: video init failed\n");
#endif
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Problem", "Error init video!", NULL);
        return 1;
    }
#ifdef BEDLAM2_PS5_NATIVE
    bedlam2_ps5_trace("Bedlam2 trace: video init complete\n");
    bedlam2_ps5_input_init();
    bedlam2_ps5_trace("Bedlam2 trace: input init complete\n");
    if (bedlam2_ps5_engine_memory_init())
    {
        bedlam2_ps5_trace("Bedlam2 trace: engine memory init failed\n");
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Problem",
                                 "Error initializing game memory!", NULL);
        return 3;
    }
    bedlam2_ps5_trace("Bedlam2 trace: engine memory init complete\n");
    if (bedlam2_ps5_engine_stacks_init() != 0)
    {
        bedlam2_ps5_trace("Bedlam2 trace: engine stack init failed\n");
        return 3;
    }
    bedlam2_ps5_trace("Bedlam2 trace: engine stacks ready\n");
#endif
    if (WINDOW_CURSOR.init())
    {
#ifdef BEDLAM2_PS5_NATIVE
        bedlam2_ps5_trace("Bedlam2 trace: cursor init failed\n");
#endif
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Problem", "Error init cursor!", NULL);
    }
#ifdef BEDLAM2_PS5_NATIVE
    bedlam2_ps5_trace("Bedlam2 trace: cursor init complete\n");
#endif
    // timer changed to 9 for more smoothly performance
    if (GAME_TIMER.init(9))
    {
#ifdef BEDLAM2_PS5_NATIVE
        bedlam2_ps5_trace("Bedlam2 trace: timer init failed\n");
#endif
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Problem", "Error init timer!", NULL);
        return 2;
    }
#ifdef BEDLAM2_PS5_NATIVE
    bedlam2_ps5_trace("Bedlam2 trace: timer init complete\n");
#endif
    if (SOUND_SYSTEM.init())
    {
#ifdef BEDLAM2_PS5_NATIVE
        bedlam2_ps5_trace("Bedlam2 trace: audio init failed\n");
#endif
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Problem", "Error init sound!", NULL);
    }

    // Run bedlam 2 main function
#ifdef BEDLAM2_PS5_NATIVE
    bedlam2_ps5_trace("Bedlam2 trace: entering engine\n");
    // The recovered engine entry point still uses its original 32-bit register
    // and stack convention. The PS5 shim supplies that legacy call frame.
    const int result = bedlam2_native_main();
    bedlam2_ps5_trace("Bedlam2 trace: engine returned\n");
    return result;
#else
    return main_(argc, (const char**)argv, NULL);
#endif
}

void free_win()
{
#ifdef BEDLAM2_PS5_NATIVE
    bedlam2_ps5_input_shutdown();
#endif
    // Place all destroy and deinits here
}
