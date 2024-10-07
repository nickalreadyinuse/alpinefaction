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

struct EventDifficultyGate : rf::Event
{
    int difficulty;
};

EventDifficultyGate* event_difficulty_gate_create(rf::Vector3 pos, rf::String name, int difficulty)
{
    auto* event_difficulty_gate = static_cast<EventDifficultyGate*>(rf::event_create(pos, 0x59));

    event_difficulty_gate->pos = pos;
    event_difficulty_gate->name = name;
    event_difficulty_gate->difficulty = difficulty;

    return event_difficulty_gate;
}

struct EventSetCollisionPlayer : rf::Event
{
    EventSetCollisionPlayer() : rf::Event()
    {
        // Custom initialization logic (if any)
        xlog::warn("Set_World_Collide_pl event created.");
    }

    void initialize() override
    {
        xlog::warn("Processing custom logic for Set_World_Collide_pl event: UID {}", this->uid);
    }

    void turn_on() override
    {
        xlog::warn("Turning on custom logic for Set_World_Collide_pl event: UID {}", this->uid);
        rf::local_player->collides_with_world = false;
    }

    void turn_off() override
    {
        xlog::warn("Turning off custom logic for Set_World_Collide_pl event: UID {}", this->uid);
    }

    void process() override
    {
        //xlog::warn("Processing custom logic for Set_World_Collide_pl event: UID {}", this->uid);
    }
};

EventSetCollisionPlayer* event_set_collision_create(rf::Vector3 pos)
{
    // Log the position being passed to ensure it looks valid
    xlog::warn("Creating event with position: x={}, y={}, z={}", pos.x, pos.y, pos.z);

    // Create the event using rf::event_create with the expected type
    auto* base_event = rf::event_create(pos, 91); // Returns a base event type (rf::Event*)

    // Log the pointer returned by rf::event_create to see if it's null or invalid
    xlog::warn("Base event pointer after creation: {}", reinterpret_cast<uintptr_t>(base_event));

    // Verify if the event creation was successful
    if (!base_event) {
        xlog::error("Failed to create base event for Set_World_Collide_pl.");
        return nullptr;
    }

    // Cast the base event to the specific EventSetCollisionPlayer type
    auto* event_set_collision = static_cast<EventSetCollisionPlayer*>(base_event);

    // Ensure the cast was successful
    if (!event_set_collision) {
        xlog::error("Failed to cast base event to EventSetCollisionPlayer.");
        return nullptr;
    }

    // Assign position
    event_set_collision->pos = pos;

    // Optionally, initialize other fields if necessary
    // event_set_collision->name = name;

    xlog::warn("Successfully created Set_World_Collide_pl event at position: x={}, y={}, z={}", pos.x, pos.y, pos.z);

    return event_set_collision;
}

FunHook<int __cdecl(const rf::String* name)> event_lookup_type_hook{
    0x004BD700, // Correct address for the event_lookup_type function
    [](const rf::String* name) -> int {
        xlog::warn("Looking up event with name: {}", name->c_str());

        // Custom event ID assignment
        if (*name == "Set_World_Collide_pl") {
            xlog::warn("Assigning ID 90 to Set_World_Collide_pl event");

            // Return custom event ID
            return 91;
        }
        else if (*name == "Difficulty") {
            xlog::warn("Assigning ID 89 to Difficulty event");

            // Return custom event ID
            return 90;
        }

        // Call original function for non-custom events
        return event_lookup_type_hook.call_target(name);
    }
};


CallHook<rf::Event*(int event_type)> event_allocate_hook{
    0x004871C1, // Event allocation function's address
    [](int event_type) -> rf::Event* {
        // Check if we are handling the custom event type
        if (event_type == 91) {
            xlog::warn("Allocating custom event: Set_World_Collide_pl (ID 91)");

            // Allocate the custom event
            auto* custom_event = new EventSetCollisionPlayer();

            // Initialize the custom event fields
            xlog::warn("Successfully allocated Set_World_Collide_pl event");
            return custom_event;
        }

        // Fallback to original event allocation for non-custom events
        return event_allocate_hook.call_target(event_type);
    }};


