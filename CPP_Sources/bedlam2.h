#pragma once
#include <cstdint>
////////////////////////////////////////////////////
// Here function prototypes from bedlam2.asm file //
// Here extern variables from bedlam2.asm file    //
////////////////////////////////////////////////////

extern "C" {
#ifdef BEDLAM2_PS5_NATIVE
int bedlam2_native_main();
void bedlam2_native_level_clock();
int bedlam2_ps5_engine_memory_init();
#else
int main_(int, const char **, const char **);
void level_clock();
#endif
}

#ifdef BEDLAM2_PS5_NATIVE
extern "C" {
extern volatile int32_t *bedlam2_ps5_screen_x_pos;
extern volatile int32_t *bedlam2_ps5_screen_y_pos;
extern volatile int32_t *bedlam2_ps5_game_is_playing;
extern volatile int32_t *bedlam2_ps5_map_active;
extern volatile int32_t *bedlam2_ps5_dead_screen_scale;
extern volatile int32_t *bedlam2_ps5_num_menu_lines;
}
#define screen_x_pos (*bedlam2_ps5_screen_x_pos)
#define screen_y_pos (*bedlam2_ps5_screen_y_pos)
#define game_is_playing (*bedlam2_ps5_game_is_playing)
#define map_active (*bedlam2_ps5_map_active)
#define dead_screen_scale (*bedlam2_ps5_dead_screen_scale)
#else
extern "C" volatile int32_t screen_x_pos;
extern "C" volatile int32_t screen_y_pos;
extern "C" volatile int32_t game_is_playing;
extern "C" volatile int32_t map_active;
extern "C" volatile int32_t dead_screen_scale;
#endif
