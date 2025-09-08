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
#include <filesystem>
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

bool loadouts_in_use = false;

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

// check if we need to force the server to be restricted to Alpine clients
// prevents legacy clients from connecting to servers that require features they don't support
void evaluate_mandatory_alpine_restrict() {
    auto& cfg = g_alpine_server_config;

    if (cfg.alpine_restricted_config.clients_require_alpine &&
        cfg.alpine_restricted_config.alpine_server_version_enforce_min &&
        cfg.alpine_restricted_config.reject_non_alpine_clients) {
        return; // everything is already enforced, no need to evaluate
    }

    bool require_alpine = false;
    bool require_min_version = false;
    bool reject_non_alpine = false;

    // loadouts require min server version
    if (loadouts_in_use) { // added in 1.2.0
        rf::console::print("Loadouts are configured, so 'clients require Alpine', and 'enforce min server version' have been turned on\n");
        require_min_version = true;
    }

    // KOTH requires min server version and rejecting non-alpine clients
    if (cfg.game_type == rf::NetGameType::NG_TYPE_KOTH) { // added in 1.2.0
        rf::console::print("Gametype is KOTH, so 'clients require Alpine', 'enforce min server version', and 'reject non-Alpine clients' have been turned on\n");
        require_min_version = true;
        reject_non_alpine = true;
    }


    // evaluate if we need to require min server version
    if (require_min_version) {
        require_alpine = true;
        cfg.alpine_restricted_config.alpine_server_version_enforce_min = true;
    }

    // evaluate if we need to reject non-alpine clients
    if (reject_non_alpine) {
        require_alpine = true;
        cfg.alpine_restricted_config.reject_non_alpine_clients = true;
    }

    // evaluate if we need to turn on base alpine restriction
    if (require_alpine) {
        cfg.alpine_restricted_config.clients_require_alpine = true;
    }
}

static rf::NetGameType parse_game_type(const std::string& s)
{
    if (s == "TDM" || s == "TeamDM")
        return rf::NetGameType::NG_TYPE_TEAMDM;
    if (s == "CTF")
        return rf::NetGameType::NG_TYPE_CTF;
    if (s == "KOTH")
        return rf::NetGameType::NG_TYPE_KOTH;
    return rf::NetGameType::NG_TYPE_DM;
}

