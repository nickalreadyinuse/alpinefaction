#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/ShortTypes.h>
#include <patch_common/AsmWriter.h>
#include <common/config/BuildConfig.h>
#include <common/version/version.h>
#include <common/rfproto.h>
#include <common/utils/list-utils.h>
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
#include <toml++/toml.hpp>
#include "server.h"
#include "server_internal.h"
#include "alpine_packets.h"
#include "multi.h"
#include "../os/console.h"
#include "../misc/player.h"
#include "../misc/alpine_settings.h"
#include "../main/main.h"
#include "../misc/achievements.h"
#include "../rf/file/file.h"
#include "../rf/math/vector.h"
#include "../rf/math/matrix.h"
#include "../rf/player/player.h"
#include "../rf/item.h"
#include "../rf/gameseq.h"
#include "../rf/misc.h"
#include "../rf/ai.h"
#include "../rf/multi.h"
#include "../rf/parse.h"
#include "../rf/weapon.h"
#include "../rf/entity.h"
#include "../rf/os/os.h"
#include "../rf/os/timer.h"
#include "../rf/level.h"
#include "../rf/collide.h"
#include "../purefaction/pf.h"

bool g_dedicated_launched_from_ads = false; // was the server launched from an ads file?
std::string g_ads_config_name = "";
bool g_ads_minimal_server_info = false; // print only minimal server info when launching
int g_ads_loaded_version = ADS_VERSION;

rf::CmdLineParam& get_ads_cmd_line_param()
{
    static rf::CmdLineParam ads_param{"-ads", "", true};
    return ads_param;
}

// print only minimal server info when launching
rf::CmdLineParam& get_min_cmd_line_param()
{
    static rf::CmdLineParam min_param{"-min", "", false};
    return min_param;
}

void handle_min_param()
{
    g_ads_minimal_server_info = get_min_cmd_line_param().found();
    //rf::console::print("checking min switch... {}", g_ads_minimal_server_info);
}

static rf::NetGameType parse_game_type(const std::string& s)
{
    if (s == "TDM")
        return rf::NetGameType::NG_TYPE_TEAMDM;
    if (s == "CTF")
        return rf::NetGameType::NG_TYPE_CTF;
    return rf::NetGameType::NG_TYPE_DM;
}

static SpawnLifeConfig parse_spawn_life_config(const toml::table& t, SpawnLifeConfig c)
{
    if (auto x = t["enabled"].value<bool>())
        c.enabled = *x;

    if (c.enabled) {
        if (auto v = t["value"].value<float>())
            c.set_value(*v);
    }

    return c;
}

static WelcomeMessageConfig parse_welcome_message_config(const toml::table& t, WelcomeMessageConfig c)
{
    if (auto x = t["enabled"].value<bool>())
        c.enabled = *x;

    if (c.enabled) {
        if (auto v = t["text"].value<std::string>())
            c.set_welcome_message(*v);
    }

    return c;
}

static SpawnProtectionConfig parse_spawn_protection_config(const toml::table& t, SpawnProtectionConfig c)
{
    if (auto x = t["enabled"].value<bool>())
        c.enabled = *x;

    if (c.enabled) {
        if (auto v = t["duration"].value<float>())
            c.set_duration(*v);
        if (auto v = t["use_powerup"].value<bool>())
            c.use_powerup = *v;
    }

    return c;
}

static NewSpawnLogicConfig parse_spawn_logic_config(const toml::table& t, NewSpawnLogicConfig c)
{
    if (auto x = t["respect_team_spawns"].value<bool>())
        c.respect_team_spawns = *x;
    if (auto x = t["try_avoid_players"].value<bool>())
        c.try_avoid_players = *x;
    if (auto x = t["always_avoid_last"].value<bool>())
        c.always_avoid_last = *x;
    if (auto x = t["always_use_furthest"].value<bool>())
        c.always_use_furthest = *x;
    if (auto x = t["only_avoid_enemies"].value<bool>())
        c.only_avoid_enemies = *x;
    if (auto x = t["dynamic_respawns"].value<bool>())
        c.dynamic_respawns = *x;

    if (c.dynamic_respawns) {
        c.dynamic_respawn_items.clear();
        if (auto arr = t["dynamic_respawn_items"].as_array()) {
            for (auto& elem : *arr) {
                if (auto sub = elem.as_table()) {
                    NewSpawnLogicRespawnItemConfig entry;
                    if (auto name = (*sub)["item_name"].value<std::string>())
                        entry.item_name = *name;
                    if (auto pts = (*sub)["min_respawn_points"].value<int>())
                        entry.min_respawn_points = *pts;
                    c.dynamic_respawn_items.push_back(std::move(entry));
                }
            }
        }
    }

    return c;
}

static KillRewardConfig parse_kill_reward_config(const toml::table& t, KillRewardConfig c)
{
    if (auto x = t["health"].value<float>())
        c.kill_reward_health = *x;
    if (auto x = t["armor"].value<float>())
        c.kill_reward_armor = *x;
    if (auto x = t["effective_health"].value<float>())
        c.kill_reward_effective_health = *x;
    if (auto x = t["health_is_super"].value<bool>())
        c.kill_reward_health_super = *x;
    if (auto x = t["armor_is_super"].value<bool>())
        c.kill_reward_armor_super = *x;

    return c;
}

