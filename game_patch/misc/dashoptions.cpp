#include "dashoptions.h"
#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/ShortTypes.h>
#include <patch_common/AsmWriter.h>
#include "../os/console.h"
#include "../rf/file/file.h"
#include "../rf/gr/gr.h"
#include "../rf/sound/sound.h"
#include "../rf/geometry.h"
#include "../rf/entity.h"
#include "../rf/multi.h"
#include "../rf/misc.h"
#include "../rf/os/os.h"
#include <algorithm>
#include <iostream>
//#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <windows.h>
#include <shellapi.h>
#include <xlog/xlog.h>

//namespace fs = std::filesystem;

DashOptionsConfig g_dash_options_config;

// trim leading and trailing whitespace
std::string trim(const std::string& str, bool remove_quotes = false)
{
    auto start = str.find_first_not_of(" \t\n\r");
    auto end = str.find_last_not_of(" \t\n\r");
    if (start == std::string::npos) {
        return "";
    }        

    std::string trimmed = str.substr(start, end - start + 1);

    // extract quoted value
    if (remove_quotes && trimmed.size() >= 2 && trimmed.front() == '"' && trimmed.back() == '"') {
        trimmed = trimmed.substr(1, trimmed.size() - 2);
    }

    return trimmed;
}

// Helper functions for individual data types

// parse hex formatted colors
std::tuple<int, int, int, int> extract_color_components(uint32_t color)
{
    return std::make_tuple((color >> 24) & 0xFF, // red
                           (color >> 16) & 0xFF, // green
                           (color >> 8) & 0xFF,  // blue
                           color & 0xFF          // alpha
    );
}

// strings can be provided in quotation marks or not
std::optional<OptionValue> parse_string(const std::string& value)
{
    return trim(value, true);
}

// colors can be provided in quotation marks or not
std::optional<OptionValue> parse_color(const std::string& value)
{
    std::string trimmed_value = trim(value, true);
    try {
        return std::stoul(trimmed_value, nullptr, 16);
    }
    catch (...) {
        return std::nullopt;
    }
}

// floats, ints, and bools do not use quotation marks
std::optional<OptionValue> parse_float(const std::string& value) { 
    try { return std::stof(value); } catch (...) { return std::nullopt; } 
}
std::optional<OptionValue> parse_int(const std::string& value) { 
    try { return std::stoi(value); } catch (...) { return std::nullopt; } 
}
std::optional<OptionValue> parse_bool(const std::string& value) { 
    return value == "1" || value == "true" || value == "True"; 
}

