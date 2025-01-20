#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/ShortTypes.h>
#include <patch_common/AsmWriter.h>
#include <common/config/BuildConfig.h>
#include <common/version/version.h>
#include <common/rfproto.h>
#include <xlog/xlog.h>
#include <algorithm>
#include <limits>
#include <format>
#include <sstream>
#include <numeric>
#include <unordered_set>
#include <array>
#include <windows.h>
#include <winsock2.h>
#include "server.h"
#include "server_internal.h"
#include "multi.h"
#include "../os/console.h"
#include "../misc/player.h"
#include "../main/main.h"
#include <common/utils/list-utils.h>
#include "../rf/math/vector.h"
#include "../rf/math/matrix.h"
#include "../rf/player/player.h"
#include "../rf/item.h"
#include "../rf/gameseq.h"
#include "../rf/misc.h"
#include "../rf/ai.h"
#include "../rf/multi.h"
#include "../rf/item.h"
#include "../rf/parse.h"
#include "../rf/weapon.h"
#include "../rf/entity.h"
#include "../rf/os/os.h"
#include "../rf/os/timer.h"
#include "../rf/level.h"
#include "../rf/collide.h"
#include "../purefaction/pf.h"

const char* g_rcon_cmd_whitelist[] = {
    "kick",
    "level",
    "server_password",
    "map",
    "ban",
    "ban_ip",
    "map_ext",
    "map_rest",
    "map_next",
    "map_rand",
    "map_prev",
};

std::vector<rf::RespawnPoint> new_multi_respawn_points; // new storage of spawn points to avoid hard limits
std::vector<std::tuple<std::string, rf::Vector3, rf::Matrix3>> queued_item_spawn_points; // queued generated spawns
std::optional<rf::Vector3> likely_position_of_central_item; // guess at the center of the map for generated spawns
static const std::vector<std::string> possible_central_item_names = {
    "Multi Damage Amplifier",
    "Multi Invulnerability",
    "Multi Super Armor",
    "shoulder cannon",
    "Multi Super Health"
}; // prioritized list of common central items
int current_center_item_priority = possible_central_item_names.size();

ServerAdditionalConfig g_additional_server_config;
std::string g_prev_level;
bool g_is_overtime = false;

// server config parsing helper functions
void parse_boolean_option(rf::Parser& parser, const char* key, bool& option, const char* label = nullptr) {
    if (parser.parse_optional(key)) {
        option = parser.parse_bool();
        if (label) {
            rf::console::print("{}: {}", label, option ? "true" : "false");
        }
    }
}

void parse_uint_option(rf::Parser& parser, const char* key, int& option, const char* label = nullptr) {
    if (parser.parse_optional(key)) {
        option = parser.parse_uint();
        if (label) {
            rf::console::print("{}: {}", label, option);
        }
    }
}

void parse_int_option(rf::Parser& parser, const char* key, int& option, const char* label = nullptr) {
    if (parser.parse_optional(key)) {
        option = parser.parse_int();
        if (label) {
            rf::console::print("{}: {}", label, option);
        }
    }
}

void parse_float_option(rf::Parser& parser, const char* key, float& option, const char* label = nullptr) {
    if (parser.parse_optional(key)) {
        option = parser.parse_float();
        if (label) {
            rf::console::print("{}: {}", label, option);
        }
    }
}

void parse_vote_config(const char* vote_name, VoteConfig& config, rf::Parser& parser)
{
    std::string vote_option_name = std::format("${}:", vote_name);
    if (parser.parse_optional(vote_option_name.c_str())) {
        config.enabled = parser.parse_bool();
        rf::console::print("{}: {}", vote_name, config.enabled ? "true" : "false");

        if (parser.parse_optional("+Time Limit:")) {
            config.time_limit_seconds = parser.parse_uint();
            rf::console::print("{}: {}", "+Time Limit", config.time_limit_seconds);
        }
    }
}

void parse_spawn_protection(rf::Parser& parser)
{
    if (parser.parse_optional("$Spawn Protection Enabled:")) {
        g_additional_server_config.spawn_protection.enabled = parser.parse_bool();
        rf::console::print("Spawn Protection: {}", g_additional_server_config.spawn_protection.enabled ? "true" : "false");

        parse_uint_option(parser, "+Duration:", g_additional_server_config.spawn_protection.duration, "+Duration");
        parse_boolean_option(parser, "+Use Powerup:", g_additional_server_config.spawn_protection.use_powerup, "+Use Powerup");
    }
}

void parse_respawn_logic(rf::Parser& parser)
{
    if (parser.parse_optional("$Player Respawn Logic")) {
        rf::console::print("Parsing Player Respawn Logic...");

        parse_boolean_option(parser, "+Respect Team Spawns:", g_additional_server_config.new_spawn_logic.respect_team_spawns, "+Respect Team Spawns");
        parse_boolean_option(parser, "+Prefer Avoid Players:", g_additional_server_config.new_spawn_logic.try_avoid_players, "+Prefer Avoid Players");
        parse_boolean_option(parser, "+Always Avoid Last:", g_additional_server_config.new_spawn_logic.always_avoid_last, "+Always Avoid Last");
        parse_boolean_option(parser, "+Always Use Furthest:", g_additional_server_config.new_spawn_logic.always_use_furthest, "+Always Use Furthest");
        parse_boolean_option(parser,"+Only Avoid Enemies:", g_additional_server_config.new_spawn_logic.only_avoid_enemies, "+Only Avoid Enemies");

        while (parser.parse_optional("+Use Item As Spawn Point:")) {
            rf::String item_name;
            if (parser.parse_string(&item_name)) {
                std::optional<int> threshold = parser.parse_int();
                g_additional_server_config.new_spawn_logic.allowed_respawn_items[item_name.c_str()] = threshold;
                rf::console::print("Item {} will be used for dynamic spawn points with threshold: {}", item_name.c_str(),
                                   threshold.value_or(-1));
            }
        }
    }
}

void parse_gungame(rf::Parser& parser)
{
    if (parser.parse_optional("$GunGame:")) {
        g_additional_server_config.gungame.enabled = parser.parse_bool();
        rf::console::print("GunGame Enabled: {}", g_additional_server_config.gungame.enabled ? "true" : "false");
        parse_boolean_option(parser, "+Dynamic Progression:", g_additional_server_config.gungame.dynamic_progression, "+Dynamic Progression");
        parse_boolean_option(parser, "+Rampage Rewards:", g_additional_server_config.gungame.rampage_rewards, "+Rampage Rewards");

        if (parser.parse_optional("+Final Level:")) {
            int final_kill_level = parser.parse_int();
            int final_weapon_level = parser.parse_int();
            g_additional_server_config.gungame.final_level = std::make_pair(final_kill_level, final_weapon_level);
            rf::console::print("GunGame Final Level: Kill Level {} - Weapon Level {}",
                g_additional_server_config.gungame.final_level->first,
                g_additional_server_config.gungame.final_level->second);
        }

        while (parser.parse_optional("+Level:")) {
            int kill_level = parser.parse_int();
            int weapon_level = parser.parse_int();
            g_additional_server_config.gungame.levels.emplace_back(kill_level, weapon_level);
            rf::console::print("GunGame Level Added: Kill Level {} - Weapon Level {}",
                g_additional_server_config.gungame.levels.back().first,
                g_additional_server_config.gungame.levels.back().second);
        }
    }
}

void parse_hitsounds(rf::Parser& parser) {
    if (parser.parse_optional("$Hitsounds:")) {
        g_additional_server_config.hit_sounds.enabled = parser.parse_bool();
        rf::console::print("Hitsounds Enabled: {}", g_additional_server_config.hit_sounds.enabled ? "true" : "false");

        parse_uint_option(parser, "+Sound ID:", g_additional_server_config.hit_sounds.sound_id, "+Sound ID");
        parse_uint_option(parser, "+Rate Limit:", g_additional_server_config.hit_sounds.rate_limit, "+Rate Limit");
    }
}

void parse_critical_hits(rf::Parser& parser) {
    if (parser.parse_optional("$Critical Hits:")) {
        g_additional_server_config.critical_hits.enabled = parser.parse_bool();
        rf::console::print("Critical Hits Enabled: {}", g_additional_server_config.critical_hits.enabled ? "true" : "false");

        parse_uint_option(parser, "+Attacker Sound ID:", g_additional_server_config.critical_hits.sound_id, "+Sound ID");
        parse_uint_option(parser, "+Rate Limit:", g_additional_server_config.critical_hits.rate_limit, "+Rate Limit");
        parse_uint_option(parser, "+Reward Duration:", g_additional_server_config.critical_hits.reward_duration, "+Reward Duration");
        parse_float_option(parser, "+Base Chance Percent:", g_additional_server_config.critical_hits.base_chance, "+Base Chance");
        parse_boolean_option(parser, "+Use Dynamic Chance Bonus:", g_additional_server_config.critical_hits.dynamic_scale, "+Dynamic Scaling");
        parse_float_option(parser, "+Dynamic Chance Damage Ceiling:", g_additional_server_config.critical_hits.dynamic_damage_for_max_bonus, "+Dynamic Damage Ceiling");
    }
}

void parse_overtime(rf::Parser& parser) {
    if (parser.parse_optional("$Overtime Enabled:")) {
        g_additional_server_config.overtime.enabled = parser.parse_bool();
        rf::console::print("Overtime Enabled: {}", g_additional_server_config.overtime.enabled ? "true" : "false");

        parse_uint_option(parser, "+Duration:", g_additional_server_config.overtime.additional_time, "+Duration");
        parse_boolean_option(parser, "+Consider Tied If Flag Stolen:", g_additional_server_config.overtime.tie_if_flag_stolen, "+Tie If Flag Stolen");
    }
}

void parse_weapon_stay_exemptions(rf::Parser& parser) {
    if (parser.parse_optional("$Weapon Stay Exemptions:")) {
        g_additional_server_config.weapon_stay_exemptions.enabled = parser.parse_bool();
        rf::console::print("Weapon Stay Exemptions Enabled: {}", g_additional_server_config.weapon_stay_exemptions.enabled ? "true" : "false");

        parse_boolean_option(parser, "+Flamethrower:", g_additional_server_config.weapon_stay_exemptions.flamethrower, "+Flamethrower");
        parse_boolean_option(parser, "+Control Baton:", g_additional_server_config.weapon_stay_exemptions.riot_stick, "+Control Baton");
        parse_boolean_option(parser, "+Riot Shield:", g_additional_server_config.weapon_stay_exemptions.riot_shield, "+Riot Shield");
        parse_boolean_option(parser, "+Pistol:", g_additional_server_config.weapon_stay_exemptions.handgun, "+Pistol");
        parse_boolean_option(parser, "+Shotgun:", g_additional_server_config.weapon_stay_exemptions.shotgun, "+Shotgun");
        parse_boolean_option(parser, "+Submachine Gun:", g_additional_server_config.weapon_stay_exemptions.machine_pistol, "+Submachine Gun");
        parse_boolean_option(parser, "+Sniper Rifle:", g_additional_server_config.weapon_stay_exemptions.sniper_rifle, "+Sniper Rifle");
        parse_boolean_option(parser, "+Assault Rifle:", g_additional_server_config.weapon_stay_exemptions.assault_rifle, "+Assault Rifle");
        parse_boolean_option(parser, "+Heavy Machine Gun:", g_additional_server_config.weapon_stay_exemptions.heavy_machine_gun, "+Heavy Machine Gun");
        parse_boolean_option(parser, "+Precision Rifle:", g_additional_server_config.weapon_stay_exemptions.scope_assault_rifle, "+Precision Rifle");
        parse_boolean_option(parser, "+Rail Driver:", g_additional_server_config.weapon_stay_exemptions.rail_gun, "+Rail Driver");
        parse_boolean_option(parser, "+Rocket Launcher:", g_additional_server_config.weapon_stay_exemptions.rocket_launcher, "+Rocket Launcher");
        parse_boolean_option(parser, "+Grenade:", g_additional_server_config.weapon_stay_exemptions.grenade, "+Grenade");
        parse_boolean_option(parser, "+Remote Charges:", g_additional_server_config.weapon_stay_exemptions.remote_charge, "+Remote Charges");
    }
}

