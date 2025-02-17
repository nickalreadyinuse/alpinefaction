#include <unordered_map>
#include <vector>
#include <thread>
#include <sstream>
#include <xlog/xlog.h>
#include <patch_common/CodeInjection.h>
#include <common/HttpRequest.h>
#include "../rf/os/console.h"
#include "../rf/trigger.h"
#include "../rf/entity.h"
#include "../rf/level.h"
#include "../rf/event.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../os/console.h"
#include "achievements.h"
#include "misc.h"

bool achievement_system_initialized = false;
bool synced_with_ff = false;
AchievementStateInfo achievement_state_info;
bool achievement_box_visible = true;
std::string achievement_box_name;
int achievement_box_icon = -1;
rf::TimestampRealtime achievement_box_timestamp;
rf::TimestampRealtime achievement_async_ff_timer;
int achievement_async_ff_key;
std::string achievement_async_ff_string;
std::vector<LoggedPlayerKill> achievement_async_ff_pending_kills;
constexpr int achievement_async_ff_interval = 5000; // ms

void AchievementManager::initialize()
{
    if (g_game_config.fflink_token.value().empty()) {
        std::string msg = "Achievements are unavailable because Alpine Faction is not linked to a FactionFiles account!";
        rf::console::printf("%s", msg);
        return;
    }

    std::vector<Achievement> predefined_achievements = {
        {0, "Starter Rebel", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign}, // game start
        {1, "Explosive Discovery", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign}, // secret fusion
        {2, "Tools of the Trade", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign}, // training start
        {3, "Welcome to Mars", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign}, // training finish
        {4, "_EasyFinish", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign}, // campaign finish
        {5, "_MediumFinish", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign}, // campaign finish
        {6, "_HardFinish", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign}, // campaign finish
        {7, "_ImpossibleFinish", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign}, // campaign finish
        {8, "Gone Fishin'", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign}, // kill a fish in l1s1
        {9, "Welcome", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign}, // hear eos message NOT DONE
        {10, "Red Alert", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign},
        {11, "If only you'd been faster", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign},
        {12, "Not a fan", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign},
        {13, "Exfiltration", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign},
        {14, "Unsuppressed", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign},
        {15, "_MeetCapek", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign},
        {16, "_CapekLikesToTalk", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign},
        {17, "_KillSnake", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign},
        {18, "Stowaway", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign},
        {19, "_ComputersSpace", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign},
        {20, "_DestroySpace", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign},
        {21, "_Escape", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign},
        {22, "_KillMasako", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign},
        {23, "_FFLink", "APC_Cocpit_P13.tga", AchievementCategory::general},
        {24, "_UnderSub", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign}, // release hidden sub in undersea base
        {25, "_KillDrone", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign},
        {26, "Pyroscientist", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign}, // kill capek with flamethrower
        {27, "Body Hiders", "APC_Cocpit_P13.tga", AchievementCategory::general}, // drop corpse
        {28, "Healthy as a Horse", "APC_Cocpit_P13.tga", AchievementCategory::general}, // heal from media
        {29, "Double Demolition", "APC_Cocpit_P13.tga", AchievementCategory::general}, // dupe c4
        {30, "Watchful Eye", "APC_Cocpit_P13.tga", AchievementCategory::general}, // view from security camera
        {31, "A Decent Descent", "APC_Cocpit_P13.tga", AchievementCategory::general}, // enter aesir
        {32, "Water on Mars", "APC_Cocpit_P13.tga", AchievementCategory::general}, // enter sub
        {33, "_EnterAPC", "APC_Cocpit_P13.tga", AchievementCategory::general}, // enter apc
        {34, "_EnterJeep", "APC_Cocpit_P13.tga", AchievementCategory::general}, // enter jeep
        {35, "Totally Recalled", "APC_Cocpit_P13.tga", AchievementCategory::general}, // enter driller
        {36, "Sub-dued", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign}, // enter secret sub after geothermal
        //{, "", "", AchievementCategory::base_campaign},
    };

    for (const auto& achievement : predefined_achievements) {
        achievements[achievement.uid] = achievement;
        xlog::warn("added achievement {} with uid {}", achievement.name, achievement.uid);
    }

    achievement_system_initialized = true;
    achievement_async_ff_timer.set(achievement_async_ff_interval);
    sync_with_ff();
}

