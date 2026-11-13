#pragma once

#include <cstdint>

struct MatchPausePayload;

// Match timeouts: a passed pause vote freezes the simulation on the server and every
// client (misc/sim_pause.h) until the timeout runs out or a resume vote passes. Both
// end with a fixed countdown, after which play continues from where it stopped.

// Server
bool match_pause_server_active(); // paused or counting down to resume
bool match_pause_server_paused(); // paused and not yet counting down
int match_pause_server_pausing_team();
int match_pause_server_timeouts_used(int team);
// Pausing "team" for a timeout called by an rcon holder (sv_timeout)
constexpr int match_pause_admin = 2;
// Pause vote passed: starts a timeout, or extends the one in progress
void match_pause_server_pause(int team);
// Resume vote passed: starts the resume countdown
void match_pause_server_resume();
// Drops any timeout and releases the freeze (match end, limbo, empty server)
void match_pause_server_reset();
void match_pause_server_do_frame();

// Client
void match_pause_client_on_state(const MatchPausePayload& payload);
void match_pause_client_reset();
bool match_pause_client_paused(); // paused and not yet counting down
// Stock and Alpine actions a player can't use while the match is frozen
bool match_pause_blocks_action(int action);
void match_pause_hud_render();

// Called on both sides before the limbo timer is armed: the frozen game time would stall it
void match_pause_on_limbo();
