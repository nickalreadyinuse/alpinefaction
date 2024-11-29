#include <patch_common/CodeInjection.h>
#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include <patch_common/AsmWriter.h>
#include <common/version/version.h>
#include <xlog/xlog.h>
#include <cassert>
#include <unordered_set>
#include "event_alpine.h"
#include "../misc/misc.h"
#include "../rf/object.h"
#include "../rf/event.h"
#include "../rf/entity.h"
#include "../rf/level.h"
#include "../rf/multi.h"
#include "../rf/player/player.h"
#include "../rf/os/console.h"
#include "../os/console.h"

namespace rf
{
    // event var handler storage
    std::unordered_map<const Event*, std::unordered_map<std::string, std::function<void(Event*, const std::string&)>>>
        Event::variable_handler_storage = {};
} // namespace rf

FunHook<int(const rf::String* name)> event_lookup_type_hook{
    0x004BD700,
    [](const rf::String* name) {
        //xlog::warn("Looking up event with name: {}", name->c_str());

        // Custom event name -> ID assignment
        if (*name == "SetVar") {
            return 89;
        }
        else if (*name == "Clone_Entity") {
            return 90;
        }
        else if (*name == "Set_Player_World_Collide") {
            return 91;
        }
        else if (*name == "Switch_Random") {
            return 92;
        }
        else if (*name == "Difficulty_Gate") {
            return 93;
        }
        else if (*name == "HUD_Message") {
            return 94;
        }
        else if (*name == "Play_Video") {
            return 95;
        }
        else if (*name == "Set_Level_Hardness") {
            return 96;
        }
        else if (*name == "Sequence") {
            return 97;
        }
        else if (*name == "Clear_Queued") {
            return 98;
        }
        else if (*name == "Remove_Link") {
            return 99;
        }
        else if (*name == "Fixed_Delay") {
            return 100;
        }
        else if (*name == "Add_Link") {
            return 101;
        }
        else if (*name == "Valid_Gate") {
            return 102;
        }
        else if (*name == "Goal_Math") {
            return 103;
        }
        else if (*name == "Goal_Gate") {
            return 104;
        }
        else if (*name == "Environment_Gate") {
            return 105;
        }
        else if (*name == "Inside_Gate") {
            return 106;
        }
        else if (*name == "Anchor_Marker") {
            return 107;
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
        case 89:
            return allocate_custom_event(static_cast<rf::EventSetVar*>(nullptr));

        case 90:
            return allocate_custom_event(static_cast<rf::EventCloneEntity*>(nullptr));

        case 91:
            return allocate_custom_event(static_cast<rf::EventSetCollisionPlayer*>(nullptr));

        case 92:
            return allocate_custom_event(static_cast<rf::EventSwitchRandom*>(nullptr));

        case 93:
            return allocate_custom_event(static_cast<rf::EventDifficultyGate*>(nullptr));

        case 94:
            return allocate_custom_event(static_cast<rf::EventHUDMessage*>(nullptr));

        case 95:
            return allocate_custom_event(static_cast<rf::EventPlayVideo*>(nullptr));

        case 96:
            return allocate_custom_event(static_cast<rf::EventSetLevelHardness*>(nullptr));

        case 97:
            return allocate_custom_event(static_cast<rf::EventSequence*>(nullptr));

        case 98:
            return allocate_custom_event(static_cast<rf::EventClearQueued*>(nullptr));

        case 99:
            return allocate_custom_event(static_cast<rf::EventRemoveLink*>(nullptr));

        case 100:
            return allocate_custom_event(static_cast<rf::EventFixedDelay*>(nullptr));

        case 101:
            return allocate_custom_event(static_cast<rf::EventAddLink*>(nullptr));

        case 102:
            return allocate_custom_event(static_cast<rf::EventValidGate*>(nullptr));

        case 103:
            return allocate_custom_event(static_cast<rf::EventGoalMath*>(nullptr));

        case 104:
            return allocate_custom_event(static_cast<rf::EventGoalGate*>(nullptr));

        case 105:
            return allocate_custom_event(static_cast<rf::EventEnvironmentGate*>(nullptr));

        case 106:
            return allocate_custom_event(static_cast<rf::EventInsideGate*>(nullptr));

        case 107:
            return allocate_custom_event(static_cast<rf::EventAnchorMarker*>(nullptr));

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
        case 89: {
            auto* custom_event = static_cast<rf::EventSetVar*>(eventp);
            delete custom_event;
            return;
        }

        case 90: {
            auto* custom_event = static_cast<rf::EventCloneEntity*>(eventp);
            delete custom_event;
            return;
        }

        case 91: {
            auto* custom_event = static_cast<rf::EventSetCollisionPlayer*>(eventp);
            delete custom_event;
            return;
        }

        case 92: {
            auto* custom_event = static_cast<rf::EventSwitchRandom*>(eventp);
            delete custom_event;
            return;
        }

        case 93: {
            auto* custom_event = static_cast<rf::EventDifficultyGate*>(eventp);
            delete custom_event;
            return;
        }

        case 94: {
            auto* custom_event = static_cast<rf::EventHUDMessage*>(eventp);
            delete custom_event;
            return;
        }

        case 95: {
            auto* custom_event = static_cast<rf::EventPlayVideo*>(eventp);
            delete custom_event;
            return;
        }

        case 96: {
            auto* custom_event = static_cast<rf::EventSetLevelHardness*>(eventp);
            delete custom_event;
            return;
        }

        case 97: {
            auto* custom_event = static_cast<rf::EventSequence*>(eventp);
            delete custom_event;
            return;
        }

        case 98: {
            auto* custom_event = static_cast<rf::EventClearQueued*>(eventp);
            delete custom_event;
            return;
        }

        case 99: {
            auto* custom_event = static_cast<rf::EventRemoveLink*>(eventp);
            delete custom_event;
            return;
        }

        case 100: {
            auto* custom_event = static_cast<rf::EventFixedDelay*>(eventp);
            delete custom_event;
            return;
        }

        case 101: {
            auto* custom_event = static_cast<rf::EventAddLink*>(eventp);
            delete custom_event;
            return;
        }

        case 102: {
            auto* custom_event = static_cast<rf::EventValidGate*>(eventp);
            delete custom_event;
            return;
        }

        case 103: {
            auto* custom_event = static_cast<rf::EventGoalMath*>(eventp);
            delete custom_event;
            return;
        }

        case 104: {
            auto* custom_event = static_cast<rf::EventGoalGate*>(eventp);
            delete custom_event;
            return;
        }

        case 105: {
            auto* custom_event = static_cast<rf::EventEnvironmentGate*>(eventp);
            delete custom_event;
            return;
        }

        case 106: {
            auto* custom_event = static_cast<rf::EventInsideGate*>(eventp);
            delete custom_event;
            return;
        }

        case 107: {
            auto* custom_event = static_cast<rf::EventAnchorMarker*>(eventp);
            delete custom_event;
            return;
        }

        default: // stock events
            event_deallocate_hook.call_target(eventp);
            break;
        }
    }
};

