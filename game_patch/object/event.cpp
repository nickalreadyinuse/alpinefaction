#include <patch_common/CodeInjection.h>
#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include <patch_common/AsmWriter.h>
#include <xlog/xlog.h>
#include <cassert>
#include "../rf/object.h"
#include "../rf/event.h"
#include "../rf/entity.h"
#include "../rf/level.h"
#include "../rf/multi.h"
#include "../rf/player/player.h"
#include "../rf/os/console.h"
#include "../os/console.h"

bool event_debug_enabled;

CodeInjection switch_model_event_custom_mesh_patch{
    0x004BB921,
    [](auto& regs) {
        auto& mesh_type = regs.ebx;
        if (mesh_type != rf::MESH_TYPE_UNINITIALIZED) {
            return;
        }
        auto& mesh_name = *static_cast<rf::String*>(regs.esi);
        std::string_view mesh_name_sv{mesh_name.c_str()};
        if (string_ends_with_ignore_case(mesh_name_sv, ".v3m")) {
            mesh_type = rf::MESH_TYPE_STATIC;
        }
        else if (string_ends_with_ignore_case(mesh_name_sv, ".v3c")) {
            mesh_type = rf::MESH_TYPE_CHARACTER;
        }
        else if (string_ends_with_ignore_case(mesh_name_sv, ".vfx")) {
            mesh_type = rf::MESH_TYPE_ANIM_FX;
        }
    },
};

CodeInjection switch_model_event_obj_lighting_and_physics_fix{
    0x004BB940,
    [](auto& regs) {
        rf::Object* obj = regs.edi;
        // Fix physics
        if (obj->vmesh) {
            rf::ObjectCreateInfo oci;
            oci.pos = obj->p_data.pos;
            oci.orient = obj->p_data.orient;
            oci.material = obj->material;
            oci.drag = obj->p_data.drag;
            oci.mass = obj->p_data.mass;
            oci.physics_flags = obj->p_data.flags;
            oci.radius = obj->radius;
            oci.vel = obj->p_data.vel;
            int num_vmesh_cspheres = rf::vmesh_get_num_cspheres(obj->vmesh);
            for (int i = 0; i < num_vmesh_cspheres; ++i) {
                rf::PCollisionSphere csphere;
                rf::vmesh_get_csphere(obj->vmesh, i, &csphere.center, &csphere.radius);
                csphere.spring_const = -1.0f;
                oci.spheres.add(csphere);
            }
            rf::physics_delete_object(&obj->p_data);
            rf::physics_create_object(&obj->p_data, &oci);
        }
    },
};

struct EventSetLiquidDepthHook : rf::Event
{
    float depth;
    float duration;
};

void __fastcall EventSetLiquidDepth_turn_on_new(EventSetLiquidDepthHook* this_)
{
    xlog::info("Processing Set_Liquid_Depth event: uid {} depth {:.2f} duration {:.2f}", this_->uid, this_->depth, this_->duration);
    if (this_->links.size() == 0) {
        xlog::trace("no links");
        rf::add_liquid_depth_update(this_->room, this_->depth, this_->duration);
    }
    else {
        for (auto room_uid : this_->links) {
            rf::GRoom* room = rf::level_room_from_uid(room_uid);
            xlog::trace("link {} {}", room_uid, room);
            if (room) {
                rf::add_liquid_depth_update(room, this_->depth, this_->duration);
            }
        }
    }
}

extern CallHook<void __fastcall (rf::GRoom*, int, rf::GSolid*)> liquid_depth_update_apply_all_GRoom_reset_liquid_hook;

void __fastcall liquid_depth_update_apply_all_GRoom_reset_liquid(rf::GRoom* room, int edx, rf::GSolid* solid) {
    liquid_depth_update_apply_all_GRoom_reset_liquid_hook.call_target(room, edx, solid);

    // check objects in room if they are in water
    auto* objp = rf::object_list.next_obj;
    while (objp != &rf::object_list) {
        if (objp->room == room) {
            if (objp->type == rf::OT_ENTITY) {
                auto* ep = static_cast<rf::Entity*>(objp);
                rf::entity_update_liquid_status(ep);
                bool is_in_liquid = ep->obj_flags & rf::OF_IN_LIQUID;
                // check if entity doesn't have 'swim' flag
                if (is_in_liquid && !rf::entity_can_swim(ep)) {
                    // he does not have swim animation - kill him
                    objp->life = 0.0f;
                }
            }
            else {
                rf::obj_update_liquid_status(objp);
            }
        }
        objp = objp->next_obj;
    }
}