// parse toml rules
// for base rules, load all speciifed. For not specified, defaults are in struct
// for level-specific rules, start with base rules and load anything specified beyond that
AlpineServerConfigRules parse_server_rules(const toml::table& t, const AlpineServerConfigRules& base_rules)
{
    AlpineServerConfigRules o = base_rules;

    if (auto v = t["time_limit"].value<float>())
        o.set_time_limit(*v);
    if (auto v = t["individual_kill_limit"].value<int>())
        o.set_individual_kill_limit(*v);
    if (auto v = t["team_kill_limit"].value<int>())
        o.set_team_kill_limit(*v);
    if (auto v = t["cap_limit"].value<int>())
        o.set_cap_limit(*v);
    if (auto v = t["geo_limit"].value<int>())
        o.set_geo_limit(*v);

    if (auto v = t["team_damage"].value<bool>())
        o.team_damage   = *v;
    if (auto v = t["fall_damage"].value<bool>())
        o.fall_damage   = *v;
    if (auto v = t["weapons_stay"].value<bool>())
        o.weapons_stay  = *v;
    if (auto v = t["force_respawn"].value<bool>())
        o.force_respawn = *v;
    if (auto v = t["balance_teams"].value<bool>())
        o.balance_teams = *v;
    if (auto v = t["ideal_player_count"].value<int>())
        o.set_ideal_player_count(*v);
    if (auto v = t["saving_enabled"].value<bool>())
        o.saving_enabled = *v;
    if (auto v = t["flag_dropping"].value<bool>())
        o.flag_dropping = *v;
    if (auto v = t["flag_captures_while_stolen"].value<bool>())
        o.flag_captures_while_stolen = *v;
    if (auto v = t["flag_return_time"].value<int>())
        o.set_flag_return_time(*v);
    if (auto v = t["drop_amps"].value<bool>())
        o.drop_amps = *v;
    if (auto v = t["weapon_pickups_give_full_ammo"].value<bool>())
        o.weapon_items_give_full_ammo = *v;
    if (auto v = t["infinite_reloads"].value<bool>())
        o.weapon_infinite_magazines = *v;

    if (auto sub = t["spawn_life"].as_table())
        o.spawn_life  = parse_spawn_life_config(*sub, o.spawn_life);
    if (auto sub = t["spawn_armor"].as_table())
        o.spawn_armour = parse_spawn_life_config(*sub, o.spawn_armour);
    if (auto sub = t["spawn_protection"].as_table())
        o.spawn_protection = parse_spawn_protection_config(*sub, o.spawn_protection);
    if (auto sub = t["spawn_selection"].as_table())
        o.spawn_logic = parse_spawn_logic_config(*sub, o.spawn_logic);
    if (auto sub = t["kill_rewards"].as_table())
        o.kill_rewards = parse_kill_reward_config(*sub, o.kill_rewards);
    if (auto sub = t["welcome_message"].as_table())
        o.welcome_message = parse_welcome_message_config(*sub, o.welcome_message);

    //o.weapon_stay_exemptions.exemptions.clear();
    o.weapon_stay_exemptions.add("shoulder_cannon", true); // stock default

    if (auto arr = t["weapon_stay_exemptions"].as_array()) {
        for (auto& node : *arr) {
            if (auto tbl = node.as_table()) {
                if (auto nameOpt = (*tbl)["weapon_name"].value<std::string>()) {
                    bool ex = (*tbl)["exempt"].value<bool>().value_or(true); // default true if not specified
                    o.weapon_stay_exemptions.add(*nameOpt, ex);
                }
            }
        }
    }

    if (auto arr = t["item_replacements"].as_array()) {
        for (auto& node : *arr) {
            if (auto tbl = node.as_table()) {
                auto from = (*tbl)["original"].value_or<std::string>("");
                auto to = (*tbl)["replacement"].value_or<std::string>("");
                if (!o.add_item_replacement(from, to))
                    xlog::warn("Invalid replacement {}→{}", from, to);
            }
        }
    }

    if (auto arr = t["item_respawn_time_overrides"].as_array()) {
        for (auto& node : *arr) {
            if (auto tbl = node.as_table()) {
                auto name = (*tbl)["item_name"].value_or<std::string>("");
                auto ms = (*tbl)["respawn_ms"].value_or<int>(0);
                if (!o.set_item_respawn_time(name, ms))
                    xlog::warn("Invalid respawn override for '{}'", name);
            }
        }
    }

    return o;
}

static VoteConfig parse_vote_config(const toml::table& t)
{
    VoteConfig v;
    if (auto x = t["enabled"].value<bool>())
        v.enabled = *x;

    if (v.enabled) {
        if (auto x = t["ignore_nonvoters"].value<bool>())
            v.ignore_nonvoters = *x;
        if (auto x = t["time"].value<float>())
            v.set_time_limit_seconds(*x);
    }
    return v;
}

static InactivityConfig parse_inactivity_config(const toml::table &t)
{
    InactivityConfig o;
    if (auto v = t["enabled"].value<bool>())
        o.enabled = *v;

    if (o.enabled) {
        if (auto v = t["new_player_grace"].value<float>())
            o.set_new_player_grace(*v);
        if (auto v = t["allowed_inactive"].value<float>())
            o.set_allowed_inactive(*v);
        if (auto v = t["warning_duration"].value<float>())
            o.set_warning_duration(*v);
        if (auto v = t["kick_message"].value<std::string>())
            o.kick_message = *v;
    }
    return o;
}

static DamageNotificationConfig parse_damage_notification_config(const toml::table& t)
{
    DamageNotificationConfig o;
    if (auto v = t["enabled"].value<bool>())
        o.enabled = *v;

    if (o.enabled) {
        if (auto v = t["support_legacy"].value<bool>())
            o.support_legacy_clients = *v;
    }
    return o;
}

