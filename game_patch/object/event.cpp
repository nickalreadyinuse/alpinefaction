#include <patch_common/CodeInjection.h>
#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include <patch_common/AsmWriter.h>
#include <xlog/xlog.h>
#include <cassert>
#include <unordered_set>
#include "../rf/object.h"
#include "../rf/event.h"
#include "../rf/entity.h"
#include "../rf/level.h"
#include "../rf/multi.h"
#include "../rf/player/player.h"
#include "../rf/os/console.h"
#include "../os/console.h"

bool event_debug_enabled;

namespace rf
{
    // event var handler storage
    std::unordered_map<const Event*, std::unordered_map<std::string, std::function<void(Event*, const std::string&)>>>
        Event::variable_handler_storage = {};
}

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
    "dbg_events",
    []() {
        event_debug_enabled = !event_debug_enabled;
    }
};

FunHook<int(const rf::String* name)> event_lookup_type_hook{
    0x004BD700,
    [](const rf::String* name) {
        //xlog::warn("Looking up event with name: {}", name->c_str());

        // Custom event name -> ID assignment
        if (*name == "SetVar") {
            return 90;
        }
        else if (*name == "Clone_Entity") {
            return 91;
        }
        else if (*name == "Set_Player_World_Collide") {
            return 92;
        }
        else if (*name == "Switch_Random") {
            return 93;
        }
        else if (*name == "Difficulty_Gate") {
            return 94;
        }
        else if (*name == "HUD_Message") {
            return 95;
        }
        else if (*name == "Play_Video") {
            return 96;
        }
        else if (*name == "Set_Level_Hardness") {
            return 97;
        }
        else if (*name == "Sequence") {
            return 98;
        }
        else if (*name == "Clear_Queued") {
            return 99;
        }
        else if (*name == "Remove_Link") {
            return 100;
        }
        else if (*name == "Fixed_Delay") {
            return 101;
        }

        // stock events
        return event_lookup_type_hook.call_target(name);
    }
};

FunHook<rf::Event*(int event_type)> event_allocate_hook{
    0x004B69D0,
    [](int event_type) {
        auto allocate_custom_event = [](auto event_ptr_type) -> rf::Event* {
            using EventType = std::remove_pointer_t<decltype(event_ptr_type)>;
            auto* memory = operator new(sizeof(EventType));
            if (!memory) {
                xlog::error("Failed to allocate memory for event.");
                return nullptr;
            }

            auto* custom_event = new (memory) EventType();
            //xlog::warn("Allocating event: {}", typeid(EventType).name());
            auto result = static_cast<rf::Event*>(custom_event);
            result->initialize(); // run init void after creation, var handlers are set here
            return result;
            //return static_cast<rf::Event*>(custom_event);
        };

        switch (event_type) {
        case 90:
            return allocate_custom_event(static_cast<rf::EventSetVar*>(nullptr));

        case 91:
            return allocate_custom_event(static_cast<rf::EventCloneEntity*>(nullptr));

        case 92:
            return allocate_custom_event(static_cast<rf::EventSetCollisionPlayer*>(nullptr));

        case 93:
            return allocate_custom_event(static_cast<rf::EventSwitchRandom*>(nullptr));

        case 94:
            return allocate_custom_event(static_cast<rf::EventDifficultyGate*>(nullptr));

        case 95:
            return allocate_custom_event(static_cast<rf::EventHUDMessage*>(nullptr));

        case 96:
            return allocate_custom_event(static_cast<rf::EventPlayVideo*>(nullptr));

        case 97:
            return allocate_custom_event(static_cast<rf::EventSetLevelHardness*>(nullptr));

        case 98:
            return allocate_custom_event(static_cast<rf::EventSequence*>(nullptr));

        case 99:
            return allocate_custom_event(static_cast<rf::EventClearQueued*>(nullptr));

        case 100:
            return allocate_custom_event(static_cast<rf::EventRemoveLink*>(nullptr));

        case 101:
            return allocate_custom_event(static_cast<rf::EventFixedDelay*>(nullptr));

        default: // stock events
            return event_allocate_hook.call_target(event_type);
        }
    }
};

