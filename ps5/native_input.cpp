#include "native_input.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>

#include "ps5port/overlay.hpp"

#include "bedlam2.h"
#include "bedlam2_draw.h"
#include "keyboard.h"
#include "mouse.h"
#include "native_input.h"
#include "sdl_event.h"
#include "sdl_window.h"
#include "native_debug.hpp"

extern "C"
{
int sceUserServiceGetForegroundUser(int *user_id);
int sceUserServiceGetLoginUserIdList(int user_ids[4]);
int sceUserServiceGetUserName(int user_id, char *name, std::size_t size);

volatile std::int32_t bedlam2_ps5_shop_room_active = 0;
volatile std::int32_t bedlam2_ps5_stop_movement_request = 0;
volatile std::int32_t bedlam2_ps5_escape_confirm_active = 0;
volatile std::int32_t bedlam2_ps5_escape_confirm_result = 0;
volatile std::int32_t bedlam2_ps5_pause_notice_active = 0;
}

namespace
{

constexpr Sint16 kStickDeadzone = 7000;
constexpr float kStickCursorMaxSpeedPixelsPerSecond = 360.0f;
constexpr float kShopCursorMaxSpeedPixelsPerSecond = 700.0f;
constexpr Uint32 kMaxMotionStepMs = 50;
constexpr Uint32 kMotionDiagnosticIntervalMs = 250;
constexpr unsigned int kMaxMotionDiagnostics = 32;
constexpr Uint32 kMovementCommandMinIntervalMs = 80;
constexpr Uint32 kMovementCommandMaxIntervalMs = 300;
constexpr int kMovementTargetMinRadius = 32;
constexpr int kMovementTargetMaxRadius = 96;
constexpr int kMenuFocusCenterX = 320;
constexpr int kMenuFocusRepeatMs = 220;
constexpr Sint16 kFireTriggerThreshold = 8000;
constexpr Uint32 kMovementDiagnosticIntervalMs = 250;
constexpr unsigned int kMaxMovementDiagnostics = 16;
constexpr unsigned int kMaxMovementStopDiagnostics = 8;
constexpr Uint32 kZoomRepeatIntervalMs = 150;
constexpr unsigned int kMaxZoomDiagnostics = 8;
constexpr int kGameplayAimRadius = 240;
constexpr unsigned int kMaxWeaponCycleDiagnostics = 12;
constexpr unsigned int kMaxControlActionDiagnostics = 32;

const char *const kHelpLines[] = {
    "BEDLAM 2", "LEFT STICK: MOVE ROBOT", "RIGHT STICK: AIM",
    "L1/R1: PREV/NEXT WEAPON", "D-PAD LEFT/RIGHT: ROBOT",
    "D-PAD UP/DOWN: MENU / ZOOM", "TRIANGLE: MAP", "SQUARE: CENTER VIEW",
    "TOUCHPAD: PAUSE", "R3: HUD SIZE", "OPTIONS: ESCAPE",
    "CROSS: SELECT/MOVE"};

void forward_overlay_key(const SDL_Event &event, void *)
{
    keyboard_events(event);
}

ps5port::OverlayConfig make_overlay_config()
{
    ps5port::OverlayConfig config;
    config.key_event = forward_overlay_key;
    config.help_lines = kHelpLines;
    config.help_line_count = sizeof(kHelpLines) / sizeof(kHelpLines[0]);
    return config;
}

ps5port::Overlay g_overlay(make_overlay_config());
SDL_GameController *g_controller = nullptr;
Uint32 g_last_update = 0;
Uint32 g_last_input_diagnostic = 0;
Uint32 g_last_motion_diagnostic = 0;
Uint32 g_last_movement_command = 0;
Uint32 g_last_movement_diagnostic = 0;
Uint32 g_last_zoom_adjustment = 0;
unsigned int g_motion_diagnostics_emitted = 0;
unsigned int g_movement_diagnostics_emitted = 0;
unsigned int g_movement_stop_diagnostics_emitted = 0;
unsigned int g_zoom_diagnostics_emitted = 0;
unsigned int g_weapon_cycle_diagnostics_emitted = 0;
unsigned int g_control_action_diagnostics_emitted = 0;
double g_motion_remainder_x = 0.0;
double g_motion_remainder_y = 0.0;
int g_gameplay_move_center_x = ORIGINAL_GAME_WIDTH / 2 - SIDEBAR_WIDTH / 2;
int g_gameplay_move_center_y = ORIGINAL_GAME_HEIGHT / 2;
bool g_was_gameplay = false;
bool g_was_weapon_shop = false;
bool g_left_stick_was_active = false;
bool g_zoom_in_down = false;
bool g_zoom_out_down = false;
bool g_triangle_down = false;
bool g_square_down = false;
bool g_touchpad_down = false;
bool g_options_down = false;
bool g_r3_down = false;
bool g_l1_down = false;
bool g_r1_down = false;
bool g_dpad_left_down = false;
bool g_dpad_right_down = false;
bool g_dialog_was_active = false;
bool g_dialog_left_down = false;
bool g_dialog_right_down = false;
bool g_dialog_cross_down = false;
bool g_dialog_circle_down = false;
bool g_dialog_options_down = false;
bool g_dialog_yes_selected = false;
bool g_menu_up_down = false;
bool g_menu_down_down = false;
bool g_menu_focus_initialized = false;
int g_menu_focus_index = 0;
int g_menu_focus_max_index = -1;
Uint32 g_last_menu_focus_step = 0;
bool g_left_click_down = false;
bool g_right_click_down = false;
bool g_controller_subsystem_started = false;

void close_controller()
{
    if (g_controller)
    {
        SDL_GameControllerClose(g_controller);
        g_controller = nullptr;
    }
}

void open_controller_if_available()
{
    if (g_controller && !SDL_GameControllerGetAttached(g_controller))
    {
        close_controller();
    }
    if (g_controller)
    {
        return;
    }

    const int joystick_count = SDL_NumJoysticks();
    for (int index = 0; index < joystick_count; ++index)
    {
        if (SDL_IsGameController(index))
        {
            g_controller = SDL_GameControllerOpen(index);
            if (g_controller)
            {
                char message[192];
                std::snprintf(message, sizeof(message),
                              "Bedlam2 input: opened controller index=%d name=%s attached=%d\n",
                              index, SDL_GameControllerName(g_controller),
                              SDL_GameControllerGetAttached(g_controller));
                bedlam2_ps5_trace(message);
                break;
            }
            char message[192];
            std::snprintf(message, sizeof(message),
                          "Bedlam2 input: controller open failed index=%d error=%s\n",
                          index, SDL_GetError());
            bedlam2_ps5_trace(message);
        }
    }
}

void trace_controller_state(Uint32 now)
{
    if (now - g_last_input_diagnostic < 2000)
        return;
    g_last_input_diagnostic = now;

    const int count = SDL_NumJoysticks();
    if (!g_controller)
    {
        char message[224];
        std::snprintf(message, sizeof(message),
                      "Bedlam2 input: poll controllers=%d opened=0 error=%s\n",
                      count, SDL_GetError());
        bedlam2_ps5_trace(message);
        return;
    }

    int mouse_x = 0;
    int mouse_y = 0;
    SDL_GetMouseState(&mouse_x, &mouse_y);
    char message[384];
    std::snprintf(
        message, sizeof(message),
        "Bedlam2 input: poll controllers=%d attached=%d shop=%d escape_dialog=%d pause_notice=%d lx=%d ly=%d rx=%d ry=%d dpad=%d%d%d%d cross=%d square=%d triangle=%d circle=%d L1/R1=%d%d touchpad=%d L3/R3=%d%d options=%d mouse=%d,%d game=%d,%d buttons=%d/%d\n",
        count, SDL_GameControllerGetAttached(g_controller),
        bedlam2_ps5_shop_room_active != 0,
        bedlam2_ps5_escape_confirm_active != 0,
        bedlam2_ps5_pause_notice_active != 0,
        SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_LEFTX),
        SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_LEFTY),
        SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_RIGHTX),
        SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_RIGHTY),
        SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_DPAD_UP),
        SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN),
        SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT),
        SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT),
        SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_A),
        SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_X),
        SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_Y),
        SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_B),
        SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_LEFTSHOULDER),
        SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER),
        SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_TOUCHPAD),
        SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_LEFTSTICK),
        SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_RIGHTSTICK),
        SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_START),
        mouse_x, mouse_y, CURSOR_POS_X, CURSOR_POS_Y, MOUSE_BUTTONS_STATE,
        MOUSE_BUTTONS_STATE1);
    bedlam2_ps5_trace(message);
}