static AlpineRestrictConfig parse_alpine_restrict_config(const toml::table &t)
{
    AlpineRestrictConfig o;
    if (auto v = t["advertise_alpine"].value<bool>())
        o.advertise_alpine = *v;
    if (auto v = t["clients_require_alpine"].value<bool>())
        o.clients_require_alpine = *v;

    if (o.clients_require_alpine) {
        if (auto v = t["reject_non_alpine_clients"].value<bool>())
            o.reject_non_alpine_clients = *v;
        if (auto v = t["alpine_server_version_enforce_min"].value<bool>())
            o.alpine_server_version_enforce_min = *v;
        if (auto v = t["alpine_require_release_build"].value<bool>())
            o.alpine_require_release_build = *v;
        if (auto v = t["only_welcome_alpine"].value<bool>())
            o.only_welcome_alpine = *v;
        if (auto v = t["no_player_collide"].value<bool>())
            o.no_player_collide = *v;
        if (auto v = t["location_pinging"].value<bool>())
            o.location_pinging = *v;
        if (auto sub = t["vote_match"].as_table())
            o.vote_match = parse_vote_config(*sub);
    }
    return o;
}

void load_ads_server_config(std::string ads_config_name)
{
    rf::console::print("Loading and applying server configuration from {}...\n\n", ads_config_name);

    toml::table tbl;
    try
    {
        tbl = toml::parse_file(ads_config_name);
    }
    catch (const toml::parse_error& err)
    {
        rf::console::print("  [ERROR] failed to parse {}: {}\n", ads_config_name, err.description());
        return;
    }

    AlpineServerConfig cfg;

    // core config

    if (auto v = tbl["ads_version"].value<int>())
        g_ads_loaded_version = *v;

    if (auto v = tbl["server_name"].value<std::string>())
        cfg.server_name = *v;

    if (auto v = tbl["game_type"].value<std::string>())
        cfg.game_type = parse_game_type(*v);

    if (auto v = tbl["max_players"].value<int>())
        cfg.set_max_players((int)*v);

    if (auto v = tbl["password"].value<std::string>())
        cfg.set_password(*v);

    if (auto v = tbl["rcon_password"].value<std::string>())
        cfg.set_rcon_password(*v);

    if (auto v = tbl["upnp"].value<bool>())
        cfg.upnp_enabled = *v;

    if (auto v = tbl["dynamic_rotation"].value<bool>())
        cfg.dynamic_rotation = *v;

    if (auto v = tbl["require_client_mod"].value<bool>())
        cfg.require_client_mod = *v;

    if (auto v = tbl["gaussian_spread"].value<bool>())
        cfg.gaussian_spread = *v;

    if (auto v = tbl["send_stats_message"].value<bool>())
        cfg.stats_message_enabled = *v;

    if (auto v = tbl["allow_fullbright_meshes"].value<bool>())
        cfg.allow_fullbright_meshes = *v;

    if (auto v = tbl["allow_lightmap_mode"].value<bool>())
        cfg.allow_lightmaps_only = *v;

    if (auto v = tbl["allow_disable_screenshake"].value<bool>())
        cfg.allow_disable_screenshake = *v;

    if (auto v = tbl["allow_disable_muzzle_flash"].value<bool>())
        cfg.allow_disable_muzzle_flash = *v;

    if (auto v = tbl["allow_unlimited_fps"].value<bool>())
        cfg.allow_unlimited_fps = *v;

    if (auto v = tbl["use_sp_damage_calculation"].value<bool>())
        cfg.use_sp_damage_calculation = *v;

    if (auto tblInact = tbl["inactivity"].as_table())
        cfg.inactivity_config = parse_inactivity_config(*tblInact);

    if (auto tblInact = tbl["damage_notifications"].as_table())
        cfg.damage_notification_config = parse_damage_notification_config(*tblInact);

    if (auto tblRestr = tbl["alpine_restrict"].as_table())
        cfg.alpine_restricted_config = parse_alpine_restrict_config(*tblRestr);

    if (auto t = tbl["vote_kick"].as_table())
        cfg.vote_kick = parse_vote_config(*t);
    if (auto t = tbl["vote_level"].as_table())
        cfg.vote_level = parse_vote_config(*t);
    if (auto t = tbl["vote_extend"].as_table())
        cfg.vote_extend = parse_vote_config(*t);
    if (auto t = tbl["vote_restart"].as_table())
        cfg.vote_restart = parse_vote_config(*t);
    if (auto t = tbl["vote_next"].as_table())
        cfg.vote_next = parse_vote_config(*t);
    if (auto t = tbl["vote_rand"].as_table())
        cfg.vote_rand = parse_vote_config(*t);
    if (auto t = tbl["vote_previous"].as_table())
        cfg.vote_previous = parse_vote_config(*t);

    // base rules
    if (auto base = tbl["base"].as_table()) {
        if (auto rules = (*base)["rules"].as_table()) {
            cfg.base_rules = parse_server_rules(*rules, cfg.base_rules);
        }
    }

    // levels
    if (auto lv = tbl["levels"]; lv && lv.is_array())
    {
        auto* arr = lv.as_array();
        for (auto& elem : *arr)
        {
            if (!elem.is_table())
                continue;

            auto& lvl_tbl = *elem.as_table();

            AlpineServerConfigLevelEntry entry;

            auto tmp_filename = lvl_tbl["filename"].value_or<std::string>("");

            rf::File file;
            bool found = file.find(tmp_filename.c_str());

            if (!found) {
                rf::console::print("----> Level {} is not installed!\n\n", tmp_filename);
                continue;
            }

            entry.level_filename = tmp_filename;
            entry.rule_overrides = cfg.base_rules; // handle base rules for levels without any specific rules

            // per-level rule override
            if (auto* over = lvl_tbl["rules"].as_table()) {
                entry.rule_overrides = parse_server_rules(*over, cfg.base_rules);
            }

            cfg.levels.push_back(std::move(entry));
        }
    }

    g_alpine_server_config = std::move(cfg);
}

