#include <common/utils/list-utils.h>
#include <patch_common/FunHook.h>
#include <patch_common/ShortTypes.h>
#include <patch_common/AsmWriter.h>
#include <unordered_map>
#include "multi.h"
#include "../main/main.h"
#include "server.h"
#include "../os/console.h"
#include "../rf/player/player.h"
#include "../rf/entity.h"
#include "../rf/localize.h"
#include "../rf/multi.h"
#include "../rf/weapon.h"
#include "../hud/multi_spectate.h"
#include "server_internal.h"

bool kill_messages = true;

void player_fpgun_on_player_death(rf::Player* pp);

std::unordered_map<rf::Player*, bool> final_level_notified;
std::unordered_map<rf::Player*, bool> level_completed_while_alive;

class GunGameWeaponManager
{
public:
    GunGameWeaponManager()
    {
        initialize_score_to_weapon_map();
    }

    void initialize_score_to_weapon_map()
    {
        score_to_weapon_map.clear();

        if (g_additional_server_config.gungame.dynamic_progression) {
            // Create a map to group weapon levels by tiers (kill_level)
            std::map<int, std::vector<int>> tiered_levels;
            for (const auto& [tier, weapon_level] : g_additional_server_config.gungame.levels) {
                tiered_levels[tier].emplace_back(weapon_level);
            }

            int effective_kill_limit = g_additional_server_config.gungame.final_level
                                           ? g_additional_server_config.gungame.final_level->first
                                           : rf::multi_kill_limit;

            xlog::warn("Initializing GunGame levels with Dynamic Progression. Tiers: {}, Kill Limit: {}",
                       tiered_levels.size(), effective_kill_limit);

            // Calculate the total number of weapons
            int total_weapons = 0;
            for (const auto& [_, weapons] : tiered_levels) {
                total_weapons += static_cast<int>(weapons.size());
            }

            int accumulated_kills = 0;
            int remaining_kills = effective_kill_limit;
            int total_tiers = static_cast<int>(tiered_levels.size());

            // Iterate over each tier, shuffle within each tier, and assign weapons proportionally to kill levels
            for (auto& [tier, weapons] : tiered_levels) {
                std::shuffle(weapons.begin(), weapons.end(), g_rng);

                // Calculate the proportional range for this tier
                int tier_kills = (remaining_kills * weapons.size()) / total_weapons;

                // Adjust the interval based on the weapons in this tier
                int weapon_interval = (tier_kills > 0) ? (tier_kills / weapons.size()) : 1;

                for (int weapon_level : weapons) {
                    // Assign weapon to the current kill level and increment
                    score_to_weapon_map[accumulated_kills] = weapon_level;
                    xlog::info("Tier {}, Kill Level = {}, Weapon = {}", tier, accumulated_kills, weapon_level);

                    // Accumulate kills, but stop at effective_kill_limit
                    accumulated_kills += weapon_interval;
                    accumulated_kills = std::min(accumulated_kills, effective_kill_limit);

                    // If we've hit the kill limit, stop distributing
                    if (accumulated_kills >= effective_kill_limit)
                        break;
                }

                // Update remaining kills and tiers
                remaining_kills = effective_kill_limit - accumulated_kills;
                total_weapons -= weapons.size();

                if (accumulated_kills >= effective_kill_limit) {
                    break;
                }
            }            
        }
        else {
            xlog::warn("Initializing GunGame levels with Manual Progression.");

            // Make sure the levels are ordered sequentially
            std::sort(g_additional_server_config.gungame.levels.begin(),
                      g_additional_server_config.gungame.levels.end(),
                      [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });

            // Build level array
            for (const auto& [kill_level, weapon_level] : g_additional_server_config.gungame.levels) {
                score_to_weapon_map[kill_level] = weapon_level;

                xlog::info("Kill Level = {}, Weapon = {}", kill_level, weapon_level); 
            }            
        }

        // Handle specified final level
        if (g_additional_server_config.gungame.final_level) {
            const auto& [final_kill_level, final_weapon_level] = *g_additional_server_config.gungame.final_level;
            score_to_weapon_map[final_kill_level] = final_weapon_level;

            xlog::info("Final Level: Kill Level = {}, Weapon = {}", final_kill_level, final_weapon_level);
        }
    }