void AchievementManager::process_ff_response(const std::string& response, int expected_key, bool is_initial_sync)
{
    if (response.empty()) {
        xlog::warn("No response from FF API. Will retry next time.");
        return;
    }

    std::istringstream response_stream(response);
    std::string key_token;
    std::getline(response_stream, key_token, ',');

    try {
        int key_received = std::stoi(key_token);

        // If this is an update, validate the key
        if (!is_initial_sync && key_received != expected_key) {
            xlog::warn("Response received but key mismatch. Will retry.");
            return;
        }

        // Process unlocked achievements
        std::string uid_str;
        auto& manager = AchievementManager::get_instance();
        auto& achievements = manager.get_achievements_mutable();

        while (std::getline(response_stream, uid_str, ',')) {
            int unlocked_uid = std::stoi(uid_str);

            auto it = achievements.find(unlocked_uid);
            if (it != achievements.end() && !it->second.unlocked) {
                it->second.unlocked = true;
                xlog::warn("Achievement {} unlocked via FF.", it->second.name);

                // don't notify on achievements we got in previous playthroughs
                if (is_initial_sync) {
                    it->second.notified = true;
                }
                // notify on any ff_authoritative achievements FF just told us we unlocked
                else if (!it->second.notified && it->second.type == AchievementType::ff_authoritative) {
                    manager.show_notification(it->second);
                }
            }
        }

        if (is_initial_sync) {
            synced_with_ff = true;
            std::string username = g_game_config.fflink_username.value();
            rf::console::printf("Successfully initialized Alpine Faction achievements for FactionFiles account %s", username.c_str());
        }
        else {
            xlog::warn("Successfully processed FF update [{}].", expected_key);
            achievement_async_ff_string.clear();
            achievement_async_ff_key = 0; // Reset key
        }
    }
    catch (...) {
        xlog::warn("Invalid response format from FF API: {}", response);
    }
}

void AchievementManager::sync_with_ff()
{
    std::string token = g_game_config.fflink_token.value();
    if (token.empty()) {
        xlog::warn("Skipping achievement sync: No FactionFiles authentication token found.");
        return;
    }

    std::thread([token]() {
        xlog::warn("Syncing achievements with FF...");

        std::string url = "https://link.factionfiles.com/afachievement/v1/initial/" + encode_uri_component(token);
        std::string response;

        try {
            HttpSession session("Alpine Faction v1.0.0 Achievement Sync");
            HttpRequest request(url, "GET", session);
            request.send();

            char buffer[4096];
            std::ostringstream response_stream;
            while (size_t bytes_read = request.read(buffer, sizeof(buffer))) {
                response_stream.write(buffer, bytes_read);
            }

            response = response_stream.str();
            AchievementManager::get_instance().process_ff_response(response, 0, true);
        }
        catch (const std::exception& e) {
            xlog::error("Failed to sync achievements with FF: {}", e.what());
        }
    }).detach();
}

void AchievementManager::send_update_to_ff()
{
    if (achievement_async_ff_string.empty()) {
        xlog::info("No pending achievement updates to send.");
        return;
    }

    std::string token = g_game_config.fflink_token.value();
    if (token.empty()) {
        xlog::warn("Skipping achievement update: No FactionFiles authentication token found.");
        return;
    }

    std::thread([token]() {
        xlog::warn("Sending achievement updates to FF [{}]...", achievement_async_ff_key);

        std::string url =
            "https://link.factionfiles.com/afachievement/v1/update/" + encode_uri_component(token);
        std::string response;

        try {
            HttpSession session("Alpine Faction v1.0.0 Achievement Push");
            HttpRequest request(url, "POST", session);
            request.set_content_type("application/x-www-form-urlencoded");

            std::string post_string =
                "key=" + std::to_string(achievement_async_ff_key) + "," + achievement_async_ff_string;
            request.send(post_string);

            char buffer[4096];
            std::ostringstream response_stream;
            while (size_t bytes_read = request.read(buffer, sizeof(buffer))) {
                response_stream.write(buffer, bytes_read);
            }

            response = response_stream.str();
            AchievementManager::get_instance().process_ff_response(response, achievement_async_ff_key, false);
        }
        catch (const std::exception& e) {
            xlog::error("Failed to send achievement updates: {}", e.what());
        }
    }).detach();
}

