#include <cassert>
#include <algorithm>
#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include <xlog/xlog.h>
#include "../os/console.h"
#include "../rf/input.h"
#include "../rf/os/os.h"
#include "../rf/gr/gr.h"
#include "../rf/multi.h"
#include "../rf/player/player.h"
#include "../rf/entity.h"
#include "../misc/alpine_settings.h"
#include "../main/main.h"
#include "input.h"

// Raw Input support
#ifndef HID_USAGE_PAGE_GENERIC
#define HID_USAGE_PAGE_GENERIC          ((USHORT) 0x01)
#endif
#ifndef HID_USAGE_GENERIC_MOUSE
#define HID_USAGE_GENERIC_MOUSE         ((USHORT) 0x02)
#endif

static float scope_sensitivity_value = 0.25f;
static float scanner_sensitivity_value = 0.25f;
static float applied_static_sensitivity_value = 0.25f; // value written by AsmWriter
static float applied_dynamic_sensitivity_value = 1.0f; // value written by AsmWriter

// Raw Input state
static MouseInputMode g_mouse_input_mode = MOUSE_INPUT_WIN32;
static bool g_raw_input_registered = false;
static int g_raw_mouse_delta_x = 0;
static int g_raw_mouse_delta_y = 0;
static bool g_raw_input_available = false;

bool set_direct_input_enabled(bool enabled)
{
    auto direct_input_initialized = addr_as_ref<bool>(0x01885460);
    auto mouse_di_init = addr_as_ref<int()>(0x0051E070);
    rf::direct_input_disabled = !enabled;
    if (enabled && !direct_input_initialized) {
        if (mouse_di_init() != 0) {
            xlog::error("Failed to initialize DirectInput");
            rf::direct_input_disabled = true;
            return false;
        }
    }
    if (direct_input_initialized) {
        if (rf::direct_input_disabled)
            rf::di_mouse->Unacquire();
        else
            rf::di_mouse->Acquire();
    }
    return true;
}

void enumerate_raw_input_devices()
{
    UINT numDevices = 0;
    
    // Get number of devices
    if (GetRawInputDeviceList(nullptr, &numDevices, sizeof(RAWINPUTDEVICELIST)) != 0) {
        xlog::warn("Failed to get Raw Input device count: {}", GetLastError());
        return;
    }
    
    if (numDevices == 0) {
        xlog::info("No Raw Input devices found");
        return;
    }
    
    // Get device list
    auto deviceList = std::make_unique<RAWINPUTDEVICELIST[]>(numDevices);
    if (GetRawInputDeviceList(deviceList.get(), &numDevices, sizeof(RAWINPUTDEVICELIST)) == static_cast<UINT>(-1)) {
        xlog::warn("Failed to enumerate Raw Input devices: {}", GetLastError());
        return;
    }
    
    xlog::info("Found {} Raw Input device(s):", numDevices);
    
    int mouseCount = 0;
    for (UINT i = 0; i < numDevices; i++) {
        if (deviceList[i].dwType == RIM_TYPEMOUSE) {
            mouseCount++;
            
            // Get device name
            UINT nameSize = 0;
            if (GetRawInputDeviceInfoA(deviceList[i].hDevice, RIDI_DEVICENAME, nullptr, &nameSize) != 0) {
                continue;
            }
            
            auto deviceName = std::make_unique<char[]>(nameSize + 1);
            if (GetRawInputDeviceInfoA(deviceList[i].hDevice, RIDI_DEVICENAME, deviceName.get(), &nameSize) == static_cast<UINT>(-1)) {
                continue;
            }
            
            // Get device info
            UINT infoSize = sizeof(RID_DEVICE_INFO);
            RID_DEVICE_INFO deviceInfo;
            deviceInfo.cbSize = sizeof(RID_DEVICE_INFO);
            if (GetRawInputDeviceInfoA(deviceList[i].hDevice, RIDI_DEVICEINFO, &deviceInfo, &infoSize) == static_cast<UINT>(-1)) {
                xlog::info("  Mouse #{}: {} (info unavailable)", mouseCount, deviceName.get());
                continue;
            }
            
            if (deviceInfo.dwType == RIM_TYPEMOUSE) {
                xlog::info("  Mouse #{}: {} (ID: {}, {} buttons, sample rate: {}, horizontal wheel: {})", 
                          mouseCount, 
                          deviceName.get(),
                          deviceInfo.mouse.dwId,
                          deviceInfo.mouse.dwNumberOfButtons,
                          deviceInfo.mouse.dwSampleRate,
                          deviceInfo.mouse.fHasHorizontalWheel ? "yes" : "no");
            }
        }
    }
    
    if (mouseCount == 0) {
        xlog::warn("No mouse devices found in Raw Input device list");
    } else {
        xlog::info("Total mouse devices available for Raw Input: {}", mouseCount);
    }
}