// master list of options: mapped to option ID, associated tbl, and parser
const std::unordered_map<std::string, OptionMetadata> option_metadata = {
    {"$Scoreboard Logo", {DashOptionID::ScoreboardLogo, "af_ui.tbl", parse_string}}, // applied in multi_scoreboard.cpp
    {"$Default Geomod Mesh", {DashOptionID::GeomodMesh_Default, "af_game.tbl", parse_string}}, // unsupported currently
    {"$Driller Double Geomod Mesh", {DashOptionID::GeomodMesh_DrillerDouble, "af_game.tbl", parse_string}}, // unsupported currently
    {"$Driller Single Geomod Mesh", {DashOptionID::GeomodMesh_DrillerSingle, "af_game.tbl", parse_string}}, // unsupported currently
    {"$APC Geomod Mesh", {DashOptionID::GeomodMesh_APC, "af_game.tbl", parse_string}}, // unsupported currently
    {"$Default Geomod Smoke Emitter", {DashOptionID::GeomodEmitter_Default, "af_game.tbl", parse_string}},
    {"$Driller Geomod Smoke Emitter", {DashOptionID::GeomodEmitter_Driller, "af_game.tbl", parse_string}},
    {"$Ice Geomod Texture", {DashOptionID::GeomodTexture_Ice, "af_game.tbl", parse_string}},
    {"$First Level Filename", {DashOptionID::FirstLevelFilename, "af_game.tbl", parse_string}},
    {"$Training Level Filename", {DashOptionID::TrainingLevelFilename, "af_game.tbl", parse_string}},
    {"$Disable Multiplayer Button", {DashOptionID::DisableMultiplayerButton, "af_ui.tbl", parse_bool}},
    {"$Disable Singleplayer Buttons", {DashOptionID::DisableSingleplayerButtons, "af_ui.tbl", parse_bool}},
    {"$Use Base Game Players Config", {DashOptionID::UseStockPlayersConfig, "af_game.tbl", parse_bool}},
    {"$Ignore Swap Assault Rifle Controls", {DashOptionID::IgnoreSwapAssaultRifleControls, "af_game.tbl", parse_bool}}, // applied in player.cpp
    {"$Ignore Swap Grenade Controls", {DashOptionID::IgnoreSwapGrenadeControls, "af_game.tbl", parse_bool}}, // applied in player.cpp
    {"$Assault Rifle Ammo Counter Color", {DashOptionID::AssaultRifleAmmoColor, "af_client.tbl", parse_color}},
    {"$Precision Rifle Scope Color", {DashOptionID::PrecisionRifleScopeColor, "af_client.tbl", parse_color}},
    {"$Sniper Rifle Scope Color", {DashOptionID::SniperRifleScopeColor, "af_client.tbl", parse_color}},
    {"$Rail Driver Fire Glow Color", {DashOptionID::RailDriverFireGlowColor, "af_client.tbl", parse_color}},
    {"$Rail Driver Fire Flash Color", {DashOptionID::RailDriverFireFlashColor, "af_client.tbl", parse_color}},
    {"$Summoner Trailer Button Action", {DashOptionID::SumTrailerButtonAction, "af_ui.tbl", parse_int}},
    {"+Summoner Trailer Button URL", {DashOptionID::SumTrailerButtonURL, "af_ui.tbl", parse_string}},
    {"+Summoner Trailer Button Bink Filename", {DashOptionID::SumTrailerButtonBikFile, "af_ui.tbl", parse_string}},
    {"$Player Entity Type", {DashOptionID::PlayerEntityType, "af_game.tbl", parse_string, true}},
    {"$Player Undercover Suit Entity Type", {DashOptionID::PlayerSuitEntityType, "af_game.tbl", parse_string}},
    {"$Player Undercover Scientist Entity Type", {DashOptionID::PlayerScientistEntityType, "af_game.tbl", parse_string}},
    {"$Fall Damage Land Multiplier", {DashOptionID::FallDamageLandMultiplier, "af_game.tbl", parse_float, true}},
    {"$Fall Damage Slam Multiplier", {DashOptionID::FallDamageSlamMultiplier, "af_game.tbl", parse_float, true}},
    {"$Multiplayer Walk Speed", {DashOptionID::MultiplayerWalkSpeed, "af_game.tbl", parse_float, true}},
    {"$Multiplayer Crouch Walk Speed", {DashOptionID::MultiplayerCrouchWalkSpeed, "af_game.tbl", parse_float, true}}
};

void open_url(const std::string& url)
{
    try {
        if (url.empty()) {
            xlog::error("URL is empty");
            return;
        }
        xlog::info("Opening URL: {}", url);
        HINSTANCE result = ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOW);
        if (reinterpret_cast<int>(result) <= 32) {
            xlog::error("Failed to open URL. Error code: {}", reinterpret_cast<int>(result));
        }
    }
    catch (const std::exception& ex) {
        xlog::error("Exception occurred while trying to open URL: {}", ex.what());
    }
}

CallHook<void(int, int, int, int)> fpgun_ar_ammo_digit_color_hook{
    0x004ABC03,
    [](int red, int green, int blue, int alpha) {
        auto ar_ammo_color = get_option_value<uint32_t>(DashOptionID::AssaultRifleAmmoColor);
        std::tie(red, green, blue, alpha) = extract_color_components(ar_ammo_color);
        fpgun_ar_ammo_digit_color_hook.call_target(red, green, blue, alpha);
    }
};

CallHook<void(int, int, int, int)> precision_rifle_scope_color_hook{
    0x004AC850, [](int red, int green, int blue, int alpha) {
        auto pr_scope_color = get_option_value<uint32_t>(DashOptionID::PrecisionRifleScopeColor);
        std::tie(red, green, blue, alpha) = extract_color_components(pr_scope_color);
        precision_rifle_scope_color_hook.call_target(red, green, blue, alpha);
    }
};

