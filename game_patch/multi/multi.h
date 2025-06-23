#pragma once

#include <optional>
#include <xlog/xlog.h>
#include "server_internal.h"
#include "../rf/player/player.h"
#include "../rf/entity.h"
#include "../rf/os/timer.h"

struct PlayerStatsNew : rf::PlayerLevelStats
{
    unsigned short num_kills;
    unsigned short num_deaths;
    unsigned short current_streak;
    unsigned short max_streak;
    float num_shots_hit;
    float num_shots_fired;
    float damage_received;
    float damage_given;
    float damage_given_current_life;

    void inc_kills()
    {
        ++num_kills;
        ++current_streak;
        max_streak = std::max(max_streak, current_streak);
    }

    void inc_deaths()
    {
        ++num_deaths;
        current_streak = 0;
        damage_given_current_life = 0;
    }

    void add_shots_hit(float add)
    {
        // Weapons with multiple projectiles (shotgun) use fractional values
        num_shots_hit += add;
    }

    void add_shots_fired(float add)
    {
        // Weapons with multiple projectiles (shotgun) use fractional values
        num_shots_fired += add;
    }

    void add_damage_received(float damage)
    {
        damage_received += damage;
    }

    void add_damage_given(float damage)
    {
        damage_given += damage;
        damage_given_current_life += damage;
    }

    [[nodiscard]] float calc_accuracy() const
    {
        if (num_shots_fired > 0) {
            return num_shots_hit / num_shots_fired;
        }
        return 0;
    }

    void clear()
    {
        num_kills = 0;
        num_deaths = 0;
        num_shots_hit = 0.0f;
        num_shots_fired = 0.0f;
        current_streak = 0;
        max_streak = 0;
        damage_received = 0;
        damage_given = 0;
        damage_given_current_life = 0;
    }
};

struct AlpineFactionServerInfo
{
    uint8_t version_major = 0;
    uint8_t version_minor = 0;
    bool saving_enabled = false;
    std::optional<float> max_fov;
    bool allow_fb_mesh = false;
    bool allow_lmap = false;
    bool allow_no_ss = false;
    bool no_player_collide = false;
    bool allow_no_mf = false;
    bool click_limit = false;
    std::optional<int> semi_auto_cooldown;
    bool unlimited_fps = false;
    bool gaussian_spread = false;
    bool location_pinging = false;
};

void multi_level_download_update();
void multi_do_patch();
void multi_after_full_game_init();
void multi_init_player(rf::Player* player);
void send_chat_line_packet(const char* msg, rf::Player* target, rf::Player* sender = nullptr, bool is_team_msg = false);
const std::optional<AlpineFactionServerInfo>& get_df_server_info();
void multi_level_download_do_frame();
void multi_level_download_abort();
void multi_ban_apply_patch();
int get_level_file_version(const std::string& file_name);
void print_all_player_info();
void server_set_player_weapon(rf::Player* pp, rf::Entity* ep, int weapon_type);
void start_level_in_multi(std::string filename);
std::optional<std::string> multi_ban_unban_last();
int get_semi_auto_fire_wait_override();
void mp_send_handicap_request(bool force);
