#pragma once

struct WorldHUDAssets
{
    int flag_red_d;
    int flag_blue_d;
    int flag_red_a;
    int flag_blue_a;
};

struct WorldHUDRender
{
    static constexpr float base_scale = 1.0f;
    static constexpr float reference_distance = 17.5f;
    static constexpr float min_scale = 0.5f;
    static constexpr float max_scale = 2.0f;
    static constexpr float ctf_flag_offset = 1.75f;
    static constexpr float fog_dist_multi = 0.85f;
    static constexpr float fog_dist_min = 5.0f;
    static constexpr float fog_dist_max = 100.0f;
};

void hud_world_do_frame();
void load_world_hud_assets();