std::string get_game_type_string(rf::NetGameType game_type) {
    std::string out_string;
    switch (game_type) {
        case rf::NetGameType::NG_TYPE_TEAMDM:
            out_string = "TDM";
            break;
         case rf::NetGameType::NG_TYPE_CTF:
            out_string = "CTF";
            break;
         default:
             out_string = "DM";
             break;
    }
    return out_string;
}

void print_rules(const AlpineServerConfigRules& rules, bool base = true)
{
    const auto& b = g_alpine_server_config.base_rules;

    // time limit
    if (base || rules.time_limit != b.time_limit)
        rf::console::print("  Time limit:                            {} min\n", rules.time_limit / 60.0f);

    // score/cap limit
    switch (rf::netgame.type) {
    case rf::NetGameType::NG_TYPE_TEAMDM:
        if (base || rules.team_kill_limit != b.team_kill_limit)
            rf::console::print("  Team score limit:                      {}\n", rules.team_kill_limit);
        break;
    case rf::NetGameType::NG_TYPE_CTF:
        if (base || rules.cap_limit != b.cap_limit)
            rf::console::print("  Flag capture limit:                    {}\n", rules.cap_limit);
        break;
    default:
        if (base || rules.individual_kill_limit != b.individual_kill_limit)
            rf::console::print("  Player score limit:                    {}\n", rules.individual_kill_limit);
        break;
    }

    // common limits & flags
    if (base || rules.geo_limit != b.geo_limit)
        rf::console::print("  Geomod crater limit:                   {}\n", rules.geo_limit);
    if (base || rules.team_damage != b.team_damage)
        rf::console::print("  Team damage:                           {}\n", rules.team_damage);
    if (base || rules.fall_damage != b.fall_damage)
        rf::console::print("  Fall damage:                           {}\n", rules.fall_damage);
    if (base || rules.weapons_stay != b.weapons_stay)
        rf::console::print("  Weapon stay:                           {}\n", rules.weapons_stay);
    if (base || rules.force_respawn != b.force_respawn)
        rf::console::print("  Force respawn:                         {}\n", rules.force_respawn);
    if (base || rules.balance_teams != b.balance_teams)
        rf::console::print("  Balance teams:                         {}\n", rules.balance_teams);
    if (base || rules.ideal_player_count != b.ideal_player_count)
        rf::console::print("  Ideal player count:                    {}\n", rules.ideal_player_count);
    if (base || rules.saving_enabled != b.saving_enabled)
        rf::console::print("  Position saving:                       {}\n", rules.saving_enabled);

    if (rf::netgame.type == rf::NetGameType::NG_TYPE_CTF) {
        if (base || rules.flag_dropping != b.flag_dropping)
            rf::console::print("  Flag dropping:                         {}\n", rules.flag_dropping);
        if (base || rules.flag_captures_while_stolen != b.flag_captures_while_stolen)
            rf::console::print("  Flag captures while stolen:            {}\n", rules.flag_captures_while_stolen);
        if (base || rules.ctf_flag_return_time_ms != b.ctf_flag_return_time_ms)
            rf::console::print("  Flag return time:                      {} sec\n", rules.ctf_flag_return_time_ms / 1000.0f);
    }

    if (base || rules.drop_amps != b.drop_amps)
        rf::console::print("  Drop amps:                             {}\n", rules.drop_amps);
    if (base || rules.weapon_items_give_full_ammo != b.weapon_items_give_full_ammo)
        rf::console::print("  Weapon pickups give full ammo:         {}\n", rules.weapon_items_give_full_ammo);
    if (base || rules.weapon_infinite_magazines != b.weapon_infinite_magazines)
        rf::console::print("  Infinite reloads:                      {}\n", rules.weapon_infinite_magazines);

    if (base || rules.welcome_message.enabled != b.welcome_message.enabled ||
        (rules.welcome_message.enabled && rules.welcome_message.welcome_message != b.welcome_message.welcome_message)) {
        rf::console::print("  Welcome message:                       {}\n", rules.welcome_message.enabled);
        if (rules.welcome_message.enabled) {
            rf::console::print("    Text:                                {}\n", rules.welcome_message.welcome_message);
        }
    }

    // spawn life
    if (base || rules.spawn_life.enabled != b.spawn_life.enabled ||
        (rules.spawn_life.enabled && rules.spawn_life.value != b.spawn_life.value)) {
        rf::console::print("  Custom spawn health:                   {}\n", rules.spawn_life.enabled);
        if (rules.spawn_life.enabled) {
            rf::console::print("    Value:                               {}\n", rules.spawn_life.value);
        }
    }

    // spawn armour
    if (base || rules.spawn_armour.enabled != b.spawn_armour.enabled ||
        (rules.spawn_armour.enabled && rules.spawn_armour.value != b.spawn_armour.value)) {
        rf::console::print("  Custom spawn armor:                    {}\n", rules.spawn_armour.enabled);
        if (rules.spawn_armour.enabled) {
            rf::console::print("    Value:                               {}\n", rules.spawn_armour.value);
        }
    }
     // spawn protection
    if (base || rules.spawn_protection.enabled != b.spawn_protection.enabled ||
        (rules.spawn_protection.enabled && (rules.spawn_protection.duration != b.spawn_protection.duration ||
                                            rules.spawn_protection.use_powerup != b.spawn_protection.use_powerup))) {
        rf::console::print("  Spawn protection:                      {}\n", rules.spawn_protection.enabled);
        if (rules.spawn_protection.enabled) {
            rf::console::print("    Duration:                            {} sec\n", rules.spawn_protection.duration / 1000.0f);
            rf::console::print("    Use powerup:                         {}\n", rules.spawn_protection.use_powerup);
        }
    }

    // spawn logic
    bool logicDiff = rules.spawn_logic.respect_team_spawns != b.spawn_logic.respect_team_spawns ||
                     rules.spawn_logic.try_avoid_players != b.spawn_logic.try_avoid_players ||
                     rules.spawn_logic.always_avoid_last != b.spawn_logic.always_avoid_last ||
                     rules.spawn_logic.always_use_furthest != b.spawn_logic.always_use_furthest ||
                     rules.spawn_logic.only_avoid_enemies != b.spawn_logic.only_avoid_enemies ||
                     rules.spawn_logic.dynamic_respawns != b.spawn_logic.dynamic_respawns ||
                     rules.spawn_logic.dynamic_respawn_items.size() != b.spawn_logic.dynamic_respawn_items.size();

    if (base || logicDiff) {
        rf::console::print("  Spawn logic:\n");
        rf::console::print("    Respect team spawns:                 {}\n", rules.spawn_logic.respect_team_spawns);
        rf::console::print("    Try avoid players:                   {}\n", rules.spawn_logic.try_avoid_players);
        rf::console::print("    Always avoid last:                   {}\n", rules.spawn_logic.always_avoid_last);
        rf::console::print("    Always use furthest:                 {}\n", rules.spawn_logic.always_use_furthest);
        rf::console::print("    Only avoid enemies:                  {}\n", rules.spawn_logic.only_avoid_enemies);
        rf::console::print("    Create item dynamic respawns:        {}\n", rules.spawn_logic.dynamic_respawns);

        if (rules.spawn_logic.dynamic_respawns) {
            for (auto& item : rules.spawn_logic.dynamic_respawn_items) {
                rf::console::print("      Dynamic respawn item:              {} (threshold: {})\n", item.item_name, item.min_respawn_points);
            }
        }
    }

    // kill rewards
    bool rewardDiff = rules.kill_rewards.kill_reward_health != b.kill_rewards.kill_reward_health ||
                      rules.kill_rewards.kill_reward_armor != b.kill_rewards.kill_reward_armor ||
                      rules.kill_rewards.kill_reward_effective_health != b.kill_rewards.kill_reward_effective_health ||
                      rules.kill_rewards.kill_reward_health_super != b.kill_rewards.kill_reward_health_super ||
                      rules.kill_rewards.kill_reward_armor_super != b.kill_rewards.kill_reward_armor_super;

    if (base || rewardDiff) {
        rf::console::print("  Kill rewards:\n");
        rf::console::print("    Health:                              {}\n", rules.kill_rewards.kill_reward_health);
        rf::console::print("    Armor:                               {}\n", rules.kill_rewards.kill_reward_armor);
        rf::console::print("    Effective health:                    {}\n", rules.kill_rewards.kill_reward_effective_health);
        rf::console::print("    Health is super:                     {}\n", rules.kill_rewards.kill_reward_health_super);
        rf::console::print("    Armor is super:                      {}\n", rules.kill_rewards.kill_reward_armor_super);
    }
    /*
    // weap stay exemptions
    if ( base || rules.weapon_stay_exemptions.exemptions != b.weapon_stay_exemptions.exemptions )
    {
        rf::console::print("  Weapon stay exemptions:\n");
        for (auto const& e : rules.weapon_stay_exemptions.exemptions)
        {
            rf::console::print("    {}: {}\n", e.weapon_name, e.exemption_enabled ? "exempt" : "not exempt");
        }
    }

    // item replacements
    if (base || rules.item_replacements != b.item_replacements) {
        rf::console::print("  Item replacements:\n");
        for (auto const& [orig, repl] : rules.item_replacements) {
            rf::console::print("    {} -> {}\n", orig, repl);
        }
    }

    // item spawn time overrides
    if (base || rules.item_respawn_time_overrides != b.item_respawn_time_overrides) {
        rf::console::print("  Item respawn time overrides:\n");
        for (auto const& [item, ms] : rules.item_respawn_time_overrides) {
            rf::console::print("    {}: {} ms\n", item, ms);
        }
    }*/

    //
    // Weapon stay exemptions
    //
    // Print the header if we're printing the full base, or if there's
    // at least one exemption that's different from base
    bool anyExemptionChanged = std::any_of(
        rules.weapon_stay_exemptions.exemptions.begin(),
        rules.weapon_stay_exemptions.exemptions.end(),
        [&](auto const& e){
            auto it = std::find_if(
                b.weapon_stay_exemptions.exemptions.begin(),
                b.weapon_stay_exemptions.exemptions.end(),
                [&](auto const& be){ return be.weapon_name == e.weapon_name
                                       && be.exemption_enabled == e.exemption_enabled; }
            );
            return it == b.weapon_stay_exemptions.exemptions.end();
        }
    );

    if (base || anyExemptionChanged) {
        rf::console::print("  Weapon stay exemptions:\n");
        for (auto const& e : rules.weapon_stay_exemptions.exemptions) {
            bool unchanged = std::any_of(
                b.weapon_stay_exemptions.exemptions.begin(),
                b.weapon_stay_exemptions.exemptions.end(),
                [&](auto const& be){ return be.weapon_name == e.weapon_name
                                       && be.exemption_enabled == e.exemption_enabled; }
            );
            if (base || !unchanged) {
                rf::console::print("    {:<20}: {}\n",
                    e.weapon_name,
                    e.exemption_enabled ? "exempt" : "not exempt"
                );
            }
        }
    }

    //
    // Item replacements
    //
    bool anyReplacementChanged = std::any_of(
        rules.item_replacements.begin(),
        rules.item_replacements.end(),
        [&](auto const& kv){
            auto it = b.item_replacements.find(kv.first);
            return it == b.item_replacements.end() || it->second != kv.second;
        }
    );

    if (base || anyReplacementChanged) {
        rf::console::print("  Item replacements:\n");
        for (auto const& [orig, repl] : rules.item_replacements) {
            auto it = b.item_replacements.find(orig);
            bool unchanged = (it != b.item_replacements.end() && it->second == repl);
            if (base || !unchanged) {
                rf::console::print("    {} -> {}\n", orig, repl);
            }
        }
    }

    //
    // Item respawn time overrides
    //
    bool anyRespawnChanged = std::any_of(
        rules.item_respawn_time_overrides.begin(),
        rules.item_respawn_time_overrides.end(),
        [&](auto const& kv){
            auto it = b.item_respawn_time_overrides.find(kv.first);
            return it == b.item_respawn_time_overrides.end() || it->second != kv.second;
        }
    );

    if (base || anyRespawnChanged) {
        rf::console::print("  Item respawn time overrides:\n");
        for (auto const& [item, ms] : rules.item_respawn_time_overrides) {
            auto it = b.item_respawn_time_overrides.find(item);
            bool unchanged = (it != b.item_respawn_time_overrides.end() && it->second == ms);
            if (base || !unchanged) {
                rf::console::print("    {}: {} ms\n", item, ms);
            }
        }
    }
}

