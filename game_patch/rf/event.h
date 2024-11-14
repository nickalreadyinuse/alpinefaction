#pragma once

#include "object.h"
#include "entity.h"
#include "../main/main.h"
#include "player/player.h"
#include "os/timestamp.h"

namespace rf
{
    struct Event : Object
    {
        int event_type;
        float delay_seconds;
        Timestamp delay_timestamp;
        VArray<int> links;
        int triggered_by_handle;
        int trigger_handle;
        int event_flags;
        bool delayed_msg;

        virtual void initialize(){};
        virtual void turn_on(){};
        virtual void turn_off(){};
        virtual void process(){};

        // do not directly override, override `do_` instead. Base game does not allocate these.
    virtual void activate(int trigger_handle, int triggered_by_handle, bool on)
    {
        // Call the overridable function, using default if not overridden.
        do_activate(trigger_handle, triggered_by_handle, on);
    }
    
    virtual void activate_links(int trigger_handle, int triggered_by_handle, bool on)
    {
        // Call the overridable function, using default if not overridden.
        do_activate_links(trigger_handle, triggered_by_handle, on);
    }

protected:
    // Default behavior calls the original game function
    virtual void do_activate(int trigger_handle, int triggered_by_handle, bool on)
    {
        AddrCaller{0x004B8B70}.this_call(this, trigger_handle, triggered_by_handle, on);
    }

    virtual void do_activate_links(int trigger_handle, int triggered_by_handle, bool on)
    {
        AddrCaller{0x004B8B00}.this_call(this, trigger_handle, triggered_by_handle, on);
    }
    };
    static_assert(sizeof(Event) == 0x2B8);

    // custom event structs
    // id 90
    struct EventCloneEntity : Event
    {
        void turn_on() override
        {
            xlog::warn("Turning on event UID {}", this->uid);

            for (int i = 0; i < this->links.size(); ++i) {

                int link = this->links[i];
                //xlog::warn("Link at index {}: {}", i, link);
                rf::Object* obj = rf::obj_from_handle(link);
                if (obj) {
                    rf::Entity* entity = static_cast<rf::Entity*>(obj);
                    //xlog::warn("Name: {}, UID: {}, type: {}, ent type: {}", entity->name.c_str(), entity->uid,
                    //           static_cast<int>(entity->type), entity->info_index);
                    rf::Entity* new_entity =
                        rf::entity_create(entity->info_index, entity->name, -1, pos, entity->orient, 0, -1);
                    new_entity->entity_flags = entity->entity_flags;
                    new_entity->pos = entity->pos;
                    new_entity->entity_flags2 = entity->entity_flags2;
                    new_entity->info = entity->info;
                    new_entity->info2 = entity->info2;
                    new_entity->drop_item_class = entity->drop_item_class;
                    new_entity->obj_flags = entity->obj_flags;
                    new_entity->ai.custom_attack_range = entity->ai.custom_attack_range;
                    new_entity->ai.use_custom_attack_range = entity->ai.use_custom_attack_range;
                    new_entity->ai.attack_style = entity->ai.attack_style;
                    new_entity->ai.cooperation = entity->ai.cooperation;
                    new_entity->ai.cover_style = entity->ai.cover_style;
                    for (int i = 0; i < 64; ++i) {
                        new_entity->ai.has_weapon[i] = entity->ai.has_weapon[i];
                        new_entity->ai.clip_ammo[i] = entity->ai.clip_ammo[i];
                    }

                    for (int i = 0; i < 32; ++i) {
                        new_entity->ai.ammo[i] = entity->ai.ammo[i];
                    }
                    new_entity->ai.current_secondary_weapon = entity->ai.current_secondary_weapon;
                    new_entity->ai.current_primary_weapon = entity->ai.current_primary_weapon;

                    //xlog::warn("Name: {}, UID: {}, type: {}, ent type: {}", new_entity->name.c_str(), new_entity->uid,
                    //           static_cast<int>(new_entity->type), new_entity->info_index);
                }
            }
        }
    };

    // id 91
    struct EventSetCollisionPlayer : Event
    {
        void turn_on() override
        {
            xlog::warn("Turning on event UID {}", this->uid);
            rf::local_player->collides_with_world = true;
        }
        void turn_off() override
        {
            xlog::warn("Turning off event UID {}", this->uid);
            rf::local_player->collides_with_world = false;
        }
    };

    // id 92
    struct EventSwitchRandom : Event
    {
        void turn_on() override
        {
            xlog::warn("Turning on event UID {}", this->uid);

            if (this->links.size() > 0) {
                // select a random index from links
                std::uniform_int_distribution<int> dist(0, this->links.size() - 1);
                int random_index = dist(g_rng);
                int link_handle = this->links[random_index];

                rf::Object* obj = rf::obj_from_handle(link_handle);
                if (obj) {
                    rf::Event* linked_event = static_cast<rf::Event*>(obj);
                    if (linked_event) {
                        // Send the "turn on" message
                        linked_event->turn_on();
                        xlog::warn("Randomly selected event UID {} and turned it on.", linked_event->uid);
                    }
                }
            }
            else {
                xlog::warn("Event UID {} has no links to turn on.", this->uid);
            }

        }
    };