void parse_weapon_ammo_settings(rf::Parser& parser) {
    if (parser.parse_optional("$Weapon Items Give Full Ammo:")) {
        g_additional_server_config.weapon_items_give_full_ammo = parser.parse_bool();
        rf::console::print("Weapon Items Give Full Ammo: {}", g_additional_server_config.weapon_items_give_full_ammo ? "true" : "false");

        parse_boolean_option(parser, "+Infinite Magazines:", g_additional_server_config.weapon_infinite_magazines, "Infinite Magazines");
    }
}

void parse_default_player_weapon(rf::Parser& parser) {
    if (parser.parse_optional("$Default Player Weapon:")) {
        rf::String default_weapon;
        parser.parse_string(&default_weapon);
        g_additional_server_config.default_player_weapon = default_weapon.c_str();
        rf::console::print("Default Player Weapon: {}", g_additional_server_config.default_player_weapon);

        if (parser.parse_optional("+Initial Ammo:")) {
            auto ammo = parser.parse_uint();
            g_additional_server_config.default_player_weapon_ammo = {ammo};
            rf::console::print("Initial Ammo for Default Weapon: {}", ammo);

            auto weapon_type = rf::weapon_lookup_type(g_additional_server_config.default_player_weapon.c_str());
            if (weapon_type >= 0) {
                auto& weapon_cls = rf::weapon_types[weapon_type];
                weapon_cls.max_ammo_multi = std::max<int>(weapon_cls.max_ammo_multi, ammo);
            }
        }
    }
}

void parse_force_character(rf::Parser& parser) {
    if (parser.parse_optional("$Force Player Character:")) {
        rf::String character_name;
        parser.parse_string(&character_name);
        int character_num = rf::multi_find_character(character_name.c_str());
        if (character_num != -1) {
            g_additional_server_config.force_player_character = {character_num};
            rf::console::print("Forced Player Character: {} (ID {})", character_name, character_num);
        } else {
            xlog::warn("Unknown character name in Force Player Character setting: {}", character_name);
        }
    }
}

void parse_kill_rewards(rf::Parser& parser) {
    if (parser.parse_optional("$Kill Reward")) {
        rf::console::print("Parsing Kill Rewards...");
        parse_float_option(parser, "+Effective Health:", g_additional_server_config.kill_reward_effective_health, "Kill Reward: Effective Health");
        parse_float_option(parser, "+Health:", g_additional_server_config.kill_reward_health, "Kill Reward: Health");
        parse_float_option(parser, "+Armor:", g_additional_server_config.kill_reward_armor, "Kill Reward: Armor");
        parse_boolean_option(parser, "+Health Is Super:", g_additional_server_config.kill_reward_health_super, "Kill Reward: Health Is Super");
        parse_boolean_option(parser, "+Armor Is Super:", g_additional_server_config.kill_reward_armor_super, "Kill Reward: Armor Is Super");
    }
}

void parse_miscellaneous_options(rf::Parser& parser) {
    parse_int_option(parser, "$Desired Player Count:", g_additional_server_config.desired_player_count, "Desired Player Count");
    parse_float_option(parser, "$Spawn Health:", g_additional_server_config.spawn_life, "Spawn Health");
    parse_float_option(parser, "$Spawn Armor:", g_additional_server_config.spawn_armor, "Spawn Armor");
    parse_boolean_option(parser, "$Use SP Damage Calculation:", g_additional_server_config.use_sp_damage_calculation, "Use SP Damage Calculation");
    parse_int_option(parser, "$CTF Flag Return Time:", g_additional_server_config.ctf_flag_return_time_ms, "CTF Flag Return Time");
    parse_int_option(parser, "$Anticheat Level:", g_additional_server_config.anticheat_level, "Anticheat Level");
    parse_boolean_option(parser, "$No Player Collide:", g_additional_server_config.no_player_collide, "No Player Collide");
    parse_boolean_option(parser, "$Dynamic Rotation:", g_additional_server_config.dynamic_rotation, "Dynamic Rotation");
    parse_boolean_option(parser, "$Require Client Mod:", g_additional_server_config.require_client_mod, "Clients Require Mod");
    //parse_int_option(parser, "$Semi Auto Minimum Fire Wait:", g_additional_server_config.click_limiter_fire_wait, "Semi Auto Minimum Fire Wait");
    parse_float_option(parser, "$Player Damage Modifier:", g_additional_server_config.player_damage_modifier, "Player Damage Modifier");
    parse_boolean_option(parser, "$UPnP Enabled:", g_additional_server_config.upnp_enabled, "UPnP Enabled");
    parse_boolean_option(parser, "$Send Player Stats Message:", g_additional_server_config.stats_message_enabled, "Send Player Stats Message");
    parse_boolean_option(parser, "$Drop Amps On Death:", g_additional_server_config.drop_amps, "Drop Amps On Death");
    parse_boolean_option(parser, "$Flag Dropping:", g_additional_server_config.flag_dropping, "Flag Dropping");
    parse_boolean_option(parser, "$Saving Enabled:", g_additional_server_config.saving_enabled, "Saving Enabled");
    parse_boolean_option(parser, "$Allow Fullbright Meshes:", g_additional_server_config.allow_fullbright_meshes, "Allow Fullbright Meshes");
    parse_boolean_option(parser, "$Allow Lightmaps Only Mode:", g_additional_server_config.allow_lightmaps_only, "Allow Lightmaps Only Mode");
    parse_boolean_option(parser, "$Allow Disable Screenshake:", g_additional_server_config.allow_disable_screenshake, "Allow Disable Screenshake");
    parse_boolean_option(parser, "$Allow Disable Muzzle Flash Lights:", g_additional_server_config.allow_disable_muzzle_flash, "Allow Disable Muzzle Flash Lights");
    //parse_boolean_option(parser, "$Enforce Semi Auto Fire Rate Limit:", g_additional_server_config.apply_click_limiter, "Semi Auto Fire Rate Limit");

    if (parser.parse_optional("$Welcome Message:")) {
        rf::String welcome_message;
        parser.parse_string(&welcome_message);
        g_additional_server_config.welcome_message = welcome_message.c_str();
        rf::console::print("Welcome Message Set: {}", g_additional_server_config.welcome_message);
    }
}

void parse_item_respawn_time_override(rf::Parser& parser) {
    while (parser.parse_optional("$Item Respawn Time Override:")) {
        rf::String item_name;
        parser.parse_string(&item_name);
        auto new_time = parser.parse_uint();
        g_additional_server_config.item_respawn_time_overrides[item_name.c_str()] = new_time;
        rf::console::print("Item Respawn Time Override: {} -> {}ms", item_name.c_str(), new_time);
    }
}

void parse_item_replacements(rf::Parser& parser) {
    while (parser.parse_optional("$Item Replacement:")) {
        rf::String old_item, new_item;
        parser.parse_string(&old_item);
        parser.parse_string(&new_item);
        g_additional_server_config.item_replacements[old_item.c_str()] = new_item.c_str();
        rf::console::print("Item Replaced: {} -> {}", old_item.c_str(), new_item.c_str());
    }
}

void load_additional_server_config(rf::Parser& parser) {
    // Vote config
    parse_vote_config("Vote Kick", g_additional_server_config.vote_kick, parser);
    parse_vote_config("Vote Level", g_additional_server_config.vote_level, parser);
    parse_vote_config("Vote Extend", g_additional_server_config.vote_extend, parser);
    parse_vote_config("Vote Restart", g_additional_server_config.vote_restart, parser);
    parse_vote_config("Vote Next", g_additional_server_config.vote_next, parser);
    parse_vote_config("Vote Random", g_additional_server_config.vote_rand, parser);
    parse_vote_config("Vote Previous", g_additional_server_config.vote_previous, parser);
    parse_vote_config("Vote Match", g_additional_server_config.vote_match, parser);

    // Core config
    parse_spawn_protection(parser);
    parse_respawn_logic(parser);
    parse_gungame(parser);
    parse_hitsounds(parser);
    parse_critical_hits(parser);
    parse_overtime(parser);
    parse_weapon_stay_exemptions(parser);
    parse_weapon_ammo_settings(parser);
    parse_default_player_weapon(parser);
    parse_force_character(parser);
    parse_kill_rewards(parser);

    // Misc config
    parse_miscellaneous_options(parser);

    // separate for now because it needs to use std::optional<float>
    if (parser.parse_optional("$Max FOV:")) {
        float max_fov = parser.parse_float();
        if (max_fov > 0.0f) {
            g_additional_server_config.max_fov = {max_fov};
            rf::console::print("Max FOV: {}", g_additional_server_config.max_fov.value_or(180));
        }
    }

    if (parser.parse_optional("$Enforce Semi Auto Fire Rate Limit:")) {
        g_additional_server_config.apply_click_limiter = parser.parse_bool();
        rf::console::print("Enforce Semi Auto Fire Rate Limit: {}",
                            g_additional_server_config.overtime.enabled ? "true" : "false");
        if (parser.parse_optional("+Cooldown:")) {
            int fire_wait = parser.parse_int();
            g_additional_server_config.semi_auto_cooldown = {fire_wait};
            rf::console::print("+Cooldown: {}", g_additional_server_config.semi_auto_cooldown.value_or(0));
        }
    }


    // Repeatable config
    parse_item_respawn_time_override(parser);
    parse_item_replacements(parser);
}

// memory addresses for weapon stay exemption indexes
constexpr std::pair<bool WeaponStayExemptionConfig::*, uintptr_t> weapon_exemptions[] = {
    {&WeaponStayExemptionConfig::rocket_launcher, 0x00872458},
    {&WeaponStayExemptionConfig::heavy_machine_gun, 0x00872460},
    {&WeaponStayExemptionConfig::sniper_rifle, 0x00872440},
    {&WeaponStayExemptionConfig::assault_rifle, 0x00872470},
    {&WeaponStayExemptionConfig::machine_pistol, 0x0085CCD8},
    {&WeaponStayExemptionConfig::shotgun, 0x00872108},
    {&WeaponStayExemptionConfig::scope_assault_rifle, 0x0087245C},
    {&WeaponStayExemptionConfig::grenade, 0x00872118},
    {&WeaponStayExemptionConfig::remote_charge, 0x0087210C},
    {&WeaponStayExemptionConfig::handgun, 0x00872114},
    {&WeaponStayExemptionConfig::flamethrower, 0x0087243C},
    {&WeaponStayExemptionConfig::riot_stick, 0x00872468},
    {&WeaponStayExemptionConfig::riot_shield, 0x0085CCE4},
    {&WeaponStayExemptionConfig::rail_gun, 0x00872124}};