CallHook<void __fastcall (rf::GRoom* room, int edx, rf::GSolid* geo)> liquid_depth_update_apply_all_GRoom_reset_liquid_hook{
    0x0045E4AC,
    liquid_depth_update_apply_all_GRoom_reset_liquid,
};

CallHook<int(rf::AiPathInfo*)> ai_path_release_on_load_level_event_crash_fix{
    0x004BBD99,
    [](rf::AiPathInfo* pathp) {
        // Clear GPathNode pointers before level load
        pathp->adjacent_node1 = nullptr;
        pathp->adjacent_node2 = nullptr;
        return ai_path_release_on_load_level_event_crash_fix.call_target(pathp);
    },
};

FunHook<void()> event_level_init_post_hook{
    0x004BD890,
    []() {
        event_level_init_post_hook.call_target();
        if (string_equals_ignore_case(rf::level.filename, "L5S2.rfl")) {
            // HACKFIX: make Set_Liquid_Depth events properties in lava control room more sensible
            xlog::trace("Changing Set_Liquid_Depth events in this level...");
            auto* event1 = static_cast<EventSetLiquidDepthHook*>(rf::event_lookup_from_uid(3940));
            auto* event2 = static_cast<EventSetLiquidDepthHook*>(rf::event_lookup_from_uid(4132));
            if (event1 && event2 && event1->duration == 0.15f && event2->duration == 0.15f) {
                event1->duration = 1.5f;
                event2->duration = 1.5f;
            }
        }
        if (string_equals_ignore_case(rf::level.filename, "L5S3.rfl")) {
            // Fix submarine exploding - change delay of two events to make submarine physics enabled later
            xlog::trace("Fixing Submarine exploding bug...");
            int uids[] = {4679, 4680};
            for (int uid : uids) {
                auto* event = rf::event_lookup_from_uid(uid);
                if (event && event->delay_seconds == 1.5f) {
                    event->delay_seconds += 1.5f;
                }
            }
        }
    },
};

extern FunHook<void __fastcall(rf::Event *)> EventMessage__turn_on_hook;
void __fastcall EventMessage__turn_on_new(rf::Event *this_)
{
    if (!rf::is_dedicated_server) EventMessage__turn_on_hook.call_target(this_);
}
FunHook<void __fastcall(rf::Event *this_)> EventMessage__turn_on_hook{
    0x004BB210,
    EventMessage__turn_on_new,
};

CodeInjection event_activate_injection{
    0x004B8BF4,
    [](auto& regs) {
        if (event_debug_enabled) {
            rf::Event* event = regs.esi;
            bool on = addr_as_ref<bool>(regs.esp + 0xC + 0xC);
            rf::console::print("Processing {} message in event {} ({})",
            on ? "ON" : "OFF", event->name, event->uid);
        }
    },
};

CodeInjection event_activate_injection2{
    0x004B8BE3,
    [](auto& regs) {
        if (event_debug_enabled) {
            rf::Event* event = regs.esi;
            bool on = regs.cl;
            rf::console::print("Delaying {} message in event {} ({})",
                on ? "ON" : "OFF", event->name, event->uid);
        }
    },
};

CodeInjection event_process_injection{
    0x004B8CF5,
    [](auto& regs) {
        if (event_debug_enabled) {
            rf::Event* event = regs.esi;
            rf::console::print("Processing {} message in event {} ({}) (delayed)",
                event->delayed_msg ? "ON" : "OFF", event->name, event->uid);
        }
    },
};

CodeInjection event_load_level_turn_on_injection{
    0x004BB9C9,
    [](auto& regs) {
        if (rf::local_player->flags & (rf::PF_KILL_AFTER_BLACKOUT|rf::PF_END_LEVEL_AFTER_BLACKOUT)) {
            // Ignore level transition if the player was going to die or game was going to end after a blackout effect
            regs.eip = 0x004BBA71;
        }
    }
};