// alpine events that don't forward messages by default
static const std::unordered_set<rf::EventType> forward_exempt_ids = {
    rf::EventType::SetVar,
    rf::EventType::Switch_Random,
    rf::EventType::Difficulty_Gate,
    rf::EventType::Sequence,
    rf::EventType::Clear_Queued,
    rf::EventType::Remove_Link,
    rf::EventType::Add_Link,
    rf::EventType::Valid_Gate,
    rf::EventType::Goal_Gate,
    rf::EventType::Environment_Gate,
    rf::EventType::Inside_Gate
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

// factory for SetVar events
rf::EventSetVar* event_setvar_create(const rf::Vector3* pos, std::string script_name, std::string str1)
{
    rf::Event* base_event = rf::event_create(pos, 89);
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
    rf::Event* base_event = rf::event_create(pos, 93);
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
    rf::Event* base_event = rf::event_create(pos, 94);
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
    rf::Event* base_event = rf::event_create(pos, 95);
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
    rf::Event* base_event = rf::event_create(pos, 96);
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
    rf::Event* base_event = rf::event_create(pos, 99);
    rf::EventRemoveLink* event = dynamic_cast<rf::EventRemoveLink*>(base_event);

    if (event) {
        // set remove_all
        event->remove_all = remove_all;
    }

    return event;
}

// factory for Valid_Gate events
rf::EventValidGate* event_valid_gate_create(const rf::Vector3* pos, int check_uid)
{
    rf::Event* base_event = rf::event_create(pos, 102);
    rf::EventValidGate* event = dynamic_cast<rf::EventValidGate*>(base_event);

    if (event) {
        // set check_uid
        event->check_uid = check_uid;
    }

    return event;
}

// factory for Goal_Math events
rf::EventGoalMath* event_goal_math_create(
    const rf::Vector3* pos, std::string goal, std::string operation, int value, int value2)
{
    rf::Event* base_event = rf::event_create(pos, 103);
    rf::EventGoalMath* event = dynamic_cast<rf::EventGoalMath*>(base_event);

    if (event) {
        event->goal = goal;
        event->operation = operation;
        event->value = value;
        event->value2 = value2;
    }

    return event;
}

// factory for Goal_Gate events
rf::EventGoalGate* event_goal_gate_create(
    const rf::Vector3* pos, std::string goal, std::string test_type, int value, int value2)
{
    rf::Event* base_event = rf::event_create(pos, 104);
    rf::EventGoalGate* event = dynamic_cast<rf::EventGoalGate*>(base_event);

    if (event) {
        event->goal = goal;
        event->test_type = test_type;
        event->value = value;
        event->value2 = value2;
    }

    return event;
}

// factory for Environment_Gate events
rf::EventEnvironmentGate* event_environment_gate_create(const rf::Vector3* pos, std::string environment)
{
    rf::Event* base_event = rf::event_create(pos, 105);
    rf::EventEnvironmentGate* event = dynamic_cast<rf::EventEnvironmentGate*>(base_event);

    if (event) {
        // set environment
        event->environment = environment;
    }

    return event;
}

// factory for Inside_Gate events
rf::EventInsideGate* event_inside_gate_create(const rf::Vector3* pos, int check_uid)
{
    rf::Event* base_event = rf::event_create(pos, 106);
    rf::EventInsideGate* event = dynamic_cast<rf::EventInsideGate*>(base_event);

    if (event) {
        // set check_uid
        event->check_uid = check_uid;
    }

    return event;
}

// assignment of factories for AF event types
CodeInjection level_read_events_patch {
    0x00462910, [](auto& regs) {
        int event_type = static_cast<int>(regs.ebp);

        if (event_type > 88) { // only handle AF events, stock events handled by original code
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
                case 89: { // SetVar
                    rf::Event* this_event = event_setvar_create(pos, script_name->c_str(), str1->c_str());
                    regs.eax = this_event; // set eax to created event so level_read_events can continue to use it
                    break;
                }
                case 93: { // Difficulty_Gate
                    rf::Event* this_event = event_difficulty_gate_create(pos, int1);
                    regs.eax = this_event;
                    break;
                }
                case 94: { // HUD_Message
                    rf::Event* this_event = event_hud_message_create(pos, str1->c_str());
                    regs.eax = this_event;
                    break;
                }
                case 95: { // Play_Video
                    rf::Event* this_event = event_play_video_create(pos, str1->c_str());
                    regs.eax = this_event;
                    break;
                }
                case 96: { // Set_Level_Hardness
                    rf::Event* this_event = event_set_level_hardness_create(pos, int1);
                    regs.eax = this_event;
                    break;
                }
                case 99: { // Remove_Link
                    rf::Event* this_event = event_remove_link_create(pos, bool1);
                    regs.eax = this_event;
                    break;
                }
                case 102: { // Valid_Gate
                    rf::Event* this_event = event_valid_gate_create(pos, int1);
                    regs.eax = this_event;
                    break;
                }
                case 103: { // Goal_Math
                    rf::Event* this_event = event_goal_math_create(pos, str1->c_str(), str2->c_str(), int1, int2);
                    regs.eax = this_event;
                    break;
                }
                case 104: { // Goal_Gate
                    rf::Event* this_event = event_goal_gate_create(pos, str1->c_str(), str2->c_str(), int1, int2);
                    regs.eax = this_event;
                    break;
                }
                case 105: { // Environment_Gate
                    rf::Event* this_event = event_environment_gate_create(pos, str1->c_str());
                    regs.eax = this_event;
                    break;
                }
                case 106: { // Inside_Gate
                    rf::Event* this_event = event_inside_gate_create(pos, int1);
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

        if (event->event_type == 100 && event->delay_timestamp.valid()) { // Fixed_Delay is active
            rf::console::print("Ignoring message request in active {} event ({})", event->name, event->uid);
            regs.eip = 0x004B8C35;
        }
    }
};

void apply_alpine_events()
{
    AsmWriter(0x004B68A3).jmp(0x004B68A9);        // make event_create process events with any ID (params specified)
    event_lookup_type_hook.install();             // define AF event IDs
    event_allocate_hook.install();                // load AF events at level start
    event_deallocate_hook.install();              // unload AF events at level end
    event_type_forwards_messages_patch.install(); // handle AF events that shouldn't forward messages by default
    level_read_events_patch.install();            // assign factories for AF events
    event_activate_fixed_delay.install();         // handle activations for Fixed_Delay event
}