Sint16 deadzone_axis(Sint16 value)
{
    return std::abs(static_cast<int>(value)) <= kStickDeadzone ? 0 : value;
}

double motion_for_axis(Sint16 axis, double max_speed)
{
    const Sint16 active_axis = deadzone_axis(axis);
    if (active_axis == 0)
    {
        return 0.0;
    }

    const double magnitude = static_cast<double>(std::abs(static_cast<int>(active_axis)) - kStickDeadzone) /
                             (32767.0 - kStickDeadzone);
    const double response = magnitude * magnitude;
    const double direction = active_axis < 0 ? -1.0 : 1.0;
    return direction * response * max_speed;
}

int accumulate_motion(double pixels_per_second, Uint32 elapsed_ms, double &remainder)
{
    remainder += pixels_per_second * elapsed_ms / 1000.0;
    const int applied = static_cast<int>(remainder);
    remainder -= applied;
    return applied;
}

void trace_applied_motion(int requested_x, int requested_y, int start_x,
                          int start_y, int radius, bool constrained)
{
    if (g_motion_diagnostics_emitted >= kMaxMotionDiagnostics)
    {
        return;
    }

    const Uint32 now = SDL_GetTicks();
    if (g_motion_diagnostics_emitted != 0 &&
        now - g_last_motion_diagnostic < kMotionDiagnosticIntervalMs)
    {
        return;
    }
    g_last_motion_diagnostic = now;
    ++g_motion_diagnostics_emitted;

    int game_x = 0;
    int game_y = 0;
    get_cursor_pos(&game_x, &game_y);

    char message[288];
    std::snprintf(message, sizeof(message),
                  "Bedlam2 input: aim window_delta=%d,%d game_delta=%d,%d cursor=%d,%d radius=%d clamped=%d remainder=%.3f,%.3f\n",
                  requested_x, requested_y, game_x - start_x, game_y - start_y,
                  game_x, game_y, radius, constrained,
                  g_motion_remainder_x, g_motion_remainder_y);
    bedlam2_ps5_trace(message);
}

void synchronize_game_cursor_position()
{
    int cursor_x = 0;
    int cursor_y = 0;
    get_cursor_pos(&cursor_x, &cursor_y);
    CURSOR_POS_X = cursor_x;
    CURSOR_POS_Y = cursor_y;
}

int gameplay_aim_radius()
{
    const int playfield_width = SIDEBAR_START_POS_X > 0
                                    ? SIDEBAR_START_POS_X
                                    : ORIGINAL_GAME_WIDTH - SIDEBAR_WIDTH;
    const int surface_width = GAME_WINDOW.get_game_width() > 0
                                  ? GAME_WINDOW.get_game_width()
                                  : static_cast<int>(SCREEN_SURFACE_WIDTH);
    const int surface_height = GAME_WINDOW.get_game_height() > 0
                                   ? GAME_WINDOW.get_game_height()
                                   : static_cast<int>(SCREEN_SURFACE_HEIGHT);
    const int left = std::max(0, std::min(g_gameplay_move_center_x,
                                          playfield_width - 1 - g_gameplay_move_center_x));
    const int right = std::max(0, std::min(g_gameplay_move_center_y,
                                           surface_height - 1 - g_gameplay_move_center_y));
    const int screen_limit = std::min(left, right);
    const int playfield_limit = std::max(0, std::min(g_gameplay_move_center_x,
                                                    surface_width - 1 - g_gameplay_move_center_x));
    const int screen_radius = std::max(0, std::min(screen_limit, playfield_limit));
    return std::min(kGameplayAimRadius, screen_radius);
}

