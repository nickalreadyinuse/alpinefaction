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
    // var handler storage
    std::unordered_map<const Event*, std::unordered_map<SetVarOpts, std::function<void(Event*, const std::string&)>>>
    Event::variable_handler_storage = {};





    void activate_all_events_of_type(rf::EventType event_type, int trigger_handle, int triggered_by_handle, bool on)
    {
        auto event_list = rf::find_all_events_by_type(event_type);
        for (auto* event : event_list) {
            if (event) {
                event->activate(trigger_handle, triggered_by_handle, on);
            }
        }
    }

} // namespace rf

FunHook<int(const rf::String* name)> event_lookup_type_hook{
    0x004BD700,
    [](const rf::String* name) {

        // map of alpine events and corresponding IDs
        static const std::unordered_map<std::string_view, int> custom_event_ids{
            {"Set_Variable", 100},
            {"Clone_Entity", 101},
            {"Set_Player_World_Collide", 102},
            {"Switch_Random", 103},
            {"Difficulty_Gate", 104},
            {"HUD_Message", 105},
            {"Play_Video", 106},
            {"Set_Level_Hardness", 107},
            {"Sequence", 108},
            {"Clear_Queued", 109},
            {"Remove_Link", 110},
            {"Fixed_Delay", 111},
            {"Add_Link", 112},
            {"Valid_Gate", 113},
            {"Goal_Math", 114},
            {"Goal_Gate", 115},
            {"Environment_Gate", 116},
            {"Inside_Gate", 117},
            {"Anchor_Marker", 118},
            {"Force_Unhide", 119},
            {"Set_Difficulty", 120},
            {"Set_Fog_Far_Clip", 121},
            {"AF_When_Dead", 122},
            {"Gametype_Gate", 123},
            {"When_Picked_Up", 124},
        };

        auto it = custom_event_ids.find(name->c_str());
        if (it != custom_event_ids.end()) {
            return it->second; // event ID
        }

        // handle stock events
        return event_lookup_type_hook.call_target(name);
    }
};

FunHook<rf::Event*(int event_type)> event_allocate_hook{
    0x004B69D0,
    [](int event_type) {
        using AllocFunc = std::function<rf::Event*()>;

        // map of allocators
        static const std::unordered_map<int, AllocFunc> event_allocators{
            {100, []() { return new rf::EventSetVar(); }},
            {101, []() { return new rf::EventCloneEntity(); }},
            {102, []() { return new rf::EventSetCollisionPlayer(); }},
            {103, []() { return new rf::EventSwitchRandom(); }},
            {104, []() { return new rf::EventDifficultyGate(); }},
            {105, []() { return new rf::EventHUDMessage(); }},
            {106, []() { return new rf::EventPlayVideo(); }},
            {107, []() { return new rf::EventSetLevelHardness(); }},
            {108, []() { return new rf::EventSequence(); }},
            {109, []() { return new rf::EventClearQueued(); }},
            {110, []() { return new rf::EventRemoveLink(); }},
            {111, []() { return new rf::EventFixedDelay(); }},
            {112, []() { return new rf::EventAddLink(); }},
            {113, []() { return new rf::EventValidGate(); }},
            {114, []() { return new rf::EventGoalMath(); }},
            {115, []() { return new rf::EventGoalGate(); }},
            {116, []() { return new rf::EventEnvironmentGate(); }},
            {117, []() { return new rf::EventInsideGate(); }},
            {118, []() { return new rf::EventAnchorMarker(); }},
            {119, []() { return new rf::EventForceUnhide(); }},
            {120, []() { return new rf::EventSetDifficulty(); }},
            {121, []() { return new rf::EventSetFogFarClip(); }},
            {122, []() { return new rf::EventAFWhenDead(); }},
            {123, []() { return new rf::EventGametypeGate(); }},
            {124, []() { return new rf::EventWhenPickedUp(); }},
        };

        // find type and allocate
        auto it = event_allocators.find(event_type);
        if (it != event_allocators.end()) {
            auto* event = it->second();
            event->initialize();
            return event;
        }

        // handle stock events
        return event_allocate_hook.call_target(event_type);
    }
};