// declare optional vector for weapon stay exemptions
std::optional<std::vector<uintptr_t>> weapon_stay_exempt_indexes;

// Consolidate weapon stay exemption logic for both injections
void handle_weapon_stay_exemption(BaseCodeInjection::Regs& regs, uintptr_t jump_address)
{
    if (!weapon_stay_exempt_indexes) {
        return; // no exemptions if the optional vector is not populated
    }
    for (const auto& index_addr : *weapon_stay_exempt_indexes) {
        int weapon_index = *reinterpret_cast<int*>(index_addr);
        if (regs.eax == weapon_index) {
            regs.eip = jump_address;
            return;
        }
    }
}

// Weapon stay exemption part 1: remove item when it is picked up and start respawn timer
CodeInjection weapon_stay_remove_instance_injection{
    0x0045982E, [](BaseCodeInjection::Regs& regs) { handle_weapon_stay_exemption(regs, 0x00459865); }};

// Weapon stay exemption part 2: allow picking up item by a player who already did (after it respawns)
CodeInjection weapon_stay_allow_pickup_injection{
    0x004596B4, [](BaseCodeInjection::Regs& regs) { handle_weapon_stay_exemption(regs, 0x004596CD); }};

void initialize_weapon_stay_exemptions()
{
    const auto& config = g_additional_server_config.weapon_stay_exemptions;
    if (!config.enabled) {
        xlog::debug("Weapon stay exemptions are not enabled.");
        return;
    }

    // Populate weapon stay exemption array
    weapon_stay_exempt_indexes = std::vector<uintptr_t>{};
    for (const auto& [config_member, index_addr] : weapon_exemptions) {
        // Add debug logs to see which weapons are being checked
        bool is_exempt = config.*config_member;
        xlog::debug("Checking weapon at address: {}. Is exempt? {}", index_addr, is_exempt);

        if (is_exempt) {
            weapon_stay_exempt_indexes->push_back(index_addr);
        }
    }

    // injections
    weapon_stay_remove_instance_injection.install();
    weapon_stay_allow_pickup_injection.install();
}

CodeInjection dedicated_server_load_config_patch{
    //0x0046E216,
    0x0046E103, // above $Map lines
    [](auto& regs) {
        auto& parser = *reinterpret_cast<rf::Parser*>(regs.esp - 4 + 0x4C0 - 0x470);
        load_additional_server_config(parser);

        // Insert server name in window title when hosting dedicated server
        std::string wnd_name;
        wnd_name.append(rf::netgame.name.c_str());
        wnd_name.append(" - " PRODUCT_NAME " Dedicated Server");
        SetWindowTextA(rf::main_wnd, wnd_name.c_str()); 
    },
};

CodeInjection dedicated_server_load_post_map_patch{
    0x0046E216,
    [](auto& regs) {

        initialize_weapon_stay_exemptions();

        // shuffle maplist
        if (g_additional_server_config.dynamic_rotation) {
            shuffle_level_array();
        }

        // no weapon drops on player death
        if (g_additional_server_config.gungame.enabled) {
            AsmWriter(0x0042B0D3).jmp(0x0042B2BC);
        }

        // infinite reloads
        if (g_additional_server_config.gungame.enabled || g_additional_server_config.weapon_infinite_magazines) {
            AsmWriter{0x00425506}.nop(2);
        }

        // apply SP damage calculation
        if (g_additional_server_config.use_sp_damage_calculation) {
            AsmWriter(0x0041A37A).jmp(0x0041A3C1);
        }
    },
};

std::pair<std::string_view, std::string_view> strip_by_space(std::string_view str)
{
    auto space_pos = str.find(' ');
    if (space_pos == std::string_view::npos) {
        return {str, {}};
    }
    return {str.substr(0, space_pos), str.substr(space_pos + 1)};
}

void handle_next_map_command(rf::Player* player)
{
    int next_idx = (rf::netgame.current_level_index + 1) % rf::netgame.levels.size();
    auto msg = std::format("Next level: {}", rf::netgame.levels[next_idx]);
    send_chat_line_packet(msg.c_str(), player);
}

void handle_has_map_command(rf::Player* player, std::string_view level_name)
{
    auto [is_valid, checked_level_name] = is_level_name_valid(level_name);

    auto availability = is_valid ? "available" : "NOT available";
    auto msg = std::format("Level {} is {} on the server.", checked_level_name, availability);

    send_chat_line_packet(msg.c_str(), player);
}

void handle_save_command(rf::Player* player, std::string_view save_name)
{
    auto& pdata = get_player_additional_data(player);
    rf::Entity* entity = rf::entity_from_handle(player->entity_handle);
    if (entity && g_additional_server_config.saving_enabled) {
        PlayerNetGameSaveData save_data;
        save_data.pos = entity->pos;
        save_data.orient = entity->orient;
        pdata.saves.insert_or_assign(std::string{save_name}, save_data);
        send_chat_line_packet("Your position has been saved!", player);
    }
}

void handle_load_command(rf::Player* player, std::string_view save_name)
{
    auto& pdata = get_player_additional_data(player);
    rf::Entity* entity = rf::entity_from_handle(player->entity_handle);
    if (entity && g_additional_server_config.saving_enabled && !rf::entity_is_dying(entity)) {
        auto it = pdata.saves.find(std::string{save_name});
        if (it != pdata.saves.end()) {
            auto& save_data = it->second;
            entity->p_data.pos = save_data.pos;
            entity->p_data.next_pos = save_data.pos;
            entity->pos = save_data.pos;
            entity->orient = save_data.orient;
            if (entity->obj_interp) {
                entity->obj_interp->Clear();
            }
            rf::send_entity_create_packet_to_all(entity);
            pdata.last_teleport_timestamp.set(300);
            pdata.last_teleport_pos = save_data.pos;
            send_chat_line_packet("Your position has been restored!", player);
        }
        else {
            send_chat_line_packet("You do not have any position saved!", player);
        }
    }
}

std::string get_ready_player_names(bool is_blue_team)
{
    const auto& team_players = is_blue_team ? g_match_info.ready_players_blue : g_match_info.ready_players_red;
    std::ostringstream oss;
    for (const auto& player : team_players) {
        if (oss.tellp() > 0) {
            oss << ", ";
        }
        oss << player->name;
    }
    return oss.str();
}

std::string get_unready_player_names()
{
    const auto& all_players = get_current_player_list(false);

    // Create sets
    std::unordered_set<rf::Player*> ready_players(g_match_info.ready_players_blue.begin(),
        g_match_info.ready_players_blue.end());

    ready_players.insert(g_match_info.ready_players_red.begin(), g_match_info.ready_players_red.end());

    std::ostringstream oss;
    for (const auto& player : all_players) {
        // Check if the player is not in the ready set
        if (ready_players.find(player) == ready_players.end()) {
            if (oss.tellp() > 0) {
                oss << ", ";
            }
            oss << player->name;
        }
    }
    return oss.str();
}


void handle_matchinfo_command(rf::Player* player)
{
    auto msg = std::format("\xA6 No match is queued.");

    if (g_match_info.pre_match_active) {
        if (!g_match_info.ready_players_red.empty() || !g_match_info.ready_players_blue.empty()) {
            msg = std::format("\xA6 These players are ready:\n"
                                   "RED TEAM: {}\n"
                                   "BLUE TEAM: {}\n",
                                   get_ready_player_names(0), get_ready_player_names(1));
        }
        else {
            msg = std::format("\xA6 No players are ready.");
        }        
    }

    send_chat_line_packet(msg.c_str(), player);
}

void handle_whosready_command(rf::Player* player)
{
    auto msg = std::format("\xA6 No match is queued.");

    if (g_match_info.pre_match_active) {
        msg = std::format("\xA6 Not ready: {}\n", get_unready_player_names());
    }

    send_chat_line_packet(msg.c_str(), nullptr);
}

static void handle_drop_flag_request(rf::Player* player)
{
    if (!rf::multi_get_game_type() == rf::NG_TYPE_CTF) {
        return; // can't drop flags unless in CTF
    }

    if (!g_additional_server_config.flag_dropping) {
        send_chat_line_packet("\xA6 This server has disabled flag dropping.", player);
        return;
    }

    // drop flag if held
    if (rf::multi_ctf_get_red_flag_player() == player || rf::multi_ctf_get_blue_flag_player() == player) {
        rf::multi_ctf_drop_flag(player);
        rf::ctf_flag_cooldown_timestamp.set(750);
    }
}

CodeInjection process_obj_update_set_pos_injection{
    0x0047E563,
    [](auto& regs) {
        if (!rf::is_server) {
            return;
        }
        auto& entity = addr_as_ref<rf::Entity>(regs.edi);
        auto& pos = addr_as_ref<rf::Vector3>(regs.esp + 0x9C - 0x60);
        auto player = rf::player_from_entity_handle(entity.handle);
        auto& pdata = get_player_additional_data(player);
        if (pdata.last_teleport_timestamp.valid()) {
            float dist = (pos - pdata.last_teleport_pos).len();
            if (!pdata.last_teleport_timestamp.elapsed() && dist > 1.0f) {
                // Ignore obj_update packets for some time after restoring the position
                xlog::trace("ignoring obj_update after teleportation (distance {})", dist);
                regs.eip = 0x0047DFF6;
            }
            else {
                xlog::trace("not ignoring obj_update anymore after teleportation (distance {})", dist);
                pdata.last_teleport_timestamp.invalidate();
            }
        }
    },
};

static void send_private_message_with_stats(rf::Player* player)
{
    auto* stats = static_cast<PlayerStatsNew*>(player->stats);
    int accuracy = static_cast<int>(stats->calc_accuracy() * 100.0f);
    auto str = std::format(
        "PLAYER STATS\n"
        "Kills: {} - Deaths: {} - Max Streak: {}\n"
        "Accuracy: {}% ({:.0f}/{:.0f}) - Damage Given: {:.0f} - Damage Taken: {:.0f}",
        stats->num_kills, stats->num_deaths, stats->max_streak,
        accuracy, stats->num_shots_hit, stats->num_shots_fired,
        stats->damage_given, stats->damage_received);
    send_chat_line_packet(str.c_str(), player);
}

void shuffle_level_array()
{
    std::ranges::shuffle(rf::netgame.levels, g_rng);
    xlog::info("Shuffled level rotation");
}

