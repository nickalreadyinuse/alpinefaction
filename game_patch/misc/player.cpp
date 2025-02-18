#include "player.h"
#include "../rf/player/player.h"
#include "../rf/player/camera.h"
#include "../rf/entity.h"
#include "../rf/multi.h"
#include "../rf/sound/sound.h"
#include "../rf/bmpman.h"
#include "../rf/weapon.h"
#include "../rf/level.h"
#include "../rf/hud.h"
#include "../rf/input.h"
#include "../rf/collide.h"
#include "../rf/gr/gr_light.h"
#include "../rf/os/os.h"
#include "../rf/os/frametime.h"
#include "../os/console.h"
#include "../main/main.h"
#include "../misc/alpine_options.h"
#include "../multi/multi.h"
#include "../multi/server_internal.h"
#include "../hud/multi_spectate.h"
#include "../multi/alpine_packets.h"
#include "../hud/hud_world.h"
#include <common/utils/list-utils.h>
#include <common/config/GameConfig.h>
#include <patch_common/FunHook.h>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>

std::unordered_map<const rf::Player*, PlayerAdditionalData> g_player_additional_data_map;
static rf::PlayerHeadlampSettings g_local_headlamp_settings;

void find_player(const StringMatcher& query, std::function<void(rf::Player*)> consumer)
{
    auto player_list = SinglyLinkedList{rf::player_list};
    for (auto& player : player_list) {
        if (query(player.name))
            consumer(&player);
    }
}

void reset_player_additional_data(const rf::Player* const player)
{
    g_player_additional_data_map.erase(player);
}

PlayerAdditionalData& get_player_additional_data(rf::Player* player)
{
    return g_player_additional_data_map[player];
}

// used for compatibility checks
bool is_player_minimum_af_client_version(rf::Player* player, int version_major, int version_minor) {
    auto& player_info = g_player_additional_data_map[player];

    return player_info.is_alpine &&
        player_info.alpine_version_major >= version_major &&
        player_info.alpine_version_minor >= version_minor;
}

FunHook<rf::Player*(bool)> player_create_hook{
    0x004A3310,
    [](bool is_local) {
        rf::Player* player = player_create_hook.call_target(is_local);
        multi_init_player(player);
        return player;
    },
};

FunHook<void(rf::Player*)> player_destroy_hook{
    0x004A35C0,
    [](rf::Player* player) {        
        multi_spectate_on_destroy_player(player);
        reset_player_additional_data(player);
        player_destroy_hook.call_target(player);
        if (rf::is_server) {
            remove_ready_player_silent(player);
            server_vote_on_player_leave(player);
        }
    },
};

FunHook<rf::Entity*(rf::Player*, int, const rf::Vector3*, const rf::Matrix3*, int)> player_create_entity_hook{
    0x004A4130,
    [](rf::Player* pp, int entity_type, const rf::Vector3* pos, const rf::Matrix3* orient, int multi_entity_index) {
        rf::Entity* ep = player_create_entity_hook.call_target(pp, entity_type, pos, orient, multi_entity_index);
        if (ep) {
            multi_spectate_player_create_entity_post(pp, ep);
        }
        if (pp == rf::local_player) {
            // Update sound listener position so respawn sound is not classified as too quiet to play
            rf::Vector3 cam_pos = rf::camera_get_pos(pp->cam);
            rf::Matrix3 cam_orient = rf::camera_get_orient(pp->cam);
            rf::snd_update_sounds(cam_pos, rf::zero_vector, cam_orient);
        }
        return ep;
    },
};