bool register_raw_input()
{
    if (g_raw_input_registered) {
        return true;
    }
    
    // First, enumerate available devices
    enumerate_raw_input_devices();
    
    RAWINPUTDEVICE rid;
    rid.usUsagePage = HID_USAGE_PAGE_GENERIC;
    rid.usUsage = HID_USAGE_GENERIC_MOUSE;
    rid.dwFlags = RIDEV_INPUTSINK; // Receive input even when not in foreground
    rid.hwndTarget = rf::main_wnd;
    
    if (RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        g_raw_input_registered = true;
        xlog::info("Raw Input registered successfully for all mouse devices (Usage Page: 0x{:02X}, Usage: 0x{:02X})", 
                  HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_MOUSE);
        return true;
    } else {
        xlog::error("Failed to register Raw Input: {}", GetLastError());
        return false;
    }
}

void unregister_raw_input()
{
    if (!g_raw_input_registered) {
        return;
    }
    
    RAWINPUTDEVICE rid;
    rid.usUsagePage = HID_USAGE_PAGE_GENERIC;
    rid.usUsage = HID_USAGE_GENERIC_MOUSE;
    rid.dwFlags = RIDEV_REMOVE;
    rid.hwndTarget = nullptr;
    
    if (RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        g_raw_input_registered = false;
        xlog::info("Raw Input unregistered successfully");
    } else {
        xlog::error("Failed to unregister Raw Input: {}", GetLastError());
    }
}

void process_raw_input_message(UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message != WM_INPUT || g_mouse_input_mode != MOUSE_INPUT_RAW) {
        return;
    }
    
    // Only process mouse input when the game is in the foreground and mouse is centered
    if (!rf::os_foreground() || !rf::keep_mouse_centered) {
        return;
    }
    
    UINT dwSize = 0;
    GetRawInputData((HRAWINPUT)lParam, RID_INPUT, nullptr, &dwSize, sizeof(RAWINPUTHEADER));
    
    if (dwSize == 0) {
        return;
    }
    
    auto lpb = std::make_unique<BYTE[]>(dwSize);
    if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, lpb.get(), &dwSize, sizeof(RAWINPUTHEADER)) != dwSize) {
        xlog::error("GetRawInputData does not return correct size");
        return;
    }
    
    RAWINPUT* raw = (RAWINPUT*)lpb.get();
    if (raw->header.dwType == RIM_TYPEMOUSE) {
        if (raw->data.mouse.usFlags == MOUSE_MOVE_RELATIVE) {
            g_raw_mouse_delta_x += raw->data.mouse.lLastX;
            g_raw_mouse_delta_y += raw->data.mouse.lLastY;
        }
    }
}