    std::optional<int> get_weapon_for_score(int score) const
    {
        // If score is negative, return the weapon at the lowest kill level (if any)
        if (score < 0) {
            return score_to_weapon_map.empty() ? std::nullopt : std::optional{score_to_weapon_map.begin()->second};
        }

        // Find the closest match for the score or the next higher score
        auto it = score_to_weapon_map.lower_bound(score);

        // Return exact match or closest lower score, if any
        if (it != score_to_weapon_map.end() && it->first == score) {
            return it->second; // Exact match
        }
        return (it != score_to_weapon_map.begin()) ? std::optional{std::prev(it)->second} : std::nullopt;
    }

//private:
    std::map<int, int> score_to_weapon_map;
};

GunGameWeaponManager weapon_manager;

void reset_gungame_notifications()
{
    final_level_notified.clear();
    level_completed_while_alive.clear();
}

void gungame_weapon_notification(rf::Player* player, bool just_spawned)
{    
    int current_score = player->stats->score;
    int weapon_type = *weapon_manager.get_weapon_for_score(current_score);

    if (current_score < 0 || !weapon_type) {
        return;
    }

    auto next_it = weapon_manager.score_to_weapon_map.upper_bound(current_score);
    std::string msg;

    if (next_it != weapon_manager.score_to_weapon_map.end()) {
        int next_score = next_it->first;
        int next_weapon_type = next_it->second;
        int frags_needed = next_score - current_score;
        std::string weapon_type_string = rf::weapon_types[weapon_type].display_name;
        std::string next_weapon_type_string = rf::weapon_types[next_weapon_type].display_name;

        if (just_spawned) {
            msg = std::format("Current weapon: {}. Upgrade to {} in {} frag{}!",
                weapon_type_string, next_weapon_type_string, frags_needed, frags_needed > 1 ? "s" : "");
        }
        else {
            std::vector<std::string> prefixes = {
                "Great work",
                "Nice job",
                "Nice work",
                "Awesome",
                "Congrats",
                "Whoa",
                "Nice",
                "Fantastic",
                "Frag-o-licious",
                "Cha-ching",
                "Sweet",
                "Wonderful"
            };
            std::uniform_int_distribution<int> dist(0, static_cast<int>(prefixes.size()) - 1);
            std::string prefix = prefixes[dist(g_rng)];

            msg = std::format("{}! Upgrade to {} in {} frag{}!",
                prefix, next_weapon_type_string, frags_needed, frags_needed > 1 ? "s" : "");
        }        

        send_chat_line_packet(msg.c_str(), player);
    }
    else if (!final_level_notified[player]) { // only notify on last level once per round for each player
        int frags_needed_to_win = rf::multi_kill_limit - current_score;

        if (frags_needed_to_win > 0) {
            msg = std::format("{} only needs {} more frag{} to win the game!", player->name, frags_needed_to_win,
                              frags_needed_to_win > 1 ? "s" : "");

            send_chat_line_packet(msg.c_str(), nullptr);
            final_level_notified[player] = true;
        }
    }    
}

void handle_gungame_weapon_switch(rf::Player* player, rf::Entity* entity,
    const GunGameWeaponManager& weapon_manager, bool just_spawned)
{
    if (!player || !entity) {
        xlog::error("Invalid player or entity passed to handle_gungame_weapon_switch.");
        return;
    }

    int current_score = player->stats->score;

    if (auto weapon_type_opt = weapon_manager.get_weapon_for_score(current_score); weapon_type_opt) {
        int weapon_type = *weapon_type_opt;

        if (just_spawned ||
            (weapon_type != entity->ai.current_primary_weapon &&
                !((entity->ai.current_primary_weapon == 0 || entity->ai.current_primary_weapon == 1) &&
                    (weapon_type == 0 || weapon_type == 1)))) {

            gungame_weapon_notification(player, just_spawned); // bug: doesnt work with remote charges

            if (!just_spawned) {
                // give a reward if they finished the whole level in a single life
                if (g_additional_server_config.gungame.rampage_rewards && level_completed_while_alive[player]) {
                    std::vector<std::string> prefixes = {
                    "RAMPAGE",
                    "UNSTOPPABLE",
                    "HOLY SHIT",
                    "KILLING SPREE",
                    "SQUEEGEE TIME",
                    "DOMINATION"
                    };

                    std::uniform_int_distribution<int> dist(0, static_cast<int>(prefixes.size()) - 1);
                    std::string prefix = prefixes[dist(g_rng)];

                    std::uniform_int_distribution<int> powerup_dist(0, 3);
                    int random_powerup = powerup_dist(g_rng);

                    int random_powerup_duration = 0;
                    std::string msg;                    

                    if (random_powerup <= 1) {

                        int max_duration = (random_powerup == 0) ? 10 : 20; // invuln max 10, amp max 20

                        std::uniform_int_distribution<int> duration_dist(3, max_duration);
                        random_powerup_duration = duration_dist(g_rng);

                        msg = std::format("{}!!! Your reward is {} for {} seconds!",
                            prefix, (random_powerup ? "DAMAGE AMP" : "INVULNERABILITY"), random_powerup_duration);

                        int amp_time_to_add =
                            rf::multi_powerup_get_time_until(player, random_powerup) + (random_powerup_duration * 1000);
                        rf::multi_powerup_add(player, random_powerup, amp_time_to_add);
                    }
                    else {
                        msg = std::format("{}!!! Your reward is SUPER {}!",
                            prefix, random_powerup == 2 ? "ARMOUR" : "HEALTH");
                        rf::multi_powerup_add(player, random_powerup, 10000);
                    }
                    level_completed_while_alive[player] = false; // reset rewards after granting one
                    send_chat_line_packet(msg.c_str(), player);
                    send_sound_packet_throwaway(player, 35); // Jolt_05.wav
                }                
            }

            if (current_score == 0) {
                level_completed_while_alive[player] = true;
            }
            else {
                level_completed_while_alive[player] = !just_spawned;
            }
        }

        // Switch the player's weapon to the new type
        server_add_player_weapon(player, weapon_type, true);
        server_set_player_weapon(player, entity, weapon_type);       
    }
    else {
        xlog::info("GunGame: No weapon assigned for score {}. Player '{}' retains current weapon.", current_score, player->name);
    }
}

