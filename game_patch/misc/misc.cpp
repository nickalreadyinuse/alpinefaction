#include <windows.h>
#include <common/version/version.h>
#include <common/config/BuildConfig.h>
#include <xlog/xlog.h>
#include <patch_common/AsmOpcodes.h>
#include <patch_common/AsmWriter.h>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/FunHook.h>
#include <cstring>
#include <iostream>
#include <fstream>
#include <string>
#include <algorithm>
#include <sstream>
#include "alpine_options.h"
#include "alpine_settings.h"
#include "misc.h"
#include "level.h"
#include "../sound/sound.h"
#include "../os/console.h"
#include "../main/main.h"
#include "../multi/multi.h"
#include "../multi/server.h"
#include "../rf/os/frametime.h"
#include "../rf/event.h"
#include "../rf/gr/gr.h"
#include "../rf/player/player.h"
#include "../rf/multi.h"
#include "../rf/gameseq.h"
#include "../rf/os/os.h"
#include "../rf/misc.h"
#include "../rf/mover.h"
#include "../rf/parse.h"
#include "../rf/vmesh.h"
#include "../rf/level.h"
#include "../rf/file/file.h"
#include "../object/object.h"

void achievements_apply_patch();
void apply_main_menu_patches();
void apply_save_restore_patches();
void apply_sound_patches();
void register_sound_commands();
void player_do_patch();
void player_fpgun_do_patch();
void g_solid_do_patch();
void camera_do_patch();
void ui_apply_patch();
void game_apply_patch();
void character_apply_patch();
void level_apply_patch();
void alpine_settings_apply_patch();

struct JoinMpGameData
{
    rf::NetAddr addr;
    std::string password;
};

bool g_in_mp_game = false;
bool g_jump_to_multi_server_list = false;
std::optional<JoinMpGameData> g_join_mp_game_seq_data;
std::optional<std::string> g_levelm_filename;
std::optional<rf::NetGameType> g_local_pending_game_type; // used for pending gt received from server. I don't like this being here, todo: refactor
std::optional<int> g_local_pending_win_condition;

void set_local_pending_game_type(rf::NetGameType game_type, int win_condition) {
    g_local_pending_game_type = game_type;
    g_local_pending_win_condition = win_condition;
}

void reset_local_pending_game_type() {
    g_local_pending_game_type.reset();
    g_local_pending_win_condition.reset();
}

bool tc_mod_is_loaded()
{
    return rf::mod_param.found();
}

bool af_rfl_version(int version)
{
    return version >= 300 && version <= MAXIMUM_RFL_VERSION;
}

// check if the currently loaded level is at least X version
bool rfl_version_minimum(int check_version)
{
    if (rf::level.flags & rf::LEVEL_LOADED) {
        return rf::level.version >= check_version;
    }
    else {
        return false;
    }
}

// check if we're currently in an Alpine level
bool af_rfl_is_loaded()
{
    if (rf::level.flags & rf::LEVEL_LOADED) {
        return rf::level.version >= 300 && rf::level.version <= MAXIMUM_RFL_VERSION;
    }
    else {
        return false;
    }    
}

CodeInjection critical_error_hide_main_wnd_patch{
    0x0050BA90,
    []() {
        rf::gr::close();
        if (rf::main_wnd)
            ShowWindow(rf::main_wnd, SW_HIDE);
    },
};

CodeInjection critical_error_log_injection{
    0x0050BAE8,
    [](auto& regs) {
        const char* text = regs.ecx;
        xlog::error("Critical error:\n{}", text);
    },
};

void set_jump_to_multi_server_list(bool jump)
{
    g_jump_to_multi_server_list = jump;
}

void start_join_multi_game_sequence(const rf::NetAddr& addr, const std::string& password)
{
    g_jump_to_multi_server_list = true;
    g_join_mp_game_seq_data = {JoinMpGameData{addr, password}};
}

void start_levelm_load_sequence(std::string filename)
{
    g_jump_to_multi_server_list = true;
    g_levelm_filename = filename;
}

bool multi_join_game(const rf::NetAddr& addr, const std::string& password)
{
    auto multi_set_current_server_addr = addr_as_ref<void(const rf::NetAddr& addr)>(0x0044B380);
    auto send_join_req_packet = addr_as_ref<void(const rf::NetAddr& addr, rf::String::Pod name, rf::String::Pod password, int max_rate)>(0x0047AA40);

    if (rf::gameseq_get_state() != rf::GS_MULTI_SERVER_LIST) {
        return false;
    }

    rf::String password2{password.c_str()};
    g_join_mp_game_seq_data.reset();
    multi_set_current_server_addr(addr);
    send_join_req_packet(addr, rf::local_player->name, password2, rf::local_player->net_data->max_update_rate);
    return true;
}

FunHook<void(int, int)> rf_init_state_hook{
    0x004B1AC0,
    [](int state, int old_state) {
        rf_init_state_hook.call_target(state, old_state);
        xlog::trace("state {} old_state {} g_jump_to_multi_server_list {}", state, old_state, g_jump_to_multi_server_list);

        bool exiting_game = state == rf::GS_MAIN_MENU &&
            (old_state == rf::GS_END_GAME || old_state == rf::GS_NEW_LEVEL);
        if (exiting_game && g_in_mp_game) {
            g_in_mp_game = false;
            g_jump_to_multi_server_list = true;
        }

        if (state == rf::GS_MAIN_MENU && g_jump_to_multi_server_list) {
            xlog::trace("jump to mp menu!");
            set_sound_enabled(false);
            AddrCaller{0x00443C20}.c_call(); // open_multi_menu
            old_state = state;
            state = rf::gameseq_process_deferred_change();
            rf_init_state_hook.call_target(state, old_state);
        }
        if (state == rf::GS_MULTI_MENU && g_jump_to_multi_server_list) {
            AddrCaller{0x00448B70}.c_call(); // on_mp_join_game_btn_click
            old_state = state;
            state = rf::gameseq_process_deferred_change();
            rf_init_state_hook.call_target(state, old_state);
        }
        if (state == rf::GS_MULTI_SERVER_LIST && g_jump_to_multi_server_list) {
            g_jump_to_multi_server_list = false;
            set_sound_enabled(true);

            if (g_join_mp_game_seq_data) {
                auto addr = g_join_mp_game_seq_data.value().addr;
                auto password = g_join_mp_game_seq_data.value().password;
                g_join_mp_game_seq_data.reset();
                multi_join_game(addr, password);
            }
            else if (g_levelm_filename.has_value()) {
                start_level_in_multi(g_levelm_filename.value());
                g_levelm_filename.reset();
            }
        }
    },
};

