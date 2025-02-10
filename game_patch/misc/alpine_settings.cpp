#include <patch_common/FunHook.h>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include "alpine_settings.h"
#include <common/version/version.h>
#include "../os/console.h"
#include "../rf/os/console.h"
#include "../rf/player/player.h"
#include "../rf/sound/sound.h"
#include "../rf/gr/gr.h"
#include <fstream>
#include <iostream>
#include <string>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <chrono>
#include <unordered_map>
#include <xlog/xlog.h>

bool g_loaded_alpine_settings_file = false;

std::string alpine_get_settings_filename()
{
    if (rf::mod_param.found()) {
        std::string mod_name = rf::mod_param.get_arg();
        return "alpine_settings_" + mod_name + ".ini";
    }
    return "alpine_settings.ini";
}

bool alpine_player_settings_load(rf::Player* player)
{
    std::string filename = alpine_get_settings_filename();
    std::ifstream file(filename);

    if (!file.is_open()) {
        xlog::info("Failed to read {}", filename);
        return false;
    }

    std::unordered_map<std::string, std::string> settings;
    std::string line;

    while (std::getline(file, line)) {
        if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;

        std::istringstream key_value(line);
        std::string key, value;

        if (std::getline(key_value, key, '=') && std::getline(key_value, value)) {
            settings[key] = value;
        }
    }

    file.close();

    // Load player settings
    if (settings.count("PlayerName")) {
        player->name = settings["PlayerName"].c_str();
    }
    if (settings.count("MultiplayerCharacter")) {
        player->settings.multi_character = std::stoi(settings["MultiplayerCharacter"]);
    }
    if (settings.count("GoreLevel")) {
        rf::game_set_gore_level(std::stoi(settings["GoreLevel"]));
    }
    if (settings.count("DifficultyLevel")) {
        rf::game_set_skill_level(static_cast<rf::GameDifficultyLevel>(std::stoi(settings["DifficultyLevel"])));
    }
    if (settings.count("ShowFPGun")) {
        player->settings.render_fpgun = std::stoi(settings["ShowFPGun"]);
    }
    if (settings.count("AutoswitchWeapons")) {
        player->settings.autoswitch_weapons = std::stoi(settings["AutoswitchWeapons"]);
    }
    if (settings.count("NeverAutoswitchExplosives")) {
        player->settings.dont_autoswitch_to_explosives = std::stoi(settings["NeverAutoswitchExplosives"]);
    }
    if (settings.count("ToggleCrouch")) {
        player->settings.toggle_crouch = std::stoi(settings["ToggleCrouch"]);
    }

    // Load weapon autoswitch priority
    if (settings.count("WeaponAutoswitchPriority")) {
        std::istringstream weapon_stream(settings["WeaponAutoswitchPriority"]);
        std::string weapon_id;
        int index = 0;

        while (std::getline(weapon_stream, weapon_id, ',') && index < 32) {
            player->weapon_prefs[index++] = std::stoi(weapon_id);
        }

        while (index < 32) {
            player->weapon_prefs[index++] = -1;
        }
    }

    // Load audio settings
    if (settings.count("EffectsVolume")) {
        rf::snd_set_group_volume(0, std::stof(settings["EffectsVolume"]));
    }
    if (settings.count("MusicVolume")) {
        rf::snd_set_group_volume(1, std::stof(settings["MusicVolume"]));
    }
    if (settings.count("MessagesVolume")) {
        rf::snd_set_group_volume(2, std::stof(settings["MessagesVolume"]));
    }

    // Load video settings
    if (settings.count("Gamma")) {
        rf::gr::set_gamma(std::stof(settings["Gamma"]));
    }
    if (settings.count("ShowShadows")) {
        player->settings.shadows_enabled = std::stoi(settings["ShowShadows"]);
    }
    if (settings.count("ShowDecals")) {
        player->settings.decals_enabled = std::stoi(settings["ShowDecals"]);
    }
    if (settings.count("ShowDynamicLights")) {
        player->settings.dynamic_lightining_enabled = std::stoi(settings["ShowDynamicLights"]);
    }
    if (settings.count("BilinearFiltering")) {
        player->settings.bilinear_filtering = std::stoi(settings["BilinearFiltering"]);
    }
    if (settings.count("DetailLevel")) {
        player->settings.detail_level = std::stoi(settings["DetailLevel"]);
    }
    if (settings.count("CharacterDetailLevel")) {
        player->settings.character_detail_level = std::stoi(settings["CharacterDetailLevel"]);
    }
    if (settings.count("TextureDetailLevel")) {
        player->settings.textures_resolution_level = std::stoi(settings["TextureDetailLevel"]);
    }

    // Load input settings
    if (settings.count("MouseSensitivity")) {
        player->settings.controls.mouse_sensitivity = std::stof(settings["MouseSensitivity"]);
    }

    if (settings.count("MouseYInvert")) {
        player->settings.controls.axes[1].invert = std::stoi(settings["MouseYInvert"]);
    }

    // Load binds
    for (const auto& [key, value] : settings) {
        if (key.rfind("Bind:", 0) == 0) {
            std::string action_name = key.substr(5);
            std::istringstream bind_values(value);
            std::string id_str, scan1, scan2, mouse_btn;

            if (std::getline(bind_values, id_str, ',') &&
                std::getline(bind_values, scan1, ',') &&
                std::getline(bind_values, scan2, ',') &&
                std::getline(bind_values, mouse_btn, ',')) {

                int bind_id = std::stoi(id_str);
                if (bind_id >= 0 && bind_id < player->settings.controls.num_bindings) {
                    player->settings.controls.bindings[bind_id].name = action_name.c_str();
                    player->settings.controls.bindings[bind_id].scan_codes[0] = scan1.empty() ? -1 : std::stoi(scan1);
                    player->settings.controls.bindings[bind_id].scan_codes[1] = scan2.empty() ? -1 : std::stoi(scan2);
                    player->settings.controls.bindings[bind_id].mouse_btn_id = mouse_btn.empty() ? -1 : std::stoi(mouse_btn);

                    xlog::info("Loaded Bind: {} = {}, {}, {}, {}", action_name, bind_id, scan1, scan2, mouse_btn);
                }
                else {
                    xlog::warn("Invalid Bind ID {} for action {} found in config file!", bind_id, action_name);
                }
            }
            else {
                xlog::warn("Malformed Bind entry: {} found in config file!", value);
            }
        }
    }

    rf::console::printf("Successfully loaded settings from %s", filename);
    g_loaded_alpine_settings_file = true;
    return true;
}

