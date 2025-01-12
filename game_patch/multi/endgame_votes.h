#pragma once

#include "faction_files.h"

extern bool g_player_can_endgame_vote;
void multi_player_set_can_endgame_vote(bool can_vote);
void multi_attempt_endgame_vote(bool liked);