FunHook<void(rf::Event*)> event_deallocate_hook{
    0x004B7750,
    [](rf::Event* eventp) {
        if (!eventp)
            return;

        // map of deallocators
        static const std::unordered_map<int, std::function<void(rf::Event*)>> event_deallocators{
            {100, [](rf::Event* e) { delete static_cast<rf::EventSetVar*>(e); }},
            {101, [](rf::Event* e) { delete static_cast<rf::EventCloneEntity*>(e); }},
            {102, [](rf::Event* e) { delete static_cast<rf::EventSetCollisionPlayer*>(e); }},
            {103, [](rf::Event* e) { delete static_cast<rf::EventSwitchRandom*>(e); }},
            {104, [](rf::Event* e) { delete static_cast<rf::EventDifficultyGate*>(e); }},
            {105, [](rf::Event* e) { delete static_cast<rf::EventHUDMessage*>(e); }},
            {106, [](rf::Event* e) { delete static_cast<rf::EventPlayVideo*>(e); }},
            {107, [](rf::Event* e) { delete static_cast<rf::EventSetLevelHardness*>(e); }},
            {108, [](rf::Event* e) { delete static_cast<rf::EventSequence*>(e); }},
            {109, [](rf::Event* e) { delete static_cast<rf::EventClearQueued*>(e); }},
            {110, [](rf::Event* e) { delete static_cast<rf::EventRemoveLink*>(e); }},
            {111, [](rf::Event* e) { delete static_cast<rf::EventFixedDelay*>(e); }},
            {112, [](rf::Event* e) { delete static_cast<rf::EventAddLink*>(e); }},
            {113, [](rf::Event* e) { delete static_cast<rf::EventValidGate*>(e); }},
            {114, [](rf::Event* e) { delete static_cast<rf::EventGoalMath*>(e); }},
            {115, [](rf::Event* e) { delete static_cast<rf::EventGoalGate*>(e); }},
            {116, [](rf::Event* e) { delete static_cast<rf::EventEnvironmentGate*>(e); }},
            {117, [](rf::Event* e) { delete static_cast<rf::EventInsideGate*>(e); }},
            {118, [](rf::Event* e) { delete static_cast<rf::EventAnchorMarker*>(e); }},
            {119, [](rf::Event* e) { delete static_cast<rf::EventForceUnhide*>(e); }},
            {120, [](rf::Event* e) { delete static_cast<rf::EventSetDifficulty*>(e); }},
            {121, [](rf::Event* e) { delete static_cast<rf::EventSetFogFarClip*>(e); }},
            {122, [](rf::Event* e) { delete static_cast<rf::EventAFWhenDead*>(e); }},
            {123, [](rf::Event* e) { delete static_cast<rf::EventGametypeGate*>(e); }},
            {124, [](rf::Event* e) { delete static_cast<rf::EventWhenPickedUp*>(e); }},
        };

        // find type and deallocate
        auto it = event_deallocators.find(eventp->event_type);
        if (it != event_deallocators.end()) {
            // call deallocator
            it->second(eventp);
        }
        else {
            // handle stock events
            event_deallocate_hook.call_target(eventp);
        }
    }
};

// alpine events that can't forward messages by default
bool is_forward_exempt(rf::EventType event_type) {
    static const std::unordered_set<rf::EventType> forward_exempt_ids{
        rf::EventType::Set_Variable,
        rf::EventType::Switch_Random,
        rf::EventType::Difficulty_Gate,
        rf::EventType::Sequence,
        rf::EventType::Clear_Queued,
        rf::EventType::Remove_Link,
        rf::EventType::Add_Link,
        rf::EventType::Valid_Gate,
        rf::EventType::Goal_Gate,
        rf::EventType::Environment_Gate,
        rf::EventType::Inside_Gate,
        rf::EventType::AF_When_Dead,
        rf::EventType::Gametype_Gate,
        rf::EventType::When_Picked_Up
    };

    return forward_exempt_ids.find(event_type) != forward_exempt_ids.end();
}

CodeInjection event_type_forwards_messages_patch{
    0x004B8C44, [](auto& regs) {
        auto event_type = rf::int_to_event_type(static_cast<int>(regs.eax));

        // stock events handled by original code
        if (is_forward_exempt(event_type)) {
            regs.al = false;
            regs.eip = 0x004B8C5D;
        }
    }
};

using EventFactory = std::function<rf::Event*(const rf::EventCreateParams&)>;

