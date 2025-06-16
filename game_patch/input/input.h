#pragma once

#include "../rf/player/control_config.h"

// Mouse input modes
enum MouseInputMode {
    MOUSE_INPUT_WIN32 = 0,      // Legacy Win32 mouse (WM_MOUSEMOVE)
    MOUSE_INPUT_DIRECTINPUT = 1, // DirectInput 8
    MOUSE_INPUT_RAW = 2         // Raw Input
};

rf::ControlConfigAction get_af_control(rf::AlpineControlConfigAction alpine_control);
rf::String get_action_bind_name(int action);
void mouse_apply_patch();
void key_apply_patch();

// Mouse input mode functions
bool set_mouse_input_mode(MouseInputMode mode);
const char* get_mouse_input_mode_name(MouseInputMode mode);
MouseInputMode get_current_mouse_input_mode();