FunHook<bool(int)> rf_state_is_closed_hook{
    0x004B1DD0,
    [](int state) {
        if (g_jump_to_multi_server_list)
            return true;
        return rf_state_is_closed_hook.call_target(state);
    },
};

FunHook<void()> multi_after_players_packet_hook{
    0x00482080,
    []() {
        multi_after_players_packet_hook.call_target();
        g_in_mp_game = true;
        mp_send_handicap_request(false);
    },
};

CodeInjection mover_rotating_keyframe_oob_crashfix{
    0x0046A559,
    [](auto& regs) {
        float& unk_time = *reinterpret_cast<float*>(regs.esi + 0x308);
        unk_time = 0;
        regs.eip = 0x0046A89D;
    }
};

static inline rf::MoverKeyframe* KF(const rf::Mover* m, int i)
{
    return m->keyframes[i];
}

static inline int count_keyframes(const rf::Mover* m)
{
    return m->keyframes.size();
}

static inline bool mover_forward(const rf::Mover* m)
{
    return (m->mover_flags & rf::MoverFlags::MF_DIR_FORWARD) != 0;
}

static inline void mover_set_forward(rf::Mover* m)
{
    m->mover_flags = static_cast<rf::MoverFlags>(
        m->mover_flags | rf::MoverFlags::MF_DIR_FORWARD
    );
}

static inline void mover_set_backward(rf::Mover* m)
{
    m->mover_flags = static_cast<rf::MoverFlags>(
        m->mover_flags & ~rf::MoverFlags::MF_DIR_FORWARD
    );
}

static inline bool mover_paused_at_keyframe(const rf::Mover* m)
{
    return (m->mover_flags & rf::MoverFlags::MF_PAUSED_AT_KEYFRAME) != 0;
}

static inline bool mover_paused_with_flag(const rf::Mover* m)
{
    return (m->mover_flags & rf::MoverFlags::MF_PAUSED) != 0;
}

static inline void mover_pause_at_kf(rf::Mover* m, float seconds)
{
    if (!m)
        return;

    if (seconds <= 0.0f) {
        m->mover_flags = static_cast<rf::MoverFlags>(m->mover_flags & ~rf::MoverFlags::MF_PAUSED_AT_KEYFRAME);
        m->wait_timestamp.invalidate();
        return;
    }

    const int ms = static_cast<int>(seconds * 1000.0f + 0.5f);
    m->wait_timestamp.set(ms);
    m->mover_flags = static_cast<rf::MoverFlags>(m->mover_flags | rf::MoverFlags::MF_PAUSED_AT_KEYFRAME);
    m->cur_vel = 0.0f;

    rf::mover_play_stop_sound(m);
}

static inline void mover_clear_pause(rf::Mover* m)
{
    if (!m)
        return;

    m->mover_flags = static_cast<rf::MoverFlags>(m->mover_flags & ~rf::MoverFlags::MF_PAUSED_AT_KEYFRAME);
    m->wait_timestamp.invalidate();
}

static inline bool mover_is_moving(const rf::Mover* m)
{
    return (m->stop_at_keyframe != -1);
}

static bool alpine_mover_try_bounce_door_mid_segment(
    rf::Mover* mp, int& from, int& to, rf::MoverKeyframe*& kfA, rf::MoverKeyframe*& kfB,
    float& travel_param, float& travel, rf::Vector3& delta, float& dist_sq, float& dist)
{
    if (!mp)
        return false;

    const int count = count_keyframes(mp);
    if (count < 2)
        return false;

    // Must be a door, must be obstructed by an entity
    if (!rf::mover_is_door(mp) ||
        mp->keyframe_move_type == rf::MoverKeyframeMoveType::MKMT_ONE_WAY ||
        to != 0 ||
        !rf::mover_is_obstructed_by_entity(mp))
    {
        return false;
    }

    // Ensure current movement segment is valid
    const float old_T = travel;
    if (old_T <= 0.0f)
        return false;

    // Where are we along the segment?
    const float old_t = mp->travel_time_seconds;
    const float frac = std::clamp(old_t / old_T, 0.0f, 1.0f);

    // Mirror the progress when bouncing
    const float mirrored_frac = 1.0f - frac;

    // Go back towards door open state
    std::swap(mp->start_at_keyframe, mp->stop_at_keyframe);
    mover_set_forward(mp);

    from = mp->start_at_keyframe;
    to = mp->stop_at_keyframe;

    if (from < 0 || to < 0 || from >= count || to >= count || from == to)
        return false;

    kfA = KF(mp, from);
    kfB = KF(mp, to);
    if (!kfA || !kfB)
        return false;

    // Rebuild segment geometry
    delta = kfB->pos - kfA->pos;
    dist_sq = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
    dist = (dist_sq > 0.0f) ? std::sqrt(dist_sq) : 0.0f;

    const bool dir_forward = (mp->mover_flags & rf::MoverFlags::MF_DIR_FORWARD) != 0;

    if (dir_forward) {
        travel_param = kfA->forward_time_seconds;
    }
    else {
        travel_param = kfA->reverse_time_seconds;
    }

    travel = 0.001f; // fallback value

    if (travel_param > 0.0f) {
        if (mp->mover_flags & rf::MoverFlags::MF_USE_TRAV_TIME_AS_SPD) {
            if (dist > 0.0f)
                travel = dist / travel_param; // travel time treated as velocity
        }
        else {
            travel = travel_param;
        }
    }

    // Put us on the mirrored segment
    mp->travel_time_seconds = mirrored_frac * travel;

    return true;
}

