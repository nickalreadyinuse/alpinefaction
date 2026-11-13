#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <string>
#include <common/utils/list-utils.h>
#include <common/utils/string-utils.h>
#include "match_pause.h"
#include "alpine_packets.h"
#include "rounds.h"
#include "server_internal.h"
#include "demo/demo.h"
#include "../hud/hud.h"
#include "../input/input.h"
#include "../misc/sim_pause.h"
#include "../os/console.h"
#include "../rf/gameseq.h"
#include "../rf/multi.h"
#include "../rf/player/player.h"
#include "../rf/player/control_config.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/os/timestamp.h"

// Both a cancelled and an expiring timeout end with this warning countdown, during
// which the match is still frozen
static constexpr int resume_countdown_ms = 10000;

static const char* pause_caller_name(int team)
{
    return team == rf::TEAM_RED ? "RED team" : team == rf::TEAM_BLUE ? "BLUE team" : "Admin";
}

static std::string format_duration(int seconds)
{
    return std::format("{}:{:02}", seconds / 60, seconds % 60);
}

// ============================================================================
// Server
// ============================================================================

namespace
{
    struct ServerPauseState
    {
        AfMatchPausePhase phase = AfMatchPausePhase::None;
        // Wall clock: game time is frozen for the whole timeout
        std::chrono::steady_clock::time_point phase_end{};
        int pausing_team = rf::TEAM_RED;
        std::array<int, 2> timeouts_used{};
        // Bumped on every change, so each client is sent the state once per change
        // (rf::Player::match_pause_generation_sent; a new client starts at 0)
        uint32_t generation = 0;
    };

    ServerPauseState g_server_pause;
}

static MatchPausePayload build_server_pause_payload()
{
    MatchPausePayload payload;
    payload.phase = g_server_pause.phase;
    payload.pausing_team = static_cast<uint8_t>(g_server_pause.pausing_team);
    if (g_server_pause.phase != AfMatchPausePhase::None) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            g_server_pause.phase_end - std::chrono::steady_clock::now());
        payload.remaining_ms = static_cast<uint32_t>(std::max<int64_t>(left.count(), 0));
    }
    return payload;
}

static void set_server_pause_phase(AfMatchPausePhase phase)
{
    g_server_pause.phase = phase;
    ++g_server_pause.generation;
    sim_pause_set(SimPauseReason::match_timeout, phase != AfMatchPausePhase::None);
    // The listen-server host has no connection to send it to
    if (!rf::is_dedicated_server) {
        match_pause_client_on_state(build_server_pause_payload());
    }
}

static void sync_pause_state_to_clients()
{
    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        // A reliable packet sent before the joiner's reliable socket connects is dropped
        if (!player.net_data || static_cast<int>(player.net_data->reliable_socket) < 0) {
            continue;
        }
        if (player.match_pause_generation_sent != g_server_pause.generation) {
            player.match_pause_generation_sent = g_server_pause.generation;
            af_send_match_pause(&player, build_server_pause_payload());
        }
    }
}

bool match_pause_server_active()
{
    return g_server_pause.phase != AfMatchPausePhase::None;
}

bool match_pause_server_paused()
{
    return g_server_pause.phase == AfMatchPausePhase::Paused;
}

int match_pause_server_pausing_team()
{
    return g_server_pause.pausing_team;
}

int match_pause_server_timeouts_used(int team)
{
    return team == rf::TEAM_RED || team == rf::TEAM_BLUE ? g_server_pause.timeouts_used[team] : 0;
}