bool set_mouse_input_mode(MouseInputMode mode)
{
    if (mode == g_mouse_input_mode) {
        return true;
    }
    
    // Clean up current mode
    switch (g_mouse_input_mode) {
        case MOUSE_INPUT_DIRECTINPUT:
            set_direct_input_enabled(false);
            break;
        case MOUSE_INPUT_RAW:
            unregister_raw_input();
            break;
        case MOUSE_INPUT_WIN32:
        default:
            break;
    }
    
    // Set up new mode
    bool success = true;
    switch (mode) {
        case MOUSE_INPUT_DIRECTINPUT:
            success = set_direct_input_enabled(true);
            break;
        case MOUSE_INPUT_RAW:
            success = register_raw_input();
            if (success) {
                g_raw_input_available = true;
            }
            break;
        case MOUSE_INPUT_WIN32:
        default:
            // Win32 mode doesn't need setup
            break;
    }
    
    if (success) {
        g_mouse_input_mode = mode;
        // Update configuration settings
        g_alpine_game_config.mouse_input_mode = static_cast<int>(mode);
        g_alpine_game_config.direct_input = (mode == MOUSE_INPUT_DIRECTINPUT); // Legacy compatibility
    }
    
    return success;
}

const char* get_mouse_input_mode_name(MouseInputMode mode)
{
    switch (mode) {
        case MOUSE_INPUT_WIN32: return "Win32";
        case MOUSE_INPUT_DIRECTINPUT: return "DirectInput";
        case MOUSE_INPUT_RAW: return "Raw Input";
        default: return "Unknown";
    }
}

MouseInputMode get_current_mouse_input_mode()
{
    return g_mouse_input_mode;
}

// Hook for Raw Input mouse delta retrieval
FunHook<void(int& dx, int& dy, int& dz)> mouse_get_delta_raw_hook{
    0x0051E630, // Original mouse_get_delta function
    [](int& dx, int& dy, int& dz) {
        if (g_mouse_input_mode == MOUSE_INPUT_RAW && rf::keep_mouse_centered) {
            dx = g_raw_mouse_delta_x;
            dy = g_raw_mouse_delta_y;
            dz = 0; // Raw Input doesn't provide scroll wheel in this context
            
            // Reset deltas after reading
            g_raw_mouse_delta_x = 0;
            g_raw_mouse_delta_y = 0;
        } else {
            // Fall back to original implementation
            mouse_get_delta_raw_hook.call_target(dx, dy, dz);
        }
    },
};

FunHook<void()> mouse_eval_deltas_hook{
    0x0051DC70,
    []() {
        // disable mouse when window is not active
        if (rf::os_foreground()) {
            mouse_eval_deltas_hook.call_target();
        }
    },
};

// Enhanced DirectInput hook that handles Raw Input centering
FunHook<void()> mouse_eval_deltas_di_hook{
    0x0051DEB0,
    []() {
        if (g_mouse_input_mode == MOUSE_INPUT_DIRECTINPUT) {
            mouse_eval_deltas_di_hook.call_target();
        }
        // For Raw Input, we don't call the DirectInput function

        // center cursor if in game (for all modes when mouse is centered)
        if (rf::keep_mouse_centered) {
            POINT pt{rf::gr::screen_width() / 2, rf::gr::screen_height() / 2};
            ClientToScreen(rf::main_wnd, &pt);
            SetCursorPos(pt.x, pt.y);
        }
    },
};

FunHook<void()> mouse_keep_centered_enable_hook{
    0x0051E690,
    []() {
        if (!rf::keep_mouse_centered && !rf::is_dedicated_server) {
            // Initialize the appropriate input mode
            if (g_mouse_input_mode == MOUSE_INPUT_DIRECTINPUT) {
                set_direct_input_enabled(g_alpine_game_config.direct_input);
            } else if (g_mouse_input_mode == MOUSE_INPUT_RAW) {
                register_raw_input();
            }
        }
        mouse_keep_centered_enable_hook.call_target();
    },
};

FunHook<void()> mouse_keep_centered_disable_hook{
    0x0051E6A0,
    []() {
        if (rf::keep_mouse_centered) {
            if (g_mouse_input_mode == MOUSE_INPUT_DIRECTINPUT) {
                set_direct_input_enabled(false);
            }
            // Raw Input stays registered even when mouse isn't centered
        }
        mouse_keep_centered_disable_hook.call_target();
    },
};

