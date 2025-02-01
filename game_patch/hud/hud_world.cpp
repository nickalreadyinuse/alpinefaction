#include "hud_internal.h"
#include "hud_world.h"
#include "../rf/hud.h"
#include "../rf/player/player.h"
#include "../rf/player/camera.h"
#include "../rf/entity.h"
#include "../rf/bmpman.h"
#include "../rf/multi.h"
#include "../rf/gameseq.h"
#include "../rf/level.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/localize.h"
#include "../os/console.h"
#include <patch_common/FunHook.h>
#include <patch_common/MemUtils.h>
#include <algorithm>
#include <xlog/xlog.h>

WorldHUDAssets g_world_hud_assets;

void load_world_hud_assets() {
    g_world_hud_assets.flag_red_d = rf::bm::load("af_wh_ctf_red_d.tga", -1, true);
    g_world_hud_assets.flag_blue_d = rf::bm::load("af_wh_ctf_blue_d.tga", -1, true);
    g_world_hud_assets.flag_red_a = rf::bm::load("af_wh_ctf_red_a.tga", -1, true);
    g_world_hud_assets.flag_blue_a = rf::bm::load("af_wh_ctf_blue_a.tga", -1, true);
}

void do_render_world_hud_sprite(rf::Vector3 pos, float base_scale, int bitmap_handle,
    WorldHUDRenderMode render_mode, bool stay_inside_fog, bool distance_scaling, bool only_draw_during_gameplay) {

    if (only_draw_during_gameplay && rf::gameseq_get_state() != rf::GameState::GS_GAMEPLAY) {
        return;
    }

    auto vec = pos;
    auto scale = base_scale;
    auto bitmap_mode = rf::gr::bitmap_3d_mode_no_z; // default (no_overdraw)

    // handle render mode
    switch (render_mode) {
        case WorldHUDRenderMode::no_overdraw:
            bitmap_mode = rf::gr::bitmap_3d_mode_no_z;
            break;
        case WorldHUDRenderMode::no_overdraw_glow:
            bitmap_mode = rf::gr::glow_3d_bitmap_mode;
            break;
        case WorldHUDRenderMode::overdraw:
            bitmap_mode = rf::gr::bitmap_3d_mode;
            break;
        default:
            // Fallback to default
            break;
    }

    // handle distance scaling and fog distance adjustment
    if (stay_inside_fog || distance_scaling) {
        rf::Camera* camera = rf::local_player->cam;
        rf::Vector3 camera_pos = rf::camera_get_pos(camera);
        float distance = vec.distance_to(camera_pos);

        // If the icon would be clipped due to being beyond the fog far clip, draw it just inside instead
        if (stay_inside_fog) {
            float fog_far_clip = rf::level.distance_fog_far_clip;

            // enforce min and max effective fog distance
            float max_distance = fog_far_clip > WorldHUDRender::fog_dist_min
                                     ? std::min(fog_far_clip, WorldHUDRender::fog_dist_max)
                                     : WorldHUDRender::fog_dist_max;

            // adjust sprite position
            if (distance > max_distance * WorldHUDRender::fog_dist_multi) {
                rf::Vector3 direction = vec - camera_pos;
                direction.normalize_safe();

                vec = camera_pos + (direction * (max_distance * WorldHUDRender::fog_dist_multi));
                distance = max_distance * WorldHUDRender::fog_dist_multi;
            }
        }

        // Scale icon based on distance from camera
        if (distance_scaling) {
            float scale_factor = std::max(distance, 1.0f) / WorldHUDRender::reference_distance;
            scale = std::clamp(base_scale * scale_factor, WorldHUDRender::min_scale, WorldHUDRender::max_scale);
        }
    }

    // draw sprite
    rf::gr::set_texture(bitmap_handle, -1);
    rf::gr::gr_3d_bitmap_angle(&vec, 0.0f, scale, bitmap_mode);
}

// draw CTF flag sprites
void build_ctf_flag_icons()
{
    if (!rf::ctf_red_flag_item || !rf::ctf_blue_flag_item) {
        return; // don't render unless map has both flags
    }

    bool team = rf::local_player->team;

    auto build_and_render_flag_icon = [&](bool player_team, bool flag_team) {

        rf::Vector3 vec = flag_team ? rf::ctf_blue_flag_pos : rf::ctf_red_flag_pos;
        vec.y += WorldHUDRender::ctf_flag_offset; // position icon above flag origin

        // Choose texture based on team and flag type
        int bitmap_handle = -1;
        if (flag_team) {
            bitmap_handle = player_team ? g_world_hud_assets.flag_blue_d : g_world_hud_assets.flag_blue_a;
        }
        else {
            bitmap_handle = player_team ? g_world_hud_assets.flag_red_a : g_world_hud_assets.flag_red_d;
        }

        auto render_mode = g_game_config.world_hud_overdraw ? WorldHUDRenderMode::overdraw : WorldHUDRenderMode::no_overdraw;

        do_render_world_hud_sprite(vec, 1.0, bitmap_handle, render_mode, true, true, true);
    };

    // render flag sprites
    build_and_render_flag_icon(team, false); // red
    build_and_render_flag_icon(team, true);  // blue
}

void hud_world_do_frame() {
    if (g_game_config.world_hud_ctf && rf::multi_get_game_type() == rf::NetGameType::NG_TYPE_CTF) {
        build_ctf_flag_icons();
    }
}

ConsoleCommand2 worldhudctf_cmd{
    "cl_worldhudctf",
    []() {

        g_game_config.world_hud_ctf = !g_game_config.world_hud_ctf;
        g_game_config.save();
        rf::console::print("CTF world HUD is {}", g_game_config.world_hud_ctf ? "enabled" : "disabled");
    },
    "Toggle drawing of world HUD indicators for CTF flags",
    "cl_worldhudctf",
};

ConsoleCommand2 worldhudoverdraw_cmd{
    "cl_worldhudoverdraw",
    []() {

        g_game_config.world_hud_overdraw = !g_game_config.world_hud_overdraw;
        g_game_config.save();
        rf::console::print("World HUD overdraw is {}", g_game_config.world_hud_overdraw ? "enabled" : "disabled");
    },
    "Toggle whether world HUD indicators for objectives are drawn on top of everything else",
    "cl_worldhudoverdraw",
};

void hud_world_apply_patch()
{
    // register commands
    worldhudctf_cmd.register_cmd();
    worldhudoverdraw_cmd.register_cmd();
}