void move_mouse_to_game_position(int game_x, int game_y)
{
    const int window_width = GAME_WINDOW.get_window_width();
    const int window_height = GAME_WINDOW.get_window_height();
    const int game_width = GAME_WINDOW.get_game_width();
    const int game_height = GAME_WINDOW.get_game_height();
    if (window_width <= 0 || window_height <= 0 || game_width <= 0 || game_height <= 0)
    {
        return;
    }

    const int target_x = std::max(0, std::min(window_width - 1,
                                             game_x * window_width / game_width));
    const int target_y = std::max(0, std::min(window_height - 1,
                                             game_y * window_height / game_height));
    int mouse_x = 0;
    int mouse_y = 0;
    SDL_GetMouseState(&mouse_x, &mouse_y);
    GAME_WINDOW.move_mouse_by(target_x - mouse_x, target_y - mouse_y);
    synchronize_game_cursor_position();
}

bool constrain_gameplay_aim_cursor()
{
    if (!game_is_playing)
    {
        return false;
    }

    const int radius = gameplay_aim_radius();

    const int offset_x = CURSOR_POS_X - g_gameplay_move_center_x;
    const int offset_y = CURSOR_POS_Y - g_gameplay_move_center_y;
    const double distance_squared = static_cast<double>(offset_x) * offset_x +
                                    static_cast<double>(offset_y) * offset_y;
    const double radius_squared = static_cast<double>(radius) * radius;
    if (distance_squared <= radius_squared)
    {
        return false;
    }

    const double scale = radius / std::sqrt(distance_squared);
    int target_x = g_gameplay_move_center_x +
                   static_cast<int>(std::lround(offset_x * scale));
    int target_y = g_gameplay_move_center_y +
                   static_cast<int>(std::lround(offset_y * scale));
    int target_offset_x = target_x - g_gameplay_move_center_x;
    int target_offset_y = target_y - g_gameplay_move_center_y;
    while (static_cast<double>(target_offset_x) * target_offset_x +
               static_cast<double>(target_offset_y) * target_offset_y > radius_squared)
    {
        if (std::abs(target_offset_x) >= std::abs(target_offset_y))
        {
            target_x -= target_offset_x > 0 ? 1 : -1;
        }
        else
        {
            target_y -= target_offset_y > 0 ? 1 : -1;
        }
        target_offset_x = target_x - g_gameplay_move_center_x;
        target_offset_y = target_y - g_gameplay_move_center_y;
    }

    move_mouse_to_game_position(target_x, target_y);
    return true;
}

void update_input_screen_transitions(bool gameplay, bool weapon_shop)
{
    if (gameplay && !g_was_gameplay)
    {
        // The active robot's on-screen aim origin follows the rendered
        // playfield dimensions and the configured vertical origin offset.
        // Movement destinations must use the same coordinates at every
        // supported resolution.
        const int playfield_width = SIDEBAR_START_POS_X > 0
                                        ? SIDEBAR_START_POS_X
                                        : ORIGINAL_GAME_WIDTH - SIDEBAR_WIDTH;
        const int surface_height = SCREEN_SURFACE_HEIGHT > 0
                                       ? static_cast<int>(SCREEN_SURFACE_HEIGHT)
                                       : ORIGINAL_GAME_HEIGHT;
        g_gameplay_move_center_x = playfield_width / 2;
        g_gameplay_move_center_y = surface_height / 2 - DISPLACE_ROBOT_ORIGIN_Y;
        g_motion_remainder_x = 0.0;
        g_motion_remainder_y = 0.0;
        g_last_movement_command = SDL_GetTicks() - kMovementCommandMaxIntervalMs;
    }
    if (weapon_shop && !g_was_weapon_shop)
    {
        g_motion_remainder_x = 0.0;
        g_motion_remainder_y = 0.0;
    }
    g_was_gameplay = gameplay;
    g_was_weapon_shop = weapon_shop;
}

void place_menu_focus_at_index(int index)
{
    const int menu_lines = *bedlam2_ps5_num_menu_lines;
    const int last_index = std::max(0, (menu_lines >> 16) & 0xffff);
    g_menu_focus_max_index = last_index;
    g_menu_focus_index = std::max(0, std::min(index, last_index));
    const int top = 350 - 24 * last_index;
    const int focus_y = top + g_menu_focus_index * 24 + 12;

    const int window_width = GAME_WINDOW.get_window_width();
    const int window_height = GAME_WINDOW.get_window_height();
    if (window_width <= 0 || window_height <= 0)
    {
        return;
    }

    const int menu_width = window_height * 4 / 3;
    const int menu_start_x = (window_width - menu_width) / 2;
    const int target_x = menu_start_x + kMenuFocusCenterX * menu_width / ORIGINAL_GAME_WIDTH;
    const int target_y = focus_y * window_height / ORIGINAL_GAME_HEIGHT;
    int mouse_x = 0;
    int mouse_y = 0;
    SDL_GetMouseState(&mouse_x, &mouse_y);
    GAME_WINDOW.move_mouse_by(target_x - mouse_x, target_y - mouse_y);
    CURSOR_POS_X = kMenuFocusCenterX;
    CURSOR_POS_Y = focus_y;
    g_menu_focus_initialized = true;
}

void update_menu_navigation(bool controller_attached, bool gameplay,
                            bool weapon_shop,
                            bool input_blocked, Uint32 now)
{
    const bool up_down = controller_attached &&
                         SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_DPAD_UP);
    const bool down_down = controller_attached &&
                           SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    const bool up_pressed = up_down && !g_menu_up_down;
    const bool down_pressed = down_down && !g_menu_down_down;
    const bool repeat_ready = now - g_last_menu_focus_step >= kMenuFocusRepeatMs;
    const bool up_step = up_pressed || (up_down && repeat_ready);
    const bool down_step = down_pressed || (down_down && repeat_ready);
    g_menu_up_down = up_down;
    g_menu_down_down = down_down;

    if (gameplay || weapon_shop || input_blocked || !controller_attached)
    {
        g_menu_focus_initialized = false;
        return;
    }

    const int menu_last_index = std::max(0, (*bedlam2_ps5_num_menu_lines >> 16) & 0xffff);
    if (!g_menu_focus_initialized || menu_last_index != g_menu_focus_max_index)
    {
        place_menu_focus_at_index(0);
        g_last_menu_focus_step = now;
    }
    if (up_step == down_step)
    {
        return;
    }

    place_menu_focus_at_index(g_menu_focus_index + (up_step ? -1 : 1));
    g_last_menu_focus_step = now;
}

