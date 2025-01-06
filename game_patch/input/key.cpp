#include <cctype>
#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include <xlog/xlog.h>
#include "../multi/multi.h"
#include "../rf/input.h"
#include "../rf/entity.h"
#include "../rf/multi.h"
#include "../rf/player/control_config.h"
#include "../rf/player/player.h"
#include "../rf/os/console.h"
#include "../rf/os/os.h"

static int starting_alpine_control_index = -1;

FunHook<int(int16_t)> key_to_ascii_hook{
    0x0051EFC0,
    [](int16_t key) {
        using namespace rf;
        constexpr int empty_result = 0xFF;
        if (!key) {
            return empty_result;
        }
        // special handling for Num Lock (because ToAscii API does not support it)
        switch (key & KEY_MASK) {
            // Numpad keys that always work
            case KEY_PADMULTIPLY: return static_cast<int>('*');
            case KEY_PADMINUS: return static_cast<int>('-');
            case KEY_PADPLUS: return static_cast<int>('+');
            // Disable Numpad Enter key because game is not prepared for getting new line character from this function
            case KEY_PADENTER: return empty_result;
        }
        if (GetKeyState(VK_NUMLOCK) & 1) {
            switch (key & KEY_MASK) {
                case KEY_PAD7: return static_cast<int>('7');
                case KEY_PAD8: return static_cast<int>('8');
                case KEY_PAD9: return static_cast<int>('9');
                case KEY_PAD4: return static_cast<int>('4');
                case KEY_PAD5: return static_cast<int>('5');
                case KEY_PAD6: return static_cast<int>('6');
                case KEY_PAD1: return static_cast<int>('1');
                case KEY_PAD2: return static_cast<int>('2');
                case KEY_PAD3: return static_cast<int>('3');
                case KEY_PAD0: return static_cast<int>('0');
                case KEY_PADPERIOD: return static_cast<int>('.');
            }
        }
        BYTE key_state[256] = {0};
        if (key & KEY_SHIFTED) {
            key_state[VK_SHIFT] = 0x80;
        }
        if (key & KEY_ALTED) {
            key_state[VK_MENU] = 0x80;
        }
        if (key & KEY_CTRLED) {
            key_state[VK_CONTROL] = 0x80;
        }
        int scan_code = key & 0x7F;
        auto vk = MapVirtualKeyA(scan_code, MAPVK_VSC_TO_VK);
        WCHAR unicode_chars[3];
        auto num_unicode_chars = ToUnicode(vk, scan_code, key_state, unicode_chars, std::size(unicode_chars), 0);
        if (num_unicode_chars < 1) {
            return empty_result;
        }
        char ansi_char;
#if 0 // Windows-1252 codepage support - disabled because callers of this function expects ASCII
        int num_ansi_chars = WideCharToMultiByte(1252, 0, unicode_chars, num_unicode_chars,
            &ansi_char, sizeof(ansi_char), nullptr, nullptr);
        if (num_ansi_chars == 0) {
            return empty_result;
        }
#else
        if (static_cast<char16_t>(unicode_chars[0]) >= 0x80 || !std::isprint(unicode_chars[0])) {
            return empty_result;
        }
        ansi_char = static_cast<char>(unicode_chars[0]);
#endif
        xlog::trace("vk {:x} ({}) char {}", vk, vk, ansi_char);
        return static_cast<int>(ansi_char);
    },
};

int get_key_name(int key, char* buf, size_t buf_len)
{
     LONG lparam = (key & 0x7F) << 16;
    if (key & 0x80) {
        lparam |= 1 << 24;
    }
    // Note: it seems broken on Wine with non-US layout (most likely broken MAPVK_VSC_TO_VK_EX mapping is responsible)
    int ret = GetKeyNameTextA(lparam, buf, buf_len);
    if (ret <= 0) {
        WARN_ONCE("GetKeyNameTextA failed for 0x{:X}", key);
        buf[0] = '\0';
    }
    else {
        xlog::trace("key 0x{:x} name {}", key, buf);
    }
    return ret;
}

FunHook<int(rf::String&, int)> get_key_name_hook{
    0x0043D930,
    [](rf::String& out_name, int key) {
        static char buf[32] = "";
        int result = 0;
        if (key < 0 || get_key_name(key, buf, std::size(buf)) <= 0) {
            result = -1;
        }
        out_name = buf;
        return result;
    },
};

CodeInjection key_name_in_options_patch{
    0x00450328,
    [](auto& regs) {
        static char buf[32];
        int key = regs.edx;
        get_key_name(key, buf, std::size(buf));
        regs.edi = buf;
        regs.eip = 0x0045032F;
    },
};

CodeInjection key_get_hook{
    0x0051F000,
    []() {
        // Process messages here because when watching videos main loop is not running
        rf::os_poll();
    },
};