const char* get_rand_level_filename()
{
    const std::size_t num_levels = rf::netgame.levels.size();

    if (num_levels <= 1) {
        // nowhere else to go, we're staying here!
        return rf::level_filename_to_load.c_str();
    }

    std::uniform_int_distribution<std::size_t> dist_levels(0, num_levels - 1);
    std::size_t rand_level_index = dist_levels(g_rng);

    // avoid selecting current level filename (unless it appears more than once on map list)
    if (rf::netgame.levels[rand_level_index] == rf::level_filename_to_load) {
        rand_level_index = (rand_level_index + 1) % num_levels;
    }

    return rf::netgame.levels[rand_level_index].c_str();
}

bool handle_server_chat_command(std::string_view server_command, rf::Player* sender)
{
    auto [cmd_name, cmd_arg] = strip_by_space(server_command);

    if (cmd_name == "info") {
        send_chat_line_packet(std::format("Server powered by Alpine Faction {} ({}), build date: {} {}", VERSION_STR, VERSION_CODE, __DATE__, __TIME__).c_str(), sender);
    }
    else if (cmd_name == "vote") {
        auto [vote_name, vote_arg] = strip_by_space(cmd_arg);
        handle_vote_command(vote_name, vote_arg, sender);
    }
    else if (cmd_name == "nextmap" || cmd_name == "nextlevel") {
        handle_next_map_command(sender);
    }
    else if (cmd_name == "save") {
        handle_save_command(sender, cmd_arg);
    }
    else if (cmd_name == "load") {
        handle_load_command(sender, cmd_arg);
    }
    else if (cmd_name == "stats") {
        send_private_message_with_stats(sender);
    }
    else if (cmd_name == "hasmap" || cmd_name == "haslevel") {
        handle_has_map_command(sender, cmd_arg);
    }
    else if (cmd_name == "ready") {
        set_ready_status(sender, 1);
    }
    else if (cmd_name == "unready") {
        set_ready_status(sender, 0);
    }
    else if (cmd_name == "matchinfo") {
        handle_matchinfo_command(sender);
    }
    else if (cmd_name == "whosready") {
        handle_whosready_command(sender);
    }
    else if (cmd_name == "dropflag") {
        handle_drop_flag_request(sender);
    }
    else {
        return false;
    }
    return true;
}

bool check_server_chat_command(const char* msg, rf::Player* sender)
{
    if (msg[0] == '/') {
        if (!handle_server_chat_command(msg + 1, sender))
            send_chat_line_packet("Unrecognized server command!", sender);
        return true;
    }

    auto [cmd, rest] = strip_by_space(msg);
    if (cmd == "server")
        return handle_server_chat_command(rest, sender);
    if (cmd == "vote")
        return handle_server_chat_command(msg, sender);
    return false;
}

CodeInjection spawn_protection_duration_patch{
    0x0048089A,
    [](auto& regs) {
        if (g_additional_server_config.spawn_protection.enabled) {
            if (g_additional_server_config.spawn_protection.use_powerup) {
                rf::Player* pp = regs.esi;
                rf::multi_powerup_add(pp, 0, g_additional_server_config.spawn_protection.duration);
                return;
            }
        }
        *static_cast<int*>(regs.esp) = g_additional_server_config.spawn_protection.enabled
			? g_additional_server_config.spawn_protection.duration
			: 0;
    },
};

CodeInjection detect_browser_player_patch{
    0x0047AFFB,
    [](auto& regs) {
        rf::Player* player = regs.esi;
        int conn_rate = regs.eax;
        if (conn_rate == 1 || conn_rate == 256) {
            auto& pdata = get_player_additional_data(player);
            pdata.is_browser = true;
        }
    },
};

void send_sound_packet_throwaway(rf::Player* target, int sound_id)
{
    // Send sound packet
    RF_SoundPacket packet;
    packet.header.type = RF_GPT_SOUND;
    packet.header.size = sizeof(packet) - sizeof(packet.header);
    packet.sound_id = sound_id;
    // FIXME: it does not work on RF 1.21
    packet.pos.x = packet.pos.y = packet.pos.z = std::numeric_limits<float>::quiet_NaN();
    rf::multi_io_send(target, &packet, sizeof(packet));
}

void send_sound_packet(rf::Player* target, int& last_sent_time, int rate_limit, int sound_id)
{
    // Rate limiting - max `rate_limit` times per second
    int now = rf::timer_get(1000);
    if (now - last_sent_time < 1000 / rate_limit) {
        return;
    }
    last_sent_time = now;

    // Send sound packet
    RF_SoundPacket packet;
    packet.header.type = RF_GPT_SOUND;
    packet.header.size = sizeof(packet) - sizeof(packet.header);
    packet.sound_id = sound_id;
    // FIXME: it does not work on RF 1.21
    packet.pos.x = packet.pos.y = packet.pos.z = std::numeric_limits<float>::quiet_NaN();
    rf::multi_io_send(target, &packet, sizeof(packet));
}

void send_hit_sound_packet(rf::Player* target)
{
    auto& pdata = get_player_additional_data(target);
    send_sound_packet(target, pdata.last_hitsound_sent_ms, g_additional_server_config.hit_sounds.rate_limit,
                      g_additional_server_config.hit_sounds.sound_id);
}

void send_critical_hit_packet(rf::Player* target)
{
    auto& pdata = get_player_additional_data(target);
    send_sound_packet(target, pdata.last_critsound_sent_ms, g_additional_server_config.critical_hits.rate_limit,
                      g_additional_server_config.critical_hits.sound_id);
}

FunHook<float(rf::Entity*, float, int, int, int)> entity_damage_hook{
    0x0041A350,
    [](rf::Entity* damaged_ep, float damage, int killer_handle, int damage_type, int killer_uid) {
        rf::Player* damaged_player = rf::player_from_entity_handle(damaged_ep->handle);
        rf::Player* killer_player = rf::player_from_entity_handle(killer_handle);
        bool is_pvp_damage = damaged_player && killer_player && damaged_player != killer_player;
        if (rf::is_server && is_pvp_damage) {
            damage *= g_additional_server_config.player_damage_modifier;
            if (damage == 0.0f) {
                return 0.0f;
            }

            // Check if this is a crit
            if (g_additional_server_config.critical_hits.enabled) {
                float base_chance = g_additional_server_config.critical_hits.base_chance;
                float bonus_chance = 0.0f;

                // calculate bonus chance
                if (g_additional_server_config.critical_hits.dynamic_scale && killer_player->stats) {
                    auto* killer_stats = static_cast<PlayerStatsNew*>(killer_player->stats);

                    bonus_chance = 0.1f * std::min(killer_stats->damage_given_current_life /
                        g_additional_server_config.critical_hits.dynamic_damage_for_max_bonus, 1.0f);
                }

                float critical_hit_chance = base_chance + bonus_chance;
                xlog::debug("Critical hit chance: {:.2f}", critical_hit_chance);

                std::uniform_real_distribution<float> dist_crit(0.0f, 1.0f);
                float random_value = dist_crit(g_rng);
                if (random_value < critical_hit_chance) {

                    // Apply amp modifier to the critical hit
                    damage *= (!rf::multi_powerup_has_player(killer_player, 1)) ? rf::g_multi_damage_modifier : 1.0f;

                    // On a crit, add amp for a duration
                    int amp_time_to_add = rf::multi_powerup_get_time_until(killer_player, 1) +
                                          g_additional_server_config.critical_hits.reward_duration;

                    rf::multi_powerup_add(killer_player, 1, amp_time_to_add);

                    // Notify with sound
                    send_critical_hit_packet(killer_player);
                }
            }
        }

        // should entity gib?
        if (damaged_ep->life < -5.0f &&
            damage_type == 3 &&                         // explosive
            damaged_ep->material == 3 &&                // flesh
            rf::game_get_gore_level() >= 2 &&
            !(damaged_ep->entity_flags & 0x2000000) &&  // custom_corpse (used by snakes and sea creature)
            !(damaged_ep->entity_flags & 0x1) &&        // dying
            !(damaged_ep->entity_flags & 0x1000) &&     // in_water
            !(damaged_ep->entity_flags & 0x2000))       // eye_under_water
        {
            damaged_ep->entity_flags |= 0x80;
        }

        float real_damage = entity_damage_hook.call_target(damaged_ep, damage, killer_handle, damage_type, killer_uid);

        if (rf::is_server && is_pvp_damage && real_damage > 0.0f) {

            auto* killer_player_stats = static_cast<PlayerStatsNew*>(killer_player->stats);
            killer_player_stats->add_damage_given(real_damage);

            auto* damaged_player_stats = static_cast<PlayerStatsNew*>(damaged_player->stats);
            damaged_player_stats->add_damage_received(real_damage);

            if (g_additional_server_config.hit_sounds.enabled) {
                send_hit_sound_packet(killer_player);
            }
        }

        return real_damage;
    },
};

CallHook<int(const char*)> item_lookup_type_hook{
    0x00465102,
    [](const char* cls_name) {
        if (rf::is_dedicated_server) {
            // support item replacement mapping
            auto it = g_additional_server_config.item_replacements.find(cls_name);
            if (it != g_additional_server_config.item_replacements.end())
                cls_name = it->second.c_str();
        }
        return item_lookup_type_hook.call_target(cls_name);
    },
};

/* CallHook<void(int, const char*, int, int, const rf::Vector3*, const rf::Matrix3*, int, bool, bool)>
    item_create_hook{
    0x00465175,
    [](int type, const char* name, int count, int parent_handle, const rf::Vector3* pos,
        const rf::Matrix3* orient, int respawn_time, bool permanent, bool from_packet) {

        // when creating it, check if a spawn time override is configured for this item
        if (auto it = g_additional_server_config.item_respawn_time_overrides.find(name);
            it != g_additional_server_config.item_respawn_time_overrides.end()) {
            respawn_time = it->second;
            //xlog::warn("Overriding respawn time for item '{}' to {} ms", name, respawn_time);
        }

        return item_create_hook.call_target(type, name, count, parent_handle, pos, orient, respawn_time, permanent, from_packet);
    }
};*/


CallHook<int(const char*)> find_default_weapon_for_entity_hook{
    0x004A43DA,
    [](const char* weapon_name) {
        if (rf::is_dedicated_server && !g_additional_server_config.default_player_weapon.empty()) {
            weapon_name = g_additional_server_config.default_player_weapon.c_str();
        }
        return find_default_weapon_for_entity_hook.call_target(weapon_name);
    },
};

CallHook<void(rf::Player*, int, int)> give_default_weapon_ammo_hook{
    0x004A4414,
    [](rf::Player* player, int weapon_type, int ammo) {
        if (g_additional_server_config.default_player_weapon_ammo) {
            ammo = g_additional_server_config.default_player_weapon_ammo.value();
        }
        give_default_weapon_ammo_hook.call_target(player, weapon_type, ammo);
    },
};

FunHook<bool (const char*, int)> multi_is_level_matching_game_type_hook{
    0x00445050,
    [](const char *filename, int ng_type) {
        if (ng_type == RF_GT_CTF) {
            return string_starts_with_ignore_case(filename, "ctf") || string_starts_with_ignore_case(filename, "pctf");
        }
        return string_starts_with_ignore_case(filename, "dm") || string_starts_with_ignore_case(filename, "pdm");
    },
};