void trace_movement_command(Sint16 axis_x, Sint16 axis_y)
{
    if (g_movement_diagnostics_emitted >= kMaxMovementDiagnostics)
    {
        return;
    }

    const Uint32 now = SDL_GetTicks();
    if (g_movement_diagnostics_emitted != 0 &&
        now - g_last_movement_diagnostic < kMovementDiagnosticIntervalMs)
    {
        return;
    }
    g_last_movement_diagnostic = now;
    ++g_movement_diagnostics_emitted;

    char message[192];
    std::snprintf(message, sizeof(message),
                  "Bedlam2 input: move order stick=%d,%d origin=%d,%d target=%d,%d surface=%ux%u\n",
                  axis_x, axis_y, g_gameplay_move_center_x, g_gameplay_move_center_y,
                  CURSOR_POS_LCLICK_X, CURSOR_POS_LCLICK_Y,
                  static_cast<unsigned int>(SCREEN_SURFACE_WIDTH),
                  static_cast<unsigned int>(SCREEN_SURFACE_HEIGHT));
    bedlam2_ps5_trace(message);
}

void issue_gameplay_movement_command(Sint16 axis_x, Sint16 axis_y, Uint32 now)
{
    const double normalized_x = static_cast<double>(axis_x) / 32767.0;
    const double normalized_y = static_cast<double>(axis_y) / 32767.0;
    const double raw_magnitude = std::sqrt(normalized_x * normalized_x + normalized_y * normalized_y);
    const double deadzone = static_cast<double>(kStickDeadzone) / 32767.0;
    if (raw_magnitude <= deadzone)
    {
        return;
    }

    const double magnitude = std::min((raw_magnitude - deadzone) / (1.0 - deadzone), 1.0);
    const Uint32 interval = kMovementCommandMaxIntervalMs - static_cast<Uint32>(
        magnitude * (kMovementCommandMaxIntervalMs - kMovementCommandMinIntervalMs));
    if (now - g_last_movement_command < interval)
    {
        return;
    }

    const int radius = kMovementTargetMinRadius + static_cast<int>(std::lround(
        magnitude * (kMovementTargetMaxRadius - kMovementTargetMinRadius)));
    CURSOR_POS_LCLICK_X = g_gameplay_move_center_x +
                          static_cast<int>(std::lround(normalized_x / raw_magnitude * radius));
    CURSOR_POS_LCLICK_Y = g_gameplay_move_center_y +
                          static_cast<int>(std::lround(normalized_y / raw_magnitude * radius));
    g_last_movement_command = now;
    trace_movement_command(axis_x, axis_y);
}

void request_gameplay_movement_stop()
{
    bedlam2_ps5_stop_movement_request = 1;
    if (g_movement_stop_diagnostics_emitted < kMaxMovementStopDiagnostics)
    {
        ++g_movement_stop_diagnostics_emitted;
        bedlam2_ps5_trace("Bedlam2 input: movement stop requested\n");
    }
}

void update_gameplay_movement_input(bool controller_attached, bool gameplay,
                                   bool input_blocked, Uint32 now)
{
    if (!gameplay)
    {
        g_left_stick_was_active = false;
        bedlam2_ps5_stop_movement_request = 0;
        return;
    }

    bool stick_active = false;
    Sint16 move_x = 0;
    Sint16 move_y = 0;
    if (controller_attached && !input_blocked)
    {
        move_x = SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_LEFTX);
        move_y = SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_LEFTY);
        const double normalized_x = static_cast<double>(move_x) / 32767.0;
        const double normalized_y = static_cast<double>(move_y) / 32767.0;
        const double raw_magnitude = std::sqrt(normalized_x * normalized_x +
                                               normalized_y * normalized_y);
        stick_active = raw_magnitude > static_cast<double>(kStickDeadzone) / 32767.0;
    }

    if (stick_active)
    {
        bedlam2_ps5_stop_movement_request = 0;
        issue_gameplay_movement_command(move_x, move_y, now);
        g_left_stick_was_active = true;
    }
    else if (g_left_stick_was_active)
    {
        request_gameplay_movement_stop();
        g_left_stick_was_active = false;
    }
}

void update_zoom_controls(bool controller_attached, bool gameplay, bool input_blocked)
{
    const bool zoom_in_down = controller_attached &&
                              SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_DPAD_UP);
    const bool zoom_out_down = controller_attached &&
                               SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    const Uint32 now = SDL_GetTicks();
    const bool zoom_repeat_ready = now - g_last_zoom_adjustment >= kZoomRepeatIntervalMs;
    const bool zoom_in_pressed = zoom_in_down && (!g_zoom_in_down || zoom_repeat_ready);
    const bool zoom_out_pressed = zoom_out_down && (!g_zoom_out_down || zoom_repeat_ready);
    g_zoom_in_down = zoom_in_down;
    g_zoom_out_down = zoom_out_down;

    if (!gameplay || input_blocked)
    {
        if (zoom_in_down || zoom_out_down)
        {
            g_last_zoom_adjustment = now;
        }
        return;
    }

    if (zoom_in_pressed)
    {
        GAME_WINDOW.increase_viewport_scale();
        g_last_zoom_adjustment = now;
    }
    else if (zoom_out_pressed)
    {
        GAME_WINDOW.decrease_viewport_scale();
        g_last_zoom_adjustment = now;
    }
    else
    {
        return;
    }

    if (g_zoom_diagnostics_emitted < kMaxZoomDiagnostics)
    {
        ++g_zoom_diagnostics_emitted;
        bedlam2_ps5_trace(zoom_in_pressed ? "Bedlam2 input: gameplay zoom in\n"
                                          : "Bedlam2 input: gameplay zoom out\n");
    }
}

void update_weapon_cycle(bool controller_attached, bool gameplay,
                         bool input_blocked)
{
    const bool l1_down = controller_attached &&
        SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    const bool r1_down = controller_attached &&
        SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    const bool l1_pressed = l1_down && !g_l1_down;
    const bool r1_pressed = r1_down && !g_r1_down;
    g_l1_down = l1_down;
    g_r1_down = r1_down;
    if ((!l1_pressed && !r1_pressed) || !gameplay || input_blocked)
    {
        return;
    }

    const int direction = r1_pressed ? 1 : -1;
    const int selected_slot = bedlam2_ps5_cycle_weapon_native(direction);
    if (g_weapon_cycle_diagnostics_emitted < kMaxWeaponCycleDiagnostics)
    {
        ++g_weapon_cycle_diagnostics_emitted;
        char message[128];
        if (selected_slot > 0)
        {
            std::snprintf(message, sizeof(message),
                          "Bedlam2 input: %s selected weapon slot=%d\n",
                          direction > 0 ? "R1" : "L1", selected_slot);
        }
        else
        {
            std::snprintf(message, sizeof(message),
                          "Bedlam2 input: %s found no loaded weapon\n",
                          direction > 0 ? "R1" : "L1");
        }
        bedlam2_ps5_trace(message);
    }
}