void AchievementManager::add_key_to_ff_update_map()
{
    if (!achievement_async_ff_string.empty()) {
        xlog::warn("Skipping new update generation: Previous update still awaiting FF acknowledgment.");
        return;
    }

    std::ostringstream oss;
    bool first = true;

    // Build the new update string from accumulated `pending_count` values
    for (auto& [uid, achievement] : achievements) {
        if (achievement.pending_count > 0) {
            if (!first) {
                oss << ",";
            }
            oss << uid << "=" << achievement.pending_count;
            first = false;
            achievement.pending_count = 0; // Reset after adding to string
        }
    }

    std::string update_string = oss.str();

    if (!update_string.empty()) {
        // Generate a new unique key for this update
        std::uniform_int_distribution<int> dist(1, INT_MAX);
        achievement_async_ff_key = dist(g_rng);
        achievement_async_ff_string = update_string;

        xlog::warn("Queued new achievement update [{}]: {}", achievement_async_ff_key, achievement_async_ff_string);
        send_update_to_ff(); // Send immediately
    }
    else {
        xlog::warn("No new achievements pending for FF submission.");
    }
}

// unlock basic achievements
void AchievementManager::grant_achievement(int uid)
{
    auto it = achievements.find(uid);

    if (it == achievements.end()) {
        return; // Achievement doesn't exist
    }

    if (it->second.type != AchievementType::basic) {
        return; // ff_authoritative achievements must be unlocked based on response from ff
    }

    // if we haven't been notified, do that    
    if (!it->second.notified && it->second.type == AchievementType::basic) {
        show_notification(it->second);  
    }
      
    // if we haven't unlocked this on ff yet, tick counter up by 1
    if (!it->second.unlocked) {
        it->second.pending_count += 1;
    }
}

void AchievementManager::show_notification(Achievement& achievement)
{
    achievement.notified = true;
    xlog::warn("Achievement Notified: {}", achievement.name);

    // clear previous info before repopulating
    clear_achievement_notification();

    achievement_box_icon = rf::bm::load(achievement.icon.c_str(), -1, true);
    achievement_box_name = achievement.name;
    achievement_box_timestamp.set(10000); // 10 seconds
    achievement_box_visible = true;
}

void grant_achievement(int achievement_uid)
{
    // scope key
    // 0 = any time
    // 1 = sp (gameplay) only
    // 2 = mp (gameplay) only
    // 3 = only when not in gameplay
    // 4 = only when tc mod loaded
    xlog::warn("attempting to give achievement {}", achievement_uid);
    AchievementManager::get_instance().grant_achievement(achievement_uid);
}

void initialize_achievement_manager() {
    // build achievement storage and sync with FF
    AchievementManager::get_instance().initialize();
}

void draw_achievement_box_content(int x, int y) {
    if (achievement_box_icon != -1) {
        rf::gr::set_color(255, 255, 255, 0xCC);
        rf::gr::bitmap(achievement_box_icon, x + 4, y + 4);
    }

    int str_x = x + 268;
    int str_y = y + 8;

    rf::gr::set_color(255, 255, 255, 0xCC);
    rf::gr::string_aligned(rf::gr::ALIGN_CENTER, str_x, str_y, "Achievement Unlocked!", 0);

    rf::gr::set_color(255, 255, 180, 0xCC);
    rf::gr::string_aligned(rf::gr::ALIGN_CENTER, str_x, str_y + 30, achievement_box_name.c_str(), 0);
}

