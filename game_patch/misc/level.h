#pragma once

#include <xlog/xlog.h>
#include "../rf/file/file.h"

constexpr int alpine_props_chunk_id = 0xAFBA5ED1;
constexpr int dash_level_props_chunk_id = 0xDA58FA00;

// should match structure in editor_patch\level.h
struct AlpineLevelProperties
{
    // default values for if not set
    bool legacy_cyclic_timers = true; // since rfl v302

    static AlpineLevelProperties& instance()
    {
        static AlpineLevelProperties instance;
        return instance;
    }

    void deserialize(rf::File& file)
    {
        legacy_cyclic_timers = file.read<std::uint8_t>(302); // rfl v302
        xlog::warn("[AlpineLevelProps] legacy_cyclic_timers {}", legacy_cyclic_timers);
    }
};

struct DashLevelProps
{
    // default values for if not set
    bool lightmaps_full_depth = false; // since DashLevelProps v1

    static DashLevelProps& instance()
    {
        static DashLevelProps instance;
        return instance;
    }

    void deserialize(rf::File& file)
    {
        lightmaps_full_depth = file.read<std::uint8_t>();
        xlog::debug("[DashLevelProps] lightmaps_full_depth {}", lightmaps_full_depth);
    }
};
