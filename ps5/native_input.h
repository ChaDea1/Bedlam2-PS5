#pragma once

#include <SDL.h>
#include <cstdint>

extern "C" volatile std::int32_t bedlam2_ps5_shop_room_active;

void bedlam2_ps5_input_init();
void bedlam2_ps5_input_update();
void bedlam2_ps5_input_draw_overlay(SDL_Renderer *renderer);
void bedlam2_ps5_input_shutdown();

// Selects the next (direction > 0) or previous (direction < 0) loaded weapon
// for the active robot. Returns its 1-based slot, or -1 when none are loaded.
extern "C" int bedlam2_ps5_cycle_weapon_native(int direction);
// Selects the next/previous living robot in the player's squad. Returns its
// 1-based slot, or -1 when no living squad member is available.
extern "C" int bedlam2_ps5_cycle_robot_native(int direction);
// Makes the active robot the camera tracking target.
extern "C" void bedlam2_ps5_center_view_native();