void multi_update_gungame_weapon(rf::Player* player, bool just_spawned) {
    handle_gungame_weapon_switch(player, rf::entity_from_handle(player->entity_handle), weapon_manager, just_spawned);
}

void gungame_on_player_spawn(rf::Player* player)
{
    multi_update_gungame_weapon(player, true);
}

void multi_kill_init_player(rf::Player* player)
{
    auto* stats = static_cast<PlayerStatsNew*>(player->stats);
    stats->clear();
}

FunHook<void()> multi_level_init_hook{
    0x0046E450,
    []() {
        auto player_list = SinglyLinkedList{rf::player_list};
        for (auto& player : player_list) {
            multi_kill_init_player(&player);
        }

        multi_level_init_hook.call_target();

        if (g_additional_server_config.gungame.enabled)
        {
            reset_gungame_notifications();
            if (g_additional_server_config.gungame.dynamic_progression || weapon_manager.score_to_weapon_map.empty()) {
                // Build the map at the start of each level if it's dynamic. Otherwise, only when the first map loads
                weapon_manager.initialize_score_to_weapon_map();
            }
        }
    },
};

static const char* null_to_empty(const char* str)
{
    return str ? str : "";
}

void print_kill_message(rf::Player* killed_player, rf::Player* killer_player)
{
    rf::String msg;
    const char* mui_msg;
    rf::ChatMsgColor color_id;

    rf::Entity* killer_entity = killer_player ? rf::entity_from_handle(killer_player->entity_handle) : nullptr;

    if (!killer_player) {
        color_id = rf::ChatMsgColor::default_;
        mui_msg = null_to_empty(rf::strings::was_killed_mysteriously);
        msg = rf::String::format("{}{}", killed_player->name, mui_msg);
    }
    else if (killed_player == rf::local_player) {
        color_id = rf::ChatMsgColor::white_white;
        if (killer_player == killed_player) {
            mui_msg = null_to_empty(rf::strings::you_killed_yourself);
            msg = rf::String::format("{}", mui_msg);
        }
        else if (killer_entity && killer_entity->ai.current_primary_weapon == rf::riot_stick_weapon_type) {
            mui_msg = null_to_empty(rf::strings::you_just_got_beat_down_by);
            msg = rf::String::format("{}{}!", mui_msg, killer_player->name);
        }
        else {
            mui_msg = null_to_empty(rf::strings::you_were_killed_by);

            auto& killer_name = killer_player->name;
            int killer_weapon_cls_id = killer_entity ? killer_entity->ai.current_primary_weapon : -1;
            if (killer_weapon_cls_id >= 0 && killer_weapon_cls_id < 64) {
                auto& weapon_cls = rf::weapon_types[killer_weapon_cls_id];
                auto& weapon_name = weapon_cls.display_name;
                msg = rf::String::format("{}{} ({})!", mui_msg, killer_name, weapon_name);
            }
            else {
                msg = rf::String::format("{}{}!", mui_msg, killer_name);
            }
        }
    }
    else if (killer_player == rf::local_player) {
        color_id = rf::ChatMsgColor::white_white;
        mui_msg = null_to_empty(rf::strings::you_killed);
        msg = rf::String::format("{}{}!", mui_msg, killed_player->name);
    }
    else {
        color_id = rf::ChatMsgColor::default_;
        if (killer_player == killed_player) {
            if (rf::multi_entity_is_female(killed_player->settings.multi_character))
                mui_msg = null_to_empty(rf::strings::was_killed_by_her_own_hand);
            else
                mui_msg = null_to_empty(rf::strings::was_killed_by_his_own_hand);
            msg = rf::String::format("{}{}", killed_player->name, mui_msg);
        }
        else {
            if (killer_entity && killer_entity->ai.current_primary_weapon == rf::riot_stick_weapon_type)
                mui_msg = null_to_empty(rf::strings::got_beat_down_by);
            else
                mui_msg = null_to_empty(rf::strings::was_killed_by);
            msg = rf::String::format("{}{}{}", killed_player->name, mui_msg, killer_player->name);
        }
    }

    rf::String prefix;
    rf::multi_chat_print(msg, color_id, prefix);
}

