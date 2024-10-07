#pragma once

#include "object.h"
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

    static auto& event_lookup_from_uid = addr_as_ref<Event*(int uid)>(0x004B6820);
    static auto& event_lookup_from_handle = addr_as_ref<Event*(int handle)>(0x004B6800);
    static auto& event_create = addr_as_ref<Event*(rf::Vector3 pos, int event_type)>(0x004B6870);
    static auto& event_destructor = addr_as_ref<void(rf::Event*, char flags)>(0x004BEF50);
    static auto& event_delete = addr_as_ref<void(rf::Event*)>(0x004B67C0);
    static auto& event_add_link = addr_as_ref<void(int event_handle, int handle)>(0x004B6790);
    }
