#include <xlog/xlog.h>
#include <patch_common/FunHook.h>
#include <patch_common/MemUtils.h>
#include <patch_common/AsmWriter.h>
#include <algorithm>
#include <unordered_set>
#include "hud_internal.h"
#include "hud_world.h"
#include "../object/event_alpine.h"
#include "../multi/server.h"
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

WorldHUDAssets g_world_hud_assets;
bool draw_mp_spawn_world_hud = false;
std::unordered_set<rf::EventWorldHUDSprite*> world_hud_sprite_events;
std::vector<EphemeralWorldHUDSprite> ephemeral_world_hud_sprites;

void load_world_hud_assets() {
    g_world_hud_assets.flag_red_d = rf::bm::load("af_wh_ctf_red_d.tga", -1, true);
    g_world_hud_assets.flag_blue_d = rf::bm::load("af_wh_ctf_blue_d.tga", -1, true);
    g_world_hud_assets.flag_red_a = rf::bm::load("af_wh_ctf_red_a.tga", -1, true);
    g_world_hud_assets.flag_blue_a = rf::bm::load("af_wh_ctf_blue_a.tga", -1, true);
    g_world_hud_assets.flag_red_s = rf::bm::load("af_wh_ctf_red_s.tga", -1, true);
    g_world_hud_assets.flag_blue_s = rf::bm::load("af_wh_ctf_blue_s.tga", -1, true);
    g_world_hud_assets.mp_respawn = rf::bm::load("af_wh_mp_spawn.tga", -1, true);
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
        case WorldHUDRenderMode::no_overdraw_glow:
            bitmap_mode = rf::gr::glow_3d_bitmap_mode;
            break;
        case WorldHUDRenderMode::overdraw:
            bitmap_mode = rf::gr::bitmap_3d_mode;
            break;
        case WorldHUDRenderMode::no_overdraw:
        default:
            bitmap_mode = rf::gr::bitmap_3d_mode_no_z;
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
            if (rf::multi_ctf_is_blue_flag_in_base()) {
                bitmap_handle = player_team ? g_world_hud_assets.flag_blue_d : g_world_hud_assets.flag_blue_a;
            }
            else {
                bitmap_handle = g_world_hud_assets.flag_blue_s;
            }
        }
        else {
            if (rf::multi_ctf_is_red_flag_in_base()) {
                bitmap_handle = player_team ? g_world_hud_assets.flag_red_a : g_world_hud_assets.flag_red_d;
            }
            else {
                bitmap_handle = g_world_hud_assets.flag_red_s;
            }
        }

        auto render_mode = g_game_config.world_hud_overdraw ? WorldHUDRenderMode::overdraw : WorldHUDRenderMode::no_overdraw;

        do_render_world_hud_sprite(vec, 1.0, bitmap_handle, render_mode, true, true, true);
    };

    // render flag sprites
    build_and_render_flag_icon(team, false); // red
    build_and_render_flag_icon(team, true);  // blue
}

void build_mp_respawn_icons() {
    auto all_respawn_points = get_new_multi_respawn_points();

    for (auto& point : all_respawn_points) {
        // build colour for icon and arrow
        int r = 200;
        int g = 200;
        int b = 200;

        if (point.red_team && !point.blue_team) {
            r = 167;
            g = 0;
            b = 0;
        }
        else if (point.blue_team && !point.red_team) {
            r = 52;
            g = 78;
            b = 167;
        }

        // draw an arrow in the direction of the spawn point
        rf::Vector3 arrow_end = point.position + (point.orientation.fvec * 1.5f);
        rf::gr::gr_line_arrow(
            point.position.x, point.position.y, point.position.z,
            arrow_end.x, arrow_end.y, arrow_end.z,
            r, g, b);

        rf::gr::set_color(r, g, b);
        do_render_world_hud_sprite(point.position, 1.0, g_world_hud_assets.mp_respawn,
                                   WorldHUDRenderMode::no_overdraw_glow, false, false, true);
    }
}

void build_world_hud_sprite_icons() {
    bool team = rf::local_player->team;

    for (auto& event : world_hud_sprite_events) {
        if (event->enabled) {
            if (team && event->sprite_filename_blue_int.has_value()) {
                do_render_world_hud_sprite(event->pos, event->scale, event->sprite_filename_blue_int.value_or(-1),
                    event->render_mode, false, false, true);
            }
            else if (event->sprite_filename_int.has_value()) {
                do_render_world_hud_sprite(event->pos, event->scale, event->sprite_filename_int.value_or(-1),
                    event->render_mode, false, false, true);
            }
        }
    }
}