bool should_swap_weapon_alt_fire(rf::Player* player)
{
    auto* entity = rf::entity_from_handle(player->entity_handle);
    if (!entity) {
        return false;
    }

    // Check if local entity is attached to a parent (vehicle or torret)
    auto* parent = rf::entity_from_handle(entity->host_handle);
    if (parent) {
        return false;
    }

    if (g_game_config.swap_assault_rifle_controls &&
        !get_option_or_default<bool>(AlpineOptionID::IgnoreSwapAssaultRifleControls, false) &&
        entity->ai.current_primary_weapon == rf::assault_rifle_weapon_type)
        return true;

    if (g_game_config.swap_grenade_controls &&
        !get_option_or_default<bool>(AlpineOptionID::IgnoreSwapGrenadeControls, false) &&
        entity->ai.current_primary_weapon == rf::grenade_weapon_type)
        return true;

    return false;
}

bool is_player_weapon_on(rf::Player* player, bool alt_fire) {
    if (!player) {
        player = rf::local_player;
    }
    auto* entity = rf::entity_from_handle(player->entity_handle);
    bool is_continous_fire = rf::entity_weapon_is_on(entity->handle, entity->ai.current_primary_weapon);
    bool is_alt_fire_flag_set = (entity->ai.ai_flags & rf::AIF_ALT_FIRE) != 0;
    if (should_swap_weapon_alt_fire(player)) {
        is_alt_fire_flag_set = !is_alt_fire_flag_set;
    }
    return is_continous_fire && is_alt_fire_flag_set == alt_fire;
}

FunHook<void(rf::Player*, bool, bool)> player_fire_primary_weapon_hook{
    0x004A4E80,
    [](rf::Player* player, bool alt_fire, bool was_pressed) {
        if (should_swap_weapon_alt_fire(player)) {
            alt_fire = !alt_fire;
        }
        player_fire_primary_weapon_hook.call_target(player, alt_fire, was_pressed);
    },
};

CodeInjection stop_continous_primary_fire_patch{
    0x00430EC5,
    [](auto& regs) {
        rf::Entity* entity = regs.esi;
        if (is_player_weapon_on(entity->local_player, false)) {
            regs.eip = 0x00430EDF;
        }
        else {
            regs.eip = 0x00430EF2;
        }
    }
};

CodeInjection stop_continous_alternate_fire_patch{
    0x00430F09,
    [](auto& regs) {
        rf::Entity* entity = regs.esi;
        if (is_player_weapon_on(entity->local_player, true)) {
            regs.eip = 0x00430F23;
        }
        else {
            regs.eip = 0x00430F36;
        }
    }
};

ConsoleCommand2 swap_assault_rifle_controls_cmd{
    "cl_swaparcontrols",
    []() {
        g_game_config.swap_assault_rifle_controls = !g_game_config.swap_assault_rifle_controls;
        g_game_config.save();
        rf::console::print("Swap assault rifle controls: {}",
                     g_game_config.swap_assault_rifle_controls ? "enabled" : "disabled");
        if (get_option_or_default<bool>(AlpineOptionID::IgnoreSwapAssaultRifleControls, false)) {
            rf::console::print("Note: This setting is disabled in the {} mod and will have no effect.",
                rf::mod_param.get_arg());
        }   
    },
    "Swap Assault Rifle controls",
};

ConsoleCommand2 swap_grenade_controls_cmd{
    "cl_swapnadecontrols",
    []() {
        g_game_config.swap_grenade_controls = !g_game_config.swap_grenade_controls;
        g_game_config.save();
        rf::console::print("Swap grenade controls: {}",
                     g_game_config.swap_grenade_controls ? "enabled" : "disabled");
        if (get_option_or_default<bool>(AlpineOptionID::IgnoreSwapGrenadeControls, false)) {
            rf::console::print("Note: This setting is disabled in the {} mod and will have no effect.",
                rf::mod_param.get_arg());
        }   
    },
    "Swap grenade controls",
};

ConsoleCommand2 play_join_beep_cmd{
    "mp_notifyonjoin",
    []() {
        g_game_config.player_join_beep = !g_game_config.player_join_beep;
        g_game_config.save();
        rf::console::print("Out of focus player join notifications: {}",
                     g_game_config.player_join_beep ? "enabled" : "disabled");
    },
    "Toggle notification beeps being played when a player joins the server you are in when your game doesn't have focus",
};