void alpine_control_config_add_item(rf::ControlConfig* config, const char* name, bool is_repeat,
    int16_t key1, int16_t key2, int16_t mouse_button, rf::AlpineControlConfigAction alpine_control)
{
    if (config->num_bindings >= 128) {
        return; // hard upper limit
    }

    int binding_index = starting_alpine_control_index + static_cast<int>(alpine_control);

    // Reference the current binding for clarity
    auto& binding = config->bindings[binding_index];

    // Set initial binding values
    binding.scan_codes[0] = key1;
    binding.scan_codes[1] = key2;
    binding.mouse_btn_id = mouse_button;
    binding.is_repeat = is_repeat;
    binding.name = name;

    // set "Factory Default" binding values
    binding.default_scan_codes[0] = key1;
    binding.default_scan_codes[1] = key2;
    binding.default_mouse_btn_id = mouse_button;    

    // Increment num_bindings (for control indices)
    config->num_bindings++;

    //xlog::warn("added {}, {}, {}", binding.name.c_str(), binding_index, config->bindings[binding_index].name);

    return;
}

// add new controls after all stock ones
CodeInjection control_config_init_patch{
    0x0043D329,
    [](auto& regs) {

        rf::ControlConfig* ccp = regs.esi;

        // set the starting index for Alpine controls
        // needed because some mods have a different number of weapons, so we can't hardcode the indices
        // overall limit on number of controls (stock + weapons + alpine) is 128
        if (starting_alpine_control_index == -1) {
            starting_alpine_control_index = ccp->num_bindings;
        }        

        alpine_control_config_add_item(
            ccp, "(AF) Toggle headlamp", 0, 0x21, -1, -1, rf::AlpineControlConfigAction::AF_ACTION_FLASHLIGHT);
        alpine_control_config_add_item(
            ccp, "(AF) Skip cutscene", 0, 0x25, -1, -1, rf::AlpineControlConfigAction::AF_ACTION_SKIP_CUTSCENE);
        alpine_control_config_add_item(
            ccp, "(AF) Kill yourself", 0, -1, -1, -1, rf::AlpineControlConfigAction::AF_ACTION_SELF_KILL);
        alpine_control_config_add_item(
            ccp, "(AF) Vote yes", 0, 0x3B, -1, -1, rf::AlpineControlConfigAction::AF_ACTION_VOTE_YES);
        alpine_control_config_add_item(
            ccp, "(AF) Vote no", 0, 0x3C, -1, -1, rf::AlpineControlConfigAction::AF_ACTION_VOTE_NO);
        alpine_control_config_add_item(
            ccp, "(AF) Ready for match", 0, 0x3D, -1, -1, rf::AlpineControlConfigAction::AF_ACTION_READY);
    },
};

// define actions for alpine controls
CodeInjection player_execute_action_patch{
    0x004A6283,
    [](auto& regs) {

        rf::ControlConfigAction action = regs.ebp;
        int action_index = static_cast<int>(action);
        //xlog::warn("check execute action {}", action_index);

        // only intercept alpine controls
        if (action_index >= starting_alpine_control_index) {
            if (action_index == starting_alpine_control_index +
                static_cast<int>(rf::AlpineControlConfigAction::AF_ACTION_FLASHLIGHT) &&
                !rf::is_multi) {
                (rf::entity_headlamp_is_on(rf::local_player_entity))
                    ? rf::entity_headlamp_turn_off(rf::local_player_entity)
                    : rf::entity_headlamp_turn_on(rf::local_player_entity);
            }
            else if (action_index == starting_alpine_control_index +
                static_cast<int>(rf::AlpineControlConfigAction::AF_ACTION_SKIP_CUTSCENE)) {
                
            }
            else if (action_index == starting_alpine_control_index +
                static_cast<int>(rf::AlpineControlConfigAction::AF_ACTION_SELF_KILL)) {
                rf::player_kill_self(rf::local_player);
            }
            else if (action_index == starting_alpine_control_index +
                static_cast<int>(rf::AlpineControlConfigAction::AF_ACTION_VOTE_YES) &&
                rf::is_multi && !rf::is_server) {
                send_chat_line_packet("/vote yes", nullptr);
            }
            else if (action_index == starting_alpine_control_index +
                static_cast<int>(rf::AlpineControlConfigAction::AF_ACTION_VOTE_NO) &&
                rf::is_multi && !rf::is_server) {
                send_chat_line_packet("/vote no", nullptr);
            }
            else if (action_index == starting_alpine_control_index +
                static_cast<int>(rf::AlpineControlConfigAction::AF_ACTION_READY) &&
                rf::is_multi && !rf::is_server) {
                send_chat_line_packet("/ready", nullptr);
            }
        }
    },
};

// allow processing alpine controls
CodeInjection controls_process_patch{
    0x00430E4C,
    [](auto& regs) {
        int index = regs.edi;

        // C++ doesn't have a way to dynamically get the last enum index, so just update this when adding new controls
        if (index >= starting_alpine_control_index &&
            index <= static_cast<int>(rf::AlpineControlConfigAction::AF_ACTION_SELF_KILL)) {
            //xlog::warn("passing control {}", index);
            regs.eip = 0x00430E24;
        }
    },
};

void key_apply_patch()
{
    // Handle Alpine controls
    control_config_init_patch.install();
    player_execute_action_patch.install();
    controls_process_patch.install();

    // Support non-US keyboard layouts
    key_to_ascii_hook.install();
    get_key_name_hook.install();
    key_name_in_options_patch.install();

    // Disable broken numlock handling
    write_mem<int8_t>(0x004B14B2 + 1, 0);

    // win32 console support and addition of os_poll
    key_get_hook.install();
}