static std::unordered_map<rf::EventType, EventFactory> event_factories{
    // Set_Variable
    {
        rf::EventType::Set_Variable, [](const rf::EventCreateParams& params) {
         auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Set_Variable));
            auto* event = dynamic_cast<rf::EventSetVar*>(base_event);
            if (event) {
                event->var = static_cast<rf::SetVarOpts>(params.int1);
                event->value_int = params.int2;
                event->value_float = params.float1;
                event->value_bool = params.bool1;
                event->value_str = params.str1;
            }
            return event;
        }
    },
    // Clone_Entity
    {
        rf::EventType::Clone_Entity, [](const rf::EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Clone_Entity));
            auto* event = dynamic_cast<rf::EventCloneEntity*>(base_event);
            if (event) {                
                event->ignore_item_drop = params.bool1;
            }
            return event;
        }
    },
    // Switch_Random
    {
        rf::EventType::Switch_Random, [](const rf::EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Switch_Random));
            auto* event = dynamic_cast<rf::EventSwitchRandom*>(base_event);
            if (event) {                
                event->no_repeats = params.bool1;
            }
            return event;
        }
    },
    // Difficulty_Gate
    {
        rf::EventType::Difficulty_Gate, [](const rf::EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Difficulty_Gate));
            auto* event = dynamic_cast<rf::EventDifficultyGate*>(base_event);
            if (event) {
                event->difficulty = params.int1;
            }
            return event;
        }
    },
    // HUD_Message
    {
        rf::EventType::HUD_Message, [](const rf::EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::HUD_Message));
            auto* event = dynamic_cast<rf::EventHUDMessage*>(base_event);
            if (event) {
                event->message = params.str1;
                event->duration = params.float1;
            }
            return event;
        }
    },
    // Play_Video
    {
        rf::EventType::Play_Video, [](const rf::EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Play_Video));
            auto* event = dynamic_cast<rf::EventPlayVideo*>(base_event);
            if (event) {
                event->filename = params.str1;
            }
            return event;
        }
    },
    // Set_Level_Hardness
    {
        rf::EventType::Set_Level_Hardness, [](const rf::EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Set_Level_Hardness));
            auto* event = dynamic_cast<rf::EventSetLevelHardness*>(base_event);
            if (event) {
                event->hardness = params.int1;
            }
            return event;
        }
    },
    // Sequence
    {
        rf::EventType::Sequence, [](const rf::EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Sequence));
            auto* event = dynamic_cast<rf::EventSequence*>(base_event);
            if (event) {
                event->next_link_index = params.int1;
            }
            return event;
        }
    },
    // Remove_Link
    {
        rf::EventType::Remove_Link, [](const rf::EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Remove_Link));
            auto* event = dynamic_cast<rf::EventRemoveLink*>(base_event);
            if (event) {
                event->remove_all = params.bool1;
            }
            return event;
        }
    },
    // Add_Link
    {
        rf::EventType::Add_Link, [](const rf::EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Add_Link));
            auto* event = dynamic_cast<rf::EventAddLink*>(base_event);
            if (event) {
                event->subject_uid = params.int1;
                event->inbound = params.bool1;
            }
            return event;
        }
    },
    // Valid_Gate
    {
        rf::EventType::Valid_Gate, [](const rf::EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Valid_Gate));
            auto* event = dynamic_cast<rf::EventValidGate*>(base_event);
            if (event) {
                event->check_uid = params.int1;
            }
            return event;
        }
    },
    // Goal_Math
    {
        rf::EventType::Goal_Math, [](const rf::EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Goal_Math));
            auto* event = dynamic_cast<rf::EventGoalMath*>(base_event);
            if (event) {
                event->goal = params.str1;
                event->operation = static_cast<rf::GoalMathOperation>(params.int1);
                event->value = params.int2;
            }
            return event;
        }
    },
    // Goal_Gate
    {
        rf::EventType::Goal_Gate, [](const rf::EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Goal_Gate));
            auto* event = dynamic_cast<rf::EventGoalGate*>(base_event);
            if (event) {
                event->goal = params.str1;
                event->test_type = static_cast<rf::GoalGateTests>(params.int1);
                event->value = params.int2;
            }
            return event;
        }
    },
    // Environment_Gate
    {
        rf::EventType::Environment_Gate, [](const rf::EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Environment_Gate));
            auto* event = dynamic_cast<rf::EventEnvironmentGate*>(base_event);
            if (event) {
                event->environment = params.str1;
            }
            return event;
        }
    },
    // Inside_Gate
    {
        rf::EventType::Inside_Gate, [](const rf::EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Inside_Gate));
            auto* event = dynamic_cast<rf::EventInsideGate*>(base_event);
            if (event) {
                event->check_uid = params.int1;
            }
            return event;
        }
    },
    // Set_Difficulty
    {
        rf::EventType::Set_Difficulty, [](const rf::EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Set_Difficulty));
            auto* event = dynamic_cast<rf::EventSetDifficulty*>(base_event);
            if (event) {
                event->difficulty = params.int1;
            }
            return event;
        }
    },
    // Set_Fog_Far_Clip
    {
        rf::EventType::Set_Fog_Far_Clip, [](const rf::EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Set_Fog_Far_Clip));
            auto* event = dynamic_cast<rf::EventSetFogFarClip*>(base_event);
            if (event) {
                event->far_clip = params.int1;
            }
            return event;
        }
    },
    // AF_When_Dead
    {
        rf::EventType::AF_When_Dead, [](const rf::EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::AF_When_Dead));
            auto* event = dynamic_cast<rf::EventAFWhenDead*>(base_event);
            if (event) {
                event->any_dead = params.bool1;
            }
            return event;
        }
    },
    // Gametype_Gate
    {
        rf::EventType::Gametype_Gate, [](const rf::EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Gametype_Gate));
            auto* event = dynamic_cast<rf::EventGametypeGate*>(base_event);
            if (event) {
                event->gametype = params.str1;
            }
            return event;
        }
    },
};

