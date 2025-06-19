#pragma once

#include <algorithm>

extern bool g_loaded_alpine_settings_file;

struct AlpineGameSettings
{
    // fov
    static constexpr float min_fov = 75.0f;
    static constexpr float max_fov = 160.0f;
    float horz_fov = 0.0f;
    void set_horz_fov(float fov)
    {
        if (fov == 0.0f) {
            horz_fov = 0.0f; // Allow 0.0f for auto scaling
        }
        else {
            horz_fov = std::clamp(fov, min_fov, max_fov);
        }
    }

    // fpgun fov scale
    static constexpr float min_fpgun_fov_scale = 0.1f;
    static constexpr float max_fpgun_fov_scale = 1.5f;
    float fpgun_fov_scale = 1.0f;
    void set_fpgun_fov_scale(float scale)
    {
        fpgun_fov_scale = std::clamp(scale, min_fpgun_fov_scale, max_fpgun_fov_scale);
    }

    // fpgun position offsets (stored as actual float values for fine control)
    static constexpr float min_fpgun_pos_offset = -50.0f;
    static constexpr float max_fpgun_pos_offset = 50.0f;
    static constexpr float fpgun_pos_scale_factor = 0.001f; // 1 user unit = 0.001 world units
    float fpgun_x_offset = 0.0f;
    float fpgun_y_offset = 0.0f;
    float fpgun_z_offset = 0.0f;
    void set_fpgun_pos_offsets_from_user_input(int x, int y, int z)
    {
        float scaled_x = x * fpgun_pos_scale_factor;
        float scaled_y = y * fpgun_pos_scale_factor;
        float scaled_z = z * fpgun_pos_scale_factor;
        fpgun_x_offset = std::clamp(scaled_x, min_fpgun_pos_offset, max_fpgun_pos_offset);
        fpgun_y_offset = std::clamp(scaled_y, min_fpgun_pos_offset, max_fpgun_pos_offset);
        fpgun_z_offset = std::clamp(scaled_z, min_fpgun_pos_offset, max_fpgun_pos_offset);
    }
    void set_fpgun_pos_offsets(float x, float y, float z)
    {
        fpgun_x_offset = std::clamp(x, min_fpgun_pos_offset, max_fpgun_pos_offset);
        fpgun_y_offset = std::clamp(y, min_fpgun_pos_offset, max_fpgun_pos_offset);
        fpgun_z_offset = std::clamp(z, min_fpgun_pos_offset, max_fpgun_pos_offset);
    }
    int get_fpgun_x_offset_user_units() const { return static_cast<int>(fpgun_x_offset / fpgun_pos_scale_factor); }
    int get_fpgun_y_offset_user_units() const { return static_cast<int>(fpgun_y_offset / fpgun_pos_scale_factor); }
    int get_fpgun_z_offset_user_units() const { return static_cast<int>(fpgun_z_offset / fpgun_pos_scale_factor); }

    // scope and scanner sens modifiers
    static constexpr float min_sens_mod = 0.01f;
    static constexpr float max_sens_mod = 10.0f;
    float scope_sensitivity_modifier = 0.25f;    
    void set_scope_sens_mod(float mod)
    {
        scope_sensitivity_modifier = std::clamp(mod, min_sens_mod, max_sens_mod);
    }
    float scanner_sensitivity_modifier = 0.25f;
    void set_scanner_sens_mod(float mod)
    {
        scanner_sensitivity_modifier = std::clamp(mod, min_sens_mod, max_sens_mod);
    }

    float reticle_scale = 1.0f;
    void set_reticle_scale(float scale)
    {
        reticle_scale = std::clamp(scale, 0.0f, 100.0f);
    }

    float level_sound_volume = 1.0f;
    void set_level_sound_volume(float scale)
    {
        level_sound_volume = std::clamp(scale, 0.0f, 1.0f);
    }

    // lod settings
    bool multi_no_character_lod = true;

    float entity_sim_distance = 100.0f;
    void set_entity_sim_distance(float dist)
    {
        entity_sim_distance = std::clamp(dist, 1.0f, 100000.0f);
    }