void multi_apply_kill_reward(rf::Player* player)
{
    rf::Entity* ep = rf::entity_from_handle(player->entity_handle);
    if (!ep) {
        return;
    }

    const auto& conf = server_get_df_config();

    float max_life = conf.kill_reward_health_super ? 200.0f : ep->info->max_life;
    float max_armor = conf.kill_reward_armor_super ? 200.0f : ep->info->max_armor;

    if (conf.kill_reward_health > 0.0f) {
        ep->life = std::min(ep->life + conf.kill_reward_health, max_life);
    }
    if (conf.kill_reward_armor > 0.0f) {
        ep->armor = std::min(ep->armor + conf.kill_reward_armor, max_armor);
    }
    if (conf.kill_reward_effective_health > 0.0f) {
        float life_to_add = std::min(conf.kill_reward_effective_health, max_life - ep->life);
        float armor_to_add = std::min((conf.kill_reward_effective_health - life_to_add) / 2, max_armor - ep->armor);
        ep->life += life_to_add;
        ep->armor += armor_to_add;
    }
}

void on_player_kill(rf::Player* killed_player, rf::Player* killer_player)
{
    if (kill_messages) {
        print_kill_message(killed_player, killer_player);
    }

    auto* killed_stats = static_cast<PlayerStatsNew*>(killed_player->stats);
    killed_stats->inc_deaths();

    if (killer_player) {
        auto* killer_stats = static_cast<PlayerStatsNew*>(killer_player->stats);
        if (killer_player != killed_player) {
            rf::player_add_score(killer_player, 1);
            killer_stats->inc_kills();            
        }
        else {
            rf::player_add_score(killer_player, -1);
        }

        multi_apply_kill_reward(killer_player);

        multi_spectate_on_player_kill(killed_player, killer_player);

        if (g_additional_server_config.gungame.enabled && killer_player != killed_player) {
            multi_update_gungame_weapon(killer_player, false);
        }
    }
    
    
}

FunHook<void(rf::Entity*)> entity_on_death_hook{
    0x0041FDC0,
    [](rf::Entity* entity) {
        // Reset fpgun animation when player dies
        if (rf::local_player && entity->handle == rf::local_player->entity_handle && rf::local_player->weapon_mesh_handle) {
            player_fpgun_on_player_death(rf::local_player);
        }
        entity_on_death_hook.call_target(entity);
    },
};

ConsoleCommand2 kill_messages_cmd{
    "kill_messages",
    []() {
        kill_messages = !kill_messages;
    },
    "Toggles printing of kill messages in the chatbox and the game console",
};

void multi_kill_do_patch()
{
    // Player kill handling
    using namespace asm_regs;
    AsmWriter(0x00420703)
        .push(ebx)
        .push(edi)
        .call(on_player_kill)
        .add(esp, 8)
        .jmp(0x00420B03);

    // Change player stats structure
    write_mem<i8>(0x004A33B5 + 1, sizeof(PlayerStatsNew));
    multi_level_init_hook.install();

    // Reset fpgun animation when player dies
    entity_on_death_hook.install();

    // Allow disabling kill messages
    kill_messages_cmd.register_cmd();
}
