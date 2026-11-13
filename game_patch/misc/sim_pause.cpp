#include <cstdint>
#include "sim_pause.h"
#include "../multi/demo/demo.h"
#include "../rf/object.h"
#include "../rf/sound/sound.h"
#include "../rf/os/timer.h"

namespace
{
    // SimPauseReason bits currently holding the freeze. The engine pause primitives are
    // engaged on the first reason and released with the last, so the game-time pause
    // counter stays balanced however the reasons overlap.
    unsigned g_sim_pause_reasons = 0;

    // Outside demo playback the interp rings run on wall-clock time, which the freeze cannot
    // stop: the per-frame advance (obj_interp_frame_advance_hook) steps by the clock delta since
    // frame_time_us, and sample insertion (ObjInterp::set_next_pos_orient @ 0x00483360) records
    // the gap since last_update_time into arrive_time_diff - whose 20-entry average is the
    // physics-extrapolation step and re-anchor headroom for every later sample. After a long
    // pause the first fed obj_update would log the whole pause as one arrival gap, polluting the
    // average until it leaves the ring ~20 samples later - every remote player jerks around for
    // that stretch. Stamping the per-ring clocks to "now" on unfreeze makes the pause invisible
    // to both consumers; interp_time and the recorded keyframes are untouched, so the sim
    // resumes exactly where it stopped. (During playback the clock is demo time, which already
    // held, so this is a no-op there.)
    void rebase_interp_clocks_after_freeze()
    {
        const auto now_us = static_cast<uint32_t>(demo_playback_interp_clock_us());
        const auto now_ms = static_cast<uint32_t>(demo_playback_interp_clock_ms());
        for (rf::Object* obj = rf::object_list.next_obj; obj != &rf::object_list; obj = obj->next_obj) {
            if (rf::ObjInterp* interp = obj->obj_interp) {
                interp->frame_time_us = now_us;
                if (interp->last_update_time != static_cast<uint32_t>(-1)) { // -1 = no sample yet (Clear)
                    interp->last_update_time = now_ms;
                }
            }
        }
    }
}

void sim_pause_set(SimPauseReason reason, bool paused)
{
    const bool was_active = sim_pause_active();
    if (paused) {
        g_sim_pause_reasons |= static_cast<unsigned>(reason);
    }
    else {
        g_sim_pause_reasons &= ~static_cast<unsigned>(reason);
    }
    const bool active = sim_pause_active();
    if (active == was_active) {
        return;
    }
    if (active) {
        rf::timer::inc_game_paused();
        rf::snd_pause(true);
    }
    else {
        rf::timer::dec_game_paused();
        rf::snd_pause(false);
        rebase_interp_clocks_after_freeze();
    }
}

bool sim_pause_active()
{
    return g_sim_pause_reasons != 0;
}

bool sim_pause_active(SimPauseReason reason)
{
    return (g_sim_pause_reasons & static_cast<unsigned>(reason)) != 0;
}

float sim_time_scale()
{
    return sim_pause_active() ? 0.0f : demo_playback_sim_time_scale();
}