void trace_control_action(const char *action, int value = 0)
{
    if (g_control_action_diagnostics_emitted >= kMaxControlActionDiagnostics)
    {
        return;
    }
    ++g_control_action_diagnostics_emitted;
    char message[128];
    std::snprintf(message, sizeof(message),
                  "Bedlam2 input: action=%s value=%d cursor=%d,%d\n",
                  action, value, CURSOR_POS_X, CURSOR_POS_Y);
    bedlam2_ps5_trace(message);
}

void update_hud_scale_control(bool controller_attached, bool gameplay,
                              bool input_blocked)
{
    const bool r3_down = controller_attached &&
        SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    const bool r3_pressed = r3_down && !g_r3_down;
    g_r3_down = r3_down;
    if (!r3_pressed || !gameplay || input_blocked ||
        bedlam2_ps5_escape_confirm_active || bedlam2_ps5_pause_notice_active)
    {
        return;
    }

    trace_control_action("r3-hud-scale", GAME_WINDOW.cycle_hud_scale());
}

void tap_game_key(SDL_Keycode key)
{
    GAME_KEYBOARD.key_down(key);
    GAME_KEYBOARD.key_up(key);
}

void release_mouse_buttons();

bool handle_escape_dialog_input(bool controller_attached)
{
    if (!bedlam2_ps5_escape_confirm_active)
    {
        g_dialog_was_active = false;
        return false;
    }
    if (bedlam2_ps5_escape_confirm_result != 0)
    {
        return true;
    }

    const bool left_down = controller_attached &&
        SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    const bool right_down = controller_attached &&
        SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    const bool up_down = controller_attached &&
        SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_DPAD_UP);
    const bool down_down = controller_attached &&
        SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    const bool cross_down = controller_attached &&
        SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_A);
    const bool circle_down = controller_attached &&
        SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_B);
    const bool options_down = controller_attached &&
        SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_START);

    if (!g_dialog_was_active)
    {
        g_dialog_yes_selected = false;
        g_dialog_left_down = left_down;
        g_dialog_right_down = right_down;
        g_menu_up_down = up_down;
        g_menu_down_down = down_down;
        g_dialog_cross_down = cross_down;
        g_dialog_circle_down = circle_down;
        g_dialog_options_down = options_down;
        g_dialog_was_active = true;
        return true;
    }

    const bool left_pressed = left_down && !g_dialog_left_down;
    const bool right_pressed = right_down && !g_dialog_right_down;
    const bool up_pressed = up_down && !g_menu_up_down;
    const bool down_pressed = down_down && !g_menu_down_down;
    const bool cross_pressed = cross_down && !g_dialog_cross_down;
    const bool circle_pressed = circle_down && !g_dialog_circle_down;
    const bool options_pressed = options_down && !g_dialog_options_down;
    g_dialog_left_down = left_down;
    g_dialog_right_down = right_down;
    g_menu_up_down = up_down;
    g_menu_down_down = down_down;
    g_dialog_cross_down = cross_down;
    g_dialog_circle_down = circle_down;
    g_dialog_options_down = options_down;

    if (left_pressed || right_pressed || up_pressed || down_pressed)
    {
        g_dialog_yes_selected = !g_dialog_yes_selected;
    }
    if (cross_pressed)
    {
        bedlam2_ps5_escape_confirm_result = g_dialog_yes_selected ? 1 : 2;
        trace_control_action(g_dialog_yes_selected ? "escape-confirm-yes"
                                                   : "escape-confirm-no");
    }
    else if (circle_pressed || options_pressed)
    {
        bedlam2_ps5_escape_confirm_result = 2;
        trace_control_action("escape-confirm-no");
    }
    return true;
}

bool handle_pause_notice_input(bool controller_attached)
{
    if (!bedlam2_ps5_pause_notice_active)
    {
        return false;
    }

    const bool touchpad_down = controller_attached &&
        SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_TOUCHPAD);
    const bool touchpad_pressed = touchpad_down && !g_touchpad_down;
    g_touchpad_down = touchpad_down;
    if (touchpad_pressed)
    {
        tap_game_key(SDLK_p);
        trace_control_action("touchpad-resume");
    }
    release_mouse_buttons();
    return true;
}

void update_gameplay_robot_controls(bool controller_attached, bool robot_selectable,
                                    bool input_blocked)
{
    const bool left_down = controller_attached &&
        SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    const bool right_down = controller_attached &&
        SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    const bool left_pressed = left_down && !g_dpad_left_down;
    const bool right_pressed = right_down && !g_dpad_right_down;
    g_dpad_left_down = left_down;
    g_dpad_right_down = right_down;
    if ((!left_pressed && !right_pressed) || !robot_selectable || input_blocked)
    {
        return;
    }

    const int direction = right_pressed ? 1 : -1;
    const int selected_robot = bedlam2_ps5_cycle_robot_native(direction);
    trace_control_action(direction > 0 ? "dpad-right-robot" : "dpad-left-robot",
                         selected_robot);
}

void update_primary_button_actions(bool controller_attached, bool gameplay,
                                   bool input_blocked)
{
    const bool triangle_down = controller_attached &&
        SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_Y);
    const bool square_down = controller_attached &&
        SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_X);
    const bool touchpad_down = controller_attached &&
        SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_TOUCHPAD);
    const bool options_down = controller_attached &&
        SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_START);

    const bool triangle_pressed = triangle_down && !g_triangle_down;
    const bool square_pressed = square_down && !g_square_down;
    const bool touchpad_pressed = touchpad_down && !g_touchpad_down;
    const bool options_pressed = options_down && !g_options_down;
    g_triangle_down = triangle_down;
    g_square_down = square_down;
    g_touchpad_down = touchpad_down;
    g_options_down = options_down;

    if (options_pressed)
    {
        tap_game_key(SDLK_ESCAPE);
        trace_control_action("options-escape");
    }
    if (!gameplay || input_blocked)
    {
        return;
    }
    if (triangle_pressed)
    {
        tap_game_key(SDLK_m);
        trace_control_action("triangle-map", map_active);
    }
    if (square_pressed)
    {
        bedlam2_ps5_center_view_native();
        trace_control_action("square-center-view");
    }
    if (touchpad_pressed)
    {
        tap_game_key(SDLK_p);
        trace_control_action("touchpad-pause");
    }
}