FunHook<void(rf::Event*)> event_deallocate_hook{
    0x004B7750,
    [](rf::Event* eventp) {
        if (!eventp)
            return;

        int event_type = eventp->event_type;
        //xlog::warn("Deallocating event ID: {}", event_type);

        // Handle custom event types
        switch (event_type) {
        case 90: {
            auto* custom_event = static_cast<rf::EventSetVar*>(eventp);
            delete custom_event;
            return;
        }

        case 91: {
            auto* custom_event = static_cast<rf::EventCloneEntity*>(eventp);
            delete custom_event;
            return;
        }

        case 92: {
            auto* custom_event = static_cast<rf::EventSetCollisionPlayer*>(eventp);
            delete custom_event;
            return;
        }

        case 93: {
            auto* custom_event = static_cast<rf::EventSwitchRandom*>(eventp);
            delete custom_event;
            return;
        }

        case 94: {
            auto* custom_event = static_cast<rf::EventDifficultyGate*>(eventp);
            delete custom_event;
            return;
        }

        case 95: {
            auto* custom_event = static_cast<rf::EventHUDMessage*>(eventp);
            delete custom_event;
            return;
        }

        case 96: {
            auto* custom_event = static_cast<rf::EventPlayVideo*>(eventp);
            delete custom_event;
            return;
        }

        case 97: {
            auto* custom_event = static_cast<rf::EventSetLevelHardness*>(eventp);
            delete custom_event;
            return;
        }

        case 98: {
            auto* custom_event = static_cast<rf::EventSequence*>(eventp);
            delete custom_event;
            return;
        }

        case 99: {
            auto* custom_event = static_cast<rf::EventClearQueued*>(eventp);
            delete custom_event;
            return;
        }

        case 100: {
            auto* custom_event = static_cast<rf::EventRemoveLink*>(eventp);
            delete custom_event;
            return;
        }

        case 101: {
            auto* custom_event = static_cast<rf::EventFixedDelay*>(eventp);
            delete custom_event;
            return;
        }

        default: // stock events
            event_deallocate_hook.call_target(eventp);
            break;
        }
    }
};

// list custom events that don't forward messages by default
static const std::unordered_set<rf::EventType> forward_exempt_ids = {
    rf::EventType::SetVar,
    rf::EventType::Switch_Random,
    rf::EventType::Difficulty_Gate,
    rf::EventType::Sequence,
    rf::EventType::Clear_Queued,
    rf::EventType::Remove_Link
};

// decide if a specific event type should forward messages
CodeInjection event_type_forwards_messages_patch{
    0x004B8C44, [](auto& regs) {
        auto event_type = rf::int_to_event_type(static_cast<int>(regs.eax));
        auto& result = regs.al;

        // Check if the event type is in forward_exempt_ids
        if (forward_exempt_ids.find(event_type) != forward_exempt_ids.end()) {
            result = false;
            regs.eip = 0x004B8C5D;  // Jump to the address after the check
        }
    }
};

rf::Vector3 extract_yaw_vector(const rf::Matrix3& matrix)
{
    rf::Vector3 angles;
    if (matrix.fvec.x == 0.0f && matrix.fvec.z == 0.0f) {
        angles.y = 0.0f;
    }
    else {
        angles.y = std::atan2(matrix.fvec.x, matrix.fvec.z);
    }
    return angles;
}

rf::Vector3 rotate_velocity(const rf::Matrix3& old_orient, const rf::Matrix3& new_orient, const rf::Vector3& old_vel)
{
    // convert velocity to world space
    rf::Vector3 base_velocity = old_orient.copy_transpose().rvec * old_vel.x +
                                  old_orient.copy_transpose().uvec * old_vel.y +
                                  old_orient.copy_transpose().fvec * old_vel.z;

    // rotate by new orient
    return new_orient.rvec * base_velocity.x + new_orient.uvec * base_velocity.y + new_orient.fvec * base_velocity.z;
}

