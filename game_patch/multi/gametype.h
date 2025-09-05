#pragma once

#include <optional>
#include <xlog/xlog.h>
#include "../rf/os/string.h"

static const char* const multi_rfl_prefixes[] = {
    // "m_",
    // "dm",
    // "ctf",
    "koth",
};

struct KothInfo
{
    uint32_t red_team_score = 0;
    uint32_t blue_team_score = 0;
    uint8_t num_hills = 0;
};

extern KothInfo g_koth_info;

int multi_koth_get_red_team_score();
int multi_koth_get_blue_team_score();
void gametype_do_patch();
void populate_gametype_table();
