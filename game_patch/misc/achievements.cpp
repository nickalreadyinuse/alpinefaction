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
#include "../sound/sound.h"
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
        {1, 1, "Explosive Discovery", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign_optional},
        {2, 2, "Tools of the Trade", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign_story},
        {3, 3, "Welcome to Mars", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign_story},
        {4, 4, "Too easy!", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign_optional},
        {5, 5, "Moderate Success", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign_optional},
        {6, 6, "Tough as Nails", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign_optional},
        {7, 7, "Martian All-Star", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign_optional},
        {8, 8, "Gone Fishin'", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign_optional},
        {9, 9, "Welcome", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign_story},
        {10, 10, "Red Alert", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign_optional},
        {11, 11, "If only you'd been faster", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign_story},
        {12, 12, "Not a fan", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign_story},
        {13, 13, "Sabotage", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign_story},
        {14, 14, "Unsuppressed", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign_story},
        {15, 15, "Mad Martian Meetup", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign_story},
        {16, 16, "It Suddenly Worked", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign_optional},
        {17, 17, "Who's Cleaning This Up?", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign_optional},
        {18, 18, "Stowaway", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign_story},
        {19, 19, "Try a Reboot", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign_optional},
        {20, 20, "...Oops", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign_story},
        {21, 21, "Forget About Parker!", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign_story},
        {22, 22, "Parental Visit Denied", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign_story},
        {23, 23, "Let There be Light!", "APC_Cocpit_P13.tga", AchievementCategory::singleplayer},
        {24, 24, "Submerged Secret", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign_story},
        {25, 25, "The Dangers of Recycling", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign_story},
        {26, 26, "Pyroscientist", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign_story},
        {27, 27, "Body Hiders", "APC_Cocpit_P13.tga", AchievementCategory::singleplayer},
        {28, 28, "Healthy as a Horse", "APC_Cocpit_P13.tga", AchievementCategory::singleplayer},
        {29, 29, "Double Demolition", "APC_Cocpit_P13.tga", AchievementCategory::singleplayer},
        {30, 30, "Watchful Eye", "APC_Cocpit_P13.tga", AchievementCategory::singleplayer},
        {31, 31, "A Decent Descent", "APC_Cocpit_P13.tga", AchievementCategory::singleplayer},
        {32, 32, "Water on Mars", "APC_Cocpit_P13.tga", AchievementCategory::singleplayer},
        {33, 33, "Tread Lightly", "APC_Cocpit_P13.tga", AchievementCategory::singleplayer},
        {34, 34, "We don't need roads", "APC_Cocpit_P13.tga", AchievementCategory::singleplayer},
        {35, 35, "Totally Recalled", "APC_Cocpit_P13.tga", AchievementCategory::singleplayer},
        {36, 36, "Sub-dued", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign_optional},
        {37, 37, "Goin' for a swim, sir?", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign_optional},
        {38, 38, "Hendrix Saves the Day", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign_optional},
        {39, 39, "GeoMod Limitation", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign_optional},
        {40, 40, "Amphibious Car Challenge", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign_optional},
        {41, 41, "Embrace Futility", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign_optional},
        {42, 42, "Starter Rebel", "APC_Cocpit_P13.tga", AchievementCategory::base_campaign_story},
    };

    for (const auto& achievement : predefined_achievements) {
        achievements[achievement.facet_uid] = achievement;
        xlog::warn("added achievement {} with facet_uid {}", achievement.name, achievement.facet_uid);
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
            HttpSession session("Alpine Faction v1.1.0 Achievement Sync");
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
            HttpSession session("Alpine Faction v1.1.0 Achievement Push");
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
    for (auto& [facet_uid, achievement] : achievements) {
        if (achievement.pending_count > 0) {
            if (!first) {
                oss << ",";
            }
            oss << achievement.root_uid << "=" << achievement.pending_count;
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
    xlog::warn("Achievement notified: {}", achievement.name);

    // clear previous info before repopulating
    clear_achievement_notification();

    // drawn in draw_achievement_box
    achievement_box_icon = rf::bm::load(achievement.icon.c_str(), -1, true);
    achievement_box_name = achievement.name;
    achievement_box_timestamp.set(10000); // 10 seconds
    achievement_box_visible = true;
    play_local_sound_2d(get_custom_sound_id(0), 0, 1.0f);
}

// forcefully grant achievement
void grant_achievement(int uid)
{
    if (achievement_system_initialized) {
        //xlog::warn("attempting to give achievement {}", uid);
        AchievementManager::get_instance().grant_achievement(uid);
    }    
}

void initialize_achievement_manager() {
    // build achievement storage and sync with FF
    if (!rf::is_server && !rf::is_dedicated_server) {
        AchievementManager::get_instance().initialize();
    }
}

void draw_achievement_box_content(int x, int y) {
    if (achievement_box_icon != -1) {
        rf::gr::set_color(255, 255, 255, 0xCC);
        rf::gr::bitmap(achievement_box_icon, x + 4, y + 4);
    }

    int str_x = x + 278;
    int str_y = y + 8;

    rf::gr::set_color(255, 255, 255, 0xCC);
    rf::gr::string_aligned(rf::gr::ALIGN_CENTER, str_x, str_y, "Achievement Unlocked!", 0);

    rf::gr::set_color(255, 255, 180, 0xCC);
    rf::gr::string_aligned(rf::gr::ALIGN_CENTER, str_x, str_y + 30, achievement_box_name.c_str(), 0);
}

void draw_achievement_box() {
    int w = 480;
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
    if (!rf::is_multi) {
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
                    grant_achievement(42); // game start
                }
                break;
            }

            case 7661: {
                if (string_equals_ignore_case(rfl_filename, "train02.rfl")) {
                    grant_achievement(3); // finish training
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

            case 3350: {
                if (string_equals_ignore_case(rfl_filename, "l4s4.rfl")) {
                    grant_achievement(12); // exit ventilation
                }
                break;
            }

            case 4502: {
                if (string_equals_ignore_case(rfl_filename, "l5s2.rfl")) {
                    grant_achievement(13); // destroy geothermal
                }
                break;
            }

            case 8571: {
                if (string_equals_ignore_case(rfl_filename, "l19s1.rfl")) {
                    grant_achievement(21); // merc prison
                }
                break;
            }

            case 7095: {
                if (string_equals_ignore_case(rfl_filename, "l6s3.rfl")) {
                    grant_achievement(37); // l6s3 fountain
                }
                break;
            }

            case 4029: {
                if (string_equals_ignore_case(rfl_filename, "l5s3.rfl")) {
                    grant_achievement(39); // crane button
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
                break;
            }

            case 7005: {
                if (string_equals_ignore_case(rfl_filename, "l10s4.rfl")) {
                    grant_achievement(16); // end of capek message series
                }
                break;
            }

            case 9449: {
                if (string_equals_ignore_case(rfl_filename, "l1s1.rfl")) {
                    grant_achievement(9); // first eos message
                }
                break;
            }

            case 10367: {
                if (string_equals_ignore_case(rfl_filename, "l8s4.rfl")) {
                    grant_achievement(15); // meet capek
                }
                break;
            }

            case 4407: {
                if (string_equals_ignore_case(rfl_filename, "l5s2.rfl")) {
                    grant_achievement(41); // when pump stations dead geothermal
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

// vehicle player is piloting enters water
CodeInjection entity_update_water_status_achievement_patch{
    0x004291B4,
    [](auto& regs) {
        rf::Entity* vehicle = regs.esi;
        if (vehicle) {
            xlog::warn("vehicle {} ({}) uid {}", vehicle->name, vehicle->info->name, vehicle->uid);
            if (vehicle->info->name == "Jeep01") {
                grant_achievement_sp(40); // submerge jeep in water
            }
        }
    },
};

// after delay duration, only on events that forward messages
CodeInjection event_activate_links_achievement_patch{
    0x004B8B0A,
    [](auto& regs) {
        rf::Event* event = regs.ecx;

        if (!event) {
            return;
        }

        int event_uid = event->uid;
        rf::String rfl_filename = rf::level.filename;

        // single player
        if (!rf::is_multi) {
            switch (event_uid) {
                case 8711: {
                    if (string_equals_ignore_case(rfl_filename, "l3s2.rfl")) {
                        grant_achievement(10); // train in miner registration
                    }
                    break;
                }

                case 2014: {
                    if (string_equals_ignore_case(rfl_filename, "l3s3.rfl")) {
                        grant_achievement(11); // shuttle blow up (maybe below slow isn't such a bad thing)
                    }
                    break;
                }

                case 9458: {
                    if (string_equals_ignore_case(rfl_filename, "l4s1a.rfl")) {
                        grant_achievement(38); // abandoned mines door
                    }
                    break;
                }

                default:
                    break;
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
            achievement.facet_uid,
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

        rf::console::print("Achievement notifications have been reset.");
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
    entity_update_water_status_achievement_patch.install();
    event_activate_links_achievement_patch.install();

    // Console commands
    debug_achievement_cmd.register_cmd();
    debug_reset_achievements_cmd.register_cmd();
}
