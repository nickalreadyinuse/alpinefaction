#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include "../rf/trigger.h"
#include "../rf/event.h"

enum class AchievementName : int
{
    SecretFusion,
    StartTraining,
    FinishTraining,
    FinishCampaignEasy,
    FinishCampaignMedium,
    FinishCampaignHard,
    FinishCampaignImp,
    KillFish,
    HearEos,
    LockedInTram,
    MissShuttle,
    Ventilation,
    DestroyGeothermal,
    DestroyTrashBot,
    MeetCapek,
    CapekVoiceEnd,
    KillSnake,
    GoSpaceStation,
    ComputersSpaceStation,
    DestroySpaceStation,
    MercJail,
    KillMasako,
    UseFlashlight,
    UnderwaterSub,
    KillDrone,
    KillCapekFlamethrower,
    DropCorpse,
    UseMedic,
    DupeC4,
    ViewMonitor,
    EnterAesir,
    EnterSub,
    EnterAPC,
    EnterJeep,
    EnterDriller,
    SecretSub,
    AdminFountain,
    HendrixHackDoor,
    SubCraneButton,
    JeepWater,
    DestroyPumpStations,
    StartCampaign,
    FastBomb,
    FarKill
};

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
    int facet_uid = -1;
    int root_uid = -1;
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
    std::string script_name = "";
    std::string class_name = "";
    int damage_type = -1;
    int likely_weapon = -1;
    std::string rfl_filename = "";
    std::string tc_mod = "";
};

struct AchievementStateInfo
{
    bool began_campaign_at_start = false;
    bool difficulty_unchanged_throughout_campaign = false;

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
    void grant_achievement(AchievementName achievement);
    bool is_unlocked(int uid) const;
    void send_update_to_ff();
    void sync_with_ff();
    void add_key_to_ff_update_map();
    AchievementName find_achievement_by_facet_uid(int facet_uid);

    const std::unordered_map<AchievementName, Achievement>& get_achievements() const {
    return achievements;
    }

    std::unordered_map<AchievementName, Achievement>& get_achievements_mutable() {
        return achievements;
    }

private:
    std::unordered_map<AchievementName, Achievement> achievements;

    void process_ff_response(const std::string& response, int expected_key, bool is_initial_sync);
    void show_notification(Achievement& achievement);
    const Achievement* get_achievement(AchievementName achievement_name);
};

bool is_achievement_system_initialized();

void grant_achievement(AchievementName achievement);
void grant_achievement_sp(AchievementName achievement);
void achievement_check_trigger(rf::Trigger* trigger);
void achievement_check_event(rf::Event* event);
void achievement_check_entity_death(rf::Entity* entity);
void achievement_player_killed_entity(rf::Entity* entity, int lethal_damage, int lethal_damage_type);

void achievement_system_do_frame();
void clear_achievement_notification();