//todo: rewrite with codeinjection for just vel
FunHook<void(rf::Event*)> event_player_teleport_on_hook{
    0x004B9820,
    [](rf::Event* event) {
        rf::Entity* player_entity = rf::local_player_entity;

        if (!player_entity)
            return;

        // in a vehicle
        rf::Entity* host_entity = rf::entity_from_handle(player_entity->host_handle);

        if (host_entity) {
            // base game only cares about level filename, adding UID too is safer
            if (rf::level.filename == "l20s2.rfl" && event->uid == 18458) {
                player_entity = host_entity;
            }
            else {
                rf::entity_detach_from_host(player_entity);
            }
        }

        player_entity->p_data.pos = event->pos;
        player_entity->p_data.next_pos = event->pos;
        player_entity->pos = event->pos;

        // maintain relative velocity
        player_entity->p_data.vel = rotate_velocity(player_entity->p_data.orient, event->orient, player_entity->p_data.vel);

        player_entity->control_data.phb = extract_yaw_vector(event->orient);
        player_entity->orient = event->orient;
        player_entity->p_data.orient = event->orient;
        player_entity->p_data.next_orient = event->orient;
        player_entity->eye_orient = event->orient;

        // makes walking through teleporters smoother, base game turns this off for clients
        if (rf::entity_on_ground(player_entity)) {
            rf::physics_force_to_ground(player_entity);
        }
    }
};

// factory for SetVar events
rf::EventSetVar* event_setvar_create(const rf::Vector3* pos, std::string script_name, std::string str1)
{
    rf::Event* base_event = rf::event_create(pos, 90);
    rf::EventSetVar* event = dynamic_cast<rf::EventSetVar*>(base_event);

    if (event) {
        // var_name
        const std::string_view prefix = "SetVar_";
        if (script_name.starts_with(prefix)) {
            event->var_name = script_name.substr(prefix.size());
        }

        // var_value
        if (!str1.empty()) {
            event->var_value = str1;
        }
    }

    return event;
}

// factory for Difficulty_Gate events
rf::EventDifficultyGate* event_difficulty_gate_create(const rf::Vector3* pos, int difficulty)
{
    rf::Event* base_event = rf::event_create(pos, 94);
    rf::EventDifficultyGate* event = dynamic_cast<rf::EventDifficultyGate*>(base_event);

    if (event) {
        // set difficulty
        event->difficulty = static_cast<rf::GameDifficultyLevel>(difficulty);
    }

    return event;
}

// factory for HUD_Message events
rf::EventHUDMessage* event_hud_message_create(const rf::Vector3* pos, std::string message)
{
    rf::Event* base_event = rf::event_create(pos, 95);
    rf::EventHUDMessage* event = dynamic_cast<rf::EventHUDMessage*>(base_event);

    if (event) {
        // set message
        event->message = message;
    }

    return event;
}

// factory for Play_Video events
rf::EventPlayVideo* event_play_video_create(const rf::Vector3* pos, std::string filename)
{
    rf::Event* base_event = rf::event_create(pos, 96);
    rf::EventPlayVideo* event = dynamic_cast<rf::EventPlayVideo*>(base_event);

    if (event) {
        // set filename
        event->filename = filename;
    }

    return event;
}

// factory for Set_Level_Hardness events
rf::EventSetLevelHardness* event_set_level_hardness_create(const rf::Vector3* pos, int hardness)
{
    rf::Event* base_event = rf::event_create(pos, 97);
    rf::EventSetLevelHardness* event = dynamic_cast<rf::EventSetLevelHardness*>(base_event);

    if (event) {
        // set hardness
        event->hardness = hardness;
    }

    return event;
}

// factory for Remove_Link events
rf::EventRemoveLink* event_remove_link_create(const rf::Vector3* pos, bool remove_all)
{
    rf::Event* base_event = rf::event_create(pos, 100);
    rf::EventRemoveLink* event = dynamic_cast<rf::EventRemoveLink*>(base_event);

    if (event) {
        // set remove_all
        event->remove_all = remove_all;
    }

    return event;
}