void draw_achievement_box() {
    int w = 460;
    int h = 72;
    int x = rf::gr::screen_width() - w - 4;

    // Timing variables
    const int total_time = 10000;   // Total time the box is displayed (match timestamp set) 
    const int animation_time = 500; // Time for rise & fall (0.5s each)
    int time_left = achievement_box_timestamp.time_until();
    int time_elapsed = total_time - time_left;

    // Base Y positions
    int y_base = rf::gr::screen_height() - h - 4;
    int y_hidden = y_base + h + 10; // fully off screen position
    int y = y_base; // fully visible position

    // Animation logic
    if (time_elapsed < animation_time) { // Rising up
        float progress = static_cast<float>(time_elapsed) / animation_time;
        y = y_hidden - static_cast<int>(progress * (y_hidden - y_base));
    }
    else if (time_left < animation_time) { // Falling down
        float progress = static_cast<float>(animation_time - time_left) / animation_time;
        y = y_base + static_cast<int>(progress * (y_hidden - y_base));
    }

    rf::gr::set_color(0, 0, 0, 0x80);
    rf::gr::rect(x, y, w, h);
    rf::gr::set_color(79, 216, 255, 0x80);
    rf::gr::rect_border(x, y, w, h);

    draw_achievement_box_content(x, y);
}

// called in main rf_do_frame
// runs independent of gamestate
void achievement_system_do_frame() {
    if (!achievement_system_initialized) {
        return; // not initialized
    }

    if (achievement_box_visible) {
        if (!achievement_box_timestamp.valid() || achievement_box_timestamp.elapsed()) {
            clear_achievement_notification();
        }
        else {
            draw_achievement_box();
        }
    }

    if (!achievement_async_ff_timer.valid() || achievement_async_ff_timer.elapsed()) {
        if (!synced_with_ff) {
            // Retry initial sync if it hasn't been completed yet
            xlog::warn("Initial FF achievement sync not completed. Retrying...");
            AchievementManager::get_instance().sync_with_ff();
        } 
        else {
            // Only proceed with updates if sync was successful
            xlog::warn("Timer elapsed, trying to send an update to FF");

            if (!achievement_async_ff_string.empty()) {
                xlog::warn("Previous FF update [{}] still pending acknowledgement. Retrying.", achievement_async_ff_key);
                AchievementManager::get_instance().send_update_to_ff();
            } else {
                xlog::warn("No pending FF update, attempting to build a new one.");
                AchievementManager::get_instance().add_key_to_ff_update_map();
            }
        }

        // Reset timer for next attempt
        achievement_async_ff_timer.set(achievement_async_ff_interval);
    }
}

void clear_achievement_notification() {
    achievement_box_visible = false;
    achievement_box_timestamp.invalidate();
    achievement_box_name.clear();
    achievement_box_icon = -1;
}

bool is_achievement_system_initialized() {
    return achievement_system_initialized;
}

void grant_achievement_sp(int uid) {
    if (achievement_system_initialized && !rf::is_multi) {
        grant_achievement(uid);
    }
}

// handler for achivements awarded by specific trigger activation
void achievement_check_trigger(rf::Trigger* trigger) {
    if (!trigger) {
        return;
    }

    int trigger_uid = trigger->uid;
    rf::String rfl_filename = rf::level.filename;

    // single player
    if (!rf::is_multi) {
        switch (trigger_uid) {
            case 6736: {
                if (string_equals_ignore_case(rfl_filename, "l9s3.rfl")) {
                    grant_achievement(1); // secret fusion
                }
                break;
            }

            case 8656: {
                if (string_equals_ignore_case(rfl_filename, "l1s1.rfl")) {
                    grant_achievement(0); // game start
                }
                break;
            }

            case 7661: {
                if (string_equals_ignore_case(rfl_filename, "train02.rfl")) {
                    grant_achievement(2); // finish training
                }
                break;
            }

            case 7961: {
                if (string_equals_ignore_case(rfl_filename, "train01.rfl")) {
                    grant_achievement(2); // start training
                }
                break;
            }

            case 4234: {
                if (string_equals_ignore_case(rfl_filename, "l10s3.rfl")) {
                    grant_achievement(24); // drop sub in underwater base
                }
                break;
            }

            default:
                break;
        }
    }
}