FunHook<void(rf::Player*, int)> player_make_weapon_current_selection_hook{
    0x004A4980,
    [](rf::Player* player, int weapon_type) {
        player_make_weapon_current_selection_hook.call_target(player, weapon_type);
        rf::Entity* entity = rf::entity_from_handle(player->entity_handle);
        if (entity && rf::is_multi) {
            // Reset impact delay timers when switching weapon (except in SP because of speedrunners)
            entity->ai.create_weapon_delay_timestamps[0].invalidate();
            entity->ai.create_weapon_delay_timestamps[1].invalidate();
            // Reset burst counters and timers
            entity->ai.primary_burst_fire_remaining = 0;
            entity->ai.secondary_burst_fire_remaining = 0;
            entity->ai.primary_burst_fire_next_timestamp.invalidate();
            entity->ai.secondary_burst_fire_next_timestamp.invalidate();
        }
    },
};

void __fastcall player_execute_action_timestamp_set_new(rf::Timestamp* fire_wait_timer, int, int value)
{
    if (!fire_wait_timer->valid() || fire_wait_timer->time_until() < value) {
        fire_wait_timer->set(value);
    }
}

CallHook<void __fastcall(rf::Timestamp*, int, int)> player_execute_action_timestamp_set_fire_wait_patch{
    {0x004A62C2u, 0x004A6325u},
    &player_execute_action_timestamp_set_new,
};

FunHook<void(rf::Player*, rf::ControlConfigAction, bool)> player_execute_action_hook{
    0x004A6210,
    [](rf::Player* player, rf::ControlConfigAction action, bool was_pressed) {
        if (!multi_spectate_execute_action(action, was_pressed)) {
            player_execute_action_hook.call_target(player, action, was_pressed);
        }
    },
};

FunHook<bool(rf::Player*)> player_is_local_hook{
    0x004A68D0,
    [](rf::Player* player) {
        if (multi_spectate_is_spectating()) {
            return false;
        }
        return player_is_local_hook.call_target(player);
    }
};

bool player_is_dead_and_not_spectating(rf::Player* player)
{
    if (multi_spectate_is_spectating() || !g_game_config.death_bars) {
        return false;
    }
    return rf::player_is_dead(player);
}

CallHook player_is_dead_red_bars_hook{0x00432A52, player_is_dead_and_not_spectating};
CallHook player_is_dead_scoreboard_hook{0x00437BEE, player_is_dead_and_not_spectating};
CallHook player_is_dead_scoreboard2_hook{0x00437C25, player_is_dead_and_not_spectating};

static bool player_is_dying_and_not_spectating(rf::Player* player)
{
    if (multi_spectate_is_spectating() || !g_game_config.death_bars) {
        return false;
    }
    return rf::player_is_dying(player);
}

CallHook player_is_dying_red_bars_hook{0x00432A5F, player_is_dying_and_not_spectating};
CallHook player_is_dying_scoreboard_hook{0x00437C01, player_is_dying_and_not_spectating};
CallHook player_is_dying_scoreboard2_hook{0x00437C36, player_is_dying_and_not_spectating};

FunHook<void()> players_do_frame_hook{
    0x004A26D0,
    []() {
        players_do_frame_hook.call_target();
        if (multi_spectate_is_spectating()) {
            rf::hud_do_frame(multi_spectate_get_target_player());
        }
    },
};

FunHook<void()> player_do_damage_screen_flash_hook{
    0x004A7520,
    []() {
        if (g_game_config.damage_screen_flash) {
            player_do_damage_screen_flash_hook.call_target();
        }
    },
};