// t = elapsed sec along this segment
// T = total travel time (sec) for this segment
static float alpine_trans_progress_trapezoid(float t, float T, float accel, float decel)
{
    if (T <= 0.0f)
        return 1.0f;
    if (t <= 0.0f)
        return 0.0f;
    if (t >= T)
        return 1.0f;

    if (accel <= 0.0f && decel <= 0.0f)
        return t / T;

    const float v_max_denom = T - 0.5f * (accel + decel);

    // If accel+decel is too large, fall back to linear
    if (v_max_denom <= 0.0f)
        return t / T;

    const float v_max = 1.0f / v_max_denom;
    const float cruise_end = T - decel;

    if (t < accel) {
        // quadratic accelerate
        return 0.5f * v_max * (t * t / accel);
    }

    if (t < cruise_end) {
        // cruise: full speed, constant slope
        const float area_accel = 0.5f * v_max * accel;
        return area_accel + v_max * (t - accel);
    }

    // quadratic decelerate
    const float u = T - t;
    const float remaining = 0.5f * v_max * (u * u / decel);
    return 1.0f - remaining;
}

// out_s = [0-1] progress
// out_v_norm = normalized speed, integral over [0,T] == 1
static void alpine_trans_trapezoid(float t, float T, float accel, float decel, float& out_s, float& out_v_norm)
{
    if (T <= 0.0f) {
        out_s = 1.0f;
        out_v_norm = 0.0f;
        return;
    }
    if (t <= 0.0f) {
        out_s = 0.0f;
        out_v_norm = 0.0f;
        return;
    }
    if (t >= T) {
        out_s = 1.0f;
        out_v_norm = 0.0f;
        return;
    }

    if (accel <= 0.0f && decel <= 0.0f) {
        // linear case
        out_s = t / T;
        out_v_norm = 1.0f / T; // constant speed
        return;
    }

    const float v_max_denom = T - 0.5f * (accel + decel);

    // If accel+decel is too large, fall back to linear
    if (v_max_denom <= 0.0f) {
        out_s = t / T;
        out_v_norm = 1.0f / T;
        return;
    }

    const float v_max = 1.0f / v_max_denom; // normalized speed
    const float cruise_end = T - decel;
    const float area_accel = 0.5f * v_max * accel;

    // accelerating
    if (t < accel) {
        const float a = v_max / accel; // normalized acceleration
        out_v_norm = a * t;
        out_s      = 0.5f * a * t * t;
        return;
    }
    // cruise
    if (t < cruise_end) {
        out_v_norm = v_max;
        out_s      = area_accel + v_max * (t - accel);
        return;
    }

    // decelerating
    const float u = T - t;
    const float a = v_max / decel; // normalized deceleration
    out_v_norm = a * u;

    const float area_tail = 0.5f * v_max * (u * u / decel);
    out_s = 1.0f - area_tail;
}


void alpine_mover_reached_keyframe(rf::Mover* mp)
{
    if (!mp)
        return;

    const int count = count_keyframes(mp);
    if (count < 2)
        return;

    const int from = mp->start_at_keyframe; // segment we just finished
    const int cur  = mp->stop_at_keyframe; // arrival keyframe

    if (cur < 0 || cur >= count || from < 0 || from >= count) {
        mp->stop_at_keyframe = -1;
        return;
    }

    const int first = 0;
    const int last  = count - 1;

    // Trigger event by UID
    rf::MoverKeyframe* kf_cur = KF(mp, cur);
    if (auto ev = rf::event_lookup_from_uid(kf_cur->event_uid)) {
        rf::event_activate_from_trigger(ev->handle, mp->handle, -1);
    }

    // Snap to arrival state
    mp->start_at_keyframe   = cur;
    mp->travel_time_seconds = 0.0f;
    mp->dist_travelled      = 0.0f;

    const float pause = kf_cur->pause_time_seconds;

    // stay open while player is in trigger region
    if (mp->stop_completely_at_keyframe >= 0 && cur == mp->stop_completely_at_keyframe) {
        const int count = count_keyframes(mp);
        const int last = count - 1;

        mp->start_at_keyframe = cur;
        mp->stop_at_keyframe = -1;
        mp->travel_time_seconds = 0.0f;
        mp->dist_travelled = 0.0f;

        rf::mover_play_stop_sound(mp);

        if (cur == last) {
            mover_set_backward(mp); // clear forward
        }
        else if (cur == 0) {
            mover_set_forward(mp); // set forward
        }

        return;
    }

    int next = -1; // default: stop until re-activated

    switch (mp->keyframe_move_type)
    {
        // ONE WAY
        case rf::MoverKeyframeMoveType::MKMT_ONE_WAY:
        {
            const bool forward = mover_forward(mp);

            if (forward) {
                if (cur < last) {
                    // still heading toward last
                    next = cur + 1;
                } else {
                    // reached last, stop; next activation goes backward
                    mover_set_backward(mp);
                    next = -1;
                }
            } else { // backward
                if (cur > first) {
                    next = cur - 1;
                } else {
                    // reached first, stop; next activation goes forward
                    mover_set_forward(mp);
                    next = -1;
                }
            }

            mp->stop_at_keyframe = next;
            mover_pause_at_kf(mp, pause);
            return;
        }

        // PING PONG ONCE
        case rf::MoverKeyframeMoveType::MKMT_PING_PONG_ONCE:
        {
            bool forward = mover_forward(mp);
            const int first = 0;
            const int last = count - 1;

            if (forward) {
                if (cur < last) {
                    // Still on the forward leg: go to next keyframe
                    next = cur + 1;
                }
                else {
                    // Reached last while going forward – start the backward leg
                    mover_set_backward(mp);
                    forward = false;
                    if (count > 1)
                        next = cur - 1;
                }
            }
            else { // backward
                if (cur > first) {
                    // Still going back toward first
                    next = cur - 1;
                }
                else {
                    // complete, stop here
                    next = -1;

                    // reset state so next activation is clean forward
                    mover_set_forward(mp);
                    mp->stop_completely_at_keyframe = -1;
                }
            }

            mp->stop_at_keyframe = next;
            mover_pause_at_kf(mp, pause);
            return;
        }

        // PING PONG INFINITE
        case rf::MoverKeyframeMoveType::MKMT_PING_PONG_INFINITE:
        {
            bool forward = mover_forward(mp);

            if (forward) {
                if (cur < last) {
                    next = cur + 1;
                } else {
                    // reached last while going forward; bounce
                    mover_set_backward(mp);
                    forward = false;
                    if (count > 1)
                        next = cur - 1;
                }
            } else { // backward
                if (cur > first) {
                    next = cur - 1;
                } else {
                    // reached first while going backward; bounce again
                    mover_set_forward(mp);
                    forward = true;
                    if (count > 1)
                        next = cur + 1;
                }
            }

            mp->stop_at_keyframe = next;
            mover_pause_at_kf(mp, pause);
            return;
        }

        // LOOP ONCE
        case rf::MoverKeyframeMoveType::MKMT_LOOP_ONCE:
        {
            // check if loop is complete
            if (from == last && cur == first) {
                next = -1; // finished one loop
            } else {
                // keep going forward, wrapping
                next = (cur < last) ? (cur + 1) : first;
            }

            mp->stop_at_keyframe = next;
            mover_pause_at_kf(mp, pause);
            return;
        }

        // LOOP INFINITE
        case rf::MoverKeyframeMoveType::MKMT_LOOP_INFINITE:
        {
            next = (cur < last) ? (cur + 1) : first;

            mp->stop_at_keyframe = next;
            mover_pause_at_kf(mp, pause);
            return;
        }

        // LIFT
        case rf::MoverKeyframeMoveType::MKMT_LIFT:
        {
            // what should the next activation do?
            if (cur == last)
                mover_set_backward(mp);
            else if (cur == first)
                mover_set_forward(mp);

            // finished one segment
            next = -1;

            mp->stop_at_keyframe = next;
            mover_pause_at_kf(mp, pause);
            return;
        }
    }
}

