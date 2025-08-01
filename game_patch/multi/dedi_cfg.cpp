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

static const AlpineServerConfigRules default_alpine_config_rules = [] {
    AlpineServerConfigRules d;
    d.time_limit = 600.0f;
    d.individual_kill_limit = 30;
    d.team_kill_limit = 100;
    d.cap_limit = 5;
    d.geo_limit = 64;
    d.team_damage = false;
    d.fall_damage = false;
    d.weapons_stay = false;
    d.force_respawn = false;
    d.balance_teams = false;
    return d;
}();

static rf::NetGameType parse_game_type(const std::string& s)
{
    if (s == "TDM")
        return rf::NetGameType::NG_TYPE_TEAMDM;
    if (s == "CTF")
        return rf::NetGameType::NG_TYPE_CTF;
    return rf::NetGameType::NG_TYPE_DM;
}

// parse toml rules
// for base rules, populate if specified or use default
// for level-specific rules, only populate if specified
AlpineServerConfigRules parse_server_rules(const toml::table& t, bool base)
{
    AlpineServerConfigRules o;

    if (auto v = t["time_limit"].value<float>())
        o.set_time_limit(*v);
    else if (base)
        o.time_limit = default_alpine_config_rules.time_limit;

    if (auto v = t["individual_kill_limit"].value<int>())
        o.set_individual_kill_limit(*v);
    else if (base)
        o.individual_kill_limit = default_alpine_config_rules.individual_kill_limit;

    if (auto v = t["team_kill_limit"].value<int>())
        o.set_team_kill_limit(*v);
    else if (base)
        o.team_kill_limit = default_alpine_config_rules.team_kill_limit;

    if (auto v = t["cap_limit"].value<int>())
        o.set_cap_limit(*v);
    else if (base)
        o.cap_limit = default_alpine_config_rules.cap_limit;

    if (auto v = t["geo_limit"].value<int>())
        o.set_geo_limit(*v);
    else if (base)
        o.geo_limit = default_alpine_config_rules.geo_limit;

    if (auto v = t["team_damage"].value<bool>())
        o.team_damage = *v;
    else if (base)
        o.team_damage = default_alpine_config_rules.team_damage;

    if (auto v = t["fall_damage"].value<bool>())
        o.fall_damage = *v;
    else if (base)
        o.fall_damage = default_alpine_config_rules.fall_damage;

    if (auto v = t["weapons_stay"].value<bool>())
        o.weapons_stay = *v;
    else if (base)
        o.weapons_stay = default_alpine_config_rules.weapons_stay;

    if (auto v = t["force_respawn"].value<bool>())
        o.force_respawn = *v;
    else if (base)
        o.force_respawn = default_alpine_config_rules.force_respawn;

    if (auto v = t["balance_teams"].value<bool>())
        o.balance_teams = *v;
    else if (base)
        o.balance_teams = default_alpine_config_rules.balance_teams;

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

    if (auto v = tbl["server_name"].value<std::string>())
        cfg.server_name = *v;

    if (auto v = tbl["game_type"].value<std::string>())
        cfg.game_type = parse_game_type(*v);

    if (auto v = tbl["max_players"].value<int64_t>())
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

    // base rules
    if (auto base = tbl["base_rules"].as_table())
        cfg.base_rules = parse_server_rules(*base, true);

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

            // per-level rule override
            if (auto* over = lvl_tbl["rules"].as_table()) {
                entry.rule_overrides = parse_server_rules(*over, false);
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

void print_rules(const AlpineServerConfigRules &o) {

    if (o.time_limit)
        rf::console::print("  Time limit:                            {} min\n", *o.time_limit / 60.0f);

    switch (rf::netgame.type) {
        case rf::NetGameType::NG_TYPE_TEAMDM: {
            if (o.team_kill_limit)
                rf::console::print("  Team score limit:                      {}\n", *o.team_kill_limit);
            break;
        }
        case rf::NetGameType::NG_TYPE_CTF: {
            if (o.cap_limit)
                rf::console::print("  Flag capture limit:                    {}\n", *o.cap_limit);
            break;
        }
        default: { // dm
            if (o.individual_kill_limit)
                rf::console::print("  Player score limit:                    {}\n", *o.individual_kill_limit);
            break;
        }
    }
    if (o.geo_limit)
        rf::console::print("  Geomod crater limit:                   {}\n", *o.geo_limit);
    if (o.team_damage)
        rf::console::print("  Team damage:                           {}\n", *o.team_damage);
    if (o.fall_damage)
        rf::console::print("  Fall damage:                           {}\n", *o.fall_damage);
    if (o.weapons_stay)
        rf::console::print("  Weapon stay:                           {}\n", *o.weapons_stay);
    if (o.force_respawn)
        rf::console::print("  Force respawn:                         {}\n", *o.force_respawn);
    if (o.balance_teams)
        rf::console::print("  Balance teams:                         {}\n", *o.balance_teams);
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

    rf::console::print("\n---- Base rules ----\n");
    print_rules(cfg.base_rules);

    rf::console::print("\n---- Level rotation ----\n");
    for (auto const& lvl : cfg.levels) {
        //rf::console::print("Level: {}\n", lvl.level_filename);
        rf::console::print("{}\n", lvl.level_filename);
        print_rules(lvl.rule_overrides);
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

void apply_alpine_dedicated_server_rules(rf::NetGameInfo& netgame, const AlpineServerConfigRules& cfg, const AlpineServerConfigRules& base_cfg)
{
    if (cfg.time_limit.has_value())
        netgame.max_time_seconds = cfg.time_limit.value();
    else
        netgame.max_time_seconds = base_cfg.time_limit.value();

    int ind_kill = cfg.individual_kill_limit.has_value() ? cfg.individual_kill_limit.value() : base_cfg.individual_kill_limit.value();
    int team_kill = cfg.team_kill_limit.has_value() ? cfg.team_kill_limit.value() : base_cfg.team_kill_limit.value();
    int cap = cfg.cap_limit.has_value() ? cfg.cap_limit.value() : base_cfg.cap_limit.value();

    switch (netgame.type) {
        case rf::NetGameType::NG_TYPE_TEAMDM:
            netgame.max_kills = team_kill;
            break;
        case rf::NetGameType::NG_TYPE_CTF:
            netgame.max_captures = cap;
            break;
        default: // DM
            netgame.max_kills = ind_kill;
            break;
    }

     if (cfg.geo_limit.has_value())
        netgame.geomod_limit = cfg.geo_limit.value();
    else
        netgame.geomod_limit = base_cfg.geo_limit.value();


    netgame.flags &= ~( rf::NG_FLAG_TEAM_DAMAGE
                      | rf::NG_FLAG_FALL_DAMAGE
                      | rf::NG_FLAG_WEAPON_STAY
                      | rf::NG_FLAG_FORCE_RESPAWN
                      | rf::NG_FLAG_BALANCE_TEAMS );

    auto apply_flag = [&](bool override_val, bool base_val, int flag){
        if (override_val) netgame.flags |= flag;
        else if (!override_val && !base_val && false) {}
    };

    bool td = cfg.team_damage.has_value()
                ? cfg.team_damage.value()
                : base_cfg.team_damage.value();
    if (td) netgame.flags |= rf::NG_FLAG_TEAM_DAMAGE;

    bool fd = cfg.fall_damage.has_value()
                ? cfg.fall_damage.value()
                : base_cfg.fall_damage.value();
    if (fd) netgame.flags |= rf::NG_FLAG_FALL_DAMAGE;

    bool ws = cfg.weapons_stay.has_value()
                ? cfg.weapons_stay.value()
                : base_cfg.weapons_stay.value();
    if (ws) netgame.flags |= rf::NG_FLAG_WEAPON_STAY;

    bool fr = cfg.force_respawn.has_value()
                ? cfg.force_respawn.value()
                : base_cfg.force_respawn.value();
    if (fr) netgame.flags |= rf::NG_FLAG_FORCE_RESPAWN;

    bool bt = cfg.balance_teams.has_value()
                ? cfg.balance_teams.value()
                : base_cfg.balance_teams.value();
    if (bt) netgame.flags |= rf::NG_FLAG_BALANCE_TEAMS;
}

void load_and_print_alpine_dedicated_server_config(std::string ads_config_name, bool on_launch) {
    auto& netgame = rf::netgame;
    const auto& cfg = g_alpine_server_config;

    // parse toml file and update values
    // on launch does this before tracker registration
    if (!on_launch)
        load_ads_server_config(ads_config_name);

    initialize_core_alpine_dedicated_server_settings(netgame, cfg, on_launch);

    apply_alpine_dedicated_server_rules(netgame, cfg.base_rules, cfg.base_rules); // base rules

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

    // prevent a crash
    if (cfg.levels.size() < 1) {
        return;
    }

    int idx = netgame.current_level_index;

    if (!g_ads_minimal_server_info && rf::level_filename_to_load != cfg.levels[idx].level_filename.c_str()) {
        rf::console::print("Applying base rules for manually loaded level {}...\n", rf::level_filename_to_load, cfg.levels[idx].level_filename.c_str());
        return;
    }

    AlpineServerConfigRules const &override_rules =
        (idx >= 0 && idx < (int)cfg.levels.size())
          ? cfg.levels[idx].rule_overrides
          : cfg.base_rules;

    if (!g_ads_minimal_server_info)
        rf::console::print("Applying level-specific rules for server rotation index {} ({})...\n", idx, cfg.levels[idx].level_filename);

    apply_alpine_dedicated_server_rules(
        netgame,
        override_rules,
        cfg.base_rules
    );
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

ConsoleCommand2 load_server_config_cmd{
    "sv_loadconfig",
    [](std::optional<std::string> new_config) {
        if (g_dedicated_launched_from_ads) {
            load_and_print_alpine_dedicated_server_config(new_config.value_or(g_ads_config_name), false);
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
    load_server_config_cmd.register_cmd();
}
