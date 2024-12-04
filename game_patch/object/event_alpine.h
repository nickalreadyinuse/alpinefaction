#pragma once

#include <string>
#include <vector>
#include <sstream>
#include <optional>
#include <cstddef>
#include <functional>
#include "../rf/event.h"
#include "../rf/object.h"
#include "../rf/level.h"
#include "../rf/misc.h"
#include "../rf/multi.h"
#include "../rf/hud.h"
#include "../rf/entity.h"
#include "../rf/mover.h"
#include "../rf/trigger.h"
#include "../main/main.h"
#include "../rf/player/player.h"
#include "../rf/os/timestamp.h"



namespace rf
{
    struct EventCreateParams
    {
        const rf::Vector3* pos;
        std::string class_name;
        std::string script_name;
        std::string str1;
        std::string str2;
        int int1;
        int int2;
        bool bool1;
        bool bool2;
        float float1;
        float float2;
    };

    enum class GoalGateTests : int
    {
        equal,
        nequal,
        gt,
        lt,
        geq,
        leq,
        odd,
        even,
        divisible,
        ltinit,
        gtinit,
        leinit,
        geinit,
        eqinit
    };

    enum class GoalMathOperation : int
    {
        add,
        sub,
        mul,
        div,
        rdiv,
        set,
        mod,
        pow,
        neg,
        abs,
        max,
        min,
        reset
    };

    // start alpine event structs
    // id 100
    struct EventSetVar : Event
    {
        SetVarOpts var;
        int value_int = 0;
        float value_float = 0.0f;
        bool value_bool = false;
        std::string value_str = "";

        void turn_on() override
        {
            this->activate(this->trigger_handle, this->triggered_by_handle, true);
        }

        void do_activate(int trigger_handle, int triggered_by_handle, bool on) override
        {
            if (this->links.empty()) {
                return;
            }

            for (int link_handle : this->links) {
                rf::Object* obj = rf::obj_from_handle(link_handle);
                if (obj && obj->type == OT_EVENT) {
                    rf::Event* linked_event = static_cast<rf::Event*>(obj);

                    try {
                        switch (var) {
                        case SetVarOpts::int1:
                        case SetVarOpts::int2:
                            linked_event->apply_var(var, std::to_string(value_int));
                            xlog::info("Applied int value {} to event UID {}", value_int, linked_event->uid);
                            break;
                        case SetVarOpts::float1:
                        case SetVarOpts::float2:
                            linked_event->apply_var(var, std::to_string(value_float));
                            xlog::info("Applied float value {} to event UID {}", value_float, linked_event->uid);
                            break;
                        case SetVarOpts::bool1:
                        case SetVarOpts::bool2:
                            linked_event->apply_var(var, value_bool ? "true" : "false");
                            xlog::info("Applied bool value {} to event UID {}", value_bool, linked_event->uid);
                            break;
                        case SetVarOpts::str1:
                        case SetVarOpts::str2:
                            linked_event->apply_var(var, value_str);
                            xlog::info("Applied string value '{}' to event UID {}", value_str, linked_event->uid);
                            break;
                        default:
                            xlog::warn("Unsupported SetVarOpts value for event UID {}", this->uid);
                            break;
                        }
                    }
                    catch (const std::exception& ex) {
                        xlog::error("Failed to apply variable for Set_Variable event UID {} to linked event UID {} - {}",
                                    this->uid, linked_event->uid, ex.what());
                    }
                }
            }
        }
    };

    // id 101
    struct EventCloneEntity : Event
    {
        bool ignore_item_drop = 0;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers();

            auto& handlers = variable_handler_storage[this];
            handlers[SetVarOpts::bool1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventCloneEntity*>(event);
                this_event->ignore_item_drop = (value == "true");
            };
        }