CallHook<void(int, int, int, int)> sniper_rifle_scope_color_hook{
    0x004AC458, [](int red, int green, int blue, int alpha) {
        auto sr_scope_color = get_option_value<uint32_t>(DashOptionID::SniperRifleScopeColor);
        std::tie(red, green, blue, alpha) = extract_color_components(sr_scope_color);
        sniper_rifle_scope_color_hook.call_target(red, green, blue, alpha);
    }
};

CallHook<void(int, int, int, int)> rail_gun_fire_glow_hook{
    0x004AC00E, [](int red, int green, int blue, int alpha) {
        auto rail_glow_color = get_option_value<uint32_t>(DashOptionID::RailDriverFireGlowColor);
        std::tie(red, green, blue, alpha) = extract_color_components(rail_glow_color);
        rail_gun_fire_glow_hook.call_target(red, green, blue, alpha);
    }
};

CallHook<void(int, int, int, int)> rail_gun_fire_flash_hook{
    0x004AC04A, [](int red, int green, int blue, int alpha) {
        auto rail_flash_color = get_option_value<uint32_t>(DashOptionID::RailDriverFireFlashColor);
        std::tie(red, green, blue, std::ignore) = extract_color_components(rail_flash_color);
        rail_gun_fire_flash_hook.call_target(red, green, blue, alpha);
    }
};

// consolidated logic for handling geo mesh changes
int handle_geomod_shape_create(const char* filename, const std::optional<std::string>& config_value,
                               CallHook<int(const char*)>& hook)
{
    std::string original_filename{filename};
    std::string modded_filename = config_value.value_or(original_filename);
    return hook.call_target(modded_filename.c_str());
}

// Set default geo mesh
CallHook<int(const char*)> default_geomod_shape_create_hook{
    0x004374CF, [](const char* filename) -> int {
        auto modded_filename = get_option_value<std::string>(DashOptionID::GeomodMesh_Default);
        return default_geomod_shape_create_hook.call_target(modded_filename.c_str());
    }
};

// Set driller double geo mesh
CallHook<int(const char*)> driller_double_geomod_shape_create_hook{
    0x004374D9, [](const char* filename) -> int {
        auto modded_filename = get_option_value<std::string>(DashOptionID::GeomodMesh_DrillerDouble);
        return driller_double_geomod_shape_create_hook.call_target(modded_filename.c_str());
    }
};

// Set driller single geo mesh
CallHook<int(const char*)> driller_single_geomod_shape_create_hook{
    0x004374E3, [](const char* filename) -> int {
        auto modded_filename = get_option_value<std::string>(DashOptionID::GeomodMesh_DrillerSingle);
        return driller_single_geomod_shape_create_hook.call_target(modded_filename.c_str());
    }
};

// Set APC geo mesh
CallHook<int(const char*)> apc_geomod_shape_create_hook{
    0x004374ED, [](const char* filename) -> int {
        auto modded_filename = get_option_value<std::string>(DashOptionID::GeomodMesh_APC);
        return apc_geomod_shape_create_hook.call_target(modded_filename.c_str());
    }
};

void apply_geomod_mesh_patch()
{
    
    /* if (g_dash_options_config.geomodmesh_default.has_value()) {
        AsmWriter(0x00437543).call(0x004ECED0);
        const char filename = g_dash_options_config.geomodmesh_default->c_str();
        xlog::warn("set geo mesh to {}", filename);
        static const char NEW_GEOMESH_FILENAME[] = "NewFile.v3d";

        AsmWriter(0x004374C0).push(reinterpret_cast<uintptr_t>(NEW_GEOMESH_FILENAME));

    }*/


    /*// array of geomod mesh options
    std::array<std::pair<DashOptionID, void (*)()>, 4> geomod_mesh_hooks = {
        {{DashOptionID::GeomodMesh_Default, [] { default_geomod_shape_create_hook.install(); }},
         {DashOptionID::GeomodMesh_DrillerDouble, [] { driller_double_geomod_shape_create_hook.install(); }},
         {DashOptionID::GeomodMesh_DrillerSingle, [] { driller_single_geomod_shape_create_hook.install(); }},
         {DashOptionID::GeomodMesh_APC, [] { apc_geomod_shape_create_hook.install(); }}
        }};

    bool any_option_loaded = false;

    // install only the hooks for the ones that were set
    for (const auto& [option_id, install_fn] : geomod_mesh_hooks) {
        if (g_dash_options_config.is_option_loaded(option_id)) {
            install_fn();
            any_option_loaded = true;
        }
    }*/
    bool any_option_loaded = false;
    // if any one was set, apply the necessary patches
    if (any_option_loaded) {
        AsmWriter(0x00437543).call(0x004ECED0); // Replace the call to load v3d instead of embedded
        //rf::geomod_shape_init();                // Reinitialize geomod shapes
    }
}