// multi_spawn_player_server_side
FunHook<void(rf::Player*)> spawn_player_sync_ammo_hook{
    0x00480820,
    [](rf::Player* player) {
        spawn_player_sync_ammo_hook.call_target(player);
        // if default player weapon has ammo override sync ammo using additional reload packet
        if (g_additional_server_config.default_player_weapon_ammo && !rf::player_is_dead(player)) {
            rf::Entity* entity = rf::entity_from_handle(player->entity_handle);
            RF_ReloadPacket packet;
            packet.header.type = RF_GPT_RELOAD;
            packet.header.size = sizeof(packet) - sizeof(packet.header);
            packet.entity_handle = entity->handle;
            int weapon_type = entity->ai.current_primary_weapon;
            packet.weapon = weapon_type;
            packet.clip_ammo = entity->ai.clip_ammo[weapon_type];
            int ammo_type = rf::weapon_types[weapon_type].ammo_type;
            packet.ammo = entity->ai.ammo[ammo_type];
            rf::multi_io_send_reliable(player, reinterpret_cast<uint8_t*>(&packet), sizeof(packet), 0);
        }
    },
};

CallHook<void(char*)> get_mod_name_require_client_mod_hook{
    {
        0x0047B1E0, // send_game_info_packet
        0x004B32A3, // init_anticheat_checksums
    },
    [](char* mod_name) {
        if (!rf::is_dedicated_server || g_additional_server_config.require_client_mod) {
            get_mod_name_require_client_mod_hook.call_target(mod_name);
        }
        else {
            mod_name[0] = '\0';
        }
    },
};

CodeInjection send_ping_time_wrap_fix{
    0x0047CCE8,
    [](auto& regs) {
        auto& io_stats = addr_as_ref<rf::MultiIoStats>(regs.esi);
        auto player = addr_as_ref<rf::Player*>(regs.esp + 0xC + 0x4);
        if (!io_stats.send_ping_packet_timestamp.valid() || io_stats.send_ping_packet_timestamp.elapsed()) {
            xlog::trace("sending ping");
            io_stats.send_ping_packet_timestamp.set(3000);
            rf::multi_ping_player(player);
            io_stats.last_ping_time = rf::timer_get(1000);
        }
        regs.eip = 0x0047CD64;
    },
};

CodeInjection multi_on_new_player_injection{
    0x0047B013,
    [](auto& regs) {
        rf::Player* player = regs.esi;
        in_addr addr;
        addr.S_un.S_addr = ntohl(player->net_data->addr.ip_addr);
        rf::console::print("{}{} ({})", player->name,  rf::strings::has_joined, inet_ntoa(addr));
        regs.eip = 0x0047B051;
    },
};

static bool check_player_ac_status([[maybe_unused]] rf::Player* player)
{
#ifdef HAS_PF
    if (g_additional_server_config.anticheat_level > 0) {
        bool verified = pf_is_player_verified(player);
        if (!verified) {
            send_chat_line_packet(
                "Sorry! Your spawn request was rejected because verification of your client software failed. "
                "Please use the latest officially released version of Alpine Faction.",
                player);
            return false;
        }

        int ac_level = pf_get_player_ac_level(player);
        if (ac_level < g_additional_server_config.anticheat_level) {
            auto msg = std::format(
                "Sorry! Your spawn request was rejected because your client did not pass anti-cheat verification (your level {}, required {}). "
                "Please make sure you do not have any mods installed and that your client software is up to date.",
                ac_level, g_additional_server_config.anticheat_level
            );
            send_chat_line_packet(msg.c_str(), player);
            return false;
        }
    }
#endif // HAS_PF
    return true;
}

std::vector<rf::Player*> get_current_player_list(bool include_browsers)
{
    std::vector<rf::Player*> player_list;

    SinglyLinkedList<rf::Player> linked_player_list{rf::player_list};

    for (auto& player : linked_player_list) {
        auto additional_data = get_player_additional_data(&player);

        if (include_browsers || !(additional_data.is_browser || ends_with(player.name, " (Bot)"))) {
            player_list.push_back(&player);
        }
    }

    return player_list;
}


bool is_player_ready(rf::Player* player)
{
    return g_match_info.pre_match_active &&
        (g_match_info.ready_players_red.contains(player) || g_match_info.ready_players_blue.contains(player));
}

bool is_player_in_match(rf::Player* player)
{
    return g_match_info.match_active && g_match_info.active_match_players.contains(player);
}

void update_pre_match_powerups(rf::Player* player)
{
    rf::multi_powerup_remove_all_for_player(player);

    if (g_match_info.pre_match_active) {
        rf::multi_powerup_add(player, 0, 3600000);

        if (g_match_info.ready_players_red.contains(player) || g_match_info.ready_players_blue.contains(player)) {
            rf::multi_powerup_add(player, 1, 3600000);
        }
    }
}

void start_match()
{
    auto msg = std::format(
        "\n>>>>>>>>>>>>>>>>> {}v{} MATCH STARTING NOW <<<<<<<<<<<<<<<<<\n"
        "RED TEAM: {}\n"
        "BLUE TEAM: {}\n",
        g_match_info.team_size, g_match_info.team_size,
        get_ready_player_names(0), get_ready_player_names(1));

    send_chat_line_packet(msg.c_str(), nullptr);

    g_match_info.active_match_players.clear();

    g_match_info.active_match_players.insert(g_match_info.ready_players_red.begin(),
                                             g_match_info.ready_players_red.end());
    g_match_info.ready_players_red.clear();

    g_match_info.active_match_players.insert(g_match_info.ready_players_blue.begin(),
                                             g_match_info.ready_players_blue.end());
    g_match_info.ready_players_blue.clear();

    restart_current_level();

    // restore time limit when starting match
    rf::multi_time_limit = g_match_info.time_limit_on_pre_match_start.value_or(10.0f);
}

void cancel_match()
{
    rf::console::print("Canceling match");
    if (g_match_info.match_active) {
        load_next_level(); // end the round if active match is canceled
    }
    else {
        // restore the level timer and limit if pre-match is canceled        
        rf::level.time = 0.0f;
        rf::multi_time_limit = g_match_info.time_limit_on_pre_match_start.value_or(10.0f);        
    }

    g_match_info.reset();

    for (rf::Player* player : get_current_player_list(false)) {
        update_pre_match_powerups(player);
    }
}

void start_pre_match()
{
    if (g_match_info.pre_match_queued) {
        g_match_info.pre_match_active = g_match_info.pre_match_queued;
        g_match_info.pre_match_queued = false;
        g_match_info.pre_match_start_time = std::time(nullptr);
        g_match_info.last_ready_reminder_time = g_match_info.pre_match_start_time; // don't remind immediately

        // store time limit for later, remove level timer during pre-match
        g_match_info.time_limit_on_pre_match_start = rf::multi_time_limit;
        rf::multi_time_limit = 0.0f;

        auto msg = std::format("\n>>>>>>>>>>>>>>>>> {}v{} MATCH QUEUED <<<<<<<<<<<<<<<<<\n"
                               "Waiting for players. Use \"/ready\" to ready up, or \"/vote nomatch\" to call a vote to cancel.",
                               g_match_info.team_size, g_match_info.team_size);
        send_chat_line_packet(msg.c_str(), nullptr);

        for (rf::Player* player : get_current_player_list(false)) {
            update_pre_match_powerups(player);
        }
    }
}

void add_ready_player(rf::Player* player)
{
    auto& team_ready_list = (player->team == 0) ? g_match_info.ready_players_red : g_match_info.ready_players_blue;
    const std::string_view team_name = (player->team == 0) ? "RED" : "BLUE";

    if (team_ready_list.contains(player)) {
        send_chat_line_packet("You are already ready.", player);
        return;
    }

    if (team_ready_list.size() >= g_match_info.team_size) {
        send_chat_line_packet("Your team is full.", player);
        return;
    }

    team_ready_list.insert(player);
    update_pre_match_powerups(player);

    auto ready_msg = std::format("{} ({}) is ready!", player->name.c_str(), team_name);
    send_chat_line_packet(ready_msg.c_str(), nullptr);

    const auto ready_red = g_match_info.ready_players_red.size();
    const auto ready_blue = g_match_info.ready_players_blue.size();
    const auto required_players = g_match_info.team_size;

    if (ready_red >= required_players && ready_blue >= required_players) {
        send_chat_line_packet("\xA6 All players are ready. Match starting!", nullptr);
        g_match_info.everyone_ready = true;
        start_match(); // Start the match
    }
    else {
        auto waiting_msg = std::format("Still waiting for players - RED: {}, BLUE: {}.", required_players - ready_red,
                                       required_players - ready_blue);
        send_chat_line_packet(waiting_msg.c_str(), nullptr);
    }
}

void remove_ready_player(rf::Player* player)
{
    bool was_in_red = g_match_info.ready_players_red.erase(player) > 0;
    bool was_in_blue = g_match_info.ready_players_blue.erase(player) > 0;

    if (!was_in_red && !was_in_blue) {
        send_chat_line_packet("You were not marked as ready.", player);
        return;
    }

    update_pre_match_powerups(player);

    auto msg = std::format("{} is no longer ready! Still waiting for players - RED: {}, BLUE: {}.",
                           player->name.c_str(), g_match_info.team_size - g_match_info.ready_players_red.size(),
                           g_match_info.team_size - g_match_info.ready_players_blue.size());
    send_chat_line_packet(msg.c_str(), nullptr);
}

void set_ready_status(rf::Player* player, bool is_ready)
{
    if (g_match_info.pre_match_active) {
        if (is_ready) {
            add_ready_player(player);
        }
        else {
            remove_ready_player(player);
        }
    }
    else {
        send_chat_line_packet("No match is queued. Use \"/vote match\" to queue a match.", player);
    }
}

void match_do_frame()
{
    if (!g_additional_server_config.vote_match.enabled) {
        return;
    }

    if (rf::multi_num_players() <= 0) {
        if (g_match_info.match_active || g_match_info.pre_match_active) {
            cancel_match();
        }
        return; // no reminders to an empty server
    }

    std::time_t current_time = std::time(nullptr);

    if (!g_match_info.match_active && !g_match_info.pre_match_active)
    {
        if (current_time >= g_match_info.last_match_reminder_time + 270) {
            g_match_info.last_match_reminder_time = current_time;

            send_chat_line_packet(
                "\xA6 No active match. Use \"/vote match <type> <map filename>\" to call a match vote.", nullptr);
        }
    }
    else if (g_match_info.pre_match_active) {
        int reminder_interval = (current_time - g_match_info.pre_match_start_time) > 90 ? 15 : 30;

        if (current_time >= g_match_info.last_ready_reminder_time + reminder_interval) {
            g_match_info.last_ready_reminder_time = current_time;

            const auto ready_red = g_match_info.ready_players_red.size();
            const auto ready_blue = g_match_info.ready_players_blue.size();

            for (rf::Player* player : get_current_player_list(false)) {
                if (!is_player_ready(player)) {                    
                    auto msg = std::format(
                        "\xA6 You are NOT ready! {}v{} match queued, waiting for players - RED: {}, BLUE: {}.\n"
                        "Use \"/ready\" to ready up, or \"/vote nomatch\" to call a vote to cancel the match.",
                        g_match_info.team_size, g_match_info.team_size,
                        g_match_info.team_size - ready_red, g_match_info.team_size - ready_blue);
                    send_chat_line_packet(msg.c_str(), player);
                }
            }
        }
    }
}

