#include "tiles.h"
#include "bedlam2_draw.h"
#ifdef BEDLAM2_PS5_NATIVE
#include "native_engine_memory.h"
#define BEDLAM2_ALLOC_ARRAY(type, count) \
    bedlam2::native_memory::allocate_zeroed_array<type>(count)
#define BEDLAM2_FREE_ARRAY(memory) bedlam2::native_memory::release_array(memory)
#else
#define BEDLAM2_ALLOC_ARRAY(type, count) new type[count]()
#define BEDLAM2_FREE_ARRAY(memory) delete[] memory
#endif

Tiles::Tiles()
{
}

Tiles::~Tiles()
{
    BEDLAM2_FREE_ARRAY(Z_BUFFER_PTR);
    Z_BUFFER_PTR = nullptr;
}

int Tiles::init_vars(const Resolution_settings &settings)
{
    NUM_TILES_X = settings.m_num_tiles_x;
    NUM_TILES_Y = settings.m_num_tiles_y;
    TILES_START_POS_X = settings.m_tiles_start_pos_x;
    TILES_START_POS_Y = settings.m_tiles_start_pos_y;

    VETRICAL_TILE_LEVEL_OFFSET = TILES_START_POS_Y / TILE_HEIGHT + 9;

    NOT_VISIBLE_TILES_X = settings.m_not_visible_tiles_x;
    NOT_VISIBLE_TILES_Y = settings.m_not_visible_tiles_y;

    DISPLACE_SCREEN_X = (ORIGINAL_GAME_WIDTH - settings.m_resolution.get_width()) / 2;
    DISPLACE_SCREEN_Y = 0;

    DISPLACE_ROBOT_ORIGIN_Y = settings.m_displace_origin_y;

    if (Z_BUFFER_PTR)
    {
        BEDLAM2_FREE_ARRAY(Z_BUFFER_PTR);
        Z_BUFFER_PTR = nullptr;
    }

    uint32_t max_z_levels = 8;
    Z_BUFFER_PTR =
        BEDLAM2_ALLOC_ARRAY(uint32_t, NUM_TILES_X * NUM_TILES_Y * max_z_levels);
    if (Z_BUFFER_PTR == nullptr)
    {
        return 1;
    }
    return 0;
}