int geomod_shape_create(const char* filename, bool embedded)
{
    int shape_index = rf::g_num_geomod_shapes;
    rf::g_geomod_shapes_strings[shape_index] = filename;

    rf::GSolid* shape = embedded ?
        rf::g_solid_load_v3d_embedded(filename) : rf::g_solid_load_v3d(filename);

    rf::g_geomod_shapes_meshes[shape_index * 3] = reinterpret_cast<int>(shape);

    return rf::g_num_geomod_shapes++;
}

FunHook<void()> geomod_shape_init_hook{
    0x004374C0,
    []() {
        xlog::warn("Initing geomeshes");
        rf::g_num_geomod_shapes = 0;
        //rf::g_geomod_shapes_strings.clear();
        //rf::g_geomod_shapes_meshes.clear();

        geomod_shape_create("Holey01.v3d", true);
        geomod_shape_create("bit_driller_double.v3d", true);
        geomod_shape_create("bit_driller_single.v3d", true);
        geomod_shape_create("Holey_APC.v3d", true);
        rf::geomod_shape_shutdown();
    }
};

// Override default geomod smoke emitter
CallHook<int(const char*)> default_geomod_emitter_get_index_hook{
    0x00437150, [](const char* emitter_name) -> int {
        auto modded_emitter_name = get_option_value<std::string>(DashOptionID::GeomodEmitter_Default);
        return default_geomod_emitter_get_index_hook.call_target(modded_emitter_name.c_str());
    }
};

// Override driller geomod smoke emitter
CallHook<int(const char*)> driller_geomod_emitter_get_index_hook{
    0x0043715F, [](const char* emitter_name) -> int {
        auto modded_emitter_name = get_option_value<std::string>(DashOptionID::GeomodEmitter_Driller);
        return driller_geomod_emitter_get_index_hook.call_target(modded_emitter_name.c_str());
    }
};

// Replace ice geomod region texture
CallHook<int(const char*, int, bool)> ice_geo_crater_bm_load_hook {
    {
        0x004673B5, // chunks
        0x00466BEF, // crater
    },
    [](const char* filename, int path_id, bool generate_mipmaps) -> int {
        auto modded_filename = get_option_value<std::string>(DashOptionID::GeomodTexture_Ice);
        return ice_geo_crater_bm_load_hook.call_target(modded_filename.c_str(), path_id, generate_mipmaps);
    }
};

// fall damage when impacting
CallHook<void(rf::Entity*, float)> physics_calc_fall_damage_slam_hook{
    0x0049DE39,
    [](rf::Entity* entity, float rel_vel) {
        float damage_multiplier = get_option_or_default<float>(DashOptionID::FallDamageSlamMultiplier, 1.0f);
        float adjusted_rel_vel = rel_vel * damage_multiplier;

        xlog::warn("New slam damage value is {}", adjusted_rel_vel);

        physics_calc_fall_damage_slam_hook.call_target(entity, adjusted_rel_vel);
    }
};

// fall damage when landing
CallHook<void(rf::Entity*, float)> physics_calc_fall_damage_land_hook{
    0x004A0C28,
    [](rf::Entity* entity, float rel_vel) {
        float damage_multiplier = get_option_or_default<float>(DashOptionID::FallDamageLandMultiplier, 1.0f);
        float adjusted_rel_vel = rel_vel * damage_multiplier;

        xlog::warn("New land damage value is {}", adjusted_rel_vel);

        physics_calc_fall_damage_land_hook.call_target(entity, adjusted_rel_vel);
    }
};