std::pair<bool, std::string> is_level_name_valid(std::string_view level_name_input)
{
    std::string level_name{level_name_input};

    // add ".rfl" if it's missing
    if (!string_ends_with_ignore_case(level_name, ".rfl")) {
        level_name += ".rfl";
    }

    bool is_valid = rf::get_file_checksum(level_name.c_str()) != 0;

    return {is_valid, level_name};
}

// Function to count total spawned players (including bots and humans)
int count_spawned_players()
{
    auto player_list = SinglyLinkedList{rf::player_list};

    return std::count_if(player_list.begin(), player_list.end(), [](rf::Player& player) {
        rf::Entity* entity = rf::entity_from_handle(player.entity_handle);
        return entity != nullptr;
    });
}

FunHook<void(rf::Player*)> multi_spawn_player_server_side_hook{
    0x00480820,
    [](rf::Player* player) {
        if (g_additional_server_config.force_player_character) {
            player->settings.multi_character = g_additional_server_config.force_player_character.value();
        }
        if (!check_player_ac_status(player)) {
            return;
        }
        if (g_match_info.match_active && !is_player_in_match(player)) {
            send_chat_line_packet("You cannot spawn because a match is in progress. Please feel free to spectate.", player);
            return;
        }
        if (g_additional_server_config.desired_player_count < 32 &&
            ends_with(player->name, " (Bot)") &&
            count_spawned_players() >= g_additional_server_config.desired_player_count) {
            std::string msg = std::format("You're a bot and you can't spawn right now.");

            send_chat_line_packet(msg.c_str(), player);
            return;
        }

        multi_spawn_player_server_side_hook.call_target(player);

        if (g_additional_server_config.gungame.enabled && player) {
            gungame_on_player_spawn(player);
            //multi_update_gungame_weapon(player, true);            
        }

        rf::Entity* ep = rf::entity_from_handle(player->entity_handle);
        if (ep) {
            if (g_additional_server_config.spawn_life > -1.0f) {
                ep->life = g_additional_server_config.spawn_life;
            }
            if (g_additional_server_config.spawn_armor > -1.0f) {
                ep->armor = g_additional_server_config.spawn_armor;
            }
        }

        if (g_match_info.pre_match_active) {
            update_pre_match_powerups(player);
        }
    },
};

static float get_weapon_shot_stats_delta(rf::Weapon* wp)
{
    int num_projectiles = wp->info->num_projectiles;
    rf::Entity* parent_ep = rf::entity_from_handle(wp->parent_handle);
    if (parent_ep && parent_ep->entity_flags2 & 0x1000) { // EF2_SHOTGUN_DOUBLE_BULLET_UNK
        num_projectiles *= 2;
    }
    return 1.0f / num_projectiles;
}

static bool multi_is_team_game_type()
{
    return rf::multi_get_game_type() != rf::NG_TYPE_DM;
}

static void maybe_increment_weapon_hits_stat(int hit_obj_handle, rf::Weapon *wp)
{
    rf::Entity* attacker_ep = rf::entity_from_handle(wp->parent_handle);
    if (!attacker_ep) {
        return;
    }

    rf::Entity* hit_ep = rf::entity_from_handle(hit_obj_handle);
    if (!hit_ep) {
        return;
    }

    rf::Player* attacker_pp = rf::player_from_entity_handle(attacker_ep->handle);
    rf::Player* hit_pp = rf::player_from_entity_handle(hit_ep->handle);
    if (!attacker_pp || !hit_pp) {
        return;
    }

    if (!multi_is_team_game_type() || attacker_pp->team != hit_pp->team) {
        auto* stats = static_cast<PlayerStatsNew*>(attacker_pp->stats);
        stats->add_shots_hit(get_weapon_shot_stats_delta(wp));
        xlog::trace("hit a_ep {} wp {} h_ep {}", attacker_ep, wp, hit_ep);
    }
}

FunHook<int(rf::LevelCollisionOut*, rf::Weapon*)> multi_lag_comp_handle_hit_hook{
    0x0046F380,
    [](rf::LevelCollisionOut *col_info, rf::Weapon *wp) {
        if (rf::is_server) {
            maybe_increment_weapon_hits_stat(col_info->obj_handle, wp);
        }
        return multi_lag_comp_handle_hit_hook.call_target(col_info, wp);
    },
};

FunHook<void(rf::Entity*, rf::Weapon*)> multi_lag_comp_weapon_fire_hook{
    0x0046F7E0,
    [](rf::Entity *ep, rf::Weapon *wp) {
        multi_lag_comp_weapon_fire_hook.call_target(ep, wp);
        rf::Player* pp = rf::player_from_entity_handle(ep->handle);
        if (pp && pp->stats) {
            auto* stats = static_cast<PlayerStatsNew*>(pp->stats);
            stats->add_shots_fired(get_weapon_shot_stats_delta(wp));
            xlog::trace("fired a_ep {} wp {}", ep, wp);
        }
    },
};

void server_reliable_socket_ready(rf::Player* player)
{
    if (!g_additional_server_config.welcome_message.empty()) {
        auto msg = string_replace(g_additional_server_config.welcome_message, "$PLAYER", player->name.c_str());
        send_chat_line_packet(msg.c_str(), player);
    }
    if (g_match_info.pre_match_active) {    
        auto msg = std::format("\xA6 {}v{} match is queued and waiting for players! Use \"/ready\" to ready up.",
            g_match_info.team_size, g_match_info.team_size);

        send_chat_line_packet(msg.c_str(), nullptr);
    }
}

CodeInjection multi_limbo_init_injection{
    0x0047C286,
    [](auto& regs) {
        if (!rf::player_list) {
            xlog::trace("Wait between levels shortened because server is empty");
            addr_as_ref<int>(regs.esp) = 100;
        }

        if (g_match_info.match_active) {
            send_chat_line_packet("\xA6 Match complete!", nullptr);
            g_match_info.reset();
        }
        else if (g_match_info.pre_match_active && g_match_info.everyone_ready) {
            addr_as_ref<int>(regs.esp) = 5000;
            g_match_info.match_active = g_match_info.everyone_ready;
            g_match_info.everyone_ready = false;
            g_match_info.pre_match_active = false;
        }
    },
};

CodeInjection multi_level_init_injection{
    0x0046E450,
    [](auto& regs) {
        if (g_additional_server_config.dynamic_rotation && rf::netgame.current_level_index ==
                    rf::netgame.levels.size() - 1 && rf::netgame.levels.size() > 1) {
                // if this is the last level in the list and dynamic rotation is on, shuffle
                shuffle_level_array();
            }    
    },
};
bool round_is_tied(rf::NetGameType game_type)
{
    if (rf::multi_num_players() <= 1) {
        return false;
    }

    switch (game_type) {
    case rf::NG_TYPE_DM: {
        const auto current_players = get_current_player_list(false);

        if (current_players.empty())
            return false;

        const auto [highest_score, players_with_highest_score] = [&]() {
            int highest = (*current_players.begin())->stats->score;
            int count = 0;

            for (const auto* player : current_players) {
                if (player->stats->score > highest) {
                    highest = player->stats->score;
                    count = 1;
                }
                else if (player->stats->score == highest) {
                    ++count;
                }
            }
            return std::make_pair(highest, count);
        }();

        return players_with_highest_score >= 2;
    }
    case rf::NG_TYPE_CTF: {
        int red_score = rf::multi_ctf_get_red_team_score();
        int blue_score = rf::multi_ctf_get_blue_team_score();
        //xlog::warn("red: {}, blue: {}", red_score, blue_score);

        if (red_score == blue_score) {
            return true;
        }

        if (g_additional_server_config.overtime.tie_if_flag_stolen) {        
            bool red_flag_stolen = !rf::multi_ctf_is_red_flag_in_base();
            bool blue_flag_stolen = !rf::multi_ctf_is_blue_flag_in_base();

            // not currently tied, but if the team with the flag right now caps it, they will be
            return (red_flag_stolen && blue_score == red_score - 1) || (blue_flag_stolen && red_score == blue_score - 1);
        }
        else {
            return false;
        }        
    }
    case rf::NG_TYPE_TEAMDM: {
        return rf::multi_tdm_get_red_team_score() == rf::multi_tdm_get_blue_team_score();
    }
    default:
        return false;
    }
}

FunHook<void()> multi_check_for_round_end_hook{
    0x0046E7C0,
    []() {
        bool time_up = (rf::multi_time_limit > 0.0f && rf::level.time >= rf::multi_time_limit);
        bool round_over = time_up;
        const auto game_type = rf::multi_get_game_type();

        if (g_is_overtime) {
            round_over = (time_up || !round_is_tied(game_type));
        }
        else {
            switch (game_type) {
            case rf::NG_TYPE_DM: {
                auto current_players = get_current_player_list(true);

                if (current_players.empty())
                    break;

                for (rf::Player* player : current_players) {
                    if (player->stats->score >= rf::multi_kill_limit) {
                        round_over = true;
                        break;
                    }
                }
                break;
            }
            case rf::NG_TYPE_CTF: {
                if (rf::multi_ctf_get_red_team_score() >= rf::multi_cap_limit ||
                    rf::multi_ctf_get_blue_team_score() >= rf::multi_cap_limit) {
                    round_over = true;
                }
                break;
            }
            case rf::NG_TYPE_TEAMDM: {
                if (rf::multi_tdm_get_red_team_score() >= rf::multi_kill_limit ||
                    rf::multi_tdm_get_blue_team_score() >= rf::multi_kill_limit) {
                    round_over = true;
                }
                break;
            }
            default:
                break;
            }
        }

        if (round_over && rf::gameseq_get_state() != rf::GS_MULTI_LIMBO) {
            //xlog::warn("round time up {}, overtime? {}, already? {}, tied? {}", time_up, g_additional_server_config.overtime.enabled, g_is_overtime, round_is_tied(game_type));

            if (time_up && g_additional_server_config.overtime.enabled && !g_is_overtime && round_is_tied(game_type)) {
                g_is_overtime = true;
                extend_round_time(g_additional_server_config.overtime.additional_time);

                std::string msg = std::format("\xA6 OVERTIME! Game will end when the tie is broken");
                msg += g_additional_server_config.overtime.additional_time > 0
                           ? std::format(", or in {} minutes!", g_additional_server_config.overtime.additional_time)
                           : "!";
                send_chat_line_packet(msg.c_str(), nullptr);
            }
            else {
                rf::multi_change_level(nullptr);
            }
        }
    }
};