ConsoleCommand2 debug_event_msg_cmd{
    "debug_event_msg",
    []() {
        event_debug_enabled = !event_debug_enabled;
    }
};

struct EventSetCollisionPlayer : rf::Event
{
    EventSetCollisionPlayer() : rf::Event()
    {
        xlog::warn("Creating event UID {}", this->uid);
    }

    void initialize() override
    {
        xlog::warn("Initializing event UID {}", this->uid);
    }

    void turn_on() override
    {
        xlog::warn("On response for event UID {}", this->uid);
        rf::local_player->collides_with_world = true;
        for (int i = 0; i < this->links.size(); ++i) {

            int link = this->links[i];
            xlog::warn("Link at index {}: {}", i, link);
            rf::Object* obj = rf::obj_from_handle(link);
            if (obj) {
                rf::Entity* entity = static_cast<rf::Entity*>(obj);
                xlog::warn("Name: {}, UID: {}, type: {}, ent type: {}", entity->name.c_str(), entity->uid,
                           static_cast<int>(entity->type), entity->info_index);
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

                xlog::warn("Name: {}, UID: {}, type: {}, ent type: {}", new_entity->name.c_str(), new_entity->uid,
                           static_cast<int>(new_entity->type), new_entity->info_index);
            }
            

        }
    }

    void turn_off() override
    {
        xlog::warn("Off response for event UID {}", this->uid);
        rf::local_player->collides_with_world = false;
    }

    void process() override
    {
        // activated every frame
        //xlog::warn("Processing event UID {}", this->uid);
    }
};

struct EventDifficultyGate : rf::Event
{
    int difficulty;
};

struct EventCloneEntity : rf::Event
{
    EventCloneEntity() : rf::Event()
    {
        xlog::warn("Creating event UID {}", this->uid);
    }

    void initialize() override
    {
        xlog::warn("Initializing event UID {}", this->uid);
    }

    void turn_on() override
    {
        xlog::warn("On response for event UID {}", this->uid);

        for (int i = 0; i < this->links.size(); ++i) {

            int link = this->links[i];
            xlog::warn("Link at index {}: {}", i, link);
            // rf::Object* obj = rf::obj_from_handle(link);
            rf::Event* obj = rf::event_lookup_from_handle(link);
            xlog::warn("event type: {}", obj->event_type);
            xlog::warn("Name: {}, UID: {}, type: {}, event type: {}", obj->name.c_str(), obj->uid,
                       static_cast<int>(obj->type), obj->event_type);
        }
    }

    void turn_off() override
    {
        xlog::warn("Off response for event UID {}", this->uid);
    }

    void process() override
    {
        // activated every frame
        // xlog::warn("Processing event UID {}", this->uid);
    }
};

FunHook<int __cdecl(const rf::String* name)> event_lookup_type_hook{
    0x004BD700, // Correct address for the event_lookup_type function
    [](const rf::String* name) -> int {
        xlog::warn("Looking up event with name: {}", name->c_str());

        // Custom event ID assignment
        if (*name == "Difficulty") {
            return 90;
        }
        else if (*name == "Set_Player_World_Collide") {
            return 91;
        }
        else if (*name == "Redirector") {
            return 92;
        }
        else if (*name == "Clone_Entity") {
            return 93;
        }

        return event_lookup_type_hook.call_target(name);
    }
};

CallHook<rf::Event*(int event_type)> event_allocate_hook{
    0x004871C1,
    [](int event_type) -> rf::Event* {

        if (event_type == 91) {
            auto* custom_event = new EventSetCollisionPlayer();
            return custom_event;
        }
        if (event_type == 93) {
            auto* custom_event = new EventSetCollisionPlayer();
            return custom_event;
        }

        return event_allocate_hook.call_target(event_type);
    }
};

