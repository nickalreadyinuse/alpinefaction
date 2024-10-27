#pragma once

#include "object.h"
#include "entity.h"
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

        virtual void initialize() = 0;
        virtual void turn_on() = 0;
        virtual void turn_off() = 0;
        virtual void process() = 0;

    };
    static_assert(sizeof(Event) == 0x2B8);

    // custom event structs
    // id 90
    struct EventCloneEntity : Event
    {
        void initialize() override {}
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
        void turn_off() override {}
        void process() override {}
    };

    // id 91
    struct EventSetCollisionPlayer : Event
    {
        void initialize() override {}
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
        void process() override {}
    };


    static auto& event_lookup_from_uid = addr_as_ref<Event*(int uid)>(0x004B6820);
    static auto& event_lookup_from_handle = addr_as_ref<Event*(int handle)>(0x004B6800);
    static auto& event_create = addr_as_ref<Event*(rf::Vector3 pos, int event_type)>(0x004B6870);
    static auto& event_destructor = addr_as_ref<void(rf::Event*, char flags)>(0x004BEF50);
    static auto& event_delete = addr_as_ref<void(rf::Event*)>(0x004B67C0);
    static auto& event_add_link = addr_as_ref<void(int event_handle, int handle)>(0x004B6790);
    }