FunHook<int(const char*, uint8_t, const rf::Vector3*, const rf::Matrix3*, bool, bool, bool)> multi_respawn_create_point_hook{
    0x00470190,
    [](const char* name, uint8_t team, const rf::Vector3* pos, const rf::Matrix3* orient, bool red_team, bool blue_team, bool bot) 
    {
        constexpr size_t max_respawn_points = 2048; // raise limit 32 -> 2048
        
        if (new_multi_respawn_points.size() >= max_respawn_points) {
            return -1;
        }

        new_multi_respawn_points.emplace_back(rf::RespawnPoint{
            rf::String(name),
            team, // unused
            *pos,
            *orient,
            red_team,
            blue_team,
            bot
        });

        xlog::debug("New spawn point added! Name: {}, Team: {}, RedTeam: {}, BlueTeam: {}, Bot: {}",
            name, team, red_team, blue_team, bot);

        if (pos) {
            xlog::debug("Position: ({}, {}, {})", pos->x, pos->y, pos->z);
        }

        xlog::debug("Current number of spawn points: {}", new_multi_respawn_points.size());

        return 0;
    }
};

// clear spawn point array and reset last spawn index at level start
FunHook<void()> multi_respawn_level_init_hook {
    0x00470180,
    []() {
        new_multi_respawn_points.clear();        
        
        auto player_list = get_current_player_list(false);
        std::for_each(player_list.begin(), player_list.end(),
            [](rf::Player* player) { get_player_additional_data(player).last_spawn_point_index.reset(); });        

        multi_respawn_level_init_hook.call_target();
    }
};

// more flexible replacement for get_nearest_other_player_dist_sq in stock game
float get_nearest_other_player(const rf::Player* player, const rf::Vector3* spawn_pos,
    rf::Vector3* other_player_pos_out, bool only_enemies = false)
{
    float min_dist_sq = std::numeric_limits<float>::max();
    const bool is_team_game = multi_is_team_game_type();
    const int player_team = player->team;

    auto player_list = get_current_player_list(false);

    for (const auto* other_player : player_list) {
        if (other_player == player || (only_enemies && is_team_game && other_player->team == player_team)) {
            continue;
        }

        if (auto* other_entity = rf::entity_from_handle(other_player->entity_handle)) {
            const float dist_sq = rf::vec_dist_squared(spawn_pos, &other_entity->pos);

            if (dist_sq < min_dist_sq) {
                min_dist_sq = dist_sq;
                if (other_player_pos_out) {
                    *other_player_pos_out = other_entity->pos;
                }
            }
        }
    }

    return min_dist_sq;
}

FunHook<int(rf::Vector3*, rf::Matrix3*, rf::Player*)> multi_respawn_get_next_point_hook{
    0x00470300,
    [](rf::Vector3* pos, rf::Matrix3* orient, rf::Player* player) {

        // if map has no respawn points
        if (new_multi_respawn_points.empty()) {
            *pos = rf::level.player_start_pos;
            *orient = rf::level.player_start_orient;
            xlog::warn("No Multiplayer Respawn Points found. Spawning {} at the Player Start.", player->name);
            return -1;
        }

        // if player is invalid (should never happen)
        if (!player) {
            std::uniform_int_distribution<int> dist(0, new_multi_respawn_points.size() - 1);
            int index = dist(g_rng);
            *pos = new_multi_respawn_points[index].position;
            *orient = new_multi_respawn_points[index].orientation;
            return 0;
        }

        auto& pdata = get_player_additional_data(player);
        const int team = player->team;
        const int last_index = pdata.last_spawn_point_index.value_or(-1);
        const bool is_team_game = multi_is_team_game_type();

        const bool avoid_last = g_additional_server_config.new_spawn_logic.always_avoid_last;
        const bool avoid_players = g_additional_server_config.new_spawn_logic.try_avoid_players;
        const bool use_furthest = g_additional_server_config.new_spawn_logic.always_use_furthest;
        const bool respect_team_spawns = g_additional_server_config.new_spawn_logic.respect_team_spawns;
        const bool only_enemies = g_additional_server_config.new_spawn_logic.only_avoid_enemies;

        std::vector<const rf::RespawnPoint*> available_points;
        bool players_found = false;

        for (auto& point : new_multi_respawn_points) {
            if (is_team_game && respect_team_spawns) {
                if ((team == 0 && !point.red_team) || (team == 1 && !point.blue_team)) {
                    continue;
                }
            }

            const float dist = get_nearest_other_player(player, &point.position, nullptr, only_enemies);
            point.dist_other_player = dist;

            if (dist != std::numeric_limits<float>::max()) {
                players_found = true;
            }

            available_points.push_back(&point);
        }

        // handle case where no valid spawns are found (full rng)
        if (available_points.empty()) {
            std::uniform_int_distribution<int> dist(0, new_multi_respawn_points.size() - 1);
            const int index = dist(g_rng);
            *pos = new_multi_respawn_points[index].position;
            *orient = new_multi_respawn_points[index].orientation;
            return 1;
        }

        // handle case where no players are spawned but use_furthest is on (full rng within valid)
        if (use_furthest && !players_found) {
            std::uniform_int_distribution<int> dist(0, available_points.size() - 1);
            int index = dist(g_rng);
            *pos = available_points[index]->position;
            *orient = available_points[index]->orientation;
            pdata.last_spawn_point_index = index;
            return 1;
        }

        // sort valid points by distance from nearest other player (prefer further)
        if (avoid_players || use_furthest) {
            std::sort(available_points.begin(), available_points.end(),
                      [](const rf::RespawnPoint* a, const rf::RespawnPoint* b) {
                          return a->dist_other_player > b->dist_other_player;
                      });
        }

        int selected_index = 0;

        
        if (use_furthest) { // select always using furthest (no RNG)
            selected_index = std::distance(
                available_points.begin(), std::find(available_points.begin(),
                    available_points.end(), available_points[0]));

            if (avoid_last && last_index == selected_index && available_points.size() > 1) {
                selected_index = std::distance(
                    available_points.begin(), std::find(available_points.begin(),
                        available_points.end(), available_points[1]));
            }
        }
        else if (!avoid_players) { // select with full RNG if we don't care about distance from players
            std::uniform_int_distribution<int> dist(0, available_points.size() - 1);
            selected_index = dist(g_rng);
        }
        else { // select with weighted RNG if we do care about distance from players
            std::uniform_real_distribution<double> real_dist(0.0, 1.0);
            int random_index = static_cast<int>(std::sqrt(real_dist(g_rng)) * (available_points.size() - 1) + 0.5);

            selected_index = random_index;

            if (avoid_last && last_index == selected_index && available_points.size() > 1) {
                for (size_t i = 0; i < available_points.size(); ++i) {
                    if (i != last_index) {
                        selected_index = i;
                        break;
                    }
                }
            }
        }

        *pos = available_points[selected_index]->position;
        *orient = available_points[selected_index]->orientation;
        pdata.last_spawn_point_index = selected_index;
        //xlog::warn("Player {} (blue? {}) requested a spawn point. Giving them index {}. its red? {}, blue? {}", player->name, player->team, selected_index,
        //    available_points[selected_index]->red_team, available_points[selected_index]->blue_team);

        return 1;
    }
};

bool are_flags_initialized()
{
    return rf::ctf_red_flag_item != nullptr && rf::ctf_blue_flag_item != nullptr;
}

// returns 1 if closer to red, 0 if closer to blue, nullopt if no flags or flags are the same position
std::optional<int> is_closer_to_red_flag(const rf::Vector3* pos)
{
    if (!are_flags_initialized()) {
        return std::nullopt;
    }

    rf::Vector3 red_flag_pos, blue_flag_pos;
    rf::multi_ctf_get_red_flag_pos(&red_flag_pos);
    rf::multi_ctf_get_blue_flag_pos(&blue_flag_pos);

    if (red_flag_pos.x == blue_flag_pos.x &&
        red_flag_pos.y == blue_flag_pos.y &&
        red_flag_pos.z == blue_flag_pos.z) {
        return std::nullopt;
    }

    float dist_to_red_sq =  std::pow(pos->x - red_flag_pos.x, 2) +
                            std::pow(pos->y - red_flag_pos.y, 2) +
                            std::pow(pos->z - red_flag_pos.z, 2);

    float dist_to_blue_sq = std::pow(pos->x - blue_flag_pos.x, 2) +
                            std::pow(pos->y - blue_flag_pos.y, 2) +
                            std::pow(pos->z - blue_flag_pos.z, 2);

    return dist_to_red_sq < dist_to_blue_sq ? 1 : 0;
}

void create_spawn_point_from_item(const std::string& name, const rf::Vector3* pos, rf::Matrix3* orient)
{
    bool red_spawn = true;
    bool blue_spawn = true;

    if (multi_is_team_game_type()) {
        if (auto is_closer_to_red = is_closer_to_red_flag(pos); is_closer_to_red.has_value()) {
            red_spawn = *is_closer_to_red == 1;
            blue_spawn = !red_spawn;
        }
    }

    rf::multi_respawn_create_point(name.c_str(), 0, pos, orient, red_spawn, blue_spawn, false);
}

int get_item_priority(const std::string& item_name)
{
    auto it = std::find(possible_central_item_names.begin(), possible_central_item_names.end(), item_name);
    return it != possible_central_item_names.end() ?
        std::distance(possible_central_item_names.begin(), it) : possible_central_item_names.size();
}

void adjust_yaw_to_face_center(rf::Matrix3& orient, const rf::Vector3& pos, const rf::Vector3& center)
{
    rf::Vector3 direction = center - pos;
    direction.normalize();
    orient.fvec = direction;
    orient.uvec = rf::Vector3{0.0f, 1.0f, 0.0f};
    orient.rvec = orient.uvec.cross(orient.fvec);
    orient.rvec.normalize();
    orient.uvec = orient.fvec.cross(orient.rvec);
    orient.uvec.normalize();
}

void process_queued_spawn_points_from_items()
{    
    if (g_additional_server_config.new_spawn_logic.allowed_respawn_items.empty()) {
        return; // early return if no spawn points are to be generated
    }

    auto map_center = likely_position_of_central_item;

    for (auto& [name, pos, orient] : queued_item_spawn_points) {
        rf::Matrix3 adjusted_orient = orient;

        if (map_center) {
            adjust_yaw_to_face_center(adjusted_orient, pos, *map_center);
        }

        create_spawn_point_from_item(name, &pos, &adjusted_orient);
    }

    //reset item generated spawn vars
    queued_item_spawn_points.clear();
    likely_position_of_central_item.reset();
    current_center_item_priority = possible_central_item_names.size();
}

CallHook<rf::Item*(int, const char*, int, int, const rf::Vector3*, rf::Matrix3*, int, bool, bool)> item_create_hook{
    0x00465175,
    [](int type, const char* name, int count, int parent_handle, const rf::Vector3* pos, rf::Matrix3* orient,
       int respawn_time, bool permanent, bool from_packet) {

        // when creating it, check if a spawn time override is configured for this item
        if (auto it = g_additional_server_config.item_respawn_time_overrides.find(name);
            it != g_additional_server_config.item_respawn_time_overrides.end()) {
            respawn_time = it->second;
        }

        if (rf::is_dedicated_server && !g_additional_server_config.new_spawn_logic.allowed_respawn_items.empty()) {
            const auto& allowed_items = g_additional_server_config.new_spawn_logic.allowed_respawn_items;

            auto it = allowed_items.find(name);
            if (it != allowed_items.end() &&
                (!it->second || it->second == 0 || *it->second > static_cast<int>(new_multi_respawn_points.size()))) {

                queued_item_spawn_points.emplace_back(std::string(name), *pos, *orient);                
            }

            // make best guess at the center of the map
            int item_priority = get_item_priority(name);
            if (item_priority < possible_central_item_names.size()) {
                if (!likely_position_of_central_item || item_priority < current_center_item_priority) {
                    likely_position_of_central_item = *pos;
                    current_center_item_priority = item_priority;
                }
            }
        }

        return item_create_hook.call_target(
            type, name, count, parent_handle, pos, orient, respawn_time, permanent, from_packet);
    }
};

