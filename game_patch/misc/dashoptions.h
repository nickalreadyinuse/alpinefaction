#pragma once

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <cstddef>
#include <xlog/xlog.h>

enum class DashOptionID
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
    _optioncount // dummy for total count
};

constexpr std::size_t to_index(DashOptionID option_id)
{
    return static_cast<std::size_t>(option_id);
}

constexpr std::size_t option_count = to_index(DashOptionID::_optioncount);

// Variant type to represent all possible configuration option values
using OptionValue = std::variant<std::string, uint32_t, float, int, bool>;

// Metadata structure to store option information, including its parsing function
struct OptionMetadata
{
    DashOptionID id;
    std::string filename;
    std::function<std::optional<OptionValue>(const std::string&)> parse_function;
};

// Main configuration structure for options
struct DashOptionsConfig
{
    // Store options in a map with their parsed values
    std::unordered_map<DashOptionID, OptionValue> options;
    std::array<bool, option_count> options_loaded = {}; // Track loaded options

    // Check if a specific option is loaded
    bool is_option_loaded(DashOptionID option_id) const
    {
        return options_loaded[static_cast<std::size_t>(option_id)];
    }
};

// Global instance of DashOptionsConfig
extern DashOptionsConfig g_dash_options_config;

// Function to load and parse the configuration files
void load_dashoptions_config();
std::string trim(const std::string& str);

template<typename T>
inline T get_option_value(DashOptionID id)
{
    return std::get<T>(g_dash_options_config.options.at(id));
}

// Helper function to retrieve an option or a default value
template<typename T>
inline T get_option_or_default(DashOptionID id, T default_value)
{
    return g_dash_options_config.is_option_loaded(id) ? get_option_value<T>(id) : default_value;
}