ConsoleCommand2 input_mode_cmd{
    "inputmode",
    [](std::optional<int> mode_opt) {
        if (mode_opt) {
            int requested_mode = mode_opt.value();
            if (requested_mode < 0 || requested_mode > 2) {
                rf::console::print("Invalid input mode. Valid modes: 0 (Win32), 1 (DirectInput), 2 (Raw Input)");
                return;
            }
            
            MouseInputMode new_mode = static_cast<MouseInputMode>(requested_mode);
            if (set_mouse_input_mode(new_mode)) {
                rf::console::print("Input mode set to: {} ({})", requested_mode, get_mouse_input_mode_name(new_mode));
            } else {
                rf::console::print("Failed to set input mode to: {} ({})", requested_mode, get_mouse_input_mode_name(new_mode));
            }
        } else {
            // Cycle through modes (legacy behavior)
            MouseInputMode next_mode;
            switch (g_mouse_input_mode) {
                case MOUSE_INPUT_WIN32:
                    next_mode = MOUSE_INPUT_DIRECTINPUT;
                    break;
                case MOUSE_INPUT_DIRECTINPUT:
                    next_mode = MOUSE_INPUT_RAW;
                    break;
                case MOUSE_INPUT_RAW:
                default:
                    next_mode = MOUSE_INPUT_WIN32;
                    break;
            }
            
            if (set_mouse_input_mode(next_mode)) {
                rf::console::print("Input mode: {} ({})", static_cast<int>(next_mode), get_mouse_input_mode_name(next_mode));
            } else {
                rf::console::print("Failed to change input mode");
            }
        }
    },
    "Cycles input modes or sets specific mode (0=Win32, 1=DirectInput, 2=Raw Input)",
    "inputmode [mode]",
};

ConsoleCommand2 ms_cmd{
    "ms",
    [](std::optional<float> value_opt) {
        if (value_opt) {
            float value = value_opt.value();
            value = std::clamp(value, 0.0f, 1.0f);
            rf::local_player->settings.controls.mouse_sensitivity = value;
        }
        rf::console::print("Mouse sensitivity: {:.4f}", rf::local_player->settings.controls.mouse_sensitivity);
    },
    "Sets mouse sensitivity",
    "ms <value>",
};

void update_scope_sensitivity()
{
    scope_sensitivity_value = g_alpine_game_config.scope_sensitivity_modifier;

    applied_dynamic_sensitivity_value =
        (1 / (4 * g_alpine_game_config.scope_sensitivity_modifier)) * rf::scope_sensitivity_constant;
}

void update_scanner_sensitivity()
{
    scanner_sensitivity_value = g_alpine_game_config.scanner_sensitivity_modifier;
}

ConsoleCommand2 static_scope_sens_cmd{
    "cl_staticscopesens",
    []() {
        g_alpine_game_config.scope_static_sensitivity = !g_alpine_game_config.scope_static_sensitivity;
        rf::console::print("Scope sensitivity is {}", g_alpine_game_config.scope_static_sensitivity ? "static" : "dynamic");
    },
    "Toggle whether scope mouse sensitivity is static or dynamic (based on zoom level)."
};

ConsoleCommand2 scope_sens_cmd{
    "cl_scopesens",
    [](std::optional<float> value_opt) {
        if (value_opt) {
            g_alpine_game_config.set_scope_sens_mod(value_opt.value());
            update_scope_sensitivity();
        }
        else {
            rf::console::print("Scope sensitivity modifier: {:.2f}", g_alpine_game_config.scope_sensitivity_modifier);
        }
    },
    "Sets mouse sensitivity modifier used while in a scope.",
    "cl_scopesens <value> (valid range: 0.0 - 10.0)",
};

ConsoleCommand2 scanner_sens_cmd{
    "cl_scannersens",
    [](std::optional<float> value_opt) {
        if (value_opt) {
            g_alpine_game_config.set_scanner_sens_mod(value_opt.value());
            update_scanner_sensitivity();
        }
        else {
            rf::console::print("Scanner sensitivity modifier: {:.2f}", g_alpine_game_config.scanner_sensitivity_modifier);
        }
    },
    "Sets mouse sensitivity modifier used while in a scanner.",
    "cl_scannersens <value> (valid range: 0.0 - 10.0)",
};