void server_add_player_weapon(rf::Player* player, int weapon_type, bool full_ammo)
{
    rf::WeaponInfo& winfo = rf::weapon_types[weapon_type];
    int ammo_count = winfo.clip_size;
    if (full_ammo) {
        if (g_additional_server_config.gungame.enabled && !rf::weapon_uses_clip(weapon_type)) {
            // hackfix: in gungame, set max ammo to 999 for non-clip weapons
            winfo.max_ammo = 9999;
        }        
        ammo_count = winfo.max_ammo + winfo.clip_size;
    }
    rf::Entity* ep = rf::entity_from_handle(player->entity_handle);
    //ep->ai.clip_ammo[weapon_type] = winfo.clip_size;
    //ep->ai.ammo[winfo.ammo_type] = winfo.max_ammo;
    rf::ai_add_weapon(&ep->ai, weapon_type, ammo_count);
     if (!rf::weapon_uses_clip(weapon_type)) {
        if (!rf::player_is_dead(player)) {
            //xlog::warn("reloading non-clip weapon");
            rf::Entity* entity = rf::entity_from_handle(player->entity_handle);
            RF_ReloadPacket packet;
            packet.header.type = RF_GPT_RELOAD;
            packet.header.size = sizeof(packet) - sizeof(packet.header);
            packet.entity_handle = entity->handle;
            packet.weapon = weapon_type;
            packet.ammo = entity->ai.clip_ammo[weapon_type]; // could be zeroed
            packet.clip_ammo = entity->ai.ammo[winfo.ammo_type];
            rf::multi_io_send_reliable(player, reinterpret_cast<uint8_t*>(&packet), sizeof(packet), 0);
        }
    }
    //xlog::warn("gave player {} weapon {} with ammo {}", player->name, weapon_type, ammo_count);
}

void entity_drop_powerup(rf::Entity* ep, int powerup_type, int count)
{
    if (!ep || (ep->p_data.flags & 0x400000) != 0) {
        return;
    }

    rf::Vector3 drop_position = ep->pos;
    rf::Matrix3 drop_orient = ep->orient;

    rf::Item* dropped_item = nullptr;

    switch (powerup_type) {
    case 1:
        dropped_item = rf::item_create(33, "Multi Damage Amplifier", count, -1, &drop_position, &drop_orient, -1, 0, 0);
        break;
    case 0:
        dropped_item = rf::item_create(32, "Multi Invulnerability", count, -1, &drop_position, &drop_orient, -1, 0, 0);
        break;
    default:
        xlog::warn("Unknown powerup type: {}", powerup_type);
        return;
    }

    if (dropped_item) {
        xlog::warn("Dropped {} with count {}", dropped_item->name, count);
        dropped_item->item_flags |= 8u;
        rf::send_obj_kill_packet(ep, dropped_item, nullptr);
    }
}

// drop_amps and bagman
CodeInjection entity_maybe_die_patch{
    0x00420600,
    [](auto& regs) {
        if (rf::is_multi && (rf::is_server || rf::is_dedicated_server) &&
            (g_additional_server_config.drop_amps || g_additional_server_config.bagman.enabled)) {

            rf::Entity* ep = regs.esi;

            if (ep) {
                //xlog::warn("{} died", ep->name);
                rf::Player* player = rf::player_from_entity_handle(ep->handle);

                if (rf::multi_powerup_has_player(player, 1)) {
                    int amp_count = 0;
                    if (g_additional_server_config.bagman.enabled) {
                        amp_count = 100000;
                    }
                    else {
                        int time_left = rf::multi_powerup_get_time_until(player, 1);
                        amp_count = time_left >= 1000 ? time_left / 1000 : 0; // item_touch_multi_amp multiplies by 1k
                    }

                    //xlog::warn("amp count {}", amp_count);

                    if (amp_count >= 1) {
                        entity_drop_powerup(ep, 1, amp_count); 
                    }
                }

                if (g_additional_server_config.drop_amps && rf::multi_powerup_has_player(player, 0)) {
                    int invuln_count = 0;
                    int time_left = rf::multi_powerup_get_time_until(player, 0);
                    invuln_count = time_left >= 1000 ? time_left / 1000 : 0; // item_touch_multi_amp multiplies by 1k

                    //xlog::warn("invuln count {}", invuln_count);

                    if (invuln_count >= 1) {
                        entity_drop_powerup(ep, 0, invuln_count);
                    }
                }
            }
        }
    },
};

// bagman
CodeInjection item_get_oldest_dynamic_patch{
    0x00458858,
    [](auto& regs) {
        if (g_additional_server_config.bagman.enabled) {

            rf::Item* item = regs.esi;

            if (item) {
                //xlog::warn("checked item {}, UID {}", item->name, item->uid);
                if (item->name == "Multi Damage Amplifier") {
                    //xlog::warn("found bag item, ensuring it persists");
                    regs.eip = 0x0045887E;
                }
            }
        }
    },
};

CallHook<rf::Entity*(int, const char*, int, rf::Vector3*, rf::Matrix3*, int, int)> entity_create_no_collide_hook {
    0x004A41D3,
    [](int type, const char* name, int parent_handle, rf::Vector3* pos, rf::Matrix3* orient, int create_flags, int mp_character) {

        if (get_df_server_info().has_value() && get_df_server_info()->no_player_collide) {
            create_flags |= 0x4;
        }

        return entity_create_no_collide_hook.call_target(type, name, parent_handle, pos, orient, create_flags, mp_character);
    }
};

void server_init()
{
    // Update the message when a dedicated server launches with some wrong options
    static char new_config_error_message[] =
        "Oh no! Either you haven't specified any maps, or you have an error in your dedicated server configuration.\n"
        "If this issue persists, join the community Discord at https://discord.gg/factionfiles for some assistance.\n";
    AsmWriter{0x0046E230}.push(reinterpret_cast<int32_t>(new_config_error_message));

    // Handle no player collide server option
    entity_create_no_collide_hook.install();

    // Handle dropping amps on death
    entity_maybe_die_patch.install();
    item_get_oldest_dynamic_patch.install();

    // Override rcon command whitelist
    write_mem_ptr(0x0046C794 + 1, g_rcon_cmd_whitelist);
    write_mem_ptr(0x0046C7D1 + 2, g_rcon_cmd_whitelist + std::size(g_rcon_cmd_whitelist));

    // Additional server config
    dedicated_server_load_config_patch.install();
    dedicated_server_load_post_map_patch.install();

    // Apply customized spawn protection duration
    spawn_protection_duration_patch.install();

    // Detect if player joining to the server is a browser
    detect_browser_player_patch.install();

    // Critical hits and hit sounds
    entity_damage_hook.install();

    // Item replacements
    item_lookup_type_hook.install();

    // Item respawn time overrides
    // item_create_hook.install();

    // Default player weapon class and ammo override
    find_default_weapon_for_entity_hook.install();
    give_default_weapon_ammo_hook.install();
    //spawn_player_sync_ammo_hook.install();

    init_server_commands();

    // Remove level prefix restriction (dm/ctf) for 'level' command and dedicated_server.txt
    AsmWriter(0x004350FE).nop(2);
    AsmWriter(0x0046E179).nop(2);

    // In Multi -> Create game fix level filtering so 'pdm' and 'pctf' is supported
    multi_is_level_matching_game_type_hook.install();

    // Allow disabling mod name announcement
    get_mod_name_require_client_mod_hook.install();

    // Fix items not being respawned after time in ms wraps around (~25 days)
    AsmWriter(0x004599DB).nop(2);

    // Fix sending ping packets after time in ms wraps around (~25 days)
    send_ping_time_wrap_fix.install();

    // Ignore obj_update position for some time after teleportation
    process_obj_update_set_pos_injection.install();

    // Customized dedicated server console message when player joins
    multi_on_new_player_injection.install();
    AsmWriter(0x0047B061, 0x0047B064).add(asm_regs::esp, 0x14);

    // DF respawn point selection logic
    multi_respawn_level_init_hook.install();
    multi_respawn_create_point_hook.install();
    multi_respawn_get_next_point_hook.install();
    item_create_hook.install(); // also used for respawn time overrides

    // Support forcing player character
    multi_spawn_player_server_side_hook.install();

    // Hook lag compensation functions to calculate accuracy only for weapons with bullets
    // Note: weapons with bullets (projectiles) are created twice server-side so hooking weapon_create would
    // be problematic (PF went this way and its accuracy stat is broken)
    multi_lag_comp_handle_hit_hook.install();
    multi_lag_comp_weapon_fire_hook.install();

    // Set lower bound of server max players clamp range to 1 (instead of 2)
    write_mem<i8>(0x0046DD4F + 1, 1);

    // Reduce limbo duration if server is empty
    multi_limbo_init_injection.install();

    // Shuffle rotation when the last map in the list is loaded
    multi_level_init_injection.install();

    // Check if round is finished or if overtime should begin
    multi_check_for_round_end_hook.install();
}

void server_do_frame()
{
    server_vote_do_frame();
    match_do_frame();
    process_delayed_kicks();
}

void server_on_limbo_state_enter()
{
    g_is_overtime = false;
    g_prev_level = rf::level.filename.c_str();
    server_vote_on_limbo_state_enter();

    // Clear save data for all players
    auto player_list = SinglyLinkedList{rf::player_list};
    for (auto& player : player_list) {
        auto& pdata = get_player_additional_data(&player);
        pdata.saves.clear();
        pdata.last_teleport_timestamp.invalidate();
        if (g_additional_server_config.stats_message_enabled) {
            send_private_message_with_stats(&player);
        }
    }
}

bool server_is_saving_enabled()
{
    return g_additional_server_config.saving_enabled;
}

bool server_allow_fullbright_meshes()
{
    return g_additional_server_config.allow_fullbright_meshes;
}

bool server_allow_lightmaps_only()
{
    return g_additional_server_config.allow_lightmaps_only;
}

bool server_allow_disable_screenshake()
{
    return g_additional_server_config.allow_disable_screenshake;
}

bool server_no_player_collide()
{
    return g_additional_server_config.no_player_collide;
}

bool server_allow_disable_muzzle_flash()
{
    return g_additional_server_config.allow_disable_muzzle_flash;
}

bool server_apply_click_limiter()
{
    return g_additional_server_config.apply_click_limiter;
}

bool server_weapon_items_give_full_ammo()
{
    return g_additional_server_config.weapon_items_give_full_ammo;
}

const ServerAdditionalConfig& server_get_df_config()
{
    return g_additional_server_config;
}