void match_pause_server_pause(int team)
{
    const int duration_s = g_alpine_server_config.vote_pause.duration_seconds;
    const auto duration = std::chrono::seconds{duration_s};
    if (team == rf::TEAM_RED || team == rf::TEAM_BLUE) {
        ++g_server_pause.timeouts_used[team];
    }

    if (g_server_pause.phase == AfMatchPausePhase::None) {
        g_server_pause.pausing_team = team;
        g_server_pause.phase_end = std::chrono::steady_clock::now() + duration;
        af_broadcast_automated_chat_msg(
            std::format("Timeout called by {} ({}).", pause_caller_name(team), format_duration(duration_s)));
    }
    else {
        // Extending during the resume countdown pauses again: it adds to what was left
        g_server_pause.phase_end = std::max(g_server_pause.phase_end, std::chrono::steady_clock::now()) + duration;
        af_broadcast_automated_chat_msg(
            std::format("Timeout extended by {} (+{}).", pause_caller_name(team), format_duration(duration_s)));
    }
    set_server_pause_phase(AfMatchPausePhase::Paused);
}

void match_pause_server_resume()
{
    if (g_server_pause.phase != AfMatchPausePhase::Paused) {
        return;
    }
    g_server_pause.phase_end = std::chrono::steady_clock::now() + std::chrono::milliseconds{resume_countdown_ms};
    af_broadcast_automated_chat_msg(std::format("Timeout canceled. Resuming in {} seconds!", resume_countdown_ms / 1000));
    set_server_pause_phase(AfMatchPausePhase::Resuming);
}

// For rcon holders: no vote, no per-team limit, works with [vote_pause] disabled
ConsoleCommand2 sv_timeout_cmd{
    "sv_timeout",
    [](std::optional<std::string> arg) {
        if (!rf::is_server || !g_match_info.match_active) {
            rf::console::print("There is no match in progress.\n");
        }
        else if (arg && string_iequals(*arg, "end")) {
            if (!match_pause_server_paused()) {
                rf::console::print("There is no timeout to end.\n");
            }
            else {
                match_pause_server_resume();
            }
        }
        else {
            match_pause_server_pause(match_pause_admin);
        }
    },
    "Call a match timeout, extend the one in progress, or end it with 'end'.",
    "sv_timeout [end]",
};

void match_pause_server_reset()
{
    g_server_pause.timeouts_used = {};
    if (g_server_pause.phase != AfMatchPausePhase::None) {
        set_server_pause_phase(AfMatchPausePhase::None);
    }
}

void match_pause_server_do_frame()
{
    if (g_server_pause.phase != AfMatchPausePhase::None) {
        if (!g_match_info.match_active || rf::gameseq_get_state() != rf::GS_GAMEPLAY) {
            match_pause_server_reset();
        }
        else {
            const auto now = std::chrono::steady_clock::now();
            if (g_server_pause.phase == AfMatchPausePhase::Paused
                && g_server_pause.phase_end - now <= std::chrono::milliseconds{resume_countdown_ms}) {
                af_broadcast_automated_chat_msg(
                    std::format("Timeout ending. Resuming in {} seconds!", resume_countdown_ms / 1000));
                set_server_pause_phase(AfMatchPausePhase::Resuming);
            }
            else if (g_server_pause.phase == AfMatchPausePhase::Resuming && now >= g_server_pause.phase_end) {
                af_broadcast_automated_chat_msg("Match resumed!");
                set_server_pause_phase(AfMatchPausePhase::None);
            }
        }
    }
    sync_pause_state_to_clients();
}

// ============================================================================
// Client
// ============================================================================

namespace
{
    struct ClientPauseState
    {
        AfMatchPausePhase phase = AfMatchPausePhase::None;
        int pausing_team = rf::TEAM_RED;
        rf::TimestampRealtime phase_end;
        // The resume countdown borrows the round-start countdown display
        bool countdown_shown = false;
    };

    ClientPauseState g_client_pause;
}