void alpine_control_config_serialize(std::ofstream& file, const rf::ControlConfig& cc)
{
    file << "\n[InputSettings]\n";
    file << "MouseSensitivity=" << cc.mouse_sensitivity << "\n";
    file << "MouseYInvert=" << cc.axes[1].invert << "\n";

    file << "\n[ActionBinds]\n";
    file << "; Format is Bind:{Name}={ID},{ScanCode0},{ScanCode1},{MouseButtonID}\n";

    // Key bind format: Bind:ActionName=ID,PrimaryScanCode,SecondaryScanCode,MouseButtonID
    for (int i = 0; i < cc.num_bindings; ++i) {
        xlog::info("Saving Bind: {} = {}, {}, {}, {}", 
                   cc.bindings[i].name, 
                   i, 
                   cc.bindings[i].scan_codes[0], 
                   cc.bindings[i].scan_codes[1], 
                   cc.bindings[i].mouse_btn_id);

        file << "Bind:" << cc.bindings[i].name << "=" 
             << i << "," 
             << cc.bindings[i].scan_codes[0] << "," 
             << cc.bindings[i].scan_codes[1] << ","
             << cc.bindings[i].mouse_btn_id << "\n";
    }
}

void alpine_player_settings_save(rf::Player* player)
{
    std::string filename = alpine_get_settings_filename();
    std::ofstream file(filename);

    if (!file.is_open()) {
        xlog::info("Failed to write {}", filename);
        return;
    }

    std::time_t current_time = std::time(nullptr);
    std::tm* now_tm = std::localtime(&current_time);

    file << "; Alpine Faction Settings File";
    if (rf::mod_param.found()) {
        std::string mod_name = rf::mod_param.get_arg();
        file << " for mod " << mod_name;
    }
    file << "\n\n; This file is automatically generated by Alpine Faction.\n";
    file << "; You can edit it manually, but make sure you really know what you are doing.\n";
    file << "; Editing this file while the game is running is NOT recommended.\n\n";
    file << "; Last saved " << std::put_time(now_tm, "%Y-%m-%d %H:%M:%S");
    file << " by " << PRODUCT_NAME_VERSION << " (" << VERSION_CODE << ")\n\n";

    // Player
    file << "\n[PlayerSettings]\n";
    file << "PlayerName=" << player->name << "\n";
    file << "MultiplayerCharacter=" << player->settings.multi_character << "\n";
    file << "GoreLevel=" << rf::game_get_gore_level() << "\n";
    file << "DifficultyLevel=" << static_cast<int>(rf::game_get_skill_level()) << "\n";
    file << "ShowFPGun=" << player->settings.render_fpgun << "\n";
    file << "AutoswitchWeapons=" << player->settings.autoswitch_weapons << "\n";
    file << "NeverAutoswitchExplosives=" << player->settings.dont_autoswitch_to_explosives << "\n";
    file << "ToggleCrouch=" << player->settings.toggle_crouch << "\n";

    // Autoswitch priority
    file << "WeaponAutoswitchPriority=";
    bool first = true;
    for (int i = 0; i < 32; ++i) {
        int weapon_id = player->weapon_prefs[i];
        if (weapon_id > -1) { // Only save valid weapons
            if (!first) {
                file << ",";
            }
            file << weapon_id;
            first = false;
        }
    }
    file << "\n";

    // Audio
    file << "\n[AudioSettings]\n";
    file << "EffectsVolume=" << rf::snd_get_group_volume(0) << "\n";
    file << "MusicVolume=" << rf::snd_get_group_volume(1) << "\n";
    file << "MessagesVolume=" << rf::snd_get_group_volume(2) << "\n";

    // Video
    file << "\n[VideoSettings]\n";
    file << "Gamma=" << rf::gr::gamma << "\n";
    file << "ShowShadows=" << player->settings.shadows_enabled << "\n";
    file << "ShowDecals=" << player->settings.decals_enabled << "\n";
    file << "ShowDynamicLights=" << player->settings.dynamic_lightining_enabled << "\n";
    file << "BilinearFiltering=" << player->settings.bilinear_filtering << "\n";
    file << "DetailLevel=" << player->settings.detail_level << "\n";
    file << "CharacterDetailLevel=" << player->settings.character_detail_level << "\n";
    file << "TextureDetailLevel=" << player->settings.textures_resolution_level << "\n";
    
    alpine_control_config_serialize(file, player->settings.controls);

    file.close();
    xlog::info("Saved settings to {}", filename);
    return;
}