void alpine_mover_process_linear(rf::Mover* mp)
{
    if (!mp)
        return;

    const int count = count_keyframes(mp);
    if (count < 2)
        return;

    // keep current position pinned if not moving
    if (!mover_is_moving(mp)) {
        const int cur = mp->start_at_keyframe;
        if (cur >= 0 && cur < count)
            mp->p_data.next_pos = KF(mp, cur)->pos;
        return;
    }

    int from = mp->start_at_keyframe;
    int to = mp->stop_at_keyframe;

    if (from < 0 || to < 0 || from >= count || to >= count || from == to)
        return;

    rf::MoverKeyframe* kfA = KF(mp, from);
    rf::MoverKeyframe* kfB = KF(mp, to);
    if (!kfA || !kfB)
        return;

    const bool dir_forward = (mp->mover_flags & rf::MoverFlags::MF_DIR_FORWARD) != 0;

    float travel_param = 0.0f;
    if (dir_forward) {
        travel_param = kfA->forward_time_seconds;
    }
    else { // backwards
        travel_param = kfA->reverse_time_seconds;
    }

    // Compute segment distance
    rf::Vector3 delta = kfB->pos - kfA->pos;
    float dist_sq = rf::vec_dist_squared(&kfA->pos, &kfB->pos);
    float dist = rf::vec_dist(&kfA->pos, &kfB->pos);

    float travel = 0.001f; // fallback

    if (travel_param > 0.0f) {
        if (mp->mover_flags & rf::MoverFlags::MF_USE_TRAV_TIME_AS_SPD) {
            // travel_param as velocity
            if (dist > 0.0f)
                travel = dist / travel_param;
        }
        else {
            // travel_param as time
            travel = travel_param;
        }
    }

    bool bounced_this_frame =
        alpine_mover_try_bounce_door_mid_segment(mp, from, to, kfA, kfB, travel_param, travel, delta, dist_sq, dist);

    // Obstructed and has not bounced this frame
    if (!bounced_this_frame && rf::mover_is_obstructed(mp)) {
        const float obstruct_pause = kfB->pause_time_seconds;
        mover_pause_at_kf(mp, obstruct_pause);
        return;
    }

    // Advance elapsed time
    mp->travel_time_seconds += rf::frametime;
    if (mp->travel_time_seconds > travel)
        mp->travel_time_seconds = travel;

    float accel = kfA->ramp_up_time_seconds;
    float decel = kfA->ramp_down_time_seconds;

    // For "Travel Time as Velocity" mode, ensure accel/decel fit within travel
    const bool use_travel_time_as_velocity = (mp->mover_flags & rf::MoverFlags::MF_USE_TRAV_TIME_AS_SPD) != 0;
    if (use_travel_time_as_velocity && travel > 0.0f) {
        const float ramp_sum = accel + decel;
        if (ramp_sum > 0.0f) {
            const float max_ramp_sum = 0.9f * travel;
            if (ramp_sum > max_ramp_sum) {
                const float scale = max_ramp_sum / ramp_sum;
                accel *= scale;
                decel *= scale;
            }
        }
    }

    // Calculate and set mover properties for this frame
    float s = 0.0f;
    float v_norm = 0.0f;
    alpine_trans_trapezoid(mp->travel_time_seconds, travel, accel, decel, s, v_norm);
    mp->dist_travelled = dist * s; // s = path fraction
    mp->cur_vel = dist * v_norm;
    const rf::Vector3 pos{kfA->pos.x + delta.x * s, kfA->pos.y + delta.y * s, kfA->pos.z + delta.z * s};
    mp->p_data.next_pos = pos;

    // Did we reach the end of this segment?
    if (mp->travel_time_seconds >= travel - 1e-4f) {
        // Snap cleanly to destination
        mp->p_data.next_pos = kfB->pos;
        mp->travel_time_seconds = 0.0f;
        mp->dist_travelled = 0.0f;

        const int old_stop = mp->stop_at_keyframe;

        alpine_mover_reached_keyframe(mp);

        // no longer have a next keyframe, fully stop
        if (!mover_is_moving(mp)) {
            if (!mover_paused_at_keyframe(mp)) {
                rf::mover_play_stop_sound(mp);
            }
        }

        rf::mover_update_item_status(mp);
    }
    /* xlog::warn("mover {} seg {} -> {}, dir_fwd={}, use_trav_as_spd={}, "
               "fwdA={}, revA={}, travel_param={}, travel={}, frametime={}",
               mp->handle, from, to, dir_forward, (mp->mover_flags & rf::MoverFlags::MF_USE_TRAV_TIME_AS_SPD) != 0,
               kfA->forward_time_seconds, kfA->reverse_time_seconds, travel_param, travel, rf::frametime);*/
}