void print_alpine_dedicated_server_config_info(bool verbose) {
    auto& netgame = rf::netgame;
    const auto& cfg = g_alpine_server_config;

    rf::console::print("\n---- Core configuration ----\n");
    rf::console::print("  Server port:                           {} (UDP)\n", netgame.server_addr.port);
    rf::console::print("  Server name:                           {}\n", netgame.name);
    rf::console::print("  Password:                              {}\n", netgame.password);
    rf::console::print("  Rcon password:                         {}\n", rf::rcon_password);
    rf::console::print("  Max players:                           {}\n", netgame.max_players);
    rf::console::print("  Game type:                             {}\n", get_game_type_string(netgame.type));
    rf::console::print("  Levels in rotation:                    {}\n", cfg.levels.size());
    rf::console::print("  Dynamic rotation:                      {}\n", cfg.dynamic_rotation);

    if (rf::mod_param.found()) {
        rf::console::print("  TC mod loaded:                         {}\n", rf::mod_param.get_arg());
        rf::console::print("  Clients must match TC mod:             {}\n", cfg.require_client_mod);
    }    

    // minimal server config printing
    if (!verbose) {
        rf::console::print("\n----> Enter sv_printconfig to print verbose server config.\n\n");
        return;
    }

    rf::console::print("  Gaussian bullet spread:                {}\n", cfg.gaussian_spread);
    rf::console::print("  End of round stats message:            {}\n", cfg.stats_message_enabled);
    rf::console::print("  Allow fullbright meshes:               {}\n", cfg.allow_fullbright_meshes);
    rf::console::print("  Allow disable screenshake:             {}\n", cfg.allow_disable_screenshake);
    rf::console::print("  Allow lightmap only mode:              {}\n", cfg.allow_lightmaps_only);
    rf::console::print("  Allow disable muzzle flash:            {}\n", cfg.allow_disable_muzzle_flash);
    rf::console::print("  Allow disable 240 FPS cap:             {}\n", cfg.allow_unlimited_fps);
    rf::console::print("  SP-style damage calculation:           {}\n", cfg.use_sp_damage_calculation);

    rf::console::print("\n---- Player inactivity settings ----\n");
    rf::console::print("  Kick inactive players:                 {}\n", cfg.inactivity_config.enabled);
    if (cfg.inactivity_config.enabled) {
        rf::console::print("    New player grace period:             {} sec\n", cfg.inactivity_config.new_player_grace_ms / 1000.0f);
        rf::console::print("    Allowed inactivity time:             {} sec\n", cfg.inactivity_config.allowed_inactive_ms / 1000.0f);
        rf::console::print("    Warning duration:                    {} sec\n", cfg.inactivity_config.warning_duration_ms / 1000.0f);
        rf::console::print("    Kick message:                        {}\n", cfg.inactivity_config.kick_message);
    }

    rf::console::print("\n---- Damage notification settings ----\n");
    rf::console::print("  Damage notifications:                  {}\n", cfg.damage_notification_config.enabled);
    if (cfg.damage_notification_config.enabled) {
        rf::console::print("    Legacy client compatibility:         {}\n", cfg.damage_notification_config.support_legacy_clients);
    }

    rf::console::print("\n---- Alpine restriction settings ----\n");
    rf::console::print("  Advertise Alpine:                      {}\n", cfg.alpine_restricted_config.advertise_alpine);
    rf::console::print("  Clients require Alpine:                {}\n", cfg.alpine_restricted_config.clients_require_alpine);
    if (cfg.alpine_restricted_config.clients_require_alpine) {
        rf::console::print("    Reject non-Alpine clients:           {}\n", cfg.alpine_restricted_config.reject_non_alpine_clients);
        rf::console::print("    Enforce min server version:          {}\n", cfg.alpine_restricted_config.alpine_server_version_enforce_min);
        rf::console::print("    Require release build:               {}\n", cfg.alpine_restricted_config.alpine_require_release_build);
        rf::console::print("    Only welcome Alpine players:         {}\n", cfg.alpine_restricted_config.only_welcome_alpine);
        rf::console::print("    No player collide:                   {}\n", cfg.alpine_restricted_config.no_player_collide);
        rf::console::print("    Location pinging:                    {}\n", cfg.alpine_restricted_config.location_pinging);
        rf::console::print("\n    -- Match mode --\n");
        auto& vm = cfg.alpine_restricted_config.vote_match;
        rf::console::print("      Enabled:                           {}\n", vm.enabled);
        if (vm.enabled) {
            rf::console::print("      Vote ignores nonvoters:            {}\n", vm.ignore_nonvoters);
            rf::console::print("      Vote time limit:                   {} sec\n", vm.time_limit_seconds);
        }
    }

    rf::console::print("\n---- Voting settings ----\n");

    auto print_vote = [&](std::string name, const VoteConfig& v) {
        rf::console::print("  {}                 {}\n", name, v.enabled);
        if (v.enabled) {
            rf::console::print("    Ignore nonvoters:                    {}\n", v.ignore_nonvoters);
            rf::console::print("    Time limit:                          {} sec\n", v.time_limit_seconds);
        }
    };

    print_vote("Vote kick enabled:    ", cfg.vote_kick);
    print_vote("Vote level enabled:   ", cfg.vote_level);
    print_vote("Vote extend enabled:  ", cfg.vote_extend);
    print_vote("Vote restart enabled: ", cfg.vote_restart);
    print_vote("Vote next enabled:    ", cfg.vote_next);
    print_vote("Vote random enabled:  ", cfg.vote_rand);
    print_vote("Vote previous enabled:", cfg.vote_previous);

    rf::console::print("\n---- Base rules ----\n");
    print_rules(cfg.base_rules);

    rf::console::print("\n---- Level rotation ----\n");
    for (auto const& lvl : cfg.levels) {
        //rf::console::print("Level: {}\n", lvl.level_filename);
        rf::console::print("{}\n", lvl.level_filename);
        print_rules(lvl.rule_overrides, false);
        //rf::console::print("\n");
    }
    rf::console::print("\n");
}

