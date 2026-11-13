#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/MemUtils.h>
#include <xlog/xlog.h>
#include "sound_wasapi.h"
#include "../main/main.h"
#include "../rf/sound/sound_ds.h"
#include "../os/console.h"
#include "../misc/alpine_settings.h"

// snd_ds_init_device (0x00521340), right after CoInitialize and before CoCreateInstance(CLSID_DirectSound).
// Hands the game a WASAPI-backed IDirectSound instead; if WASAPI cannot open a device the original call runs and
// the game uses real DirectSound.
CodeInjection snd_ds_create_direct_sound_injection{
    0x005213C8,
    [](auto& regs) {
        if (!g_game_config.wasapi_audio) {
            xlog::info("Audio output: DirectSound (WASAPI disabled in launcher)");
            return;
        }
        IDirectSound* ds = wasapi_create_direct_sound(
            g_alpine_game_config.wasapi_exclusive, g_alpine_game_config.wasapi_buffer_ms);
        if (!ds) {
            xlog::warn("Audio output: WASAPI unavailable, falling back to DirectSound");
            return;
        }
        xlog::info("Audio output: WASAPI");
        // The emulation has no DirectSound3D, EAX or A3D; turning them off makes the game pan and attenuate itself
        addr_as_ref<bool>(0x01AED340) = false; // ds3d_enabled
        addr_as_ref<bool>(0x01AD751C) = false; // eax enabled
        addr_as_ref<bool>(0x01AD7A4C) = false; // a3d enabled
        addr_as_ref<bool>(0x01AD751D) = true;  // CoInitialize was called (the skipped instruction), undone in close
        rf::direct_sound = ds;
        regs.eax = 0; // S_OK for the HRESULT check
        regs.eip = 0x005213E7;
    },
};

// snd_ds_close (0x00521840), reached from snd_term on exit. The game releases its buffers and the IDirectSound
// first; then the audio thread is stopped (a joinable std::thread left at DLL unload would call std::terminate).
FunHook<void()> snd_ds_close_hook{
    0x00521840,
    []() {
        snd_ds_close_hook.call_target();
        wasapi_shutdown();
    },
};

void wasapi_apply_settings()
{
    wasapi_configure(g_alpine_game_config.wasapi_exclusive, g_alpine_game_config.wasapi_buffer_ms);
}

ConsoleCommand2 snd_wasapi_info_cmd{
    "snd_wasapi_info",
    []() {
        if (!wasapi_is_active()) {
            rf::console::print("Audio output: DirectSound ({})",
                g_game_config.wasapi_audio ? "WASAPI failed to start, see log" : "WASAPI disabled in launcher");
            return;
        }
        WasapiStatus s = wasapi_get_status();
        if (!s.device_open) {
            rf::console::print("Audio output: WASAPI, no device open (reconnecting)");
            return;
        }
        rf::console::print("Audio output: WASAPI {} on {}", s.exclusive ? "exclusive" : "shared", s.device_name);
        if (s.exclusive_requested && !s.exclusive) {
            rf::console::print("  Exclusive mode was refused; check \"Allow applications to take exclusive "
                               "control\" in the Windows sound device properties");
        }
        rf::console::print("  Format: {} Hz, {} ch, {} bit", s.sample_rate, s.channels, s.bits);
        rf::console::print("  Buffer: {:.1f} ms ({} frames)", 1000.0 * s.buffer_frames / s.sample_rate,
            s.buffer_frames);
        if (s.period_frames > 0) {
            rf::console::print("  Low-latency engine period: {:.2f} ms", 1000.0 * s.period_frames / s.sample_rate);
        }
        rf::console::print("  Playing buffers: {}, render passes: {}", wasapi_get_playing_buffer_count(),
            wasapi_get_render_passes());
    },
    "Show the WASAPI audio output status",
};

void snd_wasapi_apply_hooks()
{
    snd_ds_create_direct_sound_injection.install();
    snd_ds_close_hook.install();
}

void snd_wasapi_register_commands()
{
    snd_wasapi_info_cmd.register_cmd();
}