rf::Event* construct_alpine_event(int event_type, const rf::EventCreateParams& params)
{
    auto it = event_factories.find(rf::int_to_event_type(event_type));
    if (it != event_factories.end()) {
        return it->second(params);
    }

    // Default for events without specific factories
    return rf::event_create(params.pos, event_type);
}

// assignment of factories for AF event types
CodeInjection level_read_events_patch {
    0x00462910, [](auto& regs) {
        int event_type = static_cast<int>(regs.ebp);

        if (event_type >= 100) { // only handle AF events, stock events handled by original code
            rf::Vector3* pos = regs.edx;

            rf::EventCreateParams params{
                pos,
                *reinterpret_cast<rf::String*>(regs.esp + 0x5C),    // class_name
                *reinterpret_cast<rf::String*>(regs.esp + 0x54),    // script_name
                *reinterpret_cast<rf::String*>(regs.esp + 0x44),    // str1
                *reinterpret_cast<rf::String*>(regs.esp + 0x4C),    // str2
                *reinterpret_cast<int*>(regs.esp - 0x24),           // int1
                *reinterpret_cast<int*>(regs.esp - 0x18),           // int2
                *reinterpret_cast<bool*>(regs.esp + 0x18),          // bool1
                *reinterpret_cast<bool*>(regs.esp + 0x20),          // bool2
                *reinterpret_cast<float*>(regs.esp + 0x1C),         // float1
                *reinterpret_cast<float*>(regs.esp + 0x24)          // float2
            };

            xlog::debug("Constructing event type {}: class_name: {}, script_name: {}, str1: {}, str2: {}, int1: {}, "
                       "int2: {}, bool1: {}, bool2: {}, float1: {}, float2: {}",
                       event_type, params.class_name, params.script_name, params.str1, params.str2, params.int1,
                       params.int2, params.bool1, params.bool2, params.float1, params.float2);


            auto* this_event = construct_alpine_event(event_type, params);
            regs.eax = this_event; // set eax to created event so level_read_events can continue to use it

            regs.eip = 0x00462915; // made the event, set stack pointer after jump table
        }
    }
};

CodeInjection event_activate_fixed_delay{
    0x004B8B91,
    [](auto& regs) {
        rf::Event* event = regs.esi;

        // check if a Fixed_Delay is active
        if (event->event_type == rf::event_type_to_int(rf::EventType::Fixed_Delay) && event->delay_timestamp.valid()) { 
            xlog::debug("Ignoring message request in active {} event ({})", event->name, event->uid);
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