CodeInjection static_zoom_sensitivity_patch {
    0x004309A2,
    [](auto& regs) {
        if (g_alpine_game_config.scope_static_sensitivity) {
            regs.eip = 0x004309D0; // use static sens calculation method for scopes (same as scanner and unscoped)
        }
    },
};

CodeInjection static_zoom_sensitivity_patch2 {
    0x004309D6,
    [](auto& regs) {
        rf::Player* player = regs.edi;

        if (player && rf::player_fpgun_is_zoomed(player)) {
            applied_static_sensitivity_value = scope_sensitivity_value;
            if (g_alpine_game_config.scope_static_sensitivity) {
                regs.al = static_cast<int8_t>(1); // make cmp at 0x004309DA test true
            }
        }
        else {
            applied_static_sensitivity_value = scanner_sensitivity_value;
        }
    },
};

rf::Vector3 fw_vector_from_non_linear_yaw_pitch(float yaw, float pitch)
{
    // Based on RF code
    rf::Vector3 fvec0;
    fvec0.y = std::sin(pitch);
    float factor = 1.0f - std::abs(fvec0.y);
    fvec0.x = factor * std::sin(yaw);
    fvec0.z = factor * std::cos(yaw);

    rf::Vector3 fvec = fvec0;
    fvec.normalize(); // vector is never zero

    return fvec;
}

float linear_pitch_from_forward_vector(const rf::Vector3& fvec)
{
    return std::asin(fvec.y);
}

rf::Vector3 fw_vector_from_linear_yaw_pitch(float yaw, float pitch)
{
    rf::Vector3 fvec;
    fvec.y = std::sin(pitch);
    fvec.x = std::cos(pitch) * std::sin(yaw);
    fvec.z = std::cos(pitch) * std::cos(yaw);
    fvec.normalize();
    return fvec;
}

float non_linear_pitch_from_fw_vector(rf::Vector3 fvec)
{
    float yaw = std::atan2(fvec.x, fvec.z);
    assert(!std::isnan(yaw));
    float fvec_y_2 = fvec.y * fvec.y;
    float y_sin = std::sin(yaw);
    float y_cos = std::cos(yaw);
    float y_sin_2 = y_sin * y_sin;
    float y_cos_2 = y_cos * y_cos;
    float p_sgn = std::signbit(fvec.y) ? -1.f : 1.f;
    if (fvec.y == 0.0f) {
        return 0.0f;
    }

    float a = 1.f / fvec_y_2 - y_sin_2 - 1.f - y_cos_2;
    float b = 2.f * p_sgn * y_sin_2 + 2.f * p_sgn * y_cos_2;
    float c = -y_sin_2 - y_cos_2;
    float delta = b * b - 4.f * a * c;
    // Note: delta is sometimes slightly below 0 - most probably because of precision error
    // To avoid NaN value delta is changed to 0 in that case
    float delta_sqrt = std::sqrt(std::max(delta, 0.0f));
    assert(!std::isnan(delta_sqrt));

    if (a == 0.0f) {
        return 0.0f;
    }

    float p_sin_1 = (-b - delta_sqrt) / (2.f * a);
    float p_sin_2 = (-b + delta_sqrt) / (2.f * a);

    float result;
    if (std::abs(p_sin_1) < std::abs(p_sin_2))
        result = std::asin(p_sin_1);
    else
        result = std::asin(p_sin_2);
    assert(!std::isnan(result));
    return result;
}

#ifdef DEBUG
void linear_pitch_test()
{
    float yaw = 3.141592f / 4.0f;
    float pitch = 3.141592f / 4.0f;
    rf::Vector3 fvec = fw_vector_from_non_linear_yaw_pitch(yaw, pitch);
    float lin_pitch = linear_pitch_from_forward_vector(fvec);
    rf::Vector3 fvec2 = fw_vector_from_linear_yaw_pitch(yaw, lin_pitch);
    float pitch2 = non_linear_pitch_from_fw_vector(fvec2);
    assert(std::abs(pitch - pitch2) < 0.00001);
}
#endif // DEBUG