        void turn_on() override
        {
            for (int i = 0; i < this->links.size(); ++i) {

                int link = this->links[i];
                rf::Object* obj = rf::obj_from_handle(link);
                if (obj) {
                    rf::Entity* entity = static_cast<rf::Entity*>(obj);
                    rf::Entity* new_entity =
                        rf::entity_create(entity->info_index, entity->name, -1, pos, entity->orient, 0, -1);
                    new_entity->entity_flags = entity->entity_flags;
                    new_entity->pos = entity->pos;
                    new_entity->entity_flags2 = entity->entity_flags2;
                    new_entity->info = entity->info;
                    new_entity->info2 = entity->info2;
                    if (!ignore_item_drop) {
                        new_entity->drop_item_class = entity->drop_item_class;
                    }                    
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
                }
            }
        }
    };

    // id 102
    struct EventSetCollisionPlayer : Event
    {
        void turn_on() override
        {
            rf::local_player->collides_with_world = true;
        }
        void turn_off() override
        {
            rf::local_player->collides_with_world = false;
        }
    };

    // id 103
    struct EventSwitchRandom : Event
    {
        bool no_repeats = false;
        std::vector<int> used_handles;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers(); // Include base handlers

            auto& handlers = variable_handler_storage[this];
            handlers[SetVarOpts::bool1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventSwitchRandom*>(event);
                this_event->no_repeats = (value == "true");
            };
        }

        void turn_on() override
        {
            if (this->links.empty()) {
                return;
            }

            // Handle "no_repeats" logic
            if (no_repeats) {
                // Filter out already used handles
                std::vector<int> available_handles;
                for (const int link_handle : this->links) {
                    if (std::find(used_handles.begin(), used_handles.end(), link_handle) == used_handles.end()) {
                        available_handles.push_back(link_handle);
                    }
                }

                // If no available handles remain, reset used_handles and start fresh
                if (available_handles.empty()) {
                    used_handles.clear();

                    // Start fresh with all links
                    for (int link_handle : this->links) {
                        available_handles.push_back(link_handle);
                    } 
                }

                // Select a random link from the available ones
                std::uniform_int_distribution<int> dist(0, available_handles.size() - 1);
                int random_index = dist(g_rng);
                int link_handle = available_handles[random_index];

                // Add selected handle to used_handles
                used_handles.push_back(link_handle);

                event_signal_on(link_handle, this->trigger_handle, this->triggered_by_handle);

                // Activate the selected event
                /* rf::Object* obj = rf::obj_from_handle(link_handle);
                if (obj && obj->type == OT_EVENT) {
                    rf::Event* linked_event = static_cast<rf::Event*>(obj);
                    if (linked_event) {
                        linked_event->turn_on();
                    }
                }*/
            }
            else {
                // Standard random selection (no "no_repeats" behavior)
                std::uniform_int_distribution<int> dist(0, this->links.size() - 1);
                int random_index = dist(g_rng);
                int link_handle = this->links[random_index];

                event_signal_on(link_handle, this->trigger_handle, this->triggered_by_handle);

                // Activate the selected event
                /* rf::Object* obj = rf::obj_from_handle(link_handle);
                if (obj && obj->type == OT_EVENT) {
                    rf::Event* linked_event = static_cast<rf::Event*>(obj);
                    if (linked_event) {
                        linked_event->turn_on();
                    }
                }*/
            }
        }
    };

    // id 104
    struct EventDifficultyGate : Event
    {
        int difficulty = 0;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers();

            auto& handlers = variable_handler_storage[this];
            handlers[SetVarOpts::int1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventDifficultyGate*>(event);
                this_event->difficulty = std::stoi(value);
            };
        }

        void turn_on() override
        {
            xlog::warn("Gate {} with UID {} is checking for difficulty {}", this->name, this->uid, difficulty);

            if (rf::game_get_skill_level() == static_cast<GameDifficultyLevel>(difficulty)) {
                activate_links(this->trigger_handle, this->triggered_by_handle, true);
            }
        }

        void turn_off() override
        {
            xlog::warn("Gate {} with UID {} is checking for difficulty {}", this->name, this->uid, difficulty);

            if (rf::game_get_skill_level() == static_cast<GameDifficultyLevel>(difficulty)) {
                activate_links(this->trigger_handle, this->triggered_by_handle, false);
            }
        }
    };

    // id 105
    struct EventHUDMessage : Event
    {
        std::string message = "";
        float duration = 0.0f;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers(); // Include base handlers

            auto& handlers = variable_handler_storage[this];
            handlers[SetVarOpts::str1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventHUDMessage*>(event);
                this_event->message = value;
            };

            handlers[SetVarOpts::float1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventHUDMessage*>(event);
                this_event->duration = std::stof(value);
            };
        }

        void turn_on() override
        {
            rf::hud_msg(message.c_str(), 0, static_cast<int>(duration * 1000), 0);
        }

        void turn_off() override
        {
            rf::hud_msg_clear();
        }
    };

    // id 106
    struct EventPlayVideo : Event
    {
        std::string filename;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers(); // Include base handlers

            auto& handlers = variable_handler_storage[this];
            handlers[SetVarOpts::str1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventPlayVideo*>(event);
                this_event->filename = value;
            };
        }

        void turn_on() override
        {
            rf::bink_play(filename.c_str());
        }
    };

    // id 107
    struct EventSetLevelHardness : Event
    {
        int hardness;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers(); // Include base handlers

            auto& handlers = variable_handler_storage[this];
            handlers[SetVarOpts::int1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventSetLevelHardness*>(event);
                this_event->hardness = std::stoi(value);
            };
        }

        void turn_on() override
        {
            rf::level.default_rock_hardness = std::clamp(hardness, 0, 100);
        }
    };

    // id 108
    struct EventSequence : Event
    {
        int next_link_index = 0;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers();

            auto& handlers = variable_handler_storage[this];
            handlers[SetVarOpts::int1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventSequence*>(event);
                this_event->next_link_index = std::stoi(value);
            };
        }

        void turn_on() override
        {

            if (this->links.empty()) {
                return;
            }

            // bounds check
            if (next_link_index < 0 || next_link_index >= static_cast<int>(this->links.size())) {
                next_link_index = 0;
            }            

            // find and activate the link
            int link_handle = this->links[next_link_index];

            event_signal_on(link_handle, this->trigger_handle, this->triggered_by_handle);

            /* rf::Object* obj = rf::obj_from_handle(link_handle);
            if (obj && obj->type == OT_EVENT) {
                rf::Event* linked_event = static_cast<rf::Event*>(obj);
                linked_event->turn_on();
            }*/

            ++next_link_index;
        }
    };


    // id 109
    struct EventClearQueued : Event
    {
        void turn_on() override
        {
            for (size_t i = 0; i < this->links.size(); ++i) {
                int link_handle = this->links[i];
                rf::Object* obj = rf::obj_from_handle(link_handle);

                if (obj && obj->type == OT_EVENT) {
                    rf::Event* linked_event = static_cast<rf::Event*>(obj);
                    linked_event->delay_timestamp.invalidate();
                    linked_event->delayed_msg = 0;
                }                
            }
        }
    };

    // id 110
    struct EventRemoveLink : Event
    {
        bool remove_all = 0;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers();

            auto& handlers = variable_handler_storage[this];
            handlers[SetVarOpts::bool1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventRemoveLink*>(event);
                this_event->remove_all = (value == "true");
            };
        }

        void turn_on() override
        {
            for (size_t i = 0; i < this->links.size(); ++i) {
                int link_handle = this->links[i];
                rf::Object* obj = rf::obj_from_handle(link_handle);

                if (obj && obj->type == OT_EVENT) {
                    rf::Event* linked_event = static_cast<rf::Event*>(obj);

                    if (remove_all) {
                        linked_event->links.clear();
                    }
                    else {
                        // Remove only the links between events in this->links
                        linked_event->links.erase_if([this](int inner_link_handle) {
                            return std::find(this->links.begin(), this->links.end(), inner_link_handle) !=
                                   this->links.end();
                        });
                    }
                }
            }
        }
    };

    // id 111
    struct EventFixedDelay : Event {}; // no allocations needed, logic handled in event.cpp

    // id 112
    struct EventAddLink : Event
    {
        int subject_uid = -1;
        bool inbound = false;

        void turn_on() override
        {
            if (this->links.empty()) {
                return;
            }

            rf::Object* subject_obj = rf::obj_lookup_from_uid(subject_uid);

            if (!subject_obj) {
                return;
            }

            int subject_handle = subject_obj->handle;

            for (size_t i = 0; i < this->links.size(); ++i) {
                int target_link_handle = this->links[i];

                if (inbound) {
                    event_add_link(target_link_handle, subject_handle);
                }
                else {
                    event_add_link(subject_handle, target_link_handle);
                }
            }
        }
    };

    // id 113
    struct EventValidGate : Event
    {
        int check_uid = -1;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers(); // Include base handlers

            auto& handlers = variable_handler_storage[this];
            handlers[SetVarOpts::int1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventValidGate*>(event);
                this_event->check_uid = std::stoi(value);
            };
        }

        void turn_on() override
        {
            rf::Object* obj = rf::obj_lookup_from_uid(check_uid);

            if (check_uid == -1 ||
                !obj ||
                obj->type == rf::OT_CORPSE ||
                (obj->type == rf::OT_ENTITY && rf::entity_from_handle(obj->handle)->life <= 0)) { // if entity, alive
                return;
            }

            activate_links(this->trigger_handle, this->triggered_by_handle, true);
        }

        void turn_off() override
        {
            rf::Object* obj = rf::obj_lookup_from_uid(check_uid);

            if (check_uid == -1 || !obj || obj->type == rf::OT_CORPSE ||
                (obj->type == rf::OT_ENTITY && rf::entity_from_handle(obj->handle)->life <= 0)) { // if entity, alive
                return;
            }

            activate_links(this->trigger_handle, this->triggered_by_handle, false);
        }
    };

    // id 114
    struct EventGoalMath : Event
    {
        std::optional<std::string> goal;
        GoalMathOperation operation = GoalMathOperation::add;
        std::optional<int> value;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers(); // Include base handlers

            auto& handlers = variable_handler_storage[this];
            handlers[SetVarOpts::str1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventGoalMath*>(event);
                this_event->goal = value;
            };

            handlers[SetVarOpts::int1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventGoalMath*>(event);
                this_event->operation = static_cast<GoalMathOperation>(std::stoi(value));
            };

            handlers[SetVarOpts::int2] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventGoalMath*>(event);
                this_event->value = std::stoi(value);
            };
        }

        void turn_on() override
        {
            if (!goal.has_value()) {
                return;
            }

            int effective_value = value.value_or(1); // Default value to 1 if not specified
            String effective_goal = goal.value_or("").c_str();

            // Find level goal
            GenericEvent* named_event = event_find_named_event(&effective_goal);

            int* goal_count_ptr = nullptr;
            int* goal_initial_ptr = nullptr;
            if (named_event) {
                goal_count_ptr = reinterpret_cast<int*>(&named_event->event_specific_data[8]); // 11 in original code
                goal_initial_ptr = reinterpret_cast<int*>(&named_event->event_specific_data[0]);
            }

            // Find persistent goal
            PersistentGoalEvent* persist_event = event_lookup_persistent_goal_event(effective_goal);

            if (!persist_event && !named_event) {
                xlog::error("Did not find a level or persistent goal named '{}'", effective_goal);
                return;
            }

            if (goal_count_ptr) {
                xlog::warn("Level goal count before operation: {}, initial: {}", *goal_count_ptr, *goal_initial_ptr);
            }

            if (persist_event) {
                xlog::warn("Persistent goal '{}', count: {}, initial count: {}", persist_event->name,
                           persist_event->count, persist_event->initial_count);
            }

            auto apply_operation = [&](int& goal_count, int initial_value) {
                switch (operation) {
                case GoalMathOperation::add:
                    goal_count += effective_value;
                    break;
                case GoalMathOperation::sub:
                    goal_count -= effective_value;
                    break;
                case GoalMathOperation::mul:
                    goal_count *= effective_value;
                    break;
                case GoalMathOperation::div:
                    if (effective_value != 0) {
                        goal_count /= effective_value;
                    }
                    else {
                        xlog::error("Division by zero attempted in EventGoalMath UID {}", this->uid);
                    }
                    break;
                case GoalMathOperation::rdiv:
                    if (goal_count != 0) {
                        goal_count = effective_value / goal_count;
                    }
                    else {
                        xlog::error("Division by zero attempted in reverse divide operation for EventGoalMath UID {}",
                                    this->uid);
                    }
                    break;
                case GoalMathOperation::set:
                    goal_count = effective_value;
                    break;
                case GoalMathOperation::mod:
                    if (effective_value != 0) {
                        goal_count %= effective_value;
                    }
                    else {
                        xlog::error("Modulo by zero attempted in EventGoalMath UID {}", this->uid);
                    }
                    break;
                case GoalMathOperation::pow:
                    if (effective_value >= 0) {
                        goal_count = static_cast<int>(std::pow(goal_count, effective_value));
                    }
                    else {
                        xlog::error("Negative exponent {} not allowed in EventGoalMath UID {}", effective_value,
                                    this->uid);
                    }
                    break;
                case GoalMathOperation::neg:
                    goal_count = -goal_count;
                    break;
                case GoalMathOperation::abs:
                    goal_count = std::abs(goal_count);
                    break;
                case GoalMathOperation::max:
                    goal_count = std::max(goal_count, effective_value);
                    break;
                case GoalMathOperation::min:
                    goal_count = std::min(goal_count, effective_value);
                    break;
                case GoalMathOperation::reset:
                    goal_count = initial_value;
                    break;
                default:
                    xlog::error("Unknown operation in EventGoalMath UID {}", this->uid);
                    break;
                }
            };

            // Apply to persistent goal
            if (persist_event) {
                xlog::warn("Applying operation '{}' with value '{}' to persistent goal '{}'",
                           static_cast<int>(operation), effective_value, persist_event->name);
                apply_operation(persist_event->count, persist_event->initial_count);
            }

            // Apply to level goal
            if (goal_count_ptr) {
                xlog::warn("Applying operation '{}' with value '{}' to level goal '{}'", static_cast<int>(operation),
                           effective_value, effective_goal);
                apply_operation(*goal_count_ptr, *goal_initial_ptr);
            }

            // Log final values
            if (persist_event) {
                xlog::warn("Persistent goal '{}', new count: {}, initial count: {}", persist_event->name,
                           persist_event->count, persist_event->initial_count);
            }

            if (goal_count_ptr) {
                xlog::warn("Level goal count after operation: {}", *goal_count_ptr);
            }
        }
    };

    // id 115
    struct EventGoalGate : Event
    {
        std::optional<std::string> goal;
        GoalGateTests test_type = GoalGateTests::equal;
        std::optional<int> value;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers(); // Include base handlers

            auto& handlers = variable_handler_storage[this];
            handlers[SetVarOpts::str1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventGoalGate*>(event);
                this_event->goal = value;
                xlog::info("apply_var: Set goal to {}", this_event->goal.value_or(""));
            };

            handlers[SetVarOpts::int1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventGoalGate*>(event);
                this_event->test_type = static_cast<GoalGateTests>(std::stoi(value));
                xlog::info("apply_var: Set test_type to {}", static_cast<int>(this_event->test_type));
            };

            handlers[SetVarOpts::int2] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventGoalGate*>(event);
                this_event->value = std::stoi(value);
                xlog::info("apply_var: Set value to {}", this_event->value.value_or(0));
            };
        }

        void turn_on() override
        {
            xlog::warn("Turning on EventGoalGate UID {}", this->uid);

            if (!goal.has_value()) {
                return;
            }

            int effective_value = value.value_or(0); // Default value to 0 if not specified
            String effective_goal = goal.value_or("").c_str();

            // find level goal
            GenericEvent* named_event = event_find_named_event(&effective_goal);

            int* goal_count_ptr = nullptr;
            int* goal_initial_ptr = nullptr;
            if (named_event) {
                goal_count_ptr = reinterpret_cast<int*>(&named_event->event_specific_data[8]); // 11 in original code
                goal_initial_ptr = reinterpret_cast<int*>(&named_event->event_specific_data[0]);
            }

            // find persistent goal
            PersistentGoalEvent* persist_event = event_lookup_persistent_goal_event(effective_goal);

            if (!persist_event && !named_event) {
                xlog::error("Did not find a level or persistent goal named '{}'", effective_goal);
                return;
            }

            int current_value = 0;
            int initial_value = 0;
            if (goal_count_ptr) {
                current_value = *goal_count_ptr;
                initial_value = goal_initial_ptr ? *goal_initial_ptr : 0;
            }
            else if (persist_event) {
                current_value = persist_event->count;
                initial_value = persist_event->initial_count;
            }

            xlog::warn("Current goal value for '{}': {}, Initial value: {}", effective_goal, current_value,
                       initial_value);

            bool pass = false;

            // Determine test type
            switch (test_type) {
            case GoalGateTests::equal:
                pass = (current_value == effective_value);
                break;
            case GoalGateTests::nequal:
                pass = (current_value != effective_value);
                break;
            case GoalGateTests::gt:
                pass = (current_value > effective_value);
                break;
            case GoalGateTests::lt:
                pass = (current_value < effective_value);
                break;
            case GoalGateTests::geq:
                pass = (current_value >= effective_value);
                break;
            case GoalGateTests::leq:
                pass = (current_value <= effective_value);
                break;
            case GoalGateTests::odd:
                pass = (current_value % 2 != 0);
                break;
            case GoalGateTests::even:
                pass = (current_value % 2 == 0);
                break;
            case GoalGateTests::divisible:
                if (effective_value != 0) {
                    pass = (current_value % effective_value == 0);
                }
                else {
                    xlog::error("Division by zero in divisible test for EventGoalGate UID {}", this->uid);
                }
                break;
            case GoalGateTests::ltinit:
                pass = (current_value < initial_value);
                break;
            case GoalGateTests::gtinit:
                pass = (current_value > initial_value);
                break;
            case GoalGateTests::leinit:
                pass = (current_value <= initial_value);
                break;
            case GoalGateTests::geinit:
                pass = (current_value >= initial_value);
                break;
            case GoalGateTests::eqinit:
                pass = (current_value == initial_value);
                break;
            default:
                xlog::error("Unknown test type in EventGoalGate UID {}", this->uid);
                break;
            }

            // Log test result
            if (pass) {
                xlog::warn("Test '{}' passed for EventGoalGate UID {}", static_cast<int>(test_type), this->uid);
                activate_links(this->trigger_handle, this->triggered_by_handle, true);
            }
            else {
                xlog::warn("Test '{}' failed for EventGoalGate UID {}", static_cast<int>(test_type), this->uid);
            }
        }
    };


    // id 116
    struct EventEnvironmentGate : Event
    {
        std::optional<std::string> environment;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers(); // Include base handlers

            auto& handlers = variable_handler_storage[this];
            handlers[SetVarOpts::str1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventEnvironmentGate*>(event);
                this_event->environment = value;
                xlog::info("apply_var: Set environment to {}", this_event->environment.value_or(""));
            };
        }

        void turn_on() override
        {
            xlog::warn("Turning on EventEnvironmentGate UID {}", this->uid);

            if (!environment.has_value()) {
                return;
            }

            const std::string& test = environment.value();
            bool pass = false;

            if (test == "multi") {
                pass = rf::is_multi;
            }
            else if (test == "single") {
                pass = !rf::is_multi;
            }
            else if(test == "server") {
                pass = (rf::is_server || rf::is_dedicated_server);
            }
            else if(test == "dedicated") {
                pass = rf::is_dedicated_server;
            }
            else if (test == "client") {
                pass = (!rf::is_server && !rf::is_dedicated_server);
            }
            else {
                xlog::error("Unknown environment test '{}' for EventEnvironmentGate UID {}", test, this->uid);
            }

            if (pass) {
                xlog::warn("Test '{}' passed for EventEnvironmentGate UID {}", test, this->uid);
                activate_links(this->trigger_handle, this->triggered_by_handle, true);
            }
            else {
                xlog::warn("Test '{}' failed for EventEnvironmentGate UID {}", test, this->uid);
            }
        }
    };

    // id 117
    struct EventInsideGate : Event
    {
        int check_uid = -1;
        std::optional<int> test_uid;

       void register_variable_handlers() override
        {
            Event::register_variable_handlers(); // Include base handlers

            auto& handlers = variable_handler_storage[this];
            handlers[SetVarOpts::int1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventInsideGate*>(event);
                this_event->check_uid = std::stoi(value);
                xlog::info("apply_var: Set check_uid to {}", this_event->check_uid);
            };

            handlers[SetVarOpts::int2] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventInsideGate*>(event);
                this_event->test_uid = std::stoi(value);
                xlog::info("apply_var: Set test_uid to {}", this_event->test_uid.value_or(-1));
            };
        }

        void turn_on() override
        {
            Object* obj = obj_lookup_from_uid(check_uid);
            GRoom* room = level_room_from_uid(check_uid);

            int obj_handle_to_test = -1;

            if (test_uid.has_value() && test_uid.value() > 0) {
                xlog::warn("test_uid has value");
                Object* obj_to_test = obj_lookup_from_uid(test_uid.value_or(-1));

                if (obj_to_test) {
                    xlog::warn("test_uid is using {}", obj_to_test->name);
                    obj_handle_to_test = obj_to_test->handle;
                }
            }
            else {
                obj_handle_to_test = triggered_by_handle;
            }

            Object* triggered_by_obj = obj_from_handle(obj_handle_to_test);            

            if (check_uid == -1 || !triggered_by_obj || (!obj && !room)) {
                return;
            }

            xlog::warn("object being tested is {}, UID {}", triggered_by_obj->name, triggered_by_obj->uid);

            bool pass = false;

            if (obj && obj->type == OT_TRIGGER) {
                Trigger* trigger = static_cast<Trigger*>(obj);

                xlog::warn("Trigger UID {} is type {}", check_uid, trigger->type);

                switch (trigger->type) {
                case 0: // sphere shape trigger
                    pass = trigger_inside_bounding_sphere(trigger, triggered_by_obj);
                    break;

                case 1: // box shape trigger
                    pass = trigger_inside_bounding_box(trigger, triggered_by_obj);
                    break;

                default:
                    xlog::warn("Unknown trigger type {} for EventInsideGate UID {}", trigger->type, this->uid);
                }
            }
            else if (room) { // handle room test
                xlog::warn("tested object's room is {}, test room is {}", triggered_by_obj->room->uid, room->uid);
                if (room && triggered_by_obj->room == room) {
                    pass = true;
                }
            }

            if (pass) {
                xlog::warn("Test passed for EventInsideGate UID {}", this->uid);
                activate_links(this->trigger_handle, this->triggered_by_handle, true);
            }
            else {
                xlog::warn("Test failed for EventInsideGate UID {}", this->uid);
            }
        }
    };

    // id 118
    struct EventAnchorMarker : Event {}; // no allocations needed, logic handled in object.cpp

    // id 119
    struct EventForceUnhide : Event
    {
        void turn_on() override
        {
            xlog::warn("turning on");
            for (const int link : this->links)
            {
                xlog::warn("checking handle {}", link);
                rf::Object* obj = rf::obj_from_handle(link);
                if (obj) {
                    xlog::warn("unhiding {}", obj->uid);
                    rf::obj_unhide(obj);
                }
            }
        }

        void turn_off() override
        {
            xlog::warn("turning off");
            for (const int link : this->links)
            {
                xlog::warn("checking handle {}", link);
                rf::Object* obj = rf::obj_from_handle(link);
                if (obj) {
                    xlog::warn("hiding {}", obj->uid);
                    rf::obj_hide(obj);
                }
            }
        }
    };

    // id 120
    struct EventSetDifficulty : Event
    {
        int difficulty = 0;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers(); // Include base handlers

            auto& handlers = variable_handler_storage[this];
            handlers[SetVarOpts::int1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventSetDifficulty*>(event);
                this_event->difficulty = std::stoi(value);
                xlog::info("apply_var: Set difficulty to {}", this_event->difficulty);
            };
        }

        void turn_on() override
        {
            xlog::warn("setting difficulty to {}", difficulty);
            rf::game_set_skill_level(static_cast<GameDifficultyLevel>(difficulty));
        }
    };

    // id 121
    struct EventSetFogFarClip : Event
    {
        int far_clip = 0;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers(); // Include base handlers

            auto& handlers = variable_handler_storage[this];
            handlers[SetVarOpts::int1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventSetFogFarClip*>(event);
                this_event->far_clip = std::stoi(value);
                xlog::warn("apply_var: Set far_clip to {} for EventSetFogFarClip UID={}", this_event->far_clip,
                           this_event->uid);
            };
        }

        void turn_on() override
        {
            xlog::warn("setting far clip to {}", far_clip);
            rf::level.distance_fog_far_clip = far_clip;
        }
    };

    // id 122
    struct EventAFWhenDead : Event
    {
        bool any_dead = 0;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers(); // Include base handlers

            auto& handlers = variable_handler_storage[this];
            handlers[SetVarOpts::bool1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventAFWhenDead*>(event);
                this_event->any_dead = (value == "true");
                xlog::warn("apply_var: Set any_dead to {} for EventAFWhenDead UID={}", this_event->any_dead,
                           this_event->uid);
            };
        }

        void turn_on() override
        {
            // only allow this event to fire when something its linked to dies
            if (!this->links.contains(this->trigger_handle)) {
                return;
            }

            // any_dead = true
            if (any_dead) {
                activate_links(this->trigger_handle, this->triggered_by_handle, true);
                return;
            }

            // any_dead = false
            bool all_dead_or_invalid = std::all_of(this->links.begin(), this->links.end(), [this](int link_handle) {
                if (link_handle == this->trigger_handle) {
                    return true; // i know im dead
                }

                rf::Object* obj = rf::obj_from_handle(link_handle);

                // not a valid object
                if (!obj) {
                    return true;
                }

                // if entity, also check if its dead
                if (obj && obj->type == rf::OT_ENTITY) {
                    rf::Entity* entity = rf::entity_from_handle(obj->handle);
                    return entity && entity->life <= 0;
                }

                // only care about entities and clutter
                return obj->type != rf::OT_CLUTTER && obj->type != rf::OT_ENTITY;
            });

            // everything else isn't dead yet
            if (!all_dead_or_invalid) {
                return;
            }

            // everything else is also dead, fire
            activate_links(this->trigger_handle, this->triggered_by_handle, true);
        }

        void do_activate_links(int trigger_handle, int triggered_by_handle, bool on) override
        {
            for (int link_handle : this->links) {
                rf::Object* obj = rf::obj_from_handle(link_handle);

                if (obj) {
                    rf::ObjectType type = obj->type;
                    switch (type) {
                        case OT_MOVER: {
                            mover_activate_from_trigger(obj->handle, -1, -1);
                            break;
                        }
                        case OT_TRIGGER: {
                            Trigger* trigger = static_cast<Trigger*>(obj);
                            trigger_enable(trigger);
                            break;
                        }
                        case OT_EVENT: {
                            Event* event = static_cast<Event*>(obj);
                            // Note can't use activate because it isn't allocated for stock events
                            event_signal_on(link_handle, -1, -1);
                            break;
                        }
                        default:
                            break;
                    }
                }
            }
        }
    };

    // id 123
    struct EventGametypeGate : Event
    {
        std::optional<std::string> gametype;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers(); // Include base handlers

            auto& handlers = variable_handler_storage[this];
            handlers[SetVarOpts::str1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventGametypeGate*>(event);
                this_event->gametype = value;
                xlog::warn("apply_var: Set gametype to '{}' for EventGametypeGate UID={}",
                           this_event->gametype.value_or(""), this_event->uid);
            };
        }

        void turn_on() override
        {
            xlog::warn("Turning on EventGametypeGate UID {}", this->uid);

            if (!gametype.has_value() || !rf::is_multi) {
                return;
            }

            const std::string& test = gametype.value();
            bool pass = false;

            if (test == "dm") {
                pass = (rf::multi_get_game_type() == rf::NG_TYPE_DM);
            }
            else if (test == "tdm" || test == "teamdm") {
                pass = (rf::multi_get_game_type() == rf::NG_TYPE_TEAMDM);
            }
            else if (test == "ctf") {
                pass = (rf::multi_get_game_type() == rf::NG_TYPE_CTF);
            }
            else {
                xlog::error("Unknown gametype test '{}' for EventGametypeGate UID {}", test, this->uid);
            }

            if (pass) {
                xlog::warn("Test '{}' passed for EventGametypeGate UID {}", test, this->uid);
                activate_links(this->trigger_handle, this->triggered_by_handle, true);
            }
            else {
                xlog::warn("Test '{}' failed for EventGametypeGate UID {}", test, this->uid);
            }
        }
    };

    // id 124
    struct EventWhenPickedUp : Event
    {
        void turn_on() override
        {
            // only allow this event to fire when something its linked to dies
            if (!this->links.contains(this->trigger_handle)) {
                return;
            }

            activate_links(this->trigger_handle, this->triggered_by_handle, true);
        }

        void do_activate_links(int trigger_handle, int triggered_by_handle, bool on) override
        {
            for (int link_handle : this->links) {
                rf::Object* obj = rf::obj_from_handle(link_handle);

                if (obj) {
                    rf::ObjectType type = obj->type;
                    switch (type) {
                    case OT_MOVER: {
                        mover_activate_from_trigger(obj->handle, -1, -1);
                        break;
                    }
                    case OT_TRIGGER: {
                        Trigger* trigger = static_cast<Trigger*>(obj);
                        trigger_enable(trigger);
                        break;
                    }
                    case OT_EVENT: {
                        Event* event = static_cast<Event*>(obj);
                        // Note can't use activate because it isn't allocated for stock events
                        event_signal_on(link_handle, -1, -1);
                        break;
                    }
                    default:
                        break;
                    }
                }
            }
        }
    };

} // namespace rf

