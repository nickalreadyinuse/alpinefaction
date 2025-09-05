#include <xlog/xlog.h>
#include <patch_common/FunHook.h>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include <common/version/version.h>
#include "gametype.h"
#include "multi.h"
#include "../rf/localize.h"

static char* const* g_af_gametype_names[4];

static char koth_name[] = "KOTH";
static char* koth_slot = koth_name;

KothInfo g_koth_info;

void populate_gametype_table() {
    g_af_gametype_names[0] = &rf::strings::dm;
    g_af_gametype_names[1] = &rf::strings::ctf;
    g_af_gametype_names[2] = &rf::strings::teamdm;
    g_af_gametype_names[3] = &koth_slot;

    for (int i = 0; i < 4; ++i) {
        const char* const* slot = g_af_gametype_names[i];
        const char* name = (slot && *slot) ? *slot : "(null)";
        xlog::warn("GameType[{}]: {} (slot={}, name_ptr={})", i, name, static_cast<const void*>(slot),
                   static_cast<const void*>(*slot));
    }
}

CallHook<char*(const char*, const char*)> listen_server_map_list_filename_contains_hook{
    0x00445730,
    [](const char* filename, const char* contains_str) {
        if (char* p = listen_server_map_list_filename_contains_hook.call_target(filename, contains_str))
            return p; // contains "ctf"

        for (const char* tok : multi_rfl_prefixes) {
            if (char* q = listen_server_map_list_filename_contains_hook.call_target(filename, tok))
                return q; // contains "koth" etc.
        }

        return static_cast<char*>(nullptr); // no match
    },
};

int multi_koth_get_red_team_score()
{
    return g_koth_info.red_team_score;
}

int multi_koth_get_blue_team_score()
{
    return g_koth_info.blue_team_score;
}

void gametype_do_patch()
{
    // index rfl files for new gamemodes when opening listen server create menu
    listen_server_map_list_filename_contains_hook.install();

    // patch gametype name references to use new table
    write_mem<uint32_t>((0x0044A8D3) + 3, (uint32_t)(uintptr_t)&g_af_gametype_names[0]); // multi_join_game_compare_func
    write_mem<uint32_t>((0x0044A8FB) + 3, (uint32_t)(uintptr_t)&g_af_gametype_names[0]); // multi_join_game_compare_func
    write_mem<uint32_t>((0x0044C1EB) + 3, (uint32_t)(uintptr_t)&g_af_gametype_names[0]); // multi_join_game_render_row
    write_mem<uint32_t>((0x0044C227) + 3, (uint32_t)(uintptr_t)&g_af_gametype_names[0]); // multi_join_game_render_row
    write_mem<uint32_t>((0x0044C724) + 3, (uint32_t)(uintptr_t)&g_af_gametype_names[0]); // multi_join_game_init

    // patch listen server create menu gametype select field
    const uintptr_t base = (uintptr_t)&g_af_gametype_names[0];
    const uintptr_t end = base + sizeof(g_af_gametype_names);
    const uintptr_t kMovBase = 0x004459B1;
    const uintptr_t kCmpEnd = 0x004459CE;
    write_mem<uint32_t>(kMovBase + 1, (uint32_t)base);
    write_mem<uint32_t>(kCmpEnd + 2, (uint32_t)end);
}
