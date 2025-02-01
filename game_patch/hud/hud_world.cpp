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

// draw CTF flag sprites
void render_flag_icons()
{
    bool team = rf::local_player->team;
    rf::Camera* camera = rf::local_player->cam;
    rf::Vector3 camera_pos = rf::camera_get_pos(camera);

    if (!rf::ctf_red_flag_item || !rf::ctf_blue_flag_item) {
        return; // don't render unless map has both flags
    }

    auto render_flag = [&](bool player_team, bool flag_team) {

        rf::Vector3 vec = flag_team ? rf::ctf_blue_flag_pos : rf::ctf_red_flag_pos;
        vec.y += WorldHUDRender::ctf_flag_offset; // icon position above flag origin

        float distance = vec.distance_to(camera_pos);
        float fog_far_clip = rf::level.distance_fog_far_clip;

        // enforce min and max effective fog distance
        float max_distance = fog_far_clip > WorldHUDRender::fog_dist_min ?
            std::min(fog_far_clip, WorldHUDRender::fog_dist_max) :
            WorldHUDRender::fog_dist_max;

        // If the icon would be clipped due to being beyond the fog far clip, draw it just inside instead
        if (distance > max_distance * WorldHUDRender::fog_dist_multi) {
            rf::Vector3 direction = vec - camera_pos;
            direction.normalize_safe();

            vec = camera_pos + (direction * (max_distance * WorldHUDRender::fog_dist_multi));
            distance = max_distance * WorldHUDRender::fog_dist_multi;
        }

        // scale based on distance from camera
        float scale_factor = std::max(distance, 1.0f) / WorldHUDRender::reference_distance;
        float scale = std::clamp(WorldHUDRender::base_scale * scale_factor,
            WorldHUDRender::min_scale, WorldHUDRender::max_scale);

        // Choose texture based on team and flag type
        if (flag_team) {
            rf::gr::set_texture(player_team ? g_world_hud_assets.flag_blue_d : g_world_hud_assets.flag_blue_a, -1);
        }
        else {
            rf::gr::set_texture(player_team ? g_world_hud_assets.flag_red_a : g_world_hud_assets.flag_red_d, -1);
        }

        rf::gr::gr_3d_bitmap_angle(&vec, 0.0f, scale, rf::gr::bitmap_3d_mode);
    };

    // render flag sprites
    render_flag(team, false);   // red
    render_flag(team, true);    // blue
}

void hud_world_do_frame() {
    if (g_game_config.world_hud_ctf && rf::multi_get_game_type() == rf::NetGameType::NG_TYPE_CTF) {
        render_flag_icons();
    }
}

ConsoleCommand2 worldhudctf_cmd{
    "mp_worldhudctf",
    []() {

        g_game_config.world_hud_ctf = !g_game_config.world_hud_ctf;
        g_game_config.save();
        rf::console::print("CTF world HUD is {}", g_game_config.world_hud_ctf ? "enabled" : "disabled");
    },
    "Toggle drawing of world HUD indicators for CTF flags",
    "mp_worldhudctf",
};

void hud_world_apply_patch()
{
    // register commands
    worldhudctf_cmd.register_cmd();
}