CallHook<void(rf::Player*)> player_settings_load_hook{
    0x004B2726,
    [](rf::Player* player) {
        if (!alpine_player_settings_load(player)) {
            rf::console::print("Alpine Faction settings file could not be loaded. Loading legacy Red Faction settings file...");
            player_settings_load_hook.call_target(player); // players.cfg
        }
    }
};

CallHook<void(rf::Player*)> player_settings_save_hook{
    {
        0x004B2D77,
        0x0044F1AF
    },
    [](rf::Player* player) {
        alpine_player_settings_save(player);
        //player_settings_save_hook.call_target(player); // players.cfg
    }
};

ConsoleCommand2 load_settings_cmd{
    "dbg_loadsettings",
    []() {
        alpine_player_settings_load(rf::local_player);
        rf::console::print("Loading settings file...");
    },
    "Force the game to read and apply settings from config file",
};

ConsoleCommand2 save_settings_cmd{
    "dbg_savesettings",
    []() {
        alpine_player_settings_save(rf::local_player);
        rf::console::print("Saving settings file...");
    },
    "Force a write of current settings to config file",
};

void alpine_settings_apply_patch()
{
    player_settings_load_hook.install();
    player_settings_save_hook.install();

    // commands
    load_settings_cmd.register_cmd();
    save_settings_cmd.register_cmd();
}
