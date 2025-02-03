#pragma once

#include <string_view>
#include <string>
#include <set>
#include <map>
#include <optional>
#include <vector>
#include "../rf/math/vector.h"
#include "../rf/math/matrix.h"
#include "../rf/os/string.h"

// Forward declarations
namespace rf
{
    struct Player;
}

struct SpawnProtectionConfig
{
    bool enabled = true;
    int duration = 1500;
    bool use_powerup = false;
};

struct InactivityConfig
{
    bool enabled = false;
    int new_player_grace_ms = 120000;
    int allowed_inactive_ms = 30000;
    int warning_duration_ms = 10000;
    std::string kick_message = "You have been marked as idle due to inactivity! You will be kicked from the game unless you respawn in the next 10 seconds.";
};

struct VoteConfig
{
    bool enabled = false;
    int time_limit_seconds = 60;
};

struct GunGameConfig
{
    bool enabled = false;
    bool dynamic_progression = false;
    bool rampage_rewards = false;
    std::optional<std::pair<int, int>> final_level;
    std::vector<std::pair<int, int>> levels;
};

struct BagmanConfig
{
    bool enabled = false;
    float bag_return_time = 25000.0f;
};

struct HitSoundsConfig
{
    bool enabled = true;
    int sound_id = 29;
    int rate_limit = 10;
};

struct CriticalHitsConfig
{
    bool enabled = false;
    int sound_id = 35;
    int rate_limit = 10;
    int reward_duration = 1500;
    float base_chance = 0.1f;
    bool dynamic_scale = true;
    float dynamic_damage_for_max_bonus = 1200.0f;
};
struct WeaponStayExemptionConfig
{
    bool enabled = false;
    bool rocket_launcher = false;
    bool heavy_machine_gun = false;
    bool sniper_rifle = false;
    bool assault_rifle = false;
    bool machine_pistol = false;
    bool shotgun = false;
    bool scope_assault_rifle = false;
    bool grenade = false;
    bool remote_charge = false;
    bool handgun = false;
    bool flamethrower = false;
    bool riot_stick = false;
    bool riot_shield = false;
    bool rail_gun = false;
};

struct OvertimeConfig
{
    bool enabled = false;
    int additional_time = 5;
    bool tie_if_flag_stolen = true;
};

struct NewSpawnLogicConfig // defaults match stock game
{
    bool respect_team_spawns = true;    
    bool try_avoid_players = true;
    bool always_avoid_last = false;
    bool always_use_furthest = false;
    bool only_avoid_enemies = false;
    std::map<std::string, std::optional<int>> allowed_respawn_items;
};

struct ServerAdditionalConfig
{
    VoteConfig vote_kick;
    VoteConfig vote_level;
    VoteConfig vote_extend;
    VoteConfig vote_restart;
    VoteConfig vote_next;
    VoteConfig vote_rand;
    VoteConfig vote_previous;
    VoteConfig vote_match;
    SpawnProtectionConfig spawn_protection;
    NewSpawnLogicConfig new_spawn_logic;
    //int spawn_protection_duration_ms = 1500;
    int desired_player_count = 32;
    float spawn_life = -1.0f;
    bool use_sp_damage_calculation = false;
    float spawn_armor= -1.0f;
    int ctf_flag_return_time_ms = 25000;
    GunGameConfig gungame;
    BagmanConfig bagman;
    HitSoundsConfig hit_sounds;
    CriticalHitsConfig critical_hits;
    WeaponStayExemptionConfig weapon_stay_exemptions;
    OvertimeConfig overtime;
    std::map<std::string, std::string> item_replacements;
    std::map<std::string, int> item_respawn_time_overrides;
    std::string default_player_weapon;
    std::optional<int> default_player_weapon_ammo;
    bool require_client_mod = true;
    float player_damage_modifier = 1.0f;
    bool saving_enabled = false;
    bool flag_dropping = true;
    bool no_player_collide = false;
    bool upnp_enabled = false;
    std::optional<int> force_player_character;
    std::optional<float> max_fov;
    bool allow_fullbright_meshes = true;
    bool allow_lightmaps_only = true;
    bool allow_disable_screenshake = true;
    bool allow_disable_muzzle_flash = true;
    bool apply_click_limiter = true;
    bool allow_unlimited_fps = false;
    std::optional<int> semi_auto_cooldown = 90;
    int anticheat_level = 0;
    bool stats_message_enabled = true;
    bool drop_amps = false;
    bool dynamic_rotation = false;
    std::string welcome_message;
    bool weapon_items_give_full_ammo = false;
    bool weapon_infinite_magazines = false;
    float kill_reward_health = 0.0f;
    float kill_reward_armor = 0.0f;
    float kill_reward_effective_health = 0.0f;
    bool kill_reward_health_super = false;
    bool kill_reward_armor_super = false;
    bool clients_require_alpine = false;
    bool reject_non_alpine_clients = false;
    bool only_welcome_alpine = false;
    bool advertise_alpine = false;
    InactivityConfig inactivity;
};

struct MatchInfo
{
    std::time_t last_match_reminder_time = 0;
    bool pre_match_queued = false;
    bool pre_match_active = false;
    std::time_t pre_match_start_time = 0;
    std::optional<float> time_limit_on_pre_match_start;
    std::time_t last_ready_reminder_time = 0;
    bool everyone_ready = false;
    bool match_active = false;
    int team_size = -1;
    std::set<rf::Player*> ready_players_red;
    std::set<rf::Player*> ready_players_blue;
    std::set<rf::Player*> active_match_players;
    std::string match_level_name;

    void reset()
    {
        last_match_reminder_time = 0;
        pre_match_queued = false;
        pre_match_active = false;
        pre_match_start_time = 0;
        time_limit_on_pre_match_start.reset();
        last_ready_reminder_time = 0;
        everyone_ready = false;
        match_active = false;
        team_size = -1;
        ready_players_red.clear();
        ready_players_blue.clear();
        active_match_players.clear();
        match_level_name.clear();
    }
};

extern ServerAdditionalConfig g_additional_server_config;
extern std::string g_prev_level;
extern MatchInfo g_match_info;



void cleanup_win32_server_console();
void handle_vote_command(std::string_view vote_name, std::string_view vote_arg, rf::Player* sender);
std::vector<rf::Player*> get_current_player_list(bool include_browsers);
std::pair<bool, std::string> is_level_name_valid(std::string_view level_name_input);
bool is_player_in_match(rf::Player* player);
bool is_player_ready(rf::Player* player);
void update_pre_match_powerups(rf::Player* player);
void start_match();
void cancel_match();
void start_pre_match();
void set_ready_status(rf::Player* player, bool is_ready);
void toggle_ready_status(rf::Player* player);
bool get_ready_status(const rf::Player* player);
void server_vote_do_frame();
void init_server_commands();
void extend_round_time(int minutes);
void restart_current_level();
void load_next_level();
void load_rand_level();
void load_prev_level();
void server_vote_on_player_leave(rf::Player* player);
void server_vote_on_limbo_state_enter();
void process_delayed_kicks();
void kick_player_delayed(rf::Player* player);
bool ends_with(const rf::String& str, const std::string& suffix);
const ServerAdditionalConfig& server_get_df_config();
