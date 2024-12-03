#pragma once

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <cstddef>
#include <xlog/xlog.h>

enum class AlpineOptionID
{
    ScoreboardLogo,
    GeomodMesh_Default,
    GeomodMesh_DrillerDouble,
    GeomodMesh_DrillerSingle,
    GeomodMesh_APC,
    GeomodEmitter_Default,
    GeomodEmitter_Driller,
    GeomodTexture_Ice,
    FirstLevelFilename,
    TrainingLevelFilename,
    DisableMultiplayerButton,
    DisableSingleplayerButtons,
    UseStockPlayersConfig,
    IgnoreSwapAssaultRifleControls,
    IgnoreSwapGrenadeControls,
    AssaultRifleAmmoColor,
    PrecisionRifleScopeColor,
    SniperRifleScopeColor,
    RailDriverFireGlowColor,
    RailDriverFireFlashColor,
    SumTrailerButtonAction,
    SumTrailerButtonURL,
    SumTrailerButtonBikFile,
    PlayerEntityType,
    PlayerSuitEntityType,
    PlayerScientistEntityType,
    FallDamageLandMultiplier,
    FallDamageSlamMultiplier,
    MultiplayerWalkSpeed,
    MultiplayerCrouchWalkSpeed,
    WalkableSlopeThreshold,
    _optioncount // dummy for total count
};

constexpr std::size_t to_index(AlpineOptionID option_id)
{
    return static_cast<std::size_t>(option_id);
}

constexpr std::size_t option_count = to_index(AlpineOptionID::_optioncount);

// Variant type to represent all possible configuration option values
using OptionValue = std::variant<std::string, uint32_t, float, int, bool>;

// Metadata structure to store option information, including its parsing function
struct OptionMetadata
{
    AlpineOptionID id;
    std::string filename;
    std::function<std::optional<OptionValue>(const std::string&)> parse_function;
    bool apply_on_server = false; // will only be set on server if true (unnecessary for most)
};

// Main configuration structure for options
struct AlpineOptionsConfig
{
    // Store options in a map with their parsed values
    std::unordered_map<AlpineOptionID, OptionValue> options;
    std::array<bool, option_count> options_loaded = {}; // Track loaded options

    // Check if a specific option is loaded
    bool is_option_loaded(AlpineOptionID option_id) const
    {
        return options_loaded[static_cast<std::size_t>(option_id)];
    }
};

// global instance
extern AlpineOptionsConfig g_alpine_options_config;

// Function to load and parse the configuration files
void load_af_options_config();
std::string trim(const std::string& str);

template<typename T>
inline T get_option_value(AlpineOptionID id)
{
    return std::get<T>(g_alpine_options_config.options.at(id));
}

// Helper function to retrieve an option or a default value
template<typename T>
inline T get_option_or_default(AlpineOptionID id, T default_value)
{
    return g_alpine_options_config.is_option_loaded(id) ? get_option_value<T>(id) : default_value;
}