CodeInjection event_allocate_injection{
    0x00462910, // Address for event allocation logic
    [](BaseCodeInjection::Regs& regs) {
        int event_type = regs.ebp; // Event type in EBP

        if (event_type == 91) { // Custom event type
            xlog::warn("Allocating custom event: Set_World_Collide_pl (ID 91)");

            // Create and allocate the custom event
            rf::Event* custom_event = new EventSetCollisionPlayer();

            if (custom_event) {
                xlog::warn("Custom event Set_World_Collide_pl allocated successfully.");
                regs.eax = reinterpret_cast<uintptr_t>(custom_event); // Return the event pointer
            }
            else {
                xlog::error("Failed to allocate custom event Set_World_Collide_pl.");
            }

            // Optionally skip the rest of the event allocation logic
            regs.eip = 0x00462915; // Jump to next instruction after custom event handling
        }
    }};




void custom_event_handler_function()
{
    xlog::warn("Processing Set_World_Collide_pl custom event...");
    // Your custom logic for this event
}

CodeInjection custom_event_injection{
    0x00462910, // Inject at the location for handling the event creation process
    [](BaseCodeInjection::Regs& regs) {
        int event_type = regs.ebp; // Get the event type
        rf::Vector3* pos_ptr =
            reinterpret_cast<rf::Vector3*>(static_cast<uintptr_t>(regs.edx)); // Read position pointer

        if (event_type == 91) {
            xlog::warn("Handling custom event: Set_World_Collide_pl (ID 91)");

            if (pos_ptr != nullptr) {
                rf::Vector3 pos = *pos_ptr;
                xlog::warn("Creating event with position: x={}, y={}, z={}", pos.x, pos.y, pos.z);

                //rf::ObjectCreateInfo oci;
                //oci.object
                //oci.pos = pos;

                

                rf::Event* custom_event = new EventSetCollisionPlayer();
                custom_event->type = rf::OT_EVENT;
                custom_event->pos = pos;
                custom_event->event_type = 91;
                //rf::Event* custom_event = rf::event_create(pos, 91);
                if (!custom_event) {
                    xlog::error("Failed to create custom event Set_World_Collide_pl.");
                    return;
                }
                // Log the created object info
                xlog::warn("Custom Event created with pos: x={}, y={}, z={}", custom_event->pos.x, custom_event->pos.y, custom_event->pos.z);
                xlog::warn("Custom event created: Set_World_Collide_pl (ID 91) Type: {}, Event Type: {}",
                           std::to_string(custom_event->type), std::to_string(custom_event->event_type));
                //custom_event->initialize();


  
                // call doesnt seem to be needed
                //regs.eip = 0x00462988;
            }
            else {
                xlog::warn("Invalid position pointer for custom event.");
            }
        }
    }
};

FunHook<rf::Event*(const rf::Vector3*, int)> event_create_hook{
    0x004B6870, // Address of the original event_create function
    [](const rf::Vector3* pos, int event_type) -> rf::Event* {
        // Log the position and event type
        xlog::warn("Hooked event_create called: pos(x={}, y={}, z={}), event_type={}", pos->x, pos->y, pos->z,
                   event_type);

        // Call the original function to preserve normal behavior
        return event_create_hook.call_target(pos, event_type);
    }
};

// allow event creation > 91
CodeInjection allow_event_type_91_injection{
    0x004B68A0,  // Address just before cmp instruction
    [](auto& regs) {
        int event_type = regs.ebp;

        if (event_type == 91) {
            xlog::warn("Allowing custom event type 91 to bypass jump");
            // Skip the cmp and jump instructions and continue event creation
            regs.eip = 0x004B68A9;  // Set eip to the address after the jump
        }
    }
};





CodeInjection log_jump_table_entry{
    0x00462324, // This is the address of the jump table entry
    [](BaseCodeInjection::Regs& regs) {
        int eax_value = regs.eax;  // Use int or uintptr_t to match register size
        int ecx_value = regs.ecx;

        xlog::warn("Event type in EAX is: {}", eax_value);
        xlog::warn("Event type in ECX is: {}", ecx_value);
    }
};

CodeInjection log_jump_table_entry2{
    0x0046237F, // This is the address of the jump table entry
    [](BaseCodeInjection::Regs& regs) {
        int eax_value = regs.eax;  // Use int or uintptr_t to match register size

        xlog::warn("Event type in later EAX is: {}", eax_value);
    }
};

void apply_event_patches()
{
    //event_lookup_type_hook.install();
    event_allocate_hook.install();
    event_lookup_type_hook.install();
    //event_create_hook.install();
    custom_event_injection.install();
    //log_jump_table_entry.install();
    //log_jump_table_entry2.install();
    allow_event_type_91_injection.install();

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