static void alpine_mover_process_pre(rf::Mover* mp)
{
    if (!mp)
        return;

    mp->p_data.vel = rf::Vector3{0.0f, 0.0f, 0.0f};

    if (rf::mover_rotates_in_place(mp)) {
        rf::mover_rotating_process_pre(mp);
        return;
    }

    const int count = count_keyframes(mp);

    if (mp->door_room) {
        auto* door_room = static_cast<std::uint8_t*>(mp->door_room);
        door_room[0x40] = 0;
        if (mp->start_at_keyframe == 0 && mp->stop_at_keyframe == -1)
            door_room[0x40] = 1;
    }

    const auto mover_flags = mp->mover_flags;

    if (mover_flags & rf::MoverFlags::MF_PAUSED) {
        // treat mover as active with zero velocity
        // allows mover_interpolate_objects to ensure child brushes/objects also have their vel zeroed when paused
        // otherwise they would keep their last frame vel and players standing on them would be pushed
        mp->mover_flags = static_cast<rf::MoverFlags>(mover_flags | (rf::MoverFlags::MF_UNK_8 | rf::MoverFlags::MF_UNK_4000));

        mp->obj_flags = static_cast<rf::ObjectFlags>(static_cast<int>(mp->obj_flags) | 0x04000000);

        const int loop_instance = mp->sound_instances[1];
        if (loop_instance != -1) {
            rf::snd_change_3d(loop_instance, mp->p_data.pos, rf::zero_vector, 1.0f);
        }

        mp->p_data.vel = rf::Vector3{0.0f, 0.0f, 0.0f};
        mp->cur_vel = 0.0f;

        return;
    }

    if (!mover_is_moving(mp)) {
        return;
    }

    // normal movement
    mp->mover_flags = static_cast<rf::MoverFlags>(mover_flags | (rf::MoverFlags::MF_UNK_8 | rf::MoverFlags::MF_UNK_4000));
    mp->obj_flags = static_cast<rf::ObjectFlags>(static_cast<int>(mp->obj_flags) | 0x04000000);

    const int loop_instance = mp->sound_instances[1];
    if (loop_instance != -1) {
        rf::snd_change_3d(loop_instance, mp->p_data.pos, rf::zero_vector, 1.0f);
    }

    // handle pause time
    if (mover_paused_at_keyframe(mp)) {
        if (!mp->wait_timestamp.elapsed()) {
            const int cur = mp->start_at_keyframe;
            if (cur >= 0 && cur < count) {
                mp->p_data.next_pos = KF(mp, cur)->pos;
            }
            mp->p_data.vel = rf::Vector3{0.0f, 0.0f, 0.0f};
            mp->cur_vel = 0.0f;
            return;
        }

        mover_clear_pause(mp);
        rf::mover_play_start_sound(mp);
    }

    alpine_mover_process_linear(mp);
}

FunHook<void(rf::Mover*)> mover_process_pre_hook{
    0x00469800,
    [](rf::Mover* mp) {
        if (!mp)
            return;

        if (AlpineLevelProperties::instance().legacy_movers) {
            mover_process_pre_hook.call_target(mp);
            return;
        }

        alpine_mover_process_pre(mp);

        //auto* brush = rf::obj_lookup_from_uid(6711);
        //xlog::warn("flags {:x},{:x}, vel {},{},{}, next_pos {},{},{}", static_cast<uint32_t>(mp->mover_flags), static_cast<uint32_t>(mp->obj_flags),
        //    brush->p_data.vel.x, brush->p_data.vel.y, brush->p_data.vel.z,
        //    brush->p_data.next_pos.x, brush->p_data.next_pos.y, brush->p_data.next_pos.z);
    },
};

CodeInjection mover_rotating_process_pre_accel_patch{
    0x0046A55F,
    [](auto& regs) {
        if (AlpineLevelProperties::instance().legacy_movers) {
            return; // legacy behaviour
        }

        rf::Mover* mp = regs.esi;
        rf::MoverKeyframe* kf0 = regs.ebp;

        if (!mp || !kf0 || mp->stop_at_keyframe == -1) {
            regs.eip = 0x0046A5D8;
            return;
        }

        const bool forward = (mp->mover_flags & rf::MoverFlags::MF_DIR_FORWARD) != 0;
        const float T = forward ? kf0->forward_time_seconds : kf0->reverse_time_seconds;

        if (T <= 0.0f) {
            regs.eip = 0x0046A6C9;
            return;
        }

        const float t = mp->travel_time_seconds;

        if (t >= T) {
            regs.eip = 0x0046A6C9;
            return;
        }

        float accel = std::max(kf0->ramp_up_time_seconds, 0.0f);
        float decel = std::max(kf0->ramp_down_time_seconds, 0.0f);

        float s = 0.0f;
        float v_norm = 0.0f;
        alpine_trans_trapezoid(t, T, accel, decel, s, v_norm);

        const float A = kf0->rotation_angle; // radians for this segment

        // Normalized speed
        float omega = A * v_norm;
        if (!forward) {
            omega = -omega;
        }

        // Integrate angle over this frame
        mp->rot_cur_pos += omega * rf::frametime;

        constexpr float two_pi = 6.2831855f;
        if (mp->rot_cur_pos >= two_pi || mp->rot_cur_pos < 0.0f) {
            mp->rot_cur_pos = std::fmod(mp->rot_cur_pos, two_pi);
            if (mp->rot_cur_pos < 0.0f)
                mp->rot_cur_pos += two_pi;
        }

        regs.eip = 0x0046A5D8;
    }
};