ConsoleCommand2 damage_screen_flash_cmd{
    "cl_damageflash",
    []() {
        g_game_config.damage_screen_flash = !g_game_config.damage_screen_flash;
        g_game_config.save();
        rf::console::print("Damage screen flash effect is {}", g_game_config.damage_screen_flash ? "enabled" : "disabled");
    },
    "Toggle damage screen flash effect",
};

rf::Vector3 get_player_look_at_point(rf::Player* player)
{
    if (!player || !player->cam) {
        return rf::Vector3{0, 0, 0}; // Fallback if player or camera is null
    }

    // Get camera position and orientation (world-space)
    rf::Vector3 p0 = rf::camera_get_pos(player->cam);
    rf::Matrix3 orient = rf::camera_get_orient(player->cam);

    // Define ray endpoint
    rf::Vector3 p1 = p0 + orient.fvec * 10000.0f; // Large range for infinite look-at

    // Perform raycast
    rf::LevelCollisionOut col_info;
    col_info.face = nullptr;
    col_info.obj_handle = -1;
    rf::Entity* entity = rf::entity_from_handle(player->entity_handle);
    bool hit = rf::collide_linesegment_level_for_multi(p0, p1, entity, nullptr, &col_info, 0.1f, false, 1.0f);

    if (hit) {
        return col_info.hit_point; // Return exact hit position
    }

    return p1; // If no hit, return faraway point
}

void ping_looked_at_location() {
    if (!rf::is_multi) {
        return;
    }

    if (!get_df_server_info().has_value() || !get_df_server_info()->location_pinging) {
        rf::String msg{"This server does not allow you to ping locations"};
        rf::String prefix;
        rf::multi_chat_print(msg, rf::ChatMsgColor::white_white, prefix);
        return;
    }

    if (rf::multi_get_game_type() == rf::NetGameType::NG_TYPE_DM) {
        rf::String msg{"Location pinging is not available in deathmatch"};
        rf::String prefix;
        rf::multi_chat_print(msg, rf::ChatMsgColor::white_white, prefix);
        return;
    }

    auto point = get_player_look_at_point(rf::local_player);
    af_send_ping_location_req_packet(&point); // send to the server to replicate to other cleints
    add_location_ping_world_hud_sprite(point, rf::local_player->name); // render locally
}

ConsoleCommand2 death_bars_cmd{
    "mp_deathbars",
    []() {
        g_game_config.death_bars = !g_game_config.death_bars;
        g_game_config.save();
        rf::console::print("Death bars are {}", g_game_config.death_bars ? "enabled" : "disabled");
    },
    "Toggle red bars at the top and bottom of screen when dead",
};

CallHook<void(rf::VMesh*, rf::Vector3*, rf::Matrix3*, void*)> player_cockpit_vmesh_render_hook{
    0x004A7907,
    [](rf::VMesh *vmesh, rf::Vector3 *pos, rf::Matrix3 *orient, void *params) {
        rf::Matrix3 new_orient = *orient;

        if (string_equals_ignore_case(rf::vmesh_get_name(vmesh), "driller01.vfx")) {
            float m = static_cast<float>(rf::gr::screen_width()) / static_cast<float>(rf::gr::screen_height()) / (4.0 / 3.0);
            new_orient.rvec *= m;
        }

        player_cockpit_vmesh_render_hook.call_target(vmesh, pos, &new_orient, params);
    }
};

CodeInjection sr_load_player_weapon_anims_injection{
    0x004B4F9E,
    [](auto& regs) {
        rf::Entity *ep = regs.ebp;
        static auto& entity_update_weapon_animations = addr_as_ref<void(void*, int)>(0x0042AB20);
        entity_update_weapon_animations(ep, ep->ai.current_primary_weapon);
    },
};

float map_range(float value, float old_min, float old_max, float new_min, float new_max)
{
    value = std::max(old_min, std::min(value, old_max));
    return ((value - old_min) / (old_max - old_min)) * (new_max - new_min) + new_min;
}