// assignment of factories for AF event types
CodeInjection level_read_events_patch {
    0x00462910, [](auto& regs) {
        int event_type = static_cast<int>(regs.ebp);

        if (event_type > 89) { // only handle AF events, stock events handled by original code
            rf::Vector3* pos = regs.edx;
            // Note: we don't need to handle delay - later part of level_read_events handles that

            // accessible event parameters
            rf::String* class_name = reinterpret_cast<rf::String*>(regs.esp + 0x5C);
            rf::String* script_name = reinterpret_cast<rf::String*>(regs.esp + 0x54);
            rf::String* str1 = reinterpret_cast<rf::String*>(regs.esp + 0x44);
            rf::String* str2 = reinterpret_cast<rf::String*>(regs.esp + 0x4C);
            int int1 = *reinterpret_cast<int*>(regs.esp - 0x24);
            int int2 = *reinterpret_cast<int*>(regs.esp - 0x18);
            bool bool1 = *reinterpret_cast<int*>(regs.esp + 0x18);

            /* for (uintptr_t offset = 0x00; offset <= 0x500; offset += 4) {
                uintptr_t current_address = regs.esp - 0x100 + offset;
                int value_at_address = *reinterpret_cast<int*>(current_address); // Interpret as int

                xlog::warn("Memory at [esp + {:#04X}]: Address=0x{:08X}, Value={}", offset, current_address,
                           value_at_address);

                // Check if the value matches the expected value (e.g., 2)
                if (value_at_address == 0) {
                    xlog::info("Found expected value at offset: {:#04X}, Address: 0x{:08X}", offset,
                               current_address);
                }
            }*/

            xlog::warn(
                "Constructing event type {}: class_name: {}, script_name: {}, str1: {}, str2: {}, int1: {}, int2: {}, bool1: {}",
                event_type, class_name->c_str(), script_name->c_str(), str1->c_str(), str2->c_str(), int1, int2, bool1);


            switch (event_type) {
                case 90: { // SetVar
                    rf::Event* this_event = event_setvar_create(pos, script_name->c_str(), str1->c_str());
                    regs.eax = this_event; // set eax to created event so level_read_events can continue to use it
                    break;
                }
                case 94: { // Difficulty_Gate
                    rf::Event* this_event = event_difficulty_gate_create(pos, int1);
                    regs.eax = this_event;
                    break;
                }
                case 95: { // HUD_Message
                    rf::Event* this_event = event_hud_message_create(pos, str1->c_str());
                    regs.eax = this_event;
                    break;
                }
                case 96: { // Play_Video
                    rf::Event* this_event = event_play_video_create(pos, str1->c_str());
                    regs.eax = this_event;
                    break;
                }
                case 97: { // Set_Level_Hardness
                    rf::Event* this_event = event_set_level_hardness_create(pos, int1);
                    regs.eax = this_event;
                    break;
                }
                case 100: { // Remove_Link
                    rf::Event* this_event = event_remove_link_create(pos, bool1); // need to find bool1
                    regs.eax = this_event;
                    break;
                }
                default: { // fallback for AF events that don't need specific factories (no configurable params)
                    rf::Event* this_event = rf::event_create(pos, event_type);
                    regs.eax = this_event;
                    break;
                }
            }

            regs.eip = 0x00462915; // made the event, set stack pointer after jump table
        }
    }
};

CodeInjection event_activate_fixed_delay{
    0x004B8B91,
    [](auto& regs) {
        rf::Event* event = regs.esi;

        if (event->event_type == 101 && event->delay_timestamp.valid()) { // Fixed_Delay
            rf::console::print("Ignoring message request in active {} event ({})", event->name, event->uid);
            regs.eip = 0x004B8C35;
        }
    }
};

void apply_event_patches()
{
    // Support custom events    
    AsmWriter(0x004B68A3).jmp(0x004B68A9); // make event_create process events with any ID (params specified)
    event_lookup_type_hook.install(); // define AF event IDs
    event_allocate_hook.install(); // load AF events at level start
    event_deallocate_hook.install(); // unload AF events at level end
    event_type_forwards_messages_patch.install(); // handle AF events that shouldn't forward messages by default
    level_read_events_patch.install(); // assign factories for AF events
    event_activate_fixed_delay.install(); // handle activations for Fixed_Delay event (id 101)
    

    // Improve player teleport behaviour
    event_player_teleport_on_hook.install();

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
