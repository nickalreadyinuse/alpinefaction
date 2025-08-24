#pragma once

#include <map>
#include <vector>
#include <string>
#include "vtypes.h"
#include "mfc_types.h"
#include "resources.h"

constexpr std::size_t stock_cdedlevel_size = 0x608;
constexpr int alpine_props_chunk_id = 0x0AFBA5ED;

// should match structure in game_patch\misc\level.h
struct AlpineLevelProperties
{
    bool legacy_cyclic_timers = false; // since rfl v302

    // default properties when not set
    // used for maps without alpine level props and maps with older alpine level props versions
    // should always match stock game behaviour
    void LoadDefaults()
    {
        legacy_cyclic_timers = true;
    }

    void Serialize(rf::File& file) const
    {
        file.write<std::uint8_t>(legacy_cyclic_timers);
    }

    void Deserialize(rf::File& file)
    {
        legacy_cyclic_timers = file.read<std::uint8_t>(302); // rfl v302
        xlog::debug("legacy_cyclic_timers {}", legacy_cyclic_timers);
    }
};

struct CDedLevel
{
    char padding_before_selection[0x298];
    VArray<int> selection;
    char padding_after_selection[0x608 - (0x298 + 0xC)];

    std::size_t BeginRflSection(rf::File& file, int chunk_id)
    {
        return AddrCaller{0x00430B60}.this_call<std::size_t>(this, &file, chunk_id);
    }

    void EndRflSection(rf::File& file, std::size_t start_pos)
    {
        return AddrCaller{0x00430B90}.this_call(this, &file, start_pos);
    }

    AlpineLevelProperties& GetAlpineLevelProperties()
    {
        return struct_field_ref<AlpineLevelProperties>(this, stock_cdedlevel_size);
    }

    static CDedLevel* Get()
    {
        return AddrCaller{0x004835F0}.c_call<CDedLevel*>();
    }
};
static_assert(sizeof(CDedLevel) == 0x608);