// handler for achivements awarded by specific event activation
void achievement_check_event(rf::Event* event) {
    if (!event) {
        return;
    }

    int event_uid = event->uid;
    rf::String rfl_filename = rf::level.filename;

    // single player
    if (!rf::is_multi) {
        switch (event_uid) {
            case 20787: {
                if (string_equals_ignore_case(rfl_filename, "l17s2.rfl")) {
                    grant_achievement(19); // computers in space station
                }
                break;
            }

            case 9613: {
                if (string_equals_ignore_case(rfl_filename, "l15s4.rfl")) {
                    grant_achievement(18); // stowaway on shuttle
                }
                break;
            }

            case 18248: {
                if (string_equals_ignore_case(rfl_filename, "l17s4.rfl")) {
                    grant_achievement(20); // destroy space station
                }
                break;
            }

            case 10626: {
                if (string_equals_ignore_case(rfl_filename, "l11s3.rfl") &&
                    rf::local_player_entity->ai.current_primary_weapon == 12 && // flamethrower
                    !rf::mod_param.found()) {
                    grant_achievement(26); // kill capek with flamethrower
                }
                xlog::warn("yep");
                break;
            }

            case 7005: {
                if (string_equals_ignore_case(rfl_filename, "l10s4.rfl")) {
                    grant_achievement(16); // end of capek message series
                }
                break;
            }

            default:
                break;
        }
    }
}

// handler for achivements awarded by specific entities dying
void achievement_check_entity_death(rf::Entity* entity) {
    if (!entity) {
        return;
    }

    // single player
    if (!rf::is_multi) {
        int entity_uid = entity->uid;
        rf::String entity_script_name = entity->name;
        rf::String entity_class_name = entity->info->name;
        rf::String rfl_filename = rf::level.filename;

        xlog::warn("entity died {}, {}, {}, {}", entity_uid, entity_script_name, entity_class_name, rfl_filename);

        // special handling since this entity is created by code and has no reliable UID
        if (entity_script_name == "masako_endgame") {
            grant_achievement(22); // kill masako
            return;
        }
    
        switch (entity_uid) {
            case 9146:
            case 9147:
            case 7338:
            case 9142: {
                if (string_equals_ignore_case(rfl_filename, "l1s1.rfl")) {
                    grant_achievement(8); // kill a fish
                }
                break;
            }

            case 7007: {
                if (string_equals_ignore_case(rfl_filename, "l10s4.rfl")) {
                    grant_achievement(17); // kill big snake
                }
                break;
            }

            case 10696: {
                if (string_equals_ignore_case(rfl_filename, "l7s4.rfl")) {
                    grant_achievement(14); // kill trash bot
                }
                break;
            }

            case 844: {
                if (string_equals_ignore_case(rfl_filename, "l3s4.rfl")) {
                    grant_achievement(25); // kill drone
                }
                break;
            }

            default:
                break;
        }
    }
}

void achievement_player_killed_entity(rf::Entity* entity, int lethal_damage, int lethal_damage_type) {
    if (!entity) {
        return;
    }

    // lethal_damage is unreliable for explosives because there are multiple damage instances sometimes
    // does not handle fire deaths

    // is only called in single player
    int entity_uid = entity->uid;
    rf::String entity_script_name = entity->name;
    rf::String entity_class_name = entity->info->name;
    rf::String rfl_filename = rf::level.filename;
    int weapon = rf::local_player_entity->ai.current_primary_weapon;

    // weapon IDs could change in mods
    if (!rf::mod_param.found()) {
        xlog::warn("player killed {} with weapon {}, damage {}, damage type {}", entity_script_name, weapon, lethal_damage, lethal_damage_type);
    }

}

CodeInjection ai_drop_corpse_achievement_patch{
    0x00409ADB,
    []() {
        grant_achievement_sp(27); // drop a corpse
    },
};