// Override first level filename for new game menu
CallHook<void(const char*)> first_load_level_hook{
    0x00443B15, [](const char* level_name) {
        auto new_level_name = get_option_value<std::string>(DashOptionID::FirstLevelFilename);
        first_load_level_hook.call_target(new_level_name.c_str());
    }
};

// Override training level filename for new game menu
CallHook<void(const char*)> training_load_level_hook{
    0x00443A85, [](const char* level_name) {
        auto new_level_name = get_option_value<std::string>(DashOptionID::TrainingLevelFilename);
        training_load_level_hook.call_target(new_level_name.c_str());
    }
};

// Implement demo_extras_summoner_trailer_click using FunHook
FunHook<void(int, int)> extras_summoner_trailer_click_hook{
    0x0043EC80, [](int x, int y) {
        xlog::debug("Summoner trailer button clicked");
        int action = get_option_value<int>(DashOptionID::SumTrailerButtonAction);

        switch (action) {
            case 1: { // Open URL
                if (g_dash_options_config.is_option_loaded(DashOptionID::SumTrailerButtonURL)) {
                    auto url = get_option_value<std::string>(DashOptionID::SumTrailerButtonURL);
                    open_url(url);
                }
                break;
            }
            case 2: // Disable button
                break;
            default: { // Play Bink video
                auto trailer_path = get_option_value<std::string>(DashOptionID::SumTrailerButtonBikFile);
                xlog::debug("Playing BIK file: {}", trailer_path);
                rf::snd_pause(true);
                rf::bink_play(trailer_path.c_str());
                rf::snd_pause(false);
                break;
            }
        }
    }
};

void handle_summoner_trailer_button()
{
    int action = get_option_value<int>(DashOptionID::SumTrailerButtonAction);
    if (action != -1) {
        if (action == 3) {
            // Action 3: Remove the button
            AsmWriter(0x0043EE14).nop(5);
        }
        else {
            // Otherwise, install the hook
            extras_summoner_trailer_click_hook.install();
        }
    }
}