static DefaultPlayerWeaponConfig parse_default_player_weapon(const toml::table& t, DefaultPlayerWeaponConfig c)
{
    if (auto x = t["weapon_name"].value<std::string>()) {
        c.set_weapon(*x);
    }

    if (c.index >= 0) {
        if (auto v = t["clips"].value<int>())
            c.num_clips = *v;
    }

    return c;
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

static ForceCharacterConfig parse_force_character_config(const toml::table& t, ForceCharacterConfig c)
{
    if (auto x = t["enabled"].value<bool>())
        c.enabled = *x;

    if (c.enabled) {
        if (auto v = t["character"].value<std::string>())
            c.set_character(*v);
    }

    return c;
}

static CriticalHitsConfig parse_critical_hits_config(const toml::table& t, CriticalHitsConfig c)
{
    if (auto x = t["enabled"].value<bool>())
        c.enabled = *x;

    if (c.enabled) {
        if (auto v = t["reward_duration"].value<int>())
            c.set_reward_duration(*v);
        if (auto v = t["base_chance"].value<float>())
            c.set_base_chance(*v);
        if (auto v = t["dynamic_scale"].value<float>())
            c.dynamic_scale = *v;
        if (auto v = t["dynamic_damage_bonus_ceiling"].value<float>())
            c.set_damage_bonus_ceiling(*v);
    }

    return c;
}

static GunGameConfig parse_gungame_config(const toml::table& t, GunGameConfig gg)
{
    if (auto v = t["enabled"].value<bool>())
        gg.enabled = *v;
    if (auto v = t["dynamic_progression"].value<bool>())
        gg.dynamic_progression = *v;
    if (auto v = t["rampage_rewards"].value<bool>())
        gg.rampage_rewards = *v;

    if (auto ft = t["final"].as_table()) {
        int kills = (*ft)["kills"].value_or<int>(-1);
        auto w = (*ft)["weapon"].value_or<std::string>("");
        if (kills >= 0 && !w.empty())
            gg.set_final_level(kills, w);
        else
            xlog::warn("GunGame: [final] requires kills>=0 and weapon");
    }

    if (auto arr = t["levels"].as_array()) {
        gg.levels.clear(); // levels array specified for this map. Otherwise, inherit base

        int prev_kills = 0;
        for (auto& node : *arr) {
            auto tbl = node.as_table();
            if (!tbl) {
                xlog::warn("GunGame: level is not a table; skipping");
                continue;
            }

            auto w = (*tbl)["weapon"].value_or<std::string>("");
            if (w.empty()) {
                xlog::warn("GunGame: level missing 'weapon'");
                continue;
            }

            if (gg.dynamic_progression) {
                int tier = (*tbl)["tier"].value_or<int>(-1);
                if (tier < 1) {
                    xlog::warn("GunGame: dynamic requires 'tier' >= 1");
                    continue;
                }
                gg.add_level_by_tier(tier, w);
            }
            else {
                int kills = (*tbl)["kills"].value_or<int>(prev_kills + 1);
                gg.add_level_by_kills(kills, w);
                prev_kills = kills;
            }
        }

        if (!gg.dynamic_progression)
            gg.normalize_manual(); // dedupe kill levels
    }

    return gg;
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
        c.clear_dynamic_respawn_items();

        if (auto arr = t["dynamic_respawn_items"].as_array()) {
            for (auto& elem : *arr) {
                if (auto sub = elem.as_table()) {
                    auto name = (*sub)["item_name"].value_or<std::string>("");
                    int pts = (*sub)["min_respawn_points"].value_or<int>(8);

                    if (!c.add_dynamic_respawn_item(name, pts)) {
                        xlog::warn("Invalid dynamic_respawn_items entry: item_name='{}'", name);
                    }
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
    if (auto v = t["pvp_damage_modifier"].value<float>())
        o.set_pvp_damage_modifier(*v);
    if (auto v = t["drop_amps"].value<bool>())
        o.drop_amps = *v;
    if (auto v = t["weapon_pickups_give_full_ammo"].value<bool>())
        o.weapon_items_give_full_ammo = *v;
    if (auto v = t["infinite_reloads"].value<bool>())
        o.weapon_infinite_magazines = *v;
    if (auto v = t["drop_weapons"].value<bool>())
        o.drop_weapons = *v;

    if (auto sub = t["spawn_weapon"].as_table())
        o.default_player_weapon = parse_default_player_weapon(*sub, o.default_player_weapon);
    if (auto sub = t["spawn_life"].as_table())
        o.spawn_life  = parse_spawn_life_config(*sub, o.spawn_life);
    if (auto sub = t["spawn_armor"].as_table())
        o.spawn_armour = parse_spawn_life_config(*sub, o.spawn_armour);

    // add default loadout
    int baton_ammo = rf::weapon_types[rf::riot_stick_weapon_type].clip_size_multi;
    o.spawn_loadout.add("Riot Stick", baton_ammo, false, true);

    if (o.default_player_weapon.index >= 0) {
        int default_ammo = rf::weapon_types[o.default_player_weapon.index].clip_size_multi * o.default_player_weapon.num_clips;
        o.spawn_loadout.add(o.default_player_weapon.weapon_name, default_ammo, false, true);
        
    }

    if (auto arr = t["spawn_loadout"].as_array()) {
        for (auto& node : *arr) {
            if (auto tbl = node.as_table()) {
                if (auto nameOpt = (*tbl)["weapon_name"].value<std::string>()) {
                    int ammo = (*tbl)["ammo"].value<int>().value_or(0);
                    bool enabled = (*tbl)["include"].value<bool>().value_or(true); // default true if not specified
                    o.spawn_loadout.add(*nameOpt, ammo, false, enabled);
                    loadouts_in_use = true;                         // used to restrict to alpine only
                    o.spawn_loadout.loadouts_active = true;         // used to decide if loadouts should be used on a specific map
                }
            }
        }
    }

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
                    xlog::warn("Invalid replacement {} -> {}", from, to);
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

    if (auto sub = t["force_character"].as_table())
        o.force_character = parse_force_character_config(*sub, o.force_character);

    if (auto sub = t["critical_hits"].as_table())
        o.critical_hits = parse_critical_hits_config(*sub, o.critical_hits);

    if (auto sub = t["gungame"].as_table())
        o.gungame = parse_gungame_config(*sub, o.gungame);

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

static OvertimeConfig parse_overtime_config(const toml::table& t)
{
    OvertimeConfig v;
    if (auto x = t["enabled"].value<bool>())
        v.enabled = *x;

    if (v.enabled) {
        if (auto x = t["time"].value<int>())
            v.set_additional_time(*x);
        if (auto x = t["tie_when_flag_stolen"].value<bool>())
            v.consider_tie_if_flag_stolen = *x;
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

static ClickLimiterConfig parse_click_limiter_config(const toml::table& t)
{
    ClickLimiterConfig o;
    if (auto v = t["enabled"].value<bool>())
        o.enabled = *v;

    if (o.enabled) {
        if (auto v = t["cooldown"].value<int>())
            o.set_cooldown(*v);
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
        if (o.vote_match.enabled) {
            if (auto sub = t["overtime"].as_table())
                o.overtime = parse_overtime_config(*sub);
        }
    }
    return o;
}

namespace fs = std::filesystem;

static bool is_include_key(std::string_view k) {
    return k == "include" || k == "includes";
}

// apply base config single keys (main and includes)
static void apply_known_key_in_order(AlpineServerConfig& cfg, const std::string& key, const toml::node& node)
{
    if (key == "ads_version") {
        if (auto v = node.value<int>())
            g_ads_loaded_version = *v;
    }
    else if (key == "server_name") {
        if (auto v = node.value<std::string>())
            cfg.server_name = *v;
    }
    else if (key == "game_type") {
        if (auto v = node.value<std::string>())
            cfg.game_type = parse_game_type(*v);
    }
    else if (key == "max_players") {
        if (auto v = node.value<int>())
            cfg.set_max_players(*v);
    }
    else if (key == "password") {
        if (auto v = node.value<std::string>())
            cfg.set_password(*v);
    }
    else if (key == "rcon_password") {
        if (auto v = node.value<std::string>())
            cfg.set_rcon_password(*v);
    }
    else if (key == "upnp") {
        if (auto v = node.value<bool>())
            cfg.upnp_enabled = *v;
    }
    else if (key == "dynamic_rotation") {
        if (auto v = node.value<bool>())
            cfg.dynamic_rotation = *v;
    }
    else if (key == "require_client_mod") {
        if (auto v = node.value<bool>())
            cfg.require_client_mod = *v;
    }
    else if (key == "gaussian_spread") {
        if (auto v = node.value<bool>())
            cfg.gaussian_spread = *v;
    }
    else if (key == "send_stats_message") {
        if (auto v = node.value<bool>())
            cfg.stats_message_enabled = *v;
    }
    else if (key == "allow_fullbright_meshes") {
        if (auto v = node.value<bool>())
            cfg.allow_fullbright_meshes = *v;
    }
    else if (key == "allow_lightmap_mode") {
        if (auto v = node.value<bool>())
            cfg.allow_lightmaps_only = *v;
    }
    else if (key == "allow_disable_screenshake") {
        if (auto v = node.value<bool>())
            cfg.allow_disable_screenshake = *v;
    }
    else if (key == "allow_disable_muzzle_flash") {
        if (auto v = node.value<bool>())
            cfg.allow_disable_muzzle_flash = *v;
    }
    else if (key == "allow_unlimited_fps") {
        if (auto v = node.value<bool>())
            cfg.allow_unlimited_fps = *v;
    }
    else if (key == "use_sp_damage_calculation") {
        if (auto v = node.value<bool>())
            cfg.use_sp_damage_calculation = *v;
    }
}

// apply base config toml tables (main and includes)
static void apply_known_table_in_order(AlpineServerConfig& cfg, const std::string& key, const toml::table& tbl)
{
    if (key == "inactivity")
        cfg.inactivity_config = parse_inactivity_config(tbl);
    else if (key == "damage_notifications")
        cfg.damage_notification_config = parse_damage_notification_config(tbl);
    else if (key == "alpine_restrict")
        cfg.alpine_restricted_config = parse_alpine_restrict_config(tbl);
    else if (key == "click_limiter")
        cfg.click_limiter_config = parse_click_limiter_config(tbl);
    else if (key == "vote_kick")
        cfg.vote_kick = parse_vote_config(tbl);
    else if (key == "vote_level")
        cfg.vote_level = parse_vote_config(tbl);
    else if (key == "vote_extend")
        cfg.vote_extend = parse_vote_config(tbl);
    else if (key == "vote_restart")
        cfg.vote_restart = parse_vote_config(tbl);
    else if (key == "vote_next")
        cfg.vote_next = parse_vote_config(tbl);
    else if (key == "vote_rand")
        cfg.vote_rand = parse_vote_config(tbl);
    else if (key == "vote_previous")
        cfg.vote_previous = parse_vote_config(tbl);
    else if (key == "base") {
        if (auto rules = tbl["rules"].as_table())
            cfg.base_rules = parse_server_rules(*rules, cfg.base_rules);
    }
    else if (key == "levels") {
        if (auto arr = tbl.as_array()) {
            for (auto& elem : *arr) {
                if (!elem.is_table())
                    continue;
                auto& lvl_tbl = *elem.as_table();
                AlpineServerConfigLevelEntry entry;

                auto tmp_filename = lvl_tbl["filename"].value_or<std::string>("");
                rf::File f;
                if (!f.find(tmp_filename.c_str())) {
                    rf::console::print("----> Level {} is not installed!\n\n", tmp_filename);
                    continue;
                }

                entry.level_filename = tmp_filename;
                entry.rule_overrides = cfg.base_rules;
                if (auto* over = lvl_tbl["rules"].as_table())
                    entry.rule_overrides = parse_server_rules(*over, cfg.base_rules);

                cfg.levels.push_back(std::move(entry));
            }
        }
    }
}

// apply known array toml nodes (main and includes)
static void apply_known_array_in_order(AlpineServerConfig& cfg, const std::string& key, const toml::array& arr)
{
    if (key == "levels") {
        for (auto& elem : arr) {
            if (!elem.is_table())
                continue;

            auto& lvl_tbl = *elem.as_table();

            for (auto&& [k, v] : lvl_tbl) {
                if (k != "filename" && k != "rules") {
                    xlog::warn("Unknown key '{}' inside a [[levels]] entry; did you intend to put it in [root]?",
                               std::string(k.str()));
                }
            }

            AlpineServerConfigLevelEntry entry;

            auto tmp_filename = lvl_tbl["filename"].value_or<std::string>("");
            rf::File f;
            if (!f.find(tmp_filename.c_str())) {
                rf::console::print("----> Level {} is not installed!\n\n", tmp_filename);
                continue;
            }

            entry.level_filename = tmp_filename;
            entry.rule_overrides = cfg.base_rules;

            if (auto* over = lvl_tbl["rules"].as_table())
                entry.rule_overrides = parse_server_rules(*over, cfg.base_rules);

            cfg.levels.push_back(std::move(entry));
        }
    }
}

// unified parser for main and included config files
static void apply_config_table_in_order(AlpineServerConfig& cfg, const toml::table& tbl, const fs::path& base_dir,
                                        std::vector<fs::path>& load_stack, int depth, ParsePass pass)
{
    if (depth > 16) {
        rf::console::print("  [ERROR] include depth exceeded under {}\n", base_dir.string());
        return;
    }

    struct Entry
    {
        std::string key;
        const toml::node* node;
        int line = std::numeric_limits<int>::max();
        int col = std::numeric_limits<int>::max();
    };

    std::vector<Entry> entries;
    entries.reserve(tbl.size());

    for (auto&& [k, v] : tbl) {
        Entry e{std::string(k.str()), &v};

        if (auto src = v.source(); src.path) {
            e.line = static_cast<int>(src.begin.line);
            e.col = static_cast<int>(src.begin.column);
        }

        entries.push_back(std::move(e));
    }

    // custom sorting so includes are processed in the proper position
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.line != b.line)
            return a.line < b.line;
        return a.col < b.col;
    });

    for (const auto& e : entries) {
        const std::string& key = e.key;
        const toml::node& v = *e.node;

        if (is_include_key(key)) {
            auto load_one = [&](const std::string& inc) {
                fs::path child = fs::weakly_canonical(base_dir / inc);
                if (std::find(load_stack.begin(), load_stack.end(), child) != load_stack.end()) {
                    rf::console::print("  [ERROR] include cycle detected at {}\n", child.string());
                    return;
                }
                try {
                    toml::table child_tbl = toml::parse_file(child.string());
                    rf::console::print("  Parsing include {} for {} pass\n", child.string(), pass == ParsePass::Core ? "config" : "level");
                    load_stack.push_back(child);
                    apply_config_table_in_order(cfg, child_tbl, child.parent_path(), load_stack, depth + 1, pass);
                    load_stack.pop_back();
                }
                catch (const toml::parse_error& err) {
                    rf::console::print("  [ERROR] failed to parse {}: {}\n", child.string(), err.description());
                }
            };

            if (auto arr = v.as_array()) {
                for (auto& n : *arr)
                    if (auto s = n.value<std::string>())
                        load_one(*s);
            }
            else if (auto s = v.value<std::string>()) {
                load_one(*s);
            }
            else {
                rf::console::print("  [WARN] '{}' must be a string or array of strings.\n", key);
            }
            continue;
        }

        // Arrays
        if (auto* arr = v.as_array()) {
            if (key == "levels") {
                if (pass == ParsePass::Levels)
                    apply_known_array_in_order(cfg, key, *arr);
            }
            continue;
        }

        // Tables
        if (auto* sub_tbl = v.as_table()) {
            if (key == "levels") {
                if (pass == ParsePass::Levels) {
                    if (auto nested = sub_tbl->as_array())
                        apply_known_array_in_order(cfg, key, *nested);
                }
                continue;
            }

            // allow root table workaround to allow root config after subsections in parent toml
            if (key == "root") {
                apply_config_table_in_order(cfg, *sub_tbl, base_dir, load_stack, depth, pass);
            }
            else {
                apply_known_table_in_order(cfg, key, *sub_tbl);
            }

            continue;
        }

        // Single keys
        if (pass == ParsePass::Core) {
            apply_known_key_in_order(cfg, key, v);
        }
    }
}

void load_ads_server_config(std::string ads_config_name)
{
    rf::console::print("Loading and applying server configuration from {}...\n\n", ads_config_name);

    loadouts_in_use = false;    // reset per load
    AlpineServerConfig cfg;     // start from defaults

    toml::table root;
    try {
        root = toml::parse_file(ads_config_name);
    }
    catch (const toml::parse_error& err) {
        rf::console::print("  [ERROR] failed to parse {}: {}\n", ads_config_name, err.description());
        return;
    }

 std::vector<fs::path> load_stack;
    const fs::path root_path = fs::weakly_canonical(fs::path(ads_config_name));
    load_stack.push_back(root_path);

    // config pass
    apply_config_table_in_order(cfg, root, root_path.parent_path(), load_stack, 0, ParsePass::Core);
    // level pass
    apply_config_table_in_order(cfg, root, root_path.parent_path(), load_stack, 0, ParsePass::Levels);

    rf::console::print("\n");

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
         case rf::NetGameType::NG_TYPE_KOTH:
             out_string = "KOTH";
             break;
         default:
             out_string = "DM";
             break;
    }
    return out_string;
}

void print_gungame(const GunGameConfig& cur, const GunGameConfig& base_cfg, bool base = true)
{
    // helper functions
    auto gg_level_equal = [](const GunGameLevelEntry& a, const GunGameLevelEntry& b) {
        return a.kills == b.kills && a.tier == b.tier && a.weapon_index == b.weapon_index;
    };

    auto gg_level_key = [](const GunGameLevelEntry& e) {
        return std::tuple<int, int, int>{e.kills, e.tier, e.weapon_index};
    };

    auto gg_canon = [&](std::vector<GunGameLevelEntry> v, bool dynamic) {
        if (dynamic) {
            v.erase(std::remove_if(v.begin(), v.end(), [](auto& e) { return e.tier < 0 || e.weapon_index < 0; }),
                    v.end());
        }
        else {
            v.erase(std::remove_if(v.begin(), v.end(), [](auto& e) { return e.kills < 0 || e.weapon_index < 0; }),
                    v.end());
        }
        std::sort(v.begin(), v.end(), [&](auto const& a, auto const& b) { return gg_level_key(a) < gg_level_key(b); });

        std::vector<GunGameLevelEntry> out;
        for (auto const& e : v) {
            if (!out.empty() && gg_level_key(out.back()) == gg_level_key(e))
                out.back() = e;
            else
                out.push_back(e);
        }
        return out;
    };

    auto gg_final_equal = [&](const std::optional<GunGameLevelEntry>& a, const std::optional<GunGameLevelEntry>& b) {
        if (a.has_value() != b.has_value())
            return false;
        return !a || gg_level_equal(*a, *b);
    };
    // end helpers

    if (base || cur.enabled != base_cfg.enabled)
        rf::console::print("  GunGame:                               {}\n", cur.enabled);

    if (!cur.enabled)
        return;

    if (base || cur.dynamic_progression != base_cfg.dynamic_progression)
        rf::console::print("    Dynamic progression:                 {}\n", cur.dynamic_progression);
    if (base || cur.rampage_rewards != base_cfg.rampage_rewards)
        rf::console::print("    Rampage rewards:                     {}\n", cur.rampage_rewards);

    bool cur_final = cur.final_level.has_value();
    bool base_final = base_cfg.final_level.has_value();
    if (base || cur_final != base_final)
        rf::console::print("    Final level:                         {}\n", cur_final);

    if (cur.final_level) {
        bool print_details = base;
        if (!print_details && base_cfg.final_level)
            print_details = !gg_level_equal(*cur.final_level, *base_cfg.final_level);
        else if (!base_cfg.final_level)
            print_details = true;

        if (print_details) {
            rf::console::print("      Kills:                             {}\n", cur.final_level->kills);
            rf::console::print("      Weapon:                            {}\n", cur.final_level->weapon_name);
        }
    }

    const bool dyn = cur.dynamic_progression;
    auto cur_levels = gg_canon(cur.levels, dyn);
    auto base_levels = gg_canon(base_cfg.levels, dyn);

    bool levels_changed = base || cur_levels.size() != base_levels.size() ||
                          !std::equal(cur_levels.begin(), cur_levels.end(), base_levels.begin(), base_levels.end(),
                                      [&](auto const& a, auto const& b) { return gg_level_equal(a, b); });

    if (!levels_changed)
        return;

    if (dyn) {
        rf::console::print("    Dynamic tiers:\n");
        for (auto const& e : cur_levels) rf::console::print("      Tier {:<3} -> {}\n", e.tier, e.weapon_name);
    }
    else {
        rf::console::print("    Levels (kills -> weapon):\n");
        for (auto const& e : cur_levels) rf::console::print("      {:>4} -> {}\n", e.kills, e.weapon_name);
    }
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

    if (base || rules.pvp_damage_modifier != b.pvp_damage_modifier)
        rf::console::print("  PvP damage modifier:                   {}\n", rules.pvp_damage_modifier);
    if (base || rules.drop_amps != b.drop_amps)
        rf::console::print("  Drop amps:                             {}\n", rules.drop_amps);
    if (base || rules.drop_weapons != b.drop_weapons)
        rf::console::print("  Drop weapons:                          {}\n", rules.drop_weapons);
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

    // spawn weapon
    if (base || rules.default_player_weapon.index != b.default_player_weapon.index ||
        (rules.default_player_weapon.num_clips != b.default_player_weapon.num_clips)) {
        rf::console::print("  Spawn weapon:                          {}\n", rules.default_player_weapon.weapon_name);
        rf::console::print("    Reserve clips:                       {}\n", rules.default_player_weapon.num_clips);
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

    // spawn loadout
    bool anySpawnLoadoutChanged = std::any_of(
        rules.spawn_loadout.red_weapons.begin(), rules.spawn_loadout.red_weapons.end(),
        [&](auto const& e){
            auto it = std::find_if(
                b.spawn_loadout.red_weapons.begin(), b.spawn_loadout.red_weapons.end(), [&](auto const& be) {
                    return be.weapon_name == e.weapon_name && be.reserve_ammo == e.reserve_ammo && be.enabled == e.enabled; }
            );
            return it == b.spawn_loadout.red_weapons.end();
        }
    );

    if (base || anySpawnLoadoutChanged) {
        rf::console::print("  Spawn loadout:\n");
        for (auto const& e : rules.spawn_loadout.red_weapons) {
            bool unchanged = std::any_of(
                b.spawn_loadout.red_weapons.begin(), b.spawn_loadout.red_weapons.end(), [&](auto const& be) {
                    return be.weapon_name == e.weapon_name && be.reserve_ammo == e.reserve_ammo && be.enabled == e.enabled;
                }
            );
            if (base || !unchanged) {
                rf::console::print("    {:<20}                 {}\n", e.weapon_name, e.enabled);
                rf::console::print("      Extra ammo:                        {}\n", e.reserve_ammo);
            }
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
    bool spawnLogicChanged =
        rules.spawn_logic.respect_team_spawns != b.spawn_logic.respect_team_spawns ||
        rules.spawn_logic.try_avoid_players != b.spawn_logic.try_avoid_players ||
        rules.spawn_logic.always_avoid_last != b.spawn_logic.always_avoid_last ||
        rules.spawn_logic.always_use_furthest != b.spawn_logic.always_use_furthest ||
        rules.spawn_logic.only_avoid_enemies != b.spawn_logic.only_avoid_enemies ||
        rules.spawn_logic.dynamic_respawns != b.spawn_logic.dynamic_respawns ||
        rules.spawn_logic.dynamic_respawn_items.size() != b.spawn_logic.dynamic_respawn_items.size();

    if (base || spawnLogicChanged) {
        rf::console::print("  Spawn logic:\n");
        if (base || rules.spawn_logic.respect_team_spawns != b.spawn_logic.respect_team_spawns)
            rf::console::print("    Respect team spawns:                 {}\n", rules.spawn_logic.respect_team_spawns);
        if (base || rules.spawn_logic.try_avoid_players != b.spawn_logic.try_avoid_players)
            rf::console::print("    Try avoid players:                   {}\n", rules.spawn_logic.try_avoid_players);
        if (base || rules.spawn_logic.always_avoid_last != b.spawn_logic.always_avoid_last)
            rf::console::print("    Always avoid last:                   {}\n", rules.spawn_logic.always_avoid_last);
        if (base || rules.spawn_logic.always_use_furthest != b.spawn_logic.always_use_furthest)
            rf::console::print("    Always use furthest:                 {}\n", rules.spawn_logic.always_use_furthest);
        if (base || rules.spawn_logic.only_avoid_enemies != b.spawn_logic.only_avoid_enemies)
            rf::console::print("    Only avoid enemies:                  {}\n", rules.spawn_logic.only_avoid_enemies);
        if (base || rules.spawn_logic.dynamic_respawns != b.spawn_logic.dynamic_respawns)
            rf::console::print("    Create item dynamic respawns:        {}\n", rules.spawn_logic.dynamic_respawns);

        if ((base || (rules.spawn_logic.dynamic_respawns != b.spawn_logic.dynamic_respawns ||
                      rules.spawn_logic.dynamic_respawn_items.size() != b.spawn_logic.dynamic_respawn_items.size())) &&
            rules.spawn_logic.dynamic_respawns) {
            for (auto const& item : rules.spawn_logic.dynamic_respawn_items) {
                bool unchanged = std::any_of(b.spawn_logic.dynamic_respawn_items.begin(),
                                             b.spawn_logic.dynamic_respawn_items.end(), [&](auto const& bi) {
                                                 return bi.item_name == item.item_name &&
                                                        bi.min_respawn_points == item.min_respawn_points;
                                             });
                if (base || !unchanged) {
                    rf::console::print("      Dynamic respawn item:              {} (threshold: {})\n", item.item_name,
                                       item.min_respawn_points);
                }
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

    // Weapon stay exemptions
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
                std::string weap_name_string = e.weapon_name + ":";
                rf::console::print("    {:<20}                 {}\n", weap_name_string, e.exemption_enabled ? "exempt" : "not exempt"
                );
            }
        }
    }

    // Item replacements
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
                rf::console::print("    {:<20}     ->          {}\n", orig, repl);
            }
        }
    }

    // Item respawn time overrides
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
                std::string item_name_string = item + ":";
                rf::console::print("    {:<20}                 {} ms\n", item_name_string, ms);
            }
        }
    }

    // force character
    if (base || rules.force_character.enabled != b.force_character.enabled ||
        (rules.force_character.enabled && rules.force_character.character_index != b.force_character.character_index)) {
        rf::console::print("  Forced character:                      {}\n", rules.force_character.enabled);
        if (rules.force_character.enabled) {
            rf::console::print("    Character:                           {} ({})\n", rules.force_character.character_name, rules.force_character.character_index);
        }
    }

    // critical hits
    if (base || rules.critical_hits.enabled != b.critical_hits.enabled ||
        (rules.critical_hits.enabled && rules.critical_hits.reward_duration != b.critical_hits.reward_duration) ||
        (rules.critical_hits.enabled && rules.critical_hits.base_chance != b.critical_hits.base_chance) ||
        (rules.critical_hits.enabled && rules.critical_hits.dynamic_scale != b.critical_hits.dynamic_scale) ||
        (rules.critical_hits.enabled && rules.critical_hits.dynamic_scale &&
         rules.critical_hits.dynamic_damage_bonus_ceiling != b.critical_hits.dynamic_damage_bonus_ceiling)
        ) {
        rf::console::print("  Critical hits:                         {}\n", rules.critical_hits.enabled);
        if (rules.critical_hits.enabled) {
            rf::console::print("    Reward duration:                     {} ms\n", rules.critical_hits.reward_duration);
            rf::console::print("    Base chance:                         {:.1f}%\n", rules.critical_hits.base_chance * 100.0f);
            rf::console::print("    Dynamic scale:                       {}\n", rules.critical_hits.dynamic_scale);
            if (rules.critical_hits.dynamic_scale) {
                rf::console::print("      Dynamic damage bonus ceiling:      {}\n", rules.critical_hits.dynamic_damage_bonus_ceiling);
            }
        }
    }

    // gungame
    print_gungame(rules.gungame, b.gungame, base);
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

    // inactivity
    rf::console::print("  Kick inactive players:                 {}\n", cfg.inactivity_config.enabled);
    if (cfg.inactivity_config.enabled) {
        rf::console::print("    New player grace period:             {} sec\n", cfg.inactivity_config.new_player_grace_ms / 1000.0f);
        rf::console::print("    Allowed inactivity time:             {} sec\n", cfg.inactivity_config.allowed_inactive_ms / 1000.0f);
        rf::console::print("    Warning duration:                    {} sec\n", cfg.inactivity_config.warning_duration_ms / 1000.0f);
        rf::console::print("    Kick message:                        {}\n", cfg.inactivity_config.kick_message);
    }

    // click limiter
    rf::console::print("  Click limiter:                         {}\n", cfg.click_limiter_config.enabled);
    if (cfg.click_limiter_config.enabled) {
        rf::console::print("    Cooldown:                            {} ms\n", cfg.click_limiter_config.cooldown);
    }

    // damage notifications
    rf::console::print("  Damage notifications:                  {}\n", cfg.damage_notification_config.enabled);
    if (cfg.damage_notification_config.enabled) {
        rf::console::print("    Legacy client compatibility:         {}\n", cfg.damage_notification_config.support_legacy_clients);
    }

    // alpine restrict
    rf::console::print("  Advertise Alpine:                      {}\n", cfg.alpine_restricted_config.advertise_alpine);
    rf::console::print("  Clients require Alpine:                {}\n", cfg.alpine_restricted_config.clients_require_alpine);
    if (cfg.alpine_restricted_config.clients_require_alpine) {
        rf::console::print("    Reject non-Alpine clients:           {}\n", cfg.alpine_restricted_config.reject_non_alpine_clients);
        rf::console::print("    Enforce min server version:          {}\n", cfg.alpine_restricted_config.alpine_server_version_enforce_min);
        rf::console::print("    Require release build:               {}\n", cfg.alpine_restricted_config.alpine_require_release_build);
        rf::console::print("    Only welcome Alpine players:         {}\n", cfg.alpine_restricted_config.only_welcome_alpine);
        rf::console::print("    No player collide:                   {}\n", cfg.alpine_restricted_config.no_player_collide);
        rf::console::print("    Location pinging:                    {}\n", cfg.alpine_restricted_config.location_pinging);

        // match mode
        auto& vm = cfg.alpine_restricted_config.vote_match;
        rf::console::print("    Match mode:                          {}\n", vm.enabled);
        if (vm.enabled) {
            rf::console::print("      Vote ignores nonvoters:            {}\n", vm.ignore_nonvoters);
            rf::console::print("      Vote time limit:                   {} sec\n", vm.time_limit_seconds);
            auto& ot = cfg.alpine_restricted_config.overtime;
            rf::console::print("      Overtime:                          {}\n", ot.enabled);
            if (ot.enabled) {
                rf::console::print("        Additional time:                 {}\n", ot.additional_time);
                rf::console::print("        Tie when flag stolen:            {}\n", ot.consider_tie_if_flag_stolen);
            }
        }
    }

    // votes
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
    for (size_t i = 0; i < cfg.levels.size(); ++i) {
        const auto& lvl = cfg.levels[i];
        rf::console::print("{} ({})\n", lvl.level_filename, i);
        print_rules(lvl.rule_overrides, false);
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

// keep the netgame levels array synced with level+rules array
void rebuild_rotation_from_cfg()
{
    auto& levels_arr = rf::netgame.levels;
    levels_arr.clear();
    for (const auto& lvlEntry : g_alpine_server_config.levels) {
        levels_arr.add(lvlEntry.level_filename.c_str());
    }
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

    evaluate_mandatory_alpine_restrict(); // force alpine restrict on if rules are configured which need it

    if (g_alpine_server_config.dynamic_rotation) {
        shuffle_level_array();
    }
    else {
        rebuild_rotation_from_cfg();
    }

    print_alpine_dedicated_server_config_info(!g_ads_minimal_server_info);
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

    if (!g_ads_minimal_server_info && rf::level_filename_to_load != netgame.levels[idx].c_str()) {
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

    load_ads_server_config(g_ads_config_name);
    const auto& cfg = g_alpine_server_config;

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

    if (netgame.levels.size() <= 0) {
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
