#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include "../rf/trigger.h"
#include "../rf/event.h"

enum class AchievementType : int
{
    basic,
    ff_authoritative
};

enum class AchievementCategory : int
{
    general,
    statistics,
    singleplayer,
    multiplayer,
    base_campaign_story,
    base_campaign_optional,
};

struct Achievement
{
    int uid = -1;
    std::string name = "";
    std::string icon = ""; // af_achico_000.tga
    AchievementCategory category = AchievementCategory::general;
    AchievementType type = AchievementType::basic;
    int pending_count = 0; // local storage, pending sending to ff
    bool unlocked = false; // if true, don't attempt to send to ff again
    bool notified = false; // if true, don't notify them again
};

struct LoggedPlayerKill
{
    int uid = -1;
    std::string rfl_filename = "";
    std::string script_name = "";
    std::string class_name = "";
    int damage_type = -1;
    int likely_weapon = -1;
    std::string tc_mod = "";
};

struct AchievementStateInfo
{
    bool began_campaign_at_start = false;
};

class AchievementManager
{
public:
    static AchievementManager& get_instance()
    {
        static AchievementManager instance;
        return instance;
    }

    void initialize();
    void grant_achievement(int uid);
    bool is_unlocked(int uid) const;
    void send_update_to_ff();
    void sync_with_ff();
    void add_key_to_ff_update_map();

    const std::unordered_map<int, Achievement>& get_achievements() const {
        return achievements;
    }

    std::unordered_map<int, Achievement>& get_achievements_mutable() {
        return achievements;
    }


private:
    std::unordered_map<int, Achievement> achievements;

    void process_ff_response(const std::string& response, int expected_key, bool is_initial_sync);
    void show_notification(Achievement& achievement);
};

bool is_achievement_system_initialized();

void grant_achievement(int uid);
void grant_achievement_sp(int uid);
void achievement_check_trigger(rf::Trigger* trigger);
void achievement_check_event(rf::Event* event);
void achievement_check_entity_death(rf::Entity* entity);
void achievement_player_killed_entity(rf::Entity* entity, int lethal_damage, int lethal_damage_type);

void achievement_system_do_frame();
void clear_achievement_notification();