void render_string_3d_pos_new(const rf::Vector3& pos, const std::string& text, int offset_x, int offset_y)
{
    rf::gr::Vertex dest;

    // Transform the position to screen space
    if (!rf::gr::rotate_vertex(&dest, pos))
    {
        rf::gr::project_vertex(&dest);

        // Check if projection was successful
        if (dest.flags & 1)
        {
            int screen_x = static_cast<int>(dest.sx) + offset_x;
            int screen_y = static_cast<int>(dest.sy) + offset_y;
            rf::gr::set_color(255, 255, 255, 223);
            auto render_mode = rf::level.distance_fog_far_clip == 0.0f ? rf::gr::text_2d_mode : rf::gr::bitmap_3d_mode;
            rf::gr::string(screen_x, screen_y, text.c_str(), -1, render_mode);
        }
    }
}

void build_ephemeral_world_hud_sprite_icons() {
    std::erase_if(ephemeral_world_hud_sprites, [](const EphemeralWorldHUDSprite& es) {
        return !es.timestamp.valid() || es.timestamp.elapsed();
    });

    for (const auto& es : ephemeral_world_hud_sprites) {
        do_render_world_hud_sprite(es.pos, 1.0f, es.bitmap, es.render_mode, true, true, true);

        // determine label width
        int text_width = 0, text_height = 0;
        rf::gr::gr_get_string_size(&text_width, &text_height, es.label.c_str(), es.label.size(), 1);
        int half_text_width = text_width / 2;

        auto text_pos = es.pos;
        render_string_3d_pos_new(text_pos, es.label.c_str(), -half_text_width, -25);
    }
}

void hud_world_do_frame() {
    if (g_game_config.world_hud_ctf && rf::multi_get_game_type() == rf::NetGameType::NG_TYPE_CTF) {
        build_ctf_flag_icons();
    }
    if (g_pre_match_active || (draw_mp_spawn_world_hud && (!rf::is_multi || rf::is_server))) {
        build_mp_respawn_icons();
    }
    if (!world_hud_sprite_events.empty()) {
        build_world_hud_sprite_icons();
    }
    if (!ephemeral_world_hud_sprites.empty()) {
        build_ephemeral_world_hud_sprite_icons();
    }
}

void populate_world_hud_sprite_events()
{
    world_hud_sprite_events.clear();

    std::vector<rf::Event*> events = rf::find_all_events_by_type(rf::EventType::World_HUD_Sprite);

    for (rf::Event* event : events) {
        if (auto* hud_sprite_event = dynamic_cast<rf::EventWorldHUDSprite*>(event)) {
            world_hud_sprite_events.insert(hud_sprite_event);
            hud_sprite_event->build_sprite_ints();
        }
    }
}

void add_location_ping_world_hud_sprite(rf::Vector3 pos, std::string player_name)
{
    // Remove any existing entry from the same player name
    std::erase_if(ephemeral_world_hud_sprites,
        [&](const EphemeralWorldHUDSprite& es) { return es.label == player_name; });

    auto bitmap = rf::bm::load("af_wh_ping1.tga", -1, true);

    EphemeralWorldHUDSprite es;
    es.bitmap = bitmap;
    es.pos = pos;
    es.label = player_name;
    es.timestamp.set(4000);

    ephemeral_world_hud_sprites.push_back(es);
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

ConsoleCommand2 worldhudmpspawns_cmd{
    "dbg_worldhudmpspawns",
    []() {
        draw_mp_spawn_world_hud = !draw_mp_spawn_world_hud;

        rf::console::print("World HUD multiplayer respawn points are {}", draw_mp_spawn_world_hud ? "enabled" : "disabled");

        if (draw_mp_spawn_world_hud && rf::is_multi && !rf::is_server) {
            rf::console::print("World HUD multiplayer respawn points will only be visible in single player or if you are the server host");
        }
    },
    "Toggle whether world HUD indicators for multiplayer respawn points are drawn",
    "dbg_worldhudmpspawns",
};

void hud_world_apply_patch()
{
    // register commands
    worldhudctf_cmd.register_cmd();
    worldhudoverdraw_cmd.register_cmd();
    worldhudmpspawns_cmd.register_cmd();
}