CodeInjection linear_pitch_patch{
    0x0049DEC9,
    [](auto& regs) {
        if (!g_alpine_game_config.mouse_linear_pitch)
            return;
        // Non-linear pitch value and delta from RF
        rf::Entity* entity = regs.esi;
        float current_yaw = entity->control_data.phb.y;
        float current_pitch_non_lin = entity->control_data.eye_phb.x;
        float& pitch_delta = *reinterpret_cast<float*>(regs.esp + 0x44 - 0x34);
        float& yaw_delta = *reinterpret_cast<float*>(regs.esp + 0x44 + 0x4);
        if (pitch_delta == 0)
            return;
        // Convert to linear space (see RotMatixFromEuler function at 004A0D70)
        auto fvec = fw_vector_from_non_linear_yaw_pitch(current_yaw, current_pitch_non_lin);
        float current_pitch_lin = linear_pitch_from_forward_vector(fvec);
        // Calculate new pitch in linear space
        float new_pitch_lin = current_pitch_lin + pitch_delta;
        float new_yaw = current_yaw + yaw_delta;
        // Clamp to [-pi, pi]
        constexpr float half_pi = 1.5707964f;
        new_pitch_lin = std::clamp(new_pitch_lin, -half_pi, half_pi);
        // Convert back to non-linear space
        auto fvec_new = fw_vector_from_linear_yaw_pitch(new_yaw, new_pitch_lin);
        float new_pitch_non_lin = non_linear_pitch_from_fw_vector(fvec_new);
        // Update non-linear pitch delta
        float new_pitch_delta = new_pitch_non_lin - current_pitch_non_lin;
        xlog::trace("non-lin {} lin {} delta {} new {}", current_pitch_non_lin, current_pitch_lin, pitch_delta,
              new_pitch_delta);
        pitch_delta = new_pitch_delta;
    },
};

ConsoleCommand2 linear_pitch_cmd{
    "cl_linearpitch",
    []() {
#ifdef DEBUG
        linear_pitch_test();
#endif

        g_alpine_game_config.mouse_linear_pitch = !g_alpine_game_config.mouse_linear_pitch;
        rf::console::print("Linear pitch is {}", g_alpine_game_config.mouse_linear_pitch ? "enabled" : "disabled");
    },
    "Toggles mouse linear pitch angle",
};

void mouse_apply_patch()
{
    // Handle zoom sens customization
    static_zoom_sensitivity_patch.install();
    static_zoom_sensitivity_patch2.install();
    AsmWriter{0x004309DE}.fmul<float>(AsmRegMem{&applied_static_sensitivity_value});
    AsmWriter{0x004309B1}.fmul<float>(AsmRegMem{&applied_dynamic_sensitivity_value});
    update_scope_sensitivity();
    update_scanner_sensitivity();

    // Disable mouse when window is not active
    mouse_eval_deltas_hook.install();

    // Add enhanced mouse input support (DirectInput + Raw Input)
    mouse_eval_deltas_di_hook.install();
    mouse_keep_centered_enable_hook.install();
    mouse_keep_centered_disable_hook.install();
    mouse_get_delta_raw_hook.install();

    // Register Raw Input message handler
    rf::os_add_msg_handler(process_raw_input_message);

    // Initialize with saved mouse input mode
    set_mouse_input_mode(static_cast<MouseInputMode>(g_alpine_game_config.mouse_input_mode));

    // Do not limit the cursor to the game window if in menu (Win32 mouse)
    AsmWriter(0x0051DD7C).jmp(0x0051DD8E);

    // Use exclusive DirectInput mode so cursor cannot exit game window
    //write_mem<u8>(0x0051E14B + 1, 5); // DISCL_EXCLUSIVE|DISCL_FOREGROUND

    // Linear vertical rotation (pitch)
    linear_pitch_patch.install();

    // Commands
    input_mode_cmd.register_cmd();
    ms_cmd.register_cmd();
    static_scope_sens_cmd.register_cmd();
    scope_sens_cmd.register_cmd();
    scanner_sens_cmd.register_cmd();
    linear_pitch_cmd.register_cmd();
}