CodeInjection player_move_flashlight_light_patch {
    0x004A6B0D,
    [](auto& regs) {
        rf::Vector3* pDest2 = static_cast<rf::Vector3*>(regs.ecx);
        rf::Entity* ep = rf::local_player_entity;
        rf::Vector3 eye_pos = ep->eye_pos;

        float dist = eye_pos.distance_to(*pDest2);
        regs.eax = *reinterpret_cast<float*>(0x005A0108) * sqrt(dist); // scale light radius

        //float mapped_dist = map_range(dist, 0.0f, *reinterpret_cast<float*>(0x005A0100), 1.0f, 0.05f);
        float mapped_dist = map_range(dist, 0.0f, g_local_headlamp_settings.max_range, 1.0f, 0.05f);
        *reinterpret_cast<float*>(0x005A00FC) =
            g_local_headlamp_settings.intensity * mapped_dist; // scale light intensity
    },
};

// called on game start and during each level post init
void update_player_flashlight() {

    //color
    if (g_alpine_level_info_config.is_option_loaded(rf::level.filename, AlpineLevelInfoID::PlayerHeadlampColor)) {
        auto headlamp_color =
            get_level_info_value<uint32_t>(rf::level.filename, AlpineLevelInfoID::PlayerHeadlampColor);
        float _a;

        std::tie(g_local_headlamp_settings.r,
            g_local_headlamp_settings.g,
            g_local_headlamp_settings.b,
            _a) = // alpha is discarded
            extract_normalized_color_components(headlamp_color);
    }
    else if (g_alpine_options_config.is_option_loaded(AlpineOptionID::PlayerHeadlampColor)) {
        auto headlamp_color = get_option_value<uint32_t>(AlpineOptionID::PlayerHeadlampColor);
        float _a;

        std::tie(g_local_headlamp_settings.r,
            g_local_headlamp_settings.g,
            g_local_headlamp_settings.b,
            _a) = // alpha is discarded
            extract_normalized_color_components(headlamp_color);
    }    
    else {
        g_local_headlamp_settings.r = 1.0f;
        g_local_headlamp_settings.g = 0.872f;
        g_local_headlamp_settings.b = 0.75f;
    }

    //intensity
    if (g_alpine_level_info_config.is_option_loaded(rf::level.filename, AlpineLevelInfoID::PlayerHeadlampIntensity)) {
        g_local_headlamp_settings.intensity = std::clamp(
            get_level_info_value<float>(rf::level.filename, AlpineLevelInfoID::PlayerHeadlampIntensity), 0.0f, 0.99f);
    }
    else if (g_alpine_options_config.is_option_loaded(AlpineOptionID::PlayerHeadlampIntensity)) {
        g_local_headlamp_settings.intensity =
            std::clamp(get_option_value<float>(AlpineOptionID::PlayerHeadlampIntensity), 0.0f, 0.99f);
    }    
    else {
        g_local_headlamp_settings.intensity = 0.6f;
    }

    //range
    if (g_alpine_level_info_config.is_option_loaded(rf::level.filename, AlpineLevelInfoID::PlayerHeadlampRange)) {
        g_local_headlamp_settings.max_range =
            get_level_info_value<float>(rf::level.filename, AlpineLevelInfoID::PlayerHeadlampRange);
    }
    else if (g_alpine_options_config.is_option_loaded(AlpineOptionID::PlayerHeadlampRange)) {
        g_local_headlamp_settings.max_range = get_option_value<float>(AlpineOptionID::PlayerHeadlampRange);
    }    
    else {
        g_local_headlamp_settings.max_range = 20.0f;
    }

    //radius
    if (g_alpine_level_info_config.is_option_loaded(rf::level.filename, AlpineLevelInfoID::PlayerHeadlampRadius)) {
        g_local_headlamp_settings.base_radius =
            get_level_info_value<float>(rf::level.filename, AlpineLevelInfoID::PlayerHeadlampRadius);
    }
    else if (g_alpine_options_config.is_option_loaded(AlpineOptionID::PlayerHeadlampRadius)) {
        g_local_headlamp_settings.base_radius = get_option_value<float>(AlpineOptionID::PlayerHeadlampRadius);
    }
    else {
        g_local_headlamp_settings.base_radius = 3.25f;
    }

    // Write the values
    AsmWriter{0x004A6AF9}.push(std::bit_cast<int32_t>(g_local_headlamp_settings.b)); // Blue (stock 1.0f)
    AsmWriter{0x004A6AFE}.push(std::bit_cast<int32_t>(g_local_headlamp_settings.g)); // Green (stock 1.0f)
    AsmWriter{0x004A6B03}.push(std::bit_cast<int32_t>(g_local_headlamp_settings.r)); // Red (stock 1.0f)

    // range defined in player_move_flashlight_light_patch
    //*reinterpret_cast<float*>(0x005A0100) = g_local_headlamp_settings.max_range;     // max range (stock 12.0)
    *reinterpret_cast<float*>(0x005A0108) = g_local_headlamp_settings.base_radius;   // base radius (stock 3.0)
    AsmWriter{0x004A6AF3}.push(2);                                                 // attenuation algo (stock 0)
}