CodeInjection mover_rotating_process_pre_ping_pong_patch{
    0x0046A84F,
    [](auto& regs) {
        if (AlpineLevelProperties::instance().legacy_movers) {
            return; // legacy behaviour
        }

        regs.eip = 0x0046A855; // skip original code which erroneously removes MF_DIR_FORWARD flag
    }
};

CodeInjection mover_interpolate_objects_force_orient_trans_patch{
    0x0046BF39, // after check for translation movers with "Force Orient" flag
    [](auto& regs) {
        if (AlpineLevelProperties::instance().legacy_movers) {
            return; // legacy behaviour
        }

        rf::Mover* mp = regs.esi;
        // for translating movers with "Force Orient", stock game code crashes on Vector3::get_substracted for stop_at_keyframe
        // because stop_at_keyframe is -1 before movement begins - we don't need to care about orient here
        // because it is not important until movement starts, so skip it to avoid the crash
        if (mp->stop_at_keyframe < 0 || mp->start_at_keyframe < 0) {
            regs.eip = 0x0046BF89;
            return;
        }
    }
};

CodeInjection mover_interpolate_objects_force_orient_rot_patch{
    0x0046BECA,
    [](auto& regs) {
        if (AlpineLevelProperties::instance().legacy_movers) {
            return; // legacy behaviour
        }

        rf::Mover* mp = regs.esi;
        // rotating movers force the orientation to follow the rotation path by default, so use "Force Orient"
        // to reverse that behavior and force the orientation to stay fixed throughout the movement
        if ((mp->mover_flags & rf::MoverFlags::MF_FORCE_ORIENT) != 0) {
            rf::Matrix3 orient;
            orient.make_identity();
            regs.ecx = reinterpret_cast<int32_t>(&orient);
        }
    }
};

CodeInjection parser_xstr_oob_fix{
    0x0051212E,
    [](auto& regs) {
        if (regs.edi >= 1000) {
            xlog::warn("XSTR index is out of bounds: {}!", static_cast<int>(regs.edi));
            regs.edi = -1;
        }
    }
};

CodeInjection ammo_tbl_buffer_overflow_fix{
    0x004C218E,
    [](auto& regs) {
        constexpr int max_ammo_types = 32;
        auto num_ammo_types = addr_as_ref<int>(0x0085C760);
        if (num_ammo_types == max_ammo_types) {
            xlog::warn("ammo.tbl limit of {} definitions has been reached!", max_ammo_types);
            regs.eip = 0x004C21B8;
        }
    },
};

CodeInjection clutter_tbl_buffer_overflow_fix{
    0x0040F49E,
    [](auto& regs) {
        constexpr int max_clutter_types = 450;
        if (regs.ecx == max_clutter_types) {
            xlog::warn("clutter.tbl limit of {} definitions has been reached!", max_clutter_types);
            regs.eip = 0x0040F4B0;
        }
    },
};

CodeInjection vclip_tbl_buffer_overflow_fix{
    0x004C1401,
    [](auto& regs) {
        constexpr int max_vclips = 64;
        auto num_vclips = addr_as_ref<int>(0x008568AC);
        if (num_vclips == max_vclips) {
            xlog::warn("vclip.tbl limit of {} definitions has been reached!", max_vclips);
            regs.eip = 0x004C1420;
        }
    },
};

CodeInjection items_tbl_buffer_overflow_fix{
    0x00458AD1,
    [](auto& regs) {
        constexpr int max_item_types = 96;
        auto num_item_types = addr_as_ref<int>(0x00644EA0);
        if (num_item_types == max_item_types) {
            xlog::warn("items.tbl limit of {} definitions has been reached!", max_item_types);
            regs.eip = 0x00458AFB;
        }
    },
};

CodeInjection explosion_tbl_buffer_overflow_fix{
    0x0048E0F3,
    [](auto& regs) {
        constexpr int max_explosion_types = 12;
        auto num_explosion_types = addr_as_ref<int>(0x0075EC44);
        if (num_explosion_types == max_explosion_types) {
            xlog::warn("explosion.tbl limit of {} definitions has been reached!", max_explosion_types);
            regs.eip = 0x0048E112;
        }
    },
};

CodeInjection weapons_tbl_primary_buffer_overflow_fix{
    0x004C6855,
    [](auto& regs) {
        constexpr int max_weapon_types = 64;
        auto num_weapon_types = addr_as_ref<int>(0x00872448);
        if (num_weapon_types == max_weapon_types) {
            xlog::warn("weapons.tbl limit of {} definitions has been reached!", max_weapon_types);
            regs.eip = 0x004C68D9;
        }
    },
};

CodeInjection weapons_tbl_secondary_buffer_overflow_fix{
    0x004C68AD,
    [](auto& regs) {
        constexpr int max_weapon_types = 64;
        auto num_weapon_types = addr_as_ref<int>(0x00872448);
        if (num_weapon_types == max_weapon_types) {
            xlog::warn("weapons.tbl limit of {} definitions has been reached!", max_weapon_types);
            regs.eip = 0x004C68D9;
        }
    },
};

CodeInjection pc_multi_tbl_buffer_overflow_fix{
    0x00475B50,
    [](auto& regs) {
        constexpr int max_multi_characters = 31;
        auto num_multi_characters = addr_as_ref<int>(0x006C9C60);
        if (num_multi_characters == max_multi_characters) {
            xlog::warn("pc_multi.tbl limit of {} definitions has been reached!", max_multi_characters);
            regs.eip = 0x0047621F;
        }
    },
};

CodeInjection emitters_tbl_buffer_overflow_fix{
    0x00496E76,
    [](auto& regs) {
        constexpr int max_emitter_types = 64;
        auto num_emitter_types = addr_as_ref<int>(0x006C9C60);
        if (num_emitter_types == max_emitter_types) {
            xlog::warn("emitters.tbl limit of {} definitions has been reached!", max_emitter_types);
            regs.eip = 0x00496F1A;
        }
    },
};