    // id 93
    struct EventGateIsEasy : Event
    {
        rf::GameDifficultyLevel difficulty = GameDifficultyLevel::DIFFICULTY_EASY;

        void turn_on() override
        {
            if (this->name == "Gate_Is_Medium") {
                difficulty = GameDifficultyLevel::DIFFICULTY_MEDIUM;
            }
            else if (this->name == "Gate_Is_Hard") {
                difficulty = GameDifficultyLevel::DIFFICULTY_HARD;
            }
            else if (this->name == "Gate_Is_Impossible") {
                difficulty = GameDifficultyLevel::DIFFICULTY_IMPOSSIBLE;
            }
            else {
                difficulty = GameDifficultyLevel::DIFFICULTY_EASY;
            }

            xlog::warn("Gate {} with UID {} is checking for difficulty {}",
                this->name, this->uid, static_cast<int>(difficulty));

            if (rf::game_get_skill_level() == difficulty) {
                activate_links(this->trigger_handle, this->triggered_by_handle, true);
            }
        }

        void turn_off() override
        {
            if (this->name == "Gate_Is_Medium") {
                difficulty = GameDifficultyLevel::DIFFICULTY_MEDIUM;
            }
            else if (this->name == "Gate_Is_Hard") {
                difficulty = GameDifficultyLevel::DIFFICULTY_HARD;
            }
            else if (this->name == "Gate_Is_Impossible") {
                difficulty = GameDifficultyLevel::DIFFICULTY_IMPOSSIBLE;
            }
            else {
                difficulty = GameDifficultyLevel::DIFFICULTY_EASY;
            }

            xlog::warn("Gate {} with UID {} is checking for difficulty {}",
                this->name, this->uid, static_cast<int>(difficulty));

            if (rf::game_get_skill_level() == difficulty) {
                activate_links(this->trigger_handle, this->triggered_by_handle, false);
            }
        }
    };

    enum class EventType : int
    {
        Attack = 1,
        Bolt_State,
        Continuous_Damage,
        Cyclic_Timer,
        Drop_Point_Marker,
        Explode,
        Follow_Player,
        Follow_Waypoints,
        Give_Item_To_Player,
        Goal_Create,
        Goal_Check,
        Goal_Set,
        Goto,
        Goto_Player,
        Heal,
        Invert,
        Load_Level,
        Look_At,
        Make_Invulnerable,
        Make_Fly,
        Make_Walk,
        Message,
        Music_Start,
        Music_Stop,
        Particle_State,
        Play_Animation,
        Play_Sound,
        Slay_Object,
        Remove_Object,
        Set_AI_Mode,
        Set_Light_State,
        Set_Liquid_Depth,
        Set_Friendliness,
        Shake_Player,
        Shoot_At,
        Shoot_Once,
        Armor,
        Spawn_Object,
        Swap_Textures,
        Switch,
        Switch_Model,
        Teleport,
        When_Dead,
        Set_Gravity,
        Alarm,
        Alarm_Siren,
        Go_Undercover,
        Delay,
        Monitor_State,
        UnHide,
        Push_Region_State,
        When_Hit,
        Headlamp_State,
        Item_Pickup_State,
        Cutscene,
        Strip_Player_Weapons,
        Fog_State,
        Detach,
        Skybox_State,
        Force_Monitor_Update,
        Black_Out_Player,
        Turn_Off_Physics,
        Teleport_Player,
        Holster_Weapon,
        Holster_Player_Weapon,
        Modify_Rotating_Mover,
        Clear_Endgame_If_Killed,
        Win_PS2_Demo,
        Enable_Navpoint,
        Play_Vclip,
        Endgame,
        Mover_Pause,
        Countdown_Begin,
        Countdown_End,
        When_Countdown_Over,
        Activate_Capek_Shield,
        When_Enter_Vehicle,
        When_Try_Exit_Vehicle,
        Fire_Weapon_No_Anim,
        Never_Leave_Vehicle,
        Drop_Weapon,
        Ignite_Entity,
        When_Cutscene_Over,
        When_Countdown_Reaches,
        Display_Fullscreen_Image,
        Defuse_Nuke,
        When_Life_Reaches,
        When_Armor_Reaches,
        Reverse_Mover,
        Clone_Entity, // 90
        Set_Player_World_Collide,
        Switch_Random,
        Gate_Is_Easy
    };

    // int to EventType
    inline EventType int_to_event_type(int id)
    {
        return static_cast<EventType>(id);
    }

    // EventType to int
    inline int event_type_to_int(EventType eventType)
    {
        return static_cast<int>(eventType);
    }

    static auto& event_lookup_from_uid = addr_as_ref<Event*(int uid)>(0x004B6820);
    static auto& event_lookup_from_handle = addr_as_ref<Event*(int handle)>(0x004B6800);
    static auto& event_create = addr_as_ref<Event*(rf::Vector3 pos, int event_type)>(0x004B6870);
    //static auto& event_destructor = addr_as_ref<void(rf::Event*, char flags)>(0x004BEF50); // probably crashes, unneeded
    static auto& event_delete = addr_as_ref<void(rf::Event*)>(0x004B67C0);
    static auto& event_add_link = addr_as_ref<void(int event_handle, int handle)>(0x004B6790);

}