// Only install hooks for options that have been loaded and specified
void apply_af_options_patches()
{
    xlog::warn("Applying Alpine Faction options patches");

    // ===========================
    // af_client.tbl
    // ===========================
    if (g_dash_options_config.is_option_loaded(DashOptionID::AssaultRifleAmmoColor)) {
        fpgun_ar_ammo_digit_color_hook.install();
    }

    if (g_dash_options_config.is_option_loaded(DashOptionID::PrecisionRifleScopeColor)) {
        precision_rifle_scope_color_hook.install();
    }

    if (g_dash_options_config.is_option_loaded(DashOptionID::SniperRifleScopeColor)) {
        sniper_rifle_scope_color_hook.install();
    }

    if (g_dash_options_config.is_option_loaded(DashOptionID::RailDriverFireGlowColor)) {
        rail_gun_fire_glow_hook.install();
    }

    if (g_dash_options_config.is_option_loaded(DashOptionID::RailDriverFireFlashColor)) {
        rail_gun_fire_flash_hook.install();
    }

    // ===========================
    // af_game.tbl
    // ===========================
    if (g_dash_options_config.is_option_loaded(DashOptionID::UseStockPlayersConfig) &&
        std::get<bool>(g_dash_options_config.options[DashOptionID::UseStockPlayersConfig])) {
        AsmWriter(0x004A8F99).jmp(0x004A9010);
        AsmWriter(0x004A8DCC).jmp(0x004A8E53);
    }    

    if (g_dash_options_config.is_option_loaded(DashOptionID::GeomodEmitter_Default)) {
        default_geomod_emitter_get_index_hook.install();
    }

    if (g_dash_options_config.is_option_loaded(DashOptionID::GeomodEmitter_Driller)) {
        driller_geomod_emitter_get_index_hook.install();
    }

    if (g_dash_options_config.is_option_loaded(DashOptionID::GeomodTexture_Ice)) {
        ice_geo_crater_bm_load_hook.install();
    }

    if (g_dash_options_config.is_option_loaded(DashOptionID::FirstLevelFilename)) {
        first_load_level_hook.install();
    }

    if (g_dash_options_config.is_option_loaded(DashOptionID::TrainingLevelFilename)) {
        training_load_level_hook.install();
    }

    if (g_dash_options_config.is_option_loaded(DashOptionID::PlayerEntityType)) {
        static std::string new_entity_type = get_option_value<std::string>(DashOptionID::PlayerEntityType);
        AsmWriter(0x0046D687).push(new_entity_type.c_str()); // multi_start
        AsmWriter(0x004706A2).push(new_entity_type.c_str()); // multi_respawn_bot
        AsmWriter(0x00480860).push(new_entity_type.c_str()); // multi_spawn_player_server_side
        AsmWriter(0x004A33CF).push(new_entity_type.c_str()); // player_allocate
        AsmWriter(0x004B003F).push(new_entity_type.c_str()); // player_undercover_init
    }

    if (g_dash_options_config.is_option_loaded(DashOptionID::PlayerScientistEntityType)) {
        static std::string new_sci_entity_type = get_option_value<std::string>(DashOptionID::PlayerScientistEntityType);
        AsmWriter(0x004B0058).push(new_sci_entity_type.c_str()); // player_undercover_init
    }

    if (g_dash_options_config.is_option_loaded(DashOptionID::PlayerSuitEntityType)) {
        static std::string new_suit_entity_type = get_option_value<std::string>(DashOptionID::PlayerSuitEntityType);
        AsmWriter(0x004B0049).push(new_suit_entity_type.c_str()); // player_undercover_init
    }

    if (g_dash_options_config.is_option_loaded(DashOptionID::FallDamageSlamMultiplier)) {
        physics_calc_fall_damage_slam_hook.install();
    }

    if (g_dash_options_config.is_option_loaded(DashOptionID::FallDamageLandMultiplier)) {
        physics_calc_fall_damage_land_hook.install();
    }

    if (g_dash_options_config.is_option_loaded(DashOptionID::MultiplayerWalkSpeed)) {
        float multiplayer_walk_speed = get_option_or_default<float>(DashOptionID::MultiplayerWalkSpeed, 9.0f);
        rf::multiplayer_walk_speed = multiplayer_walk_speed;
    }

    if (g_dash_options_config.is_option_loaded(DashOptionID::MultiplayerCrouchWalkSpeed)) {
        float multiplayer_crouch_walk_speed = get_option_or_default<float>(DashOptionID::MultiplayerCrouchWalkSpeed, 7.0f);
        rf::multiplayer_crouch_walk_speed = multiplayer_crouch_walk_speed;
    }

    // ===========================
    // af_ui.tbl
    // ===========================
    if (g_dash_options_config.is_option_loaded(DashOptionID::DisableMultiplayerButton) &&
        std::get<bool>(g_dash_options_config.options[DashOptionID::DisableMultiplayerButton])) {
        AsmWriter(0x0044391F).nop(5); // Disable multiplayer button
    }

    if (g_dash_options_config.is_option_loaded(DashOptionID::DisableSingleplayerButtons) &&
        std::get<bool>(g_dash_options_config.options[DashOptionID::DisableSingleplayerButtons])) {
        AsmWriter(0x00443906).nop(5); // Disable save button
        AsmWriter(0x004438ED).nop(5); // Disable load button
        AsmWriter(0x004438D4).nop(5); // Disable new game button
    }

    if (g_dash_options_config.is_option_loaded(DashOptionID::SumTrailerButtonAction)) {
        handle_summoner_trailer_button();
    }

    xlog::warn("Alpine Faction Options patches applied successfully");

}


    // whether should apply is determined in helper function - TODO FIX
    //apply_geomod_mesh_patch();
    //geomod_shape_init_hook.install();
    //rf::geomod_shape_init();