void match_pause_client_on_state(const MatchPausePayload& payload)
{
    // A recorded timeout is not replayed: playback has its own pause
    if (demo_playback_active()) {
        return;
    }

    g_client_pause.phase = payload.phase;
    g_client_pause.pausing_team = payload.pausing_team;
    g_client_pause.phase_end.set(static_cast<int>(payload.remaining_ms));
    sim_pause_set(SimPauseReason::match_timeout, payload.phase != AfMatchPausePhase::None);

    if (payload.phase == AfMatchPausePhase::Resuming) {
        rounds_client_set_countdown(static_cast<int>((payload.remaining_ms + 999) / 1000));
        g_client_pause.countdown_shown = true;
    }
    else if (g_client_pause.countdown_shown) {
        rounds_client_set_countdown(0);
        g_client_pause.countdown_shown = false;
    }
}

void match_pause_client_reset()
{
    if (g_client_pause.phase != AfMatchPausePhase::None || g_client_pause.countdown_shown) {
        match_pause_client_on_state(MatchPausePayload{});
    }
}

bool match_pause_client_paused()
{
    return g_client_pause.phase == AfMatchPausePhase::Paused;
}

bool match_pause_blocks_action(int action)
{
    if (!rf::is_multi || !sim_pause_active(SimPauseReason::match_timeout)) {
        return false;
    }
    switch (action) {
        case rf::CC_ACTION_CHAT:
        case rf::CC_ACTION_TEAM_CHAT:
        case rf::CC_ACTION_MESSAGES:
        case rf::CC_ACTION_MP_STATS:
            return false;
        default:
            break;
    }
    if (action < static_cast<int>(get_af_control(static_cast<rf::AlpineControlConfigAction>(0)))) {
        return true; // every other stock action acts on the frozen world
    }
    // Alpine actions that act on the world; menus, voting and spectating stay usable
    constexpr rf::AlpineControlConfigAction blocked[] = {
        rf::AF_ACTION_FLASHLIGHT,
        rf::AF_ACTION_SELF_KILL,
        rf::AF_ACTION_DROP_FLAG,
        rf::AF_ACTION_PING_LOCATION,
        rf::AF_ACTION_INSPECT_WEAPON,
        rf::AF_ACTION_SPRAY,
    };
    return std::ranges::any_of(blocked, [action](rf::AlpineControlConfigAction a) {
        return action == static_cast<int>(get_af_control(a));
    });
}

void match_pause_hud_render()
{
    if (g_client_pause.phase == AfMatchPausePhase::None || is_hud_effectively_hidden()) {
        return;
    }

    const bool resuming = g_client_pause.phase == AfMatchPausePhase::Resuming;
    const int secs_left = std::max((g_client_pause.phase_end.time_until() + 999) / 1000, 0);

    // The resume countdown's numbers are drawn by the stock final countdown
    const std::string title = resuming ? std::string{"RESUMING"}
                                       : std::format("TIMEOUT  {}", format_duration(secs_left));
    std::string hint;
    if (!resuming) {
        const std::string pause_key = get_action_bind_name(get_af_control(rf::AF_ACTION_MATCH_PAUSE));
        const std::string vote_key = get_action_bind_name(get_af_control(rf::AF_ACTION_VOTE_MENU));
        hint = std::format("{} timeout. Press {} to extend, or {} to vote to resume.",
                           pause_caller_name(g_client_pause.pausing_team), pause_key, vote_key);
    }

    const int big_font = hud_get_large_font();
    const int font = hud_get_default_font();
    const int center_x = rf::gr::screen_width() / 2;
    const int y = rf::gr::screen_height() / 4;

    rf::gr::set_color(0, 0, 0, 112);
    rf::gr::string_aligned(rf::gr::ALIGN_CENTER, center_x + 2, y + 2, title.c_str(), big_font);
    rf::gr::set_color(255, 255, 255, 225);
    rf::gr::string_aligned(rf::gr::ALIGN_CENTER, center_x, y, title.c_str(), big_font);
    if (!hint.empty()) {
        const int hint_y = y + rf::gr::get_font_height(big_font) + 4;
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, center_x, hint_y, hint.c_str(), font);
    }
}

void match_pause_on_limbo()
{
    if (rf::is_server) {
        match_pause_server_reset();
    }
    match_pause_client_reset();
}
