#pragma once

#include <string>
#include <vector>
#include <sstream>
#include <optional>
#include "object.h"
#include "level.h"
#include "misc.h"
#include "hud.h"
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

        // handler storage, defined in event.vpp
        static std::unordered_map<const Event*, std::unordered_map<std::string, std::function<void(Event*, const std::string&)>>>
            variable_handler_storage;

        // register variable handlers (AF new) plus default event initialization (does nothing)
        // safe to override, but including call to base struct initialize is a good idea for var handler registration
        virtual void initialize()
        {
            register_variable_handlers();
            AddrCaller{0x004B8CD0}.this_call(this);
        };

        // default event turning on (switch based on some default event types)
        // safe to override
        virtual void turn_on()
        {
            AddrCaller{0x004B9070}.this_call(this);
        };

        // default event turning off (switch based on some default event types)
        // safe to override
        virtual void turn_off()
        {
            AddrCaller{0x004B9F80}.this_call(this);
        };

        // default event processing, handles delays (set in activate) and switch on some default event types
        // if overridden, delays need to be handled otherwise event won't work with delay specified
        virtual void process()
        {
            AddrCaller{0x004B8CE0}.this_call(this);
        };

        // do not directly override activate and activate_links, override `do_`. Base game does not allocate these
        virtual void activate(int trigger_handle, int triggered_by_handle, bool on)
        {
            // call the overridable function, using default if not overridden
            do_activate(trigger_handle, triggered_by_handle, on);
        }
    
        virtual void activate_links(int trigger_handle, int triggered_by_handle, bool on)
        {
            // call the overridable function, using default if not overridden
            do_activate_links(trigger_handle, triggered_by_handle, on);
        }

    protected:
        // default internal event activation, handles delays, turn on/off, and event forwarding
        // if overridden, delays need to be handled otherwise event won't work with delay specified
        virtual void do_activate(int trigger_handle, int triggered_by_handle, bool on)
        {
            AddrCaller{0x004B8B70}.this_call(this, trigger_handle, triggered_by_handle, on);
        }

        // default event link activation, handles sending on/off signals to links
        // safe to override, but be careful - this is standard behaviour for all stock events
        virtual void do_activate_links(int trigger_handle, int triggered_by_handle, bool on)
        {
            AddrCaller{0x004B8B00}.this_call(this, trigger_handle, triggered_by_handle, on);
        }

        virtual void register_variable_handlers()
        {
            auto& handlers = variable_handler_storage[this];
            handlers["delay"] = [](Event* event, const std::string& value) {
                event->delay_seconds = std::stof(value);
                xlog::warn("apply_var: delay set to {}", event->delay_seconds);
            };
        }

    public:
        virtual void apply_var(const std::string& var_name, const std::string& value)
        {
            auto it = variable_handler_storage.find(this); // Access handlers by `this`
            if (it != variable_handler_storage.end()) {
                auto& handlers = it->second;
                auto handler_it = handlers.find(var_name);
                if (handler_it != handlers.end()) {
                    try {
                        handler_it->second(this, value);
                    }
                    catch (const std::exception& ex) {
                        xlog::error("apply_var: Failed to set var_name={} with value={} - {}", var_name, value,
                                    ex.what());
                    }
                }
                else {
                    xlog::warn("apply_var: Unsupported var_name={} for Event", var_name);
                }
            }
            else {
                xlog::warn("apply_var: No handlers registered for Event");
            }
        }
    };
    static_assert(sizeof(Event) == 0x2B8);

    // used for SetVar events only
    struct SetVarEventParts
    {
        bool auto_activate;
        std::string var_name;
        std::string value;
    };

    std::optional<SetVarEventParts> parse_event_name(const std::string& name); // defined in event.cpp

    // custom event structs
    // id 90
    struct EventSetVar : Event
    {
        bool fired = false;

        void turn_on() override
        {
            xlog::warn("Activating UID {}", this->uid);
            this->activate(this->trigger_handle, this->triggered_by_handle, true);
        }

        void process() override
        {
            // xlog::warn("Processing UID {}", this->uid);

            if (fired || (this->name.empty() || this->name[0] != '!')) {
                return; // Skip processing if already fired or manual activation is required
            }

            // Check if all linked events are initialized and have handlers registered
            bool all_handlers_registered = true;
            for (int link_handle : this->links) {
                rf::Object* obj = rf::obj_from_handle(link_handle);
                if (obj && obj->type == OT_EVENT) {
                    auto* linked_event = static_cast<rf::Event*>(obj);

                    // Check if the linked event has registered handlers in the static storage
                    if (Event::variable_handler_storage.find(linked_event) == Event::variable_handler_storage.end()) {
                        xlog::warn("Linked event UID {} has not registered handlers", linked_event->uid);
                        all_handlers_registered = false;
                        break;
                    }
                }
                else {
                    xlog::warn("Linked handle {} is not a valid Event", link_handle);
                }
            }

            if (!all_handlers_registered) {
                xlog::warn("UID {}: Not all linked events have registered handlers", this->uid);
                return;
            }

            xlog::warn("Activating UID {}", this->uid);
            this->activate(this->trigger_handle, this->triggered_by_handle, true);
            fired = true;
        }

        void do_activate(int trigger_handle, int triggered_by_handle, bool on) override
        {
            auto parsed_name = parse_event_name(this->name);
            if (!parsed_name) {
                xlog::error("Failed to parse event name: {}", this->name);
                return;
            }

            auto& parts = *parsed_name;

            // Apply the variable to all linked events
            for (int link_handle : this->links) {
                rf::Object* obj = rf::obj_from_handle(link_handle);
                if (obj && obj->type == OT_EVENT) {
                    rf::Event* linked_event = static_cast<rf::Event*>(obj);
                    linked_event->apply_var(parts.var_name, parts.value);
                }
            }
        }
    };

    // id 91
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

    // id 92
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

    // id 93
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

    // id 94
    struct EventDifficultyGate : Event
    {
        rf::GameDifficultyLevel difficulty = GameDifficultyLevel::DIFFICULTY_EASY;
        //bool should_apply_underwater = false;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers(); // Include base handlers

            auto& handlers = variable_handler_storage[this];
            handlers["difficulty"] = [](Event* event, const std::string& value) {
                auto* gate_event = static_cast<EventDifficultyGate*>(event);
                int difficulty_value = std::stoi(value);
                gate_event->difficulty = static_cast<rf::GameDifficultyLevel>(difficulty_value);
                xlog::warn("apply_var: Set difficulty to {} for EventDifficultyGate", difficulty_value);
            };

            // handler reg template (not used)
            //handlers["should_apply_underwater"] = [](Event* event, const std::string& value) {
            //    auto* gate_event = static_cast<EventDifficultyGate*>(event);
            //    gate_event->should_apply_underwater = (value == "true");
            //    xlog::warn("apply_var: Set should_apply_underwater to {}", gate_event->should_apply_underwater);
            //};
        }

        void turn_on() override
        {
            xlog::warn("Gate {} with UID {} is checking for difficulty {}",
                this->name, this->uid, static_cast<int>(difficulty));

            if (rf::game_get_skill_level() == difficulty) {
                activate_links(this->trigger_handle, this->triggered_by_handle, true);
            }
        }

        void turn_off() override
        {
            xlog::warn("Gate {} with UID {} is checking for difficulty {}",
                this->name, this->uid, static_cast<int>(difficulty));

            if (rf::game_get_skill_level() == difficulty) {
                activate_links(this->trigger_handle, this->triggered_by_handle, false);
            }
        }
    };

    // id 95
    struct EventHUDMessage : Event
    {
        std::string message = "";
        std::optional<int> duration;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers(); // Include base handlers

            auto& handlers = variable_handler_storage[this];
            handlers["message"] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventHUDMessage*>(event);
                this_event->message = value;
                xlog::warn("apply_var: Set message to '{}' for EventHUDMessage UID={}", this_event->message,
                           this_event->uid);
            };

            handlers["duration"] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventHUDMessage*>(event);
                this_event->duration = std::stoi(value);
                xlog::warn("apply_var: Set duration to '{}' for EventHUDMessage UID={}",
                           this_event->duration.value_or(0), this_event->uid);
            };            
        }

        void turn_on() override
        {
            rf::hud_msg(message.c_str(), 0, duration.value_or(0), 0);
        }

        void turn_off() override
        {
            rf::hud_msg_clear();
        }
    };

    // id 96
    struct EventPlayVideo : Event
    {
        std::optional<std::string> filename;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers(); // Include base handlers

            auto& handlers = variable_handler_storage[this];
            handlers["filename"] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventPlayVideo*>(event);
                this_event->filename = value;
                xlog::warn("apply_var: Set message to '{}' for EventPlayVideo UID={}", this_event->filename->c_str(),
                           this_event->uid);
            };            
        }

        void turn_on() override
        {
            if (filename) {
                rf::bink_play(filename->c_str());
            }
            else {
                xlog::error("EventPlayVideo UID={} attempted to play a video without a valid filename.", this->uid);
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
        SetVar, // 90
        Clone_Entity,
        Set_Player_World_Collide,
        Switch_Random,
        Difficulty_Gate,
        HUD_Message,
        Play_Video
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
    static auto& event_create = addr_as_ref<Event*(const rf::Vector3* pos, int event_type)>(0x004B6870);
    //static auto& event_destructor = addr_as_ref<void(rf::Event*, char flags)>(0x004BEF50); // probably crashes, unneeded
    static auto& event_delete = addr_as_ref<void(rf::Event*)>(0x004B67C0);
    static auto& event_add_link = addr_as_ref<void(int event_handle, int handle)>(0x004B6790);
    

}