void initialize_core_alpine_dedicated_server_settings(rf::NetGameInfo& netgame, const AlpineServerConfig& cfg, bool on_launch) {
    netgame.name = cfg.server_name.c_str();

    netgame.password = cfg.password.c_str();

    // note: length is truncated before saving to g_additional_server_config.rcon_password
    std::strncpy(rf::rcon_password, cfg.rcon_password.c_str(), sizeof(rf::rcon_password) - 1);
    rf::rcon_password[sizeof(rf::rcon_password) - 1] = '\0'; // null terminator
    
    if (on_launch) {
        netgame.type = cfg.game_type;
    }
    else {
        if (netgame.type != cfg.game_type) {
            rf::console::print("----> You must relaunch the server to change the gametype.\n");
        }
    }
    
    netgame.max_players = cfg.max_players;

    // other core settings are referenced directly in the structure and do not need to be initialized here
}

void apply_alpine_dedicated_server_rules(rf::NetGameInfo& netgame, const AlpineServerConfigRules& r)
{
    netgame.max_time_seconds = r.time_limit;
    switch (netgame.type) {
        case rf::NetGameType::NG_TYPE_TEAMDM:
            netgame.max_kills = r.team_kill_limit;
            break;
        case rf::NetGameType::NG_TYPE_CTF:
            netgame.max_captures = r.cap_limit;
            break;
        default:
            netgame.max_kills = r.individual_kill_limit;
            break;
    }

    netgame.geomod_limit = r.geo_limit;

    netgame.flags &= ~(rf::NG_FLAG_TEAM_DAMAGE
                     | rf::NG_FLAG_FALL_DAMAGE
                     | rf::NG_FLAG_WEAPON_STAY
                     | rf::NG_FLAG_FORCE_RESPAWN
                     | rf::NG_FLAG_BALANCE_TEAMS);

    if (r.team_damage)   netgame.flags |= rf::NG_FLAG_TEAM_DAMAGE;
    if (r.fall_damage)   netgame.flags |= rf::NG_FLAG_FALL_DAMAGE;
    if (r.weapons_stay)  netgame.flags |= rf::NG_FLAG_WEAPON_STAY;
    if (r.force_respawn) netgame.flags |= rf::NG_FLAG_FORCE_RESPAWN;
    if (r.balance_teams) netgame.flags |= rf::NG_FLAG_BALANCE_TEAMS;
}