CodeInjection ai_medic_activate_achievement_patch{
    0x0040A73C,
    []() {
        grant_achievement_sp(28); // get healed by a medic
    },
};

CodeInjection player_handle_use_keypress_remote_charge_achievement_patch{
    0x004A18EA,
    []() {
        grant_achievement_sp(29); // pick up a remote charge
    },
};

CodeInjection player_attach_to_security_camera_achievement_patch{
    0x004A1950,
    []() {
        grant_achievement_sp(30); // security monitor
    },
};

CodeInjection player_handle_use_vehicle_achievement_patch{
    0x004A1DE5,
    [](auto& regs) {
        rf::Entity* entity = regs.esi;
        if (entity) {
            //xlog::warn("entered vehicle {} ({}) uid {}", entity->name, entity->info->name, entity->uid);
            if (entity->info->name == "Fighter01") {
                grant_achievement_sp(31); // enter an aesir
            }
            else if (entity->info->name == "sub") {
                grant_achievement_sp(32); // enter a sub

                if (entity->uid == 8163 && string_equals_ignore_case(rf::level.filename, "l5s3.rfl")) {
                    grant_achievement_sp(36); // hidden sub after geothermal
                }
            }
            else if (entity->info->name == "APC") {
                grant_achievement_sp(33); // enter an APC
            }
            else if (entity->info->name == "Jeep01") {
                grant_achievement_sp(34); // enter a jeep
            }
            else if (entity->info->name == "Driller01") {
                grant_achievement_sp(35); // enter a driller
            }
        }
    },
};

ConsoleCommand2 debug_achievement_cmd{
    "dbg_achievement",
    [](int uid) {
        if (!achievement_system_initialized) {
            rf::console::print("The achievement system isn't initialized!");
            return;
        }

        rf::console::print("Checking for an achievement with UID {}...", uid);

        if (!synced_with_ff) {
            xlog::warn("Achievements are not synced with FactionFiles!");
        }
        
        // Look up the achievement
        auto& manager = AchievementManager::get_instance();
        const auto& achievements = manager.get_achievements();

        auto it = achievements.find(uid);
        if (it == achievements.end()) {
            rf::console::print("Achievement UID {} not found!", uid);
            return;
        }

        const Achievement& achievement = it->second;

        std::string msg = std::format(
            "Achievement UID {} found!\n"
            "Name: {}\n"
            "Unlocked? {}\n"
            "Notified? {}\n"
            "Type: {}\n"
            "Category: {}\n"
            "Pending Count: {}\n"
            "Icon: {}\n",
            achievement.uid,
            achievement.name,
            achievement.unlocked ? "Yes" : "No",
            achievement.notified ? "Yes" : "No",
            static_cast<int>(achievement.type),
            static_cast<int>(achievement.category),
            achievement.pending_count,
            achievement.icon
        );

        rf::console::printf(msg.c_str());
    },
    "Look up information for an achievement by UID",
    "dbg_achievement <uid>",
};

ConsoleCommand2 debug_reset_achievements_cmd{
    "dbg_resetachievements",
    []() {
        if (!achievement_system_initialized) {
            rf::console::print("The achievement system isn't initialized!");
            return;
        }

        auto& manager = AchievementManager::get_instance();
        auto& achievements = manager.get_achievements_mutable();

        for (auto& [uid, achievement] : achievements) {
            achievement.notified = false;
        }

        rf::console::print("All achievement notifications have been reset.");
    },
    "Reset achievement notifications",
    "dbg_resetachievements"
};

void achievements_apply_patch()
{
    // Achievement hooks
    ai_drop_corpse_achievement_patch.install();
    ai_medic_activate_achievement_patch.install();
    player_handle_use_keypress_remote_charge_achievement_patch.install();
    player_attach_to_security_camera_achievement_patch.install();
    player_handle_use_vehicle_achievement_patch.install();

    // Console commands
    debug_achievement_cmd.register_cmd();
    debug_reset_achievements_cmd.register_cmd();
}