FunHook<void(const char*, int)> lcl_add_message_bof_fix{
    0x004B0720,
    [](const char* str, int id) {
        constexpr int xstr_size = 1000;
        if (id < xstr_size) {
            lcl_add_message_bof_fix.call_target(str, id);
        }
        else {
            xlog::warn("strings.tbl index is out of bounds: {}", id);
        }
    },
};

CodeInjection glass_shard_level_init_fix{
    0x00435A90,
    []() {
        auto glass_shard_level_init = addr_as_ref<void()>(0x00490F60);
        glass_shard_level_init();
    },
};

int debug_print_hook(char* buf, const char *fmt, ...) {
    va_list vl;
    va_start(vl, fmt);
    int ret = std::vsprintf(buf, fmt, vl);
    va_end(vl);
    xlog::warn("{}", buf);
    return ret;
}

CallHook<int(char*, const char*)> skeleton_pagein_debug_print_patch{
    0x0053AA73,
    reinterpret_cast<int(*)(char*, const char*)>(debug_print_hook),
};

CodeInjection vmesh_col_fix{
    0x00499BCF,
    [](auto& regs) {
        auto stack_frame = regs.esp + 0xC8;
        auto& col_in = addr_as_ref<rf::VMeshCollisionInput>(regs.eax);
        // Reset flags field so start_pos/dir always gets transformed into mesh space
        // Note: MeshCollide function adds flag 2 after doing transformation into mesh space
        // If start_pos/dir is being updated for next call, flags must be reset as well
        col_in.flags = 0;
        // Reset dir field
        col_in.dir = addr_as_ref<rf::Vector3>(stack_frame - 0xAC);
    },
};

CodeInjection explosion_crash_fix{
    0x00436594,
    [](auto& regs) {
        rf::Player* player = regs.edx;
        if (player == nullptr) {
            regs.esp += 4;
            regs.eip = 0x004365EC;
        }
    },
};

CodeInjection vfile_read_stack_corruption_fix{
    0x0052D0E0,
    [](auto& regs) {
        regs.esi = regs.eax;
    },
};

CodeInjection game_set_file_paths_injection{
    0x004B1810,
    []() {
        std::string client_mods_dir = "client_mods\\";
        rf::file_add_path(client_mods_dir.c_str(), ".bik", false);

        if (tc_mod_is_loaded()) {
            std::string mod_dir = "mods\\";
            mod_dir += rf::mod_param.get_arg();
            rf::file_add_path(mod_dir.c_str(), ".bik" "bluebeard.bty", false);
        }
    },
};

CallHook level_init_pre_console_output_hook{
    0x00435ABB,
    []() {
        if (rf::is_multi) {
            // server delayed gametype swap
            if (rf::is_dedicated_server && g_dedicated_launched_from_ads) {
                apply_game_type_for_current_level();
                rf::netgame.type = get_upcoming_game_type();
                clear_explicit_upcoming_game_type_request();
            }

            // local client delayed gametype swap
            if (!rf::is_server) {
                if (g_local_pending_game_type.has_value()) {
                    rf::netgame.type = g_local_pending_game_type.value();

                    if (g_local_pending_win_condition.has_value() && get_af_server_info_mutable().has_value()) {
                        auto& server_info = get_af_server_info_mutable().value();
                        switch (rf::netgame.type) {
                        case rf::NetGameType::NG_TYPE_CTF:
                            rf::netgame.max_captures = g_local_pending_win_condition.value();
                            break;
                        case rf::NetGameType::NG_TYPE_KOTH:
                            server_info.koth_score_limit = g_local_pending_win_condition.value();
                            break;
                        case rf::NetGameType::NG_TYPE_DC:
                            server_info.dc_score_limit = g_local_pending_win_condition.value();
                            break;
                        case rf::NetGameType::NG_TYPE_REV:
                            break;
                        case rf::NetGameType::NG_TYPE_RUN:
                            break;
                        default:
                            rf::netgame.max_kills = g_local_pending_win_condition.value();
                            break;
                        }
                    }
                }

                reset_local_pending_game_type();
            }

            rf::console::print("-- Level Initializing: {} ({}) --", rf::level_filename_to_load, get_game_type_string(rf::netgame.type));
            apply_rules_for_current_level();
        }
        else { // SP
            rf::console::print("-- Level Initializing: {} --", rf::level_filename_to_load);
        }
    },
};

CodeInjection game_boot_load_alpine_options_injection{
    0x004B21B2,
    []() {
        load_af_options_config();
    }
};

CodeInjection level_read_header_patch{
    0x004615BF, [](auto& regs) {
        int version = regs.eax;
        xlog::debug("Attempting to load level with version: {}", version);

        // only needs to handle 300+, < 40 and 201 - 299 are denied by original code
        if (version >= 300 && version <= MAXIMUM_RFL_VERSION) {
            regs.eip = 0x004615DF;
        }
    }
};

CodeInjection level_read_geometry_header_patch{
    0x00461A62, [](auto& regs) {

        write_mem<float>(0x00646018, 1120403456.0f);
    }
};

CodeInjection static_bomb_code_patch{
    0x0043B4C7,
    [](auto& regs) {
        if (g_alpine_game_config.static_bomb_code) {
            // Set bomb_defuse_code1 (4-digit code)
            auto& code1 = addr_as_ref<std::array<int32_t, 4>>(0x0063914C);
            code1 = {1, 2, 3, 0};

            // Set bomb_defuse_code2 (7-digit code)
            auto& code2 = addr_as_ref<std::array<int32_t, 7>>(0x006390D8);
            code2 = {3, 2, 1, 0, 0, 2, 1};

            regs.eip = 0x0043B632; // skip RNG bomb calculation
        }
    },
};

ConsoleCommand2 static_bomb_code_cmd{
    "sp_staticbombcode",
    []() {
        g_alpine_game_config.static_bomb_code = !g_alpine_game_config.static_bomb_code;
        rf::console::print("Static bomb code is {}", g_alpine_game_config.static_bomb_code ? "enabled" : "disabled");
    },
    "Toggle bomb code between randomized (default) and static.",
};

