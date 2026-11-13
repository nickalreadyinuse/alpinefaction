#pragma once

#include <string>

// WASAPI audio backend: a WASAPI-backed implementation of IDirectSound handed to the game in place of the real one.

struct IDirectSound;

struct WasapiStatus
{
    bool device_open = false; // false while the device is being reopened or is missing
    bool exclusive = false;
    bool exclusive_requested = false;
    std::string device_name;
    int sample_rate = 0;
    int channels = 0;
    int bits = 0;
    int buffer_frames = 0;
    int period_frames = 0; // low-latency shared engine period, 0 when not in use
};

// Starts the audio thread and opens the default output device. Returns nullptr if no device could be opened, in
// which case the caller should fall back to real DirectSound.
IDirectSound* wasapi_create_direct_sound(bool exclusive, int buffer_ms);
void wasapi_shutdown();
bool wasapi_is_active();

// Reopens the output device with new settings; playing sounds carry over
void wasapi_configure(bool exclusive, int buffer_ms);

WasapiStatus wasapi_get_status();
int wasapi_get_playing_buffer_count();
int wasapi_get_render_passes();

// Hooks, settings glue and console commands (sound_wasapi_hooks.cpp)
void snd_wasapi_apply_hooks();
void snd_wasapi_register_commands();
void wasapi_apply_settings();