CodeInjection custom_event_injection{
    0x00462910,
    [](BaseCodeInjection::Regs& regs) {
        int event_type = regs.ebp;
        rf::Vector3* pos_ptr = reinterpret_cast<rf::Vector3*>(static_cast<uintptr_t>(regs.edx));

        if (event_type == 91) {

            rf::Vector3 pos = *pos_ptr;

            rf::Event* custom_event = new EventSetCollisionPlayer();

            // Log the created object info
            xlog::warn("Custom event created: Set_World_Collide_pl (ID 91) Type: {}, Event Type: {}, Position: {}, {}, {}",
                std::to_string(custom_event->type),std::to_string(custom_event->event_type),
                custom_event->pos.x, custom_event->pos.y, custom_event->pos.z);
        }
        else if (event_type == 93) {

            rf::Vector3 pos = *pos_ptr;

            rf::Event* custom_event = new EventSetCollisionPlayer();

            // Log the created object info
            xlog::warn(
                "Custom event created: Set_World_Collide_pl (ID 91) Type: {}, Event Type: {}, Position: {}, {}, {}",
                std::to_string(custom_event->type), std::to_string(custom_event->event_type), custom_event->pos.x,
                custom_event->pos.y, custom_event->pos.z);
        }
    }
};

// allow event creation > 91
CodeInjection allow_custom_events_injection{
    0x004B68A0,  // Address just before cmp instruction
    [](auto& regs) {
        int event_type = regs.ebp;

        if (event_type >= 90 && event_type <= 93) {
            xlog::warn("Allowing custom event to bypass jump table");
            regs.eip = 0x004B68A9;
        }
    }
};

// intermitant crashing on deallocate, need to debug here
CodeInjection custom_event_deallocate_injection{
    0x004B775A,
    [](BaseCodeInjection::Regs& regs) {
        int event_type = regs.eax; 
        xlog::warn("Intercepting event deallocation for event type: {}", event_type);

        // Check if the event is a custom event
        if (event_type >= 90 && event_type <= 93) {
            uintptr_t event_addr = regs.ecx;
            rf::Event* custom_event = reinterpret_cast<rf::Event*>(event_addr);

            if (custom_event && event_addr > 0x1000 && event_addr < 0xFFFFFFFF) {
                // Log details before deallocation
                xlog::warn("Deallocating custom event UID: {}, Event Type: {}", custom_event->uid,
                           custom_event->event_type);

                // Call the destructor (assuming rf::event_destructor handles everything safely)
                rf::event_destructor(custom_event, 1);
                rf::event_delete(custom_event);


                xlog::warn("Custom event deallocated successfully.");
            }
            else {
                xlog::error("Invalid or null custom event pointer: 0x{:X}", event_addr);
                return; // Abort if pointer is invalid
            }

            //regs.eip = 0x004B7AC5;
            return;
        }
        xlog::warn("Proceeding with default deallocation for event type: {}", event_type);
    }
};

// just for logging, disabled
FunHook<rf::Event*(rf::Vector3*, int)> event_create_hook{
    0x004B6870, [](rf::Vector3* pos, int event_type) -> rf::Event* {
        xlog::warn("CREATE EVENT called: pos(x={}, y={}, z={}), event_type={}", pos->x, pos->y, pos->z, event_type);
        return event_create_hook.call_target(pos, event_type);
    }
};

void apply_event_patches()
{

    event_lookup_type_hook.install();
    custom_event_injection.install();
    //event_create_hook.install();
    event_allocate_hook.install();
    allow_custom_events_injection.install();
    custom_event_deallocate_injection.install();

    // Allow custom mesh (not used in clutter.tbl or items.tbl) in Switch_Model event
    switch_model_event_custom_mesh_patch.install();
    switch_model_event_obj_lighting_and_physics_fix.install();

    // Fix Set_Liquid_Depth event
    AsmWriter(0x004BCBE0).jmp(&EventSetLiquidDepth_turn_on_new);
    liquid_depth_update_apply_all_GRoom_reset_liquid_hook.install();

    // Fix crash after level change (Load_Level event) caused by GNavNode pointers in AiPathInfo not being cleared for entities
    // being taken from the previous level
    ai_path_release_on_load_level_event_crash_fix.install();

    // Fix Message event crash on dedicated server
    EventMessage__turn_on_hook.install();

    // Level specific event fixes
    event_level_init_post_hook.install();

    // Debug event messages
    event_activate_injection.install();
    event_activate_injection2.install();
    event_process_injection.install();

    // Do not load next level if blackout is in progress
    event_load_level_turn_on_injection.install();

    // Register commands
    debug_event_msg_cmd.register_cmd();
}
