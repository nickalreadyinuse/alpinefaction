#include <common/utils/list-utils.h>
#include <patch_common/FunHook.h>
#include <patch_common/ShortTypes.h>
#include <patch_common/AsmWriter.h>
#include <unordered_map>
#include "multi.h"
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
        //int score = 0;
        for (const auto& [level, weapon_level] : g_additional_server_config.gungame.levels) {
            score_to_weapon_map[level] = weapon_level;
            //xlog::warn("{}, {}", level, weapon_level);
        }
    }

    std::optional<int> get_weapon_for_score(int score) const
    {
        auto it = score_to_weapon_map.lower_bound(score);

        // If there's an exact match or a lower bound that equals the score
        if (it != score_to_weapon_map.end() && it->first == score) {
            return it->second;
        }

        // If no exact match, get the closest lower score
        if (it != score_to_weapon_map.begin()) {
            --it;
            return it->second; // Return the closest lower weapon tier
        }

        // If no suitable score is found, return std::nullopt
        return std::nullopt;
    }

//private:
    std::map<int, int> score_to_weapon_map;
};

GunGameWeaponManager weapon_manager;

void gungame_weapon_notification(rf::Player* player)
{    
    int current_score = player->stats->score;
    int weapon_type = *weapon_manager.get_weapon_for_score(current_score);
    auto next_it = weapon_manager.score_to_weapon_map.upper_bound(current_score);
    std::string msg;

    if (next_it != weapon_manager.score_to_weapon_map.end()) {
        int next_score = next_it->first;
        int next_weapon_type = next_it->second;
        int frags_needed = next_score - current_score;
        std::string weapon_type_string = rf::weapon_types[weapon_type].display_name;
        std::string next_weapon_type_string = rf::weapon_types[next_weapon_type].display_name;

        msg = std::format("Current weapon: {}. Upgrade to {} in {} more frags!", weapon_type_string,
                          next_weapon_type_string, frags_needed);
    }
    else {
        int frags_needed_to_win = rf::multi_kill_limit - current_score;

        if (frags_needed_to_win > 0) {
            msg = std::format("Get {} more frags to win the game!", frags_needed_to_win);
        }
    }

    send_chat_line_packet(msg.c_str(), player);
}

void handle_gungame_weapon_switch(rf::Player* player, rf::Entity* entity,
    const GunGameWeaponManager& weapon_manager, bool just_spawned)
{
    if (!player || !entity) {
        xlog::error("Invalid player or entity passed to handle_gungame_weapon_switch.");
        return;
    }

    //auto* stats = static_cast<PlayerStatsNew*>(player->stats);
    //int current_score = stats->score;
    int current_score = player->stats->score;
    //xlog::warn("Score for {} is {} {}", player->name, player->stats->score, current_score);
    //bool weapon_change = false;

    if (auto weapon_type_opt = weapon_manager.get_weapon_for_score(current_score); weapon_type_opt) {
        int weapon_type = *weapon_type_opt;

        if (just_spawned || weapon_type != entity->ai.current_primary_weapon) {
            gungame_weapon_notification(player);
        }

        // Switch the player's weapon to the new type
        server_add_player_weapon(player, weapon_type, true);
        server_set_player_weapon(player, entity, weapon_type);
        // xlog::warn("GunGame: Player '{}' reached score {}. Setting weapon to type {}.", player->name,
        // current_score, weapon_type);        
    }
    else {
        xlog::info("GunGame: No weapon assigned for score {}. Player '{}' retains current weapon.", current_score, player->name);
    }
}

void multi_update_gungame_weapon(rf::Player* player, bool just_spawned) {
    handle_gungame_weapon_switch(player, rf::entity_from_handle(player->entity_handle), weapon_manager, just_spawned);
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
        if (g_additional_server_config.gungame.enabled) {
            weapon_manager.initialize_score_to_weapon_map();
        }
        multi_level_init_hook.call_target();
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

        if (g_additional_server_config.gungame.enabled) {
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
    "mp_killfeed",
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