void player_do_patch()
{
    // Support player headlamp
    player_move_flashlight_light_patch.install();
    update_player_flashlight();

    // general hooks
    player_create_hook.install();
    player_destroy_hook.install();

    // Allow swapping Assault Rifle primary and alternate fire controls
    player_fire_primary_weapon_hook.install();
    stop_continous_primary_fire_patch.install();
    stop_continous_alternate_fire_patch.install();

    // Reset impact delay timers when switching weapon to avoid delayed fire after switching
    player_make_weapon_current_selection_hook.install();

    // Fix setting fire wait timer when closing weapon switch menu
    // Note: this timer makes sense for weapons that require holding (not clicking) the control to fire (e.g. shotgun)
    player_execute_action_timestamp_set_fire_wait_patch.install();

    // spectate mode support
    player_execute_action_hook.install();
    player_create_entity_hook.install();
    player_is_local_hook.install();

    player_is_dying_red_bars_hook.install();
    player_is_dying_scoreboard_hook.install();
    player_is_dying_scoreboard2_hook.install();

    player_is_dead_red_bars_hook.install();
    player_is_dead_scoreboard_hook.install();
    player_is_dead_scoreboard2_hook.install();

    // Increase damage for kill command in Single Player
    write_mem<float>(0x004A4DF5 + 1, 100000.0f);

    // Allow undefined mp_character in player_create_entity
    // Fixes Go_Undercover event not changing player 3rd person character
    AsmWriter(0x004A414F, 0x004A4153).nop();

    // Fix hud msg never disappearing in spectate mode
    players_do_frame_hook.install();

    // Fix jeep cockpit not rendering for any jeeps entered after the first
    AsmWriter(0x004A77C3).jmp(0x004A77FB);

    // Make sure scanner bitmap is a render target in player_allocate
    write_mem<u8>(0x004A34BF + 1, rf::bm::FORMAT_RENDER_TARGET);

    // Support disabling of damage screen flash effect
    player_do_damage_screen_flash_hook.install();

    // Stretch driller cockpit when using a wide-screen
    player_cockpit_vmesh_render_hook.install();

    // Load correct third person weapon animations when restoring the game from a save file
    // Fixes mirror reflections and view from third person camera
    sr_load_player_weapon_anims_injection.install();

    // Change default 'Use' key to E
    write_mem<u8>(0x0043D0A3 + 1, rf::KEY_E);

    // Commands
    damage_screen_flash_cmd.register_cmd();
    death_bars_cmd.register_cmd();
    swap_assault_rifle_controls_cmd.register_cmd();
    swap_grenade_controls_cmd.register_cmd();
    play_join_beep_cmd.register_cmd();
}