void misc_init()
{
    // Static bomb code
    static_bomb_code_patch.install();
    static_bomb_code_cmd.register_cmd();

    //gr_set_far_clip_hook.install();
    //AsmWriter{0x0051806F}.jmp(0x00518083); // stops far clip from derendering geometry covered by fog, buggy

    // Allow loading of rfl files with supported AF-specific versions
    level_read_header_patch.install();
    AsmWriter{0x004461BF}.jmp(0x00446200); // load Level Name from all rfls (any version) on listen server create panel


    // fog experimentation - attempting to stop fp weapon cutoff. Success when using static values but not when rfl has specified values
    //fog_near_clip_patch.install();
    //gr_fog_set_hook.install();
    //AsmWriter{0x00461A5C}.nop(6);
    //level_read_geometry_header_patch.install();
    //AsmWriter(0x00461A5C).nop(6);

    // Display a more informative message to user if they try to load an unsupported rfl
    static char new_unsupported_version_message[] =
        "Unsupported version (%d).\nVisit alpinefaction.com to find a compatible client version.\n";
    AsmWriter{0x004615C6}.push(reinterpret_cast<int32_t>(new_unsupported_version_message));

    // Window title (client and server)
    write_mem_ptr(0x004B2790, PRODUCT_NAME);
    write_mem_ptr(0x004B27A4, PRODUCT_NAME);

#if NO_CD_FIX
    // No-CD fix
    write_mem<u8>(0x004B31B6, asm_opcodes::jmp_rel_short);
#endif // NO_CD_FIX

    // Disable thqlogo.bik
    if (g_game_config.fast_start) {
        write_mem<u8>(0x004B208A, asm_opcodes::jmp_rel_short);
        write_mem<u8>(0x004B24FD, asm_opcodes::jmp_rel_short);
    }

    // Crash-fix... (probably argument for function is invalid); Page Heap is needed
    write_mem<u32>(0x0056A28C + 1, 0);

    // Fix crash in shadows rendering
    write_mem<u8>(0x0054A3C0 + 2, 16);

    // Disable broken optimization of segment vs geometry collision test
    // Fixes hitting objects if mover is in the line of the shot
    AsmWriter(0x00499055).jmp(0x004990B4);

    // Disable Flamethower debug sphere drawing (optimization)
    // It is not visible in game because other things are drawn over it
    AsmWriter(0x0041AE47, 0x0041AE4C).nop();

    // Open server list menu instead of main menu when leaving multiplayer game
    rf_init_state_hook.install();
    rf_state_is_closed_hook.install();
    multi_after_players_packet_hook.install();

    // Hide main window when displaying critical error message box
    critical_error_hide_main_wnd_patch.install();

    // Log critical error message
    critical_error_log_injection.install();

    // Fix crash when skipping cutscene after robot kill in L7S4
    mover_rotating_keyframe_oob_crashfix.install();

    // Alpine translation movers
    mover_process_pre_hook.install();

    // Fix accel/decel and "Loop" mode behaviour for rotating movers
    mover_rotating_process_pre_accel_patch.install();

    // Fix "Ping Pong Infinite" mode behaviour for rotating movers
    mover_rotating_process_pre_ping_pong_patch.install();

    // Make "Force Orient" mover flag work properly
    mover_interpolate_objects_force_orient_trans_patch.install();
    mover_interpolate_objects_force_orient_rot_patch.install();

    // Fix crash in LEGO_MP mod caused by XSTR(1000, "RL"); for some reason it does not crash in PF...
    parser_xstr_oob_fix.install();

    // Fix crashes caused by too many records in tbl files
    ammo_tbl_buffer_overflow_fix.install();
    clutter_tbl_buffer_overflow_fix.install();
    vclip_tbl_buffer_overflow_fix.install();
    items_tbl_buffer_overflow_fix.install();
    explosion_tbl_buffer_overflow_fix.install();
    weapons_tbl_primary_buffer_overflow_fix.install();
    weapons_tbl_secondary_buffer_overflow_fix.install();
    pc_multi_tbl_buffer_overflow_fix.install();
    emitters_tbl_buffer_overflow_fix.install();
    lcl_add_message_bof_fix.install();

    // Fix killed glass restoration from a save file
    AsmWriter(0x0043604A).nop(5);
    glass_shard_level_init_fix.install();

    // Log error when RFA cannot be loaded
    skeleton_pagein_debug_print_patch.install();

    // Fix col-spheres vs mesh collisions
    vmesh_col_fix.install();

    // Fix crash caused by explosion near dying player-controlled entity (entity->local_player is null)
    explosion_crash_fix.install();

    // If speed reduction in background is not wanted disable that code in RF
    if (!g_game_config.reduced_speed_in_background) {
        write_mem<u8>(0x004353CC, asm_opcodes::jmp_rel_short);
    }

    // Fix stack corruption when packfile has lower size than expected
    vfile_read_stack_corruption_fix.install();

    // Improve parse error message
    // For some reason RF replaces all characters with code lower than 0x20 (space) by character with code 0x16 (SYN)
    // in "Found this text" and "Prior text" sections. This makes all new line characters (CRLF) and tabs to be
    // replaced by ugly squeres. After this change only zero character is replaced.
    write_mem<char>(0x00512389 + 2, '\1');
    write_mem<char>(0x005123B6 + 2, '\1');

    // Do not render the level twice when Message Log is open (GS_MESSAGE_LOG game state is marked as transparent)
    AsmWriter{0x0045514E}.nop(5);
    AsmWriter{0x0045515B}.nop(5);

    // Add support for Bink videos and bluebeard.bty in mods
    game_set_file_paths_injection.install();

    // Add level name to "-- Level Initializing --" message
    level_init_pre_console_output_hook.install();

    // Load alpine_options files (alpine_options.cpp)
    game_boot_load_alpine_options_injection.install();

    // Apply patches from other files
    achievements_apply_patch();
    alpine_settings_apply_patch();
    apply_main_menu_patches();
    apply_save_restore_patches();
    apply_sound_patches();
    player_do_patch();
    player_fpgun_do_patch();
    g_solid_do_patch();
    register_sound_commands();
    camera_do_patch();
    ui_apply_patch();
    game_apply_patch();
    character_apply_patch();
    level_apply_patch();
}