    float lod_dist_scale = 10.0f;
    void set_lod_dist_scale(float scale)
    {
        lod_dist_scale = std::clamp(scale, 0.1f, 1000.0f);
    }

    int monitor_resolution_scale = 2;
    void set_monitor_resolution_scale(int scale)
    {
        monitor_resolution_scale = std::clamp(scale, 1, 8);
    }

    bool scope_static_sensitivity = false;
    bool swap_ar_controls = false;
    bool swap_gn_controls = false;
    bool swap_sg_controls = false;
    bool mouse_linear_pitch = true;
    bool big_hud = false;
    int skip_cutscene_bind_alias = -1;
    bool try_disable_weapon_shake = false;
    bool try_fullbright_characters = false;
    bool try_disable_textures = false;
    bool try_disable_muzzle_flash_lights = false;
    bool world_hud_ctf_icons = true;
    bool world_hud_alt_damage_indicators = false;
    bool world_hud_overdraw = true;
    bool world_hud_big_text = true;
    bool world_hud_damage_numbers = true;
    bool world_hud_spectate_player_labels = false;
    bool world_hud_team_player_labels = false;
    bool play_hit_sounds = true;
    bool play_taunt_sounds = true;
    bool play_global_rad_msg_sounds = true;
    bool play_team_rad_msg_sounds = true;
    bool unlimited_semi_auto = false;
    bool gaussian_spread = false;
    bool multi_ricochet = false;
    bool damage_screen_flash = true;
    bool death_bars = true;
    bool mesh_static_lighting = true;
    bool show_glares = true;
    bool show_enemy_bullets = true;
    bool fps_counter = true;
    bool ping_display = true;
    bool spectate_mode_minimal_ui = false;
    bool save_console_history = false; // checked before config loaded, must be false here
    bool screen_shake_force_off = false;
    bool display_target_player_names = true;
    bool verbose_time_left_display = true;
    bool nearest_texture_filtering = false;
    bool direct_input = true;
    bool scoreboard_anim = true;
    bool autosave = true;
    bool af_branding = true;
    bool player_join_beep = false;
    bool full_range_lighting = true;
    bool always_clamp_official_lightmaps = false;
    bool static_bomb_code = false;
    bool entity_pain_sounds = true;
    bool real_armor_values = false;
    int suppress_autoswitch_alias = -1;

    std::string multiplayer_tracker = "rfgt.factionfiles.com";
    static constexpr size_t max_tracker_hostname_length = 200;
    void set_multiplayer_tracker(const std::string& tracker_hostname)
    {
        if (!tracker_hostname.empty() && tracker_hostname.length() <= max_tracker_hostname_length)
            multiplayer_tracker = tracker_hostname;
        else
            multiplayer_tracker = "rfgt.factionfiles.com";
    }

    // max_fps default is 120
    static constexpr unsigned min_fps_limit = 1u;
    static constexpr unsigned max_fps_limit = 100000u;
    static constexpr unsigned max_fps_limit_mp = 240u;
    unsigned max_fps = 240u;
    void set_max_fps(unsigned fps_value)
    {
        max_fps = std::clamp(fps_value, min_fps_limit, max_fps_limit);
    }

    unsigned server_max_fps = 60u;
    void set_server_max_fps(unsigned fps_value)
    {
        server_max_fps = std::clamp(fps_value, min_fps_limit, max_fps_limit);
    }

    // server netfps default is 1/0.085 ~= 12
    static constexpr unsigned min_server_netfps = 12u;
    static constexpr unsigned max_server_netfps = 300u;
    unsigned server_netfps = 30u;
    void set_server_netfps(unsigned netfps_value)
    {
        server_netfps = std::clamp(netfps_value, min_server_netfps, max_server_netfps);
    }
};

extern AlpineGameSettings g_alpine_game_config;
void initialize_alpine_core_config();
void alpine_core_config_save();
void set_big_hud(bool is_big);
void update_scope_sensitivity();
void update_scanner_sensitivity();
void recalc_mesh_static_lighting();
void apply_show_enemy_bullets();
void apply_console_history_setting();
void build_time_left_string_format();
void gr_update_texture_filtering();
void set_play_sound_events_volume_scale();
void apply_entity_sim_distance();
void gr_d3d_update_vsync();