void load_and_print_alpine_dedicated_server_config(std::string ads_config_name, bool on_launch) {
    auto& netgame = rf::netgame;
    const auto& cfg = g_alpine_server_config;

    // parse toml file and update values
    // on launch does this before tracker registration
    if (!on_launch)
        load_ads_server_config(ads_config_name);

    initialize_core_alpine_dedicated_server_settings(netgame, cfg, on_launch);

    apply_alpine_dedicated_server_rules(netgame, cfg.base_rules); // base rules

    print_alpine_dedicated_server_config_info(!g_ads_minimal_server_info);

    auto& levels_arr = netgame.levels;
    levels_arr.clear();
    for (auto const& lvlEntry : cfg.levels) {
        levels_arr.add(lvlEntry.level_filename.c_str());
    }
}

void apply_rules_for_current_level()
{
    auto &netgame = rf::netgame;
    auto &cfg     = g_alpine_server_config;

    //g_alpine_server_config_active_rules = level

    // prevent a crash
    if (cfg.levels.size() < 1) {
        return;
    }

    int idx = netgame.current_level_index;

    if (!g_ads_minimal_server_info && rf::level_filename_to_load != cfg.levels[idx].level_filename.c_str()) {
        g_alpine_server_config_active_rules = cfg.base_rules;
        rf::console::print("Applying base rules for manually loaded level {}...\n", rf::level_filename_to_load, cfg.levels[idx].level_filename.c_str());
        return;
    }

    AlpineServerConfigRules const &override_rules =
        (idx >= 0 && idx < (int)cfg.levels.size())
          ? cfg.levels[idx].rule_overrides
          : cfg.base_rules;

    if (!g_ads_minimal_server_info)
        rf::console::print("Applying level-specific rules for server rotation index {} ({})...\n", idx, cfg.levels[idx].level_filename);

    g_alpine_server_config_active_rules = override_rules;

    apply_alpine_dedicated_server_rules(
        netgame,
        override_rules
    );
}

