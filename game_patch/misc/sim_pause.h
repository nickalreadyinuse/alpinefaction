#pragma once

// World simulation freeze shared by demo pause and multiplayer match timeouts. While any
// reason holds it, the sim frame is skipped, game time (every rf::Timestamp) is frozen,
// playing sounds are paused and mesh animation is held. Rendering, input and the camera
// system keep running.
enum class SimPauseReason
{
    demo = 1 << 0,
    match_timeout = 1 << 1,
};

void sim_pause_set(SimPauseReason reason, bool paused);
bool sim_pause_active();
bool sim_pause_active(SimPauseReason reason);
// Multiplier that turns rf::frametime (wall clock, unaffected by the freeze) into
// simulation time: 0 while frozen, otherwise the demo playback timescale (1 outside
// playback). For code that ticks outside the sim frame. The sim-frame and vmesh freeze
// hooks live in multi/demo/demo_playback.cpp alongside the timescale.
float sim_time_scale();
