#include <patch_common/AsmWriter.h>
#include <patch_common/FunHook.h>
#include <patch_common/CallHook.h>
#include "../main/main.h"
#include "../rf/multi.h"
#include "../os/console.h"
#include "../misc/misc.h"
#include "../misc/alpine_settings.h"
#include "../multi/multi.h"
#include "../rf/player/player.h"
#include "../rf/os/frametime.h"

constexpr auto screen_shake_fps = 150.0f;

static float g_camera_shake_factor = 0.6f;

bool server_side_restrict_disable_ss = false;

FunHook<void(rf::Camera*)> camera_update_shake_hook{
    0x0040DB70,
    [](rf::Camera *camera) {
        float frame_time = rf::frametime;
        if (frame_time > 0.0001f) { // < 1000FPS
            // Fix screen shake caused by some weapons (eg. Assault Rifle)
            g_camera_shake_factor = std::pow(0.6f, frame_time / (1 / screen_shake_fps));
        }

        camera_update_shake_hook.call_target(camera);
    },
};

// camera shake for FP weapons
CallHook<void(rf::Camera*, float, float)> camera_shake_hook{
    0x00426C79,
    [](rf::Camera* cp, float amplitude, float time_seconds) {

        if (g_alpine_game_config.try_disable_weapon_shake && !server_side_restrict_disable_ss) {
            return;
        }

        camera_shake_hook.call_target(cp, amplitude, time_seconds);
    }
};

// camera shake for everything
FunHook<void(rf::Camera*, float, float)> camera_shake_global_hook{
    0x0040E0B0,
    [](rf::Camera* cp, float amplitude, float time_seconds) {

        if (g_alpine_game_config.screen_shake_force_off && !rf::is_multi) {
            return;
        }

        camera_shake_global_hook.call_target(cp, amplitude, time_seconds);
    }
};

void evaluate_restrict_disable_ss()
{
    server_side_restrict_disable_ss =
        rf::is_multi && !rf::is_server && get_df_server_info() && !get_df_server_info()->allow_no_ss;

    if (server_side_restrict_disable_ss) {
        if (g_alpine_game_config.try_disable_weapon_shake) {
            rf::console::print("This server does not allow you to disable weapon camera shake!");
        }
    }
}

ConsoleCommand2 disable_weaphake_cmd{
    "cl_weapshake",
    []() {
        g_alpine_game_config.try_disable_weapon_shake = !g_alpine_game_config.try_disable_weapon_shake;

        evaluate_restrict_disable_ss();

        rf::console::print("Camera shake from weapon fire is {}",
                           g_alpine_game_config.try_disable_weapon_shake
                               ? "disabled. In multiplayer, this will only apply if the server allows it."
                               : "enabled.");
    },
    "Disable camera shake from weapon firing. In multiplayer, this is only applied if the server allows it.",
};

ConsoleCommand2 force_disable_camerashake_cmd{
    "sp_camerashake",
    []() {
        g_alpine_game_config.screen_shake_force_off = !g_alpine_game_config.screen_shake_force_off;

        rf::console::print("All instances of camera shake are {}being forcefully turned off in single player.",
                           g_alpine_game_config.screen_shake_force_off
                               ? ""
                               : "NOT ");
    },
    "Forcefully disable all forms of camera shake. Only applies to single player.",
};

void camera_do_patch()
{
    // Fix crash when executing camera2 command in main menu
    AsmWriter(0x0040DCFC).nop(5);

    // Fix screen shake caused by some weapons (eg. Assault Rifle)
    write_mem_ptr(0x0040DBCC + 2, &g_camera_shake_factor);

    // handle turning off screen shake
    disable_weaphake_cmd.register_cmd();
    camera_shake_hook.install();
    force_disable_camerashake_cmd.register_cmd();
    camera_shake_global_hook.install();
}