void set_mouse_buttons(bool left_down, bool right_down)
{
    if (left_down != g_left_click_down)
    {
        mouse_buttons(0, left_down ? 0 : 1);
        g_left_click_down = left_down;
    }
    if (right_down != g_right_click_down)
    {
        mouse_buttons(1, right_down ? 0 : 1);
        g_right_click_down = right_down;
    }
}

void release_mouse_buttons()
{
    set_mouse_buttons(false, false);
}

std::uint8_t glyph_row(char character, int row)
{
    struct Glyph
    {
        char character;
        std::uint8_t rows[7];
    };
    static constexpr Glyph glyphs[] = {
        {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
        {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
        {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
        {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
        {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
        {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
        {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
        {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
        {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
        {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
        {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
        {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
        {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
        {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
        {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}},
        {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
        {'6', {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
        {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
        {'?', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}},
    };
    if (character == ' ' || row < 0 || row >= 7)
    {
        return 0;
    }
    for (const Glyph &glyph : glyphs)
    {
        if (glyph.character == character)
        {
            return glyph.rows[row];
        }
    }
    return 0;
}

void draw_pixel_text(SDL_Renderer *renderer, const char *text, int center_x,
                     int top_y, int scale, SDL_Color color)
{
    int length = 0;
    while (text[length] != '\0')
    {
        ++length;
    }
    const int text_width = std::max(0, (length * 6 - 1) * scale);
    int x = center_x - text_width / 2;
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    for (int index = 0; index < length; ++index)
    {
        const char character = text[index];
        for (int row = 0; row < 7; ++row)
        {
            const std::uint8_t bits = glyph_row(character, row);
            int column = 0;
            while (column < 5)
            {
                if ((bits & (1u << (4 - column))) == 0)
                {
                    ++column;
                    continue;
                }
                const int run_start = column;
                while (column < 5 && (bits & (1u << (4 - column))) != 0)
                {
                    ++column;
                }
                SDL_Rect pixel_run = {
                    x + run_start * scale,
                    top_y + row * scale,
                    (column - run_start) * scale,
                    scale};
                SDL_RenderFillRect(renderer, &pixel_run);
            }
        }
        x += 6 * scale;
    }
}

void draw_shadowed_pixel_text(SDL_Renderer *renderer, const char *text,
                              int center_x, int top_y, int scale,
                              int shadow_offset, SDL_Color color)
{
    const SDL_Color shadow = {0, 0, 0, 255};
    draw_pixel_text(renderer, text, center_x + shadow_offset,
                    top_y + shadow_offset, scale, shadow);
    draw_pixel_text(renderer, text, center_x, top_y, scale, color);
}

void draw_exit_text_notice(SDL_Renderer *renderer, int output_width,
                           int output_height, int scale)
{
    const int center_x = output_width / 2;
    const int center_y = output_height / 2;
    const int text_scale = scale * 2;
    const SDL_Color notice_white = {255, 255, 255, 255};
    const int title_y = center_y - 32 * scale;
    const int choice_y = center_y + 12 * scale;
    const int choice_offset = 44 * scale;
    draw_shadowed_pixel_text(renderer, "EXIT THIS LEVEL?", center_x,
                             title_y, text_scale, 2 * scale, notice_white);

    const int yes_center_x = center_x - choice_offset;
    const int no_center_x = center_x + choice_offset;
    draw_shadowed_pixel_text(renderer, "YES", yes_center_x, choice_y,
                             text_scale, 2 * scale, notice_white);
    draw_shadowed_pixel_text(renderer, "NO", no_center_x, choice_y,
                             text_scale, 2 * scale, notice_white);

    const int selected_center_x = g_dialog_yes_selected ? yes_center_x : no_center_x;
    const int selected_text_width = (g_dialog_yes_selected ? 17 : 11) * text_scale;
    SDL_Rect underline_shadow = {
        selected_center_x - selected_text_width / 2 + scale,
        choice_y + 8 * text_scale + scale,
        selected_text_width, scale};
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderFillRect(renderer, &underline_shadow);
    SDL_Rect underline = {underline_shadow.x - scale,
                          underline_shadow.y - scale,
                          underline_shadow.w, scale};
    SDL_SetRenderDrawColor(renderer, notice_white.r, notice_white.g,
                           notice_white.b, notice_white.a);
    SDL_RenderFillRect(renderer, &underline);
}

void draw_pause_notice(SDL_Renderer *renderer, int output_width,
                       int output_height, int scale)
{
    const int text_scale = scale * 3;
    const int top_y = (output_height - 7 * text_scale) / 2;
    const SDL_Color notice_white = {255, 255, 255, 255};
    draw_shadowed_pixel_text(renderer, "PAUSED", output_width / 2, top_y,
                             text_scale, 2 * scale, notice_white);
}

void draw_centered_notice(SDL_Renderer *renderer)
{
    if (!bedlam2_ps5_escape_confirm_active && !bedlam2_ps5_pause_notice_active)
    {
        return;
    }
    int output_width = 0;
    int output_height = 0;
    SDL_GetRendererOutputSize(renderer, &output_width, &output_height);
    if (output_width <= 0 || output_height <= 0)
    {
        return;
    }
    const int scale = std::max(1, std::min(output_width / 640, output_height / 480));
    if (bedlam2_ps5_escape_confirm_active)
    {
        draw_exit_text_notice(renderer, output_width, output_height, scale);
    }
    else if (bedlam2_ps5_pause_notice_active)
    {
        draw_pause_notice(renderer, output_width, output_height, scale);
    }
}

} // namespace

extern "C" void bedlam2_ps5_fill_player_name(char *player_name,
                                               std::size_t capacity)
{
    if (player_name == nullptr || capacity < 7 ||
        (player_name[0] != '\0' && std::strncmp(player_name, "Player", 7) != 0))
    {
        return;
    }

    int user_id = -1;
    if (sceUserServiceGetForegroundUser(&user_id) != 0 || user_id < 0)
    {
        int user_ids[4] = {-1, -1, -1, -1};
        if (sceUserServiceGetLoginUserIdList(user_ids) != 0)
        {
            return;
        }
        for (const int candidate : user_ids)
        {
            if (candidate >= 0)
            {
                user_id = candidate;
                break;
            }
        }
    }
    if (user_id < 0)
    {
        return;
    }

    char profile_name[128] = {};
    if (sceUserServiceGetUserName(user_id, profile_name, sizeof(profile_name)) != 0)
    {
        return;
    }

    char sanitized[9] = {};
    std::size_t length = 0;
    for (const unsigned char *character =
             reinterpret_cast<const unsigned char *>(profile_name);
         *character != '\0' && length < sizeof(sanitized) - 1; ++character)
    {
        const bool letter = (*character >= 'A' && *character <= 'Z') ||
                            (*character >= 'a' && *character <= 'z');
        const bool digit = *character >= '0' && *character <= '9';
        const bool punctuation = *character == ' ' || *character == '_' ||
                                 *character == '-';
        if (letter || digit || punctuation)
        {
            if (*character != ' ' || (length > 0 && sanitized[length - 1] != ' '))
            {
                sanitized[length++] = static_cast<char>(*character);
            }
        }
    }
    while (length > 0 && sanitized[length - 1] == ' ')
    {
        --length;
    }
    if (length == 0)
    {
        return;
    }

    std::memset(player_name, 0, capacity);
    std::memcpy(player_name, sanitized, length);
}

void bedlam2_ps5_input_init()
{
    const int init_status = SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    if (init_status == 0)
    {
        g_controller_subsystem_started = true;
        SDL_GameControllerEventState(SDL_ENABLE);
        open_controller_if_available();
    }
    char message[224];
    std::snprintf(message, sizeof(message),
                  "Bedlam2 input: init status=%d joysticks=%d error=%s\n",
                  init_status, SDL_NumJoysticks(), SDL_GetError());
    bedlam2_ps5_trace(message);
    g_last_update = SDL_GetTicks();
    g_overlay.reset();
    bedlam2_ps5_escape_confirm_active = 0;
    bedlam2_ps5_escape_confirm_result = 0;
    bedlam2_ps5_pause_notice_active = 0;
    g_dialog_was_active = false;
    g_dialog_yes_selected = false;
    bedlam2_ps5_stop_movement_request = 0;
    g_last_input_diagnostic = g_last_update - 2000;
    g_last_movement_command = g_last_update - kMovementCommandMaxIntervalMs;
}

void bedlam2_ps5_input_update()
{
    if (!g_controller_subsystem_started)
    {
        return;
    }

    SDL_GameControllerUpdate();
    open_controller_if_available();
    const Uint32 now = SDL_GetTicks();
    const Uint32 elapsed = std::min(now - g_last_update, kMaxMotionStepMs);
    g_last_update = now;
    const bool gameplay = game_is_playing != 0;
    const bool weapon_shop = bedlam2_ps5_shop_room_active != 0;
    update_input_screen_transitions(gameplay, weapon_shop);
    synchronize_game_cursor_position();
    trace_controller_state(now);
    const bool controller_attached =
        g_controller && SDL_GameControllerGetAttached(g_controller);
    // The in-game confirmation and pause dialogs use the controls directly.
    // Keep the Support Kit overlay from consuming controller buttons.
    g_overlay.update_buttons(0, false, false, now, controller_attached);
    const bool input_blocked = g_overlay.blocks_input();
    update_hud_scale_control(controller_attached, gameplay, input_blocked);
    if (handle_escape_dialog_input(controller_attached))
    {
        release_mouse_buttons();
        return;
    }
    if (handle_pause_notice_input(controller_attached))
    {
        return;
    }
    update_zoom_controls(controller_attached, gameplay, input_blocked);
    update_weapon_cycle(controller_attached, gameplay, input_blocked);
    update_gameplay_robot_controls(controller_attached, gameplay || weapon_shop,
                                   input_blocked);
    update_primary_button_actions(controller_attached, gameplay, input_blocked);
    update_menu_navigation(controller_attached, gameplay, weapon_shop,
                           input_blocked, now);
    update_gameplay_movement_input(controller_attached, gameplay,
                                   input_blocked, now);

    if (!controller_attached)
    {
        release_mouse_buttons();
        return;
    }

    if (g_overlay.blocks_input())
    {
        release_mouse_buttons();
        return;
    }

    if (gameplay || weapon_shop)
    {
        Sint16 axis_x = SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_RIGHTX);
        Sint16 axis_y = SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_RIGHTY);
        const int start_x = CURSOR_POS_X;
        const int start_y = CURSOR_POS_Y;
        const double max_speed = weapon_shop ? kShopCursorMaxSpeedPixelsPerSecond
                                             : kStickCursorMaxSpeedPixelsPerSecond;
        const int dx = accumulate_motion(motion_for_axis(axis_x, max_speed), elapsed,
                                          g_motion_remainder_x);
        const int dy = accumulate_motion(motion_for_axis(axis_y, max_speed), elapsed,
                                          g_motion_remainder_y);
        if (dx || dy)
        {
            GAME_WINDOW.move_mouse_by(dx, dy);
            synchronize_game_cursor_position();
        }
        const bool constrained = gameplay && constrain_gameplay_aim_cursor();
        if (dx || dy || constrained)
        {
            trace_applied_motion(dx, dy, start_x, start_y,
                                 gameplay ? gameplay_aim_radius() : 0, constrained);
        }
    }

    const bool cross = SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_A);
    const bool circle = SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_B);
    const bool fire_trigger = SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >
                              kFireTriggerThreshold;
    set_mouse_buttons(cross, circle || fire_trigger);
}

void bedlam2_ps5_input_draw_overlay(SDL_Renderer *renderer)
{
    if (!renderer)
    {
        return;
    }
    // The PS5 SDL backend cannot display SDL_Cursor objects, so draw the
    // game's own crosshair texture over gameplay and the weapon shop.
    const bool weapon_shop = bedlam2_ps5_shop_room_active != 0;
    if (SDL_ShowCursor(SDL_QUERY) != SDL_ENABLE || (!game_is_playing && !weapon_shop))
    {
        draw_centered_notice(renderer);
        return;
    }

    int mouse_x = 0;
    int mouse_y = 0;
    const int window_width = GAME_WINDOW.get_window_width();
    const int window_height = GAME_WINDOW.get_window_height();
    if (window_width > 0 && window_height > 0)
    {
        if (game_is_playing)
        {
            const int game_width = GAME_WINDOW.get_game_width();
            const int game_height = GAME_WINDOW.get_game_height();
            if (game_width > 0 && game_height > 0)
            {
                mouse_x = CURSOR_POS_X * window_width / game_width;
                mouse_y = CURSOR_POS_Y * window_height / game_height;
            }
        }
        else
        {
            const int menu_width = window_height * 4 / 3;
            const int menu_start_pos_x = (window_width - menu_width) / 2;
            mouse_x = menu_start_pos_x + CURSOR_POS_X * menu_width / ORIGINAL_GAME_WIDTH;
            mouse_y = CURSOR_POS_Y * window_height / ORIGINAL_GAME_HEIGHT;
        }
    }
    int output_width = 0;
    int output_height = 0;
    SDL_GetRendererOutputSize(renderer, &output_width, &output_height);
    if (window_width > 0 && window_height > 0 && output_width > 0 && output_height > 0)
    {
        mouse_x = mouse_x * output_width / window_width;
        mouse_y = mouse_y * output_height / window_height;
    }

    const bool gameplay_crosshair =
        game_is_playing && CURSOR_POS_X < SIDEBAR_START_POS_X;
    if ((gameplay_crosshair || weapon_shop) &&
        output_width > 0 && output_height > 0)
    {
        SDL_Texture *crosshair = WINDOW_CURSOR.get_crosshair_texture();
        const int game_width = GAME_WINDOW.get_game_width();
        const int game_height = GAME_WINDOW.get_game_height();
        const int texture_width = WINDOW_CURSOR.get_crosshair_width();
        const int texture_height = WINDOW_CURSOR.get_crosshair_height();
        if (crosshair && game_width > 0 && game_height > 0 &&
            window_width > 0 && window_height > 0 &&
            texture_width > 0 && texture_height > 0)
        {
            constexpr int kCrosshairScaleNumerator = 3;
            constexpr int kCrosshairScaleDenominator = 2;
            int draw_width = 0;
            int draw_height = 0;
            if (weapon_shop && !game_is_playing)
            {
                const int menu_width = window_height * 4 / 3;
                const int menu_output_width =
                    menu_width * output_width / window_width;
                draw_width = texture_width * kCrosshairScaleNumerator *
                             menu_output_width /
                             (kCrosshairScaleDenominator * ORIGINAL_GAME_WIDTH);
                draw_height = texture_height * kCrosshairScaleNumerator *
                              output_height /
                              (kCrosshairScaleDenominator * ORIGINAL_GAME_HEIGHT);
            }
            else
            {
                draw_width = texture_width * kCrosshairScaleNumerator *
                             output_width /
                             (kCrosshairScaleDenominator * game_width);
                draw_height = texture_height * kCrosshairScaleNumerator *
                              output_height /
                              (kCrosshairScaleDenominator * game_height);
            }
            SDL_Rect destination = {
                mouse_x - draw_width / 2,
                mouse_y - draw_height / 2,
                draw_width,
                draw_height};

            Uint8 previous_red = 255;
            Uint8 previous_green = 255;
            Uint8 previous_blue = 255;
            Uint8 previous_alpha = 255;
            SDL_GetTextureColorMod(crosshair, &previous_red,
                                   &previous_green, &previous_blue);
            SDL_GetTextureAlphaMod(crosshair, &previous_alpha);
            SDL_SetTextureColorMod(crosshair, 0, 0, 0);
            SDL_SetTextureAlphaMod(crosshair, 192);
            SDL_Rect shadow = destination;
            shadow.x += std::max(2, draw_width / 24);
            shadow.y += std::max(2, draw_height / 24);
            SDL_RenderCopy(renderer, crosshair, nullptr, &shadow);
            SDL_SetTextureColorMod(crosshair, previous_red, previous_green,
                                   previous_blue);
            SDL_SetTextureAlphaMod(crosshair, previous_alpha);
            SDL_RenderCopy(renderer, crosshair, nullptr, &destination);
            draw_centered_notice(renderer);
            return;
        }
    }

    SDL_Point outline[] = {
        {mouse_x, mouse_y}, {mouse_x, mouse_y + 16}, {mouse_x + 4, mouse_y + 12},
        {mouse_x + 7, mouse_y + 18}, {mouse_x + 10, mouse_y + 17},
        {mouse_x + 7, mouse_y + 11}, {mouse_x + 13, mouse_y + 11},
        {mouse_x, mouse_y}};
    SDL_Point fill[] = {
        {mouse_x + 1, mouse_y + 2}, {mouse_x + 1, mouse_y + 13},
        {mouse_x + 4, mouse_y + 10}, {mouse_x + 8, mouse_y + 16},
        {mouse_x + 9, mouse_y + 15}, {mouse_x + 6, mouse_y + 9},
        {mouse_x + 10, mouse_y + 10}, {mouse_x + 1, mouse_y + 2}};
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderDrawLines(renderer, outline, sizeof(outline) / sizeof(outline[0]));
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderDrawLines(renderer, fill, sizeof(fill) / sizeof(fill[0]));
    draw_centered_notice(renderer);
}

void bedlam2_ps5_input_shutdown()
{
    release_mouse_buttons();
    g_overlay.reset();
    WINDOW_CURSOR.release_crosshair_texture();
    close_controller();
    if (g_controller_subsystem_started)
    {
        SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
        g_controller_subsystem_started = false;
    }
}

extern "C" int bedlam2_ps5_keyboard_status() noexcept
{
    if (PRESSED_KEY_ARR == nullptr)
    {
        return 0;
    }
    for (int scan_code = 1; scan_code <= 256; ++scan_code)
    {
        if (PRESSED_KEY_ARR[scan_code] != 0)
        {
            return 0xFF;
        }
    }
    return 0;
}