void init_alpine_dedicated_server() {
    // remove stock game weapon stay exemption for fusion
    AsmWriter(0x00459834).jmp(0x00459836);
    AsmWriter(0x004596BA).jmp(0x004596BC);
}

void launch_alpine_dedicated_server() {
    rf::console::print("==================================================================\n");
    rf::console::print("================  Alpine Faction Dedicated Server ================\n");
    rf::console::print("==================================================================\n\n");

    auto& netgame = rf::netgame;
    const auto& cfg = g_alpine_server_config;

    load_ads_server_config(g_ads_config_name);

    if (!rf::lan_only_cmd_line_param.found()) {
        rf::console::print("Public game tracker:                     {}\n", g_alpine_game_config.multiplayer_tracker);
        rf::console::print("Attempt auto-forward via UPnP:           {}\n", cfg.upnp_enabled);
        if ((netgame.flags & rf::NG_FLAG_NOT_LAN_ONLY) != 0) {
            rf::console::print("Server was successfully registered with public game tracker.\n");
        }
        else {
            rf::console::print("----> Failed to register server with public game tracker. Did you forward the port?\n");
            rf::console::print("----> Visit alpinefaction.com/help for help resources.\n");
        }
    }
    else {
        rf::console::print("Not attempting to register server with public game tracker.\n");
    }

    load_and_print_alpine_dedicated_server_config(g_ads_config_name, true);

    if (netgame.levels.size() < 1) {
        rf::console::print("----> No valid level files were specified!\n");
        rf::console::print("----> Launching server on Glass House...\n\n");
        netgame.levels.add("glass_house.rfl");
    }

    g_alpine_server_config_active_rules = cfg.base_rules; // initialize rules with base in case it is checked before first level loads
    init_alpine_dedicated_server();
    netgame.current_level_index = 0;
    rf::multi_level_switch_queued = -1;
}

ConsoleCommand2 print_server_config_cmd{
    "sv_printconfig",
    []() {
        if (g_dedicated_launched_from_ads) {
            print_alpine_dedicated_server_config_info(true);
        }
        else {
            rf::console::print("This command is only available for Alpine Faction dedicated servers launched with the -ads switch.\n");
        }
    },
    "Print the current server configuration. Only available for ADS dedicated servers.",
};

ConsoleCommand2 print_level_rules_cmd{
    "sv_printrules",
    [](std::optional<std::string> maybe_filename) {
        if (!g_dedicated_launched_from_ads) {
            rf::console::print("This command is only available for Alpine Faction dedicated servers launched with -ads.\n");
            return;
        }

        const auto& cfg = g_alpine_server_config;
        std::vector<int> matches;

        if (maybe_filename) {
            // find all occurrences
            for (int i = 0; i < (int)cfg.levels.size(); ++i) {
                if (cfg.levels[i].level_filename == *maybe_filename)
                    matches.push_back(i);
            }
            if (matches.empty()) {
                rf::console::print("Level {} not found in rotation. If manually loaded, base rules would be used.\n", *maybe_filename);
                return;
            }
        } else {
            // use current index
            int idx = rf::netgame.current_level_index;
            if (idx < 0 || idx >= (int)cfg.levels.size()) {
                return;
            }
            matches.push_back(idx);
        }

        for (int idx : matches) {
            const auto& entry = cfg.levels[idx];

            bool manual_load = false;
            if (!maybe_filename) {
                // check if the current level was manually loaded
                manual_load = (rf::level_filename_to_load != entry.level_filename.c_str());
            }

            if (manual_load) {
                rf::console::print("\n---- Rules for level {} ----\n", rf::level_filename_to_load, idx);
                rf::console::print("  (manually loaded {} is using base rules)\n\n", rf::level_filename_to_load);
                print_rules(cfg.base_rules);
            }
            else {
                rf::console::print("\n---- Rules for level {} (index {}) ----\n", entry.level_filename, idx);
                print_rules(entry.rule_overrides);
            }
        }
    },
    "Print the rules for a level by filename, or the active rules for the current level if no filename specified"
};


ConsoleCommand2 load_server_config_cmd{
    "sv_loadconfig",
    [](std::optional<std::string> new_config) {
        if (g_dedicated_launched_from_ads) {
            load_and_print_alpine_dedicated_server_config(new_config.value_or(g_ads_config_name), false);
            apply_rules_for_current_level();
            if (g_ads_minimal_server_info) {
                //rf::console::print("Minimal info displayed because -min switch was used at launch. Enter sv_printconfig to print verbose server config.\n");
            }
        }
        else {
            rf::console::print("This command is only available for Alpine Faction dedicated servers launched with the -ads switch.\n");
        }
    },
    "Load specified server config. If none specified, reloads the server launch config. Only available for ADS dedicated servers.",
};

void dedi_cfg_init() {

    // register console commands
    print_server_config_cmd.register_cmd();
    print_level_rules_cmd.register_cmd();
    load_server_config_cmd.register_cmd();
}