void load_single_af_options_file(const std::string& file_name)
{
    auto dashoptions_file = std::make_unique<rf::File>();
    if (dashoptions_file->open(file_name.c_str()) != 0) {
        xlog::warn("Could not open {}", file_name);
        return;
    }
    xlog::warn("Successfully opened {}", file_name);

    // Read entire file content into a single string buffer
    std::string file_content;
    std::string buffer(2048, '\0');
    int bytes_read;

    while ((bytes_read = dashoptions_file->read(&buffer[0], buffer.size() - 1)) > 0) {
        buffer.resize(bytes_read); // Adjust buffer to actual bytes read
        file_content += buffer;
        buffer.resize(2048, '\0'); // Reset buffer size for next read
    }

    dashoptions_file->close();

    // Process file content line-by-line
    std::istringstream file_stream(file_content);
    std::string line;
    bool found_start = false;

    // Search for #Start marker
    while (std::getline(file_stream, line)) {
        line = trim(line, false);
        if (line == "#Start") {
            found_start = true;
            break;
        }
    }

    if (!found_start) {
        xlog::warn("No #Start marker found in {}", file_name);
        return;
    }

    // Process options until #End marker is found
    bool found_end = false;
    while (std::getline(file_stream, line)) {
        line = trim(line, false);

        if (line == "#End") {
            found_end = true;
            break;
        }
        if (line.empty() || line.starts_with("//")) {
            continue;
        }

        auto delimiter_pos = line.find(':');
        if (delimiter_pos == std::string::npos || (line[0] != '$' && line[0] != '+')) {
            continue;
        }

        std::string option_name = trim(line.substr(0, delimiter_pos), false);
        std::string option_value = trim(line.substr(delimiter_pos + 1), false);

        auto meta_it = option_metadata.find(option_name);

        // Allow any af_client*.tbl file for options designated to af_client.tbl
        bool is_af_client_variant = (meta_it->second.filename == "af_client.tbl" &&
                                     file_name.rfind("af_client", 0) == 0 && file_name.ends_with(".tbl"));

        if (meta_it != option_metadata.end() &&
            (meta_it->second.filename == file_name || is_af_client_variant) &&
            (!rf::is_dedicated_server || meta_it->second.apply_on_server)) {
            
            const auto& metadata = meta_it->second;
            auto parsed_value = metadata.parse_function(option_value);
            if (parsed_value) {
                g_dash_options_config.options[metadata.id] = *parsed_value;
                g_dash_options_config.options_loaded[static_cast<std::size_t>(metadata.id)] = true;
                xlog::warn("Parsed and applied option {}: {}", option_name, option_value);
                xlog::warn("Option ID {} marked as loaded", static_cast<std::size_t>(metadata.id));
            }
        }
        else if (meta_it != option_metadata.end()) {
            if (meta_it->second.filename != file_name && !is_af_client_variant) {
                xlog::warn("Option {} in {} skipped (wrong tbl file)", option_name, file_name);
            }
            else if (rf::is_dedicated_server && !meta_it->second.apply_on_server) {
                xlog::warn("Option {} in {} skipped (not needed on dedicated server)", option_name, file_name);
            }
            else {
                xlog::warn("Option {} in {} skipped (reason unknown)", option_name, file_name);
            }
        }
    }

    if (!found_end) {
        xlog::warn("No #End marker found in {}", file_name);
    }
}

void load_af_options_config()
{
    xlog::warn("Loading Alpine Faction Options configuration");

    // Load all af_client*.tbl files first
    // af_client.tbl overrides numbered variations because its loaded later
    for (int i = 0; i <= 9; ++i) {
        std::string numbered_file = "af_client" + std::to_string(i) + ".tbl";
        load_single_af_options_file(numbered_file);
    }

    // if a TC mod is loaded, handle the other options files
    if (rf::mod_param.found()) {
        // Collect unique file names from option_metadata
        std::unordered_set<std::string> config_files;
        for (const auto& [name, metadata] : option_metadata) {
            config_files.insert(metadata.filename);
        }

        // Now load the rest of the dashoptions files
        for (const auto& file_name : config_files) {
            load_single_af_options_file(file_name);
        }
    }    

    xlog::warn("Loaded options:");
    for (std::size_t i = 0; i < option_count; ++i) {
        if (g_dash_options_config.options_loaded[i]) {
            xlog::warn("Option {} is loaded", i);
        }
        else {
            xlog::warn("Option {} is NOT loaded", i);
        }
    }

    // Apply all specified patches based on the parsed configuration
    apply_af_options_patches();

    rf::console::print("Alpine Faction Options configuration loaded");
}
