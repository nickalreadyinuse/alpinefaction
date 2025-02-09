#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include <patch_common/FunHook.h>
#include <patch_common/CallHook.h>
#include <xlog/xlog.h>
#include "../multi/multi.h"
#include "../input/input.h"
#include "../rf/input.h"
#include "../rf/hud.h"
#include "../rf/level.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/multi.h"
#include "../rf/player/player.h"
#include "../rf/os/frametime.h"
#include "../main/main.h"
#include "../graphics/gr.h"
#include "../misc/alpine_options.h"
#include "hud_internal.h"
#include "hud.h"

static bool g_big_team_scores_hud = false;
constexpr bool g_debug_team_scores_hud = false;
static bool g_draw_vote_notification = false;
static std::string g_active_vote_type = "";
static bool g_draw_ready_notification = false;
bool g_pre_match_active = false;
static ChatMenuType g_chat_menu_active = ChatMenuType::None;
static ChatMenuLevel g_chat_menu_level = ChatMenuLevel::Base;
ChatMenuMessages g_chat_menu_ctf_messages;
ChatMenuMessages g_chat_menu_tdm_messages;
ChatMenuMessages g_chat_menu_defense_messages;
ChatMenuMessages g_chat_menu_offense_messages;
ChatMenuMessages g_chat_menu_timing_messages;
ChatMenuMessages g_chat_menu_powerup_messages;
rf::TimestampRealtime g_chat_menu_timer;

const ChatMenuMessages chat_menu_ctf_message_defaults{
    .key1_msg = "Enemy going high!",
    .key2_msg = "Enemy going middle!",
    .key3_msg = "Enemy going low!",
    .key4_msg = "Enemy flag carrier is hiding!",
    .key5_msg = "Enemy base is empty",
    .key6_msg = "I will cover the flag steal",
    .key7_msg = "Covering our flag carrier",
    .key8_msg = "",
    .key9_msg = "",
    .short_key1_msg = "High!",
};

const ChatMenuMessages chat_menu_tdm_message_defaults{
    .key1_msg = "Enemies are going high!",
    .key2_msg = "Enemies are going middle!",
    .key3_msg = "Enemies are going low!",
    .key4_msg = "",
    .key5_msg = "",
    .key6_msg = "",
    .key7_msg = "",
    .key8_msg = "",
    .key9_msg = ""
};

const ChatMenuMessages chat_menu_defense_message_defaults{
    .key1_msg = "Enemy incoming high!",
    .key2_msg = "Enemy incoming middle!",
    .key3_msg = "Enemy incoming low!",
    .key4_msg = "Our base is safe",
    .key5_msg = "Enemies are in our base",
    .key6_msg = "",
    .key7_msg = "",
    .key8_msg = "",
    .key9_msg = ""
};

const ChatMenuMessages chat_menu_offense_message_defaults{
    .key1_msg = "Going high.",
    .key2_msg = "Going middle.",
    .key3_msg = "Going low.",
    .key4_msg = "Hiding until our base is safe",
    .key5_msg = "",
    .key6_msg = "",
    .key7_msg = "",
    .key8_msg = "",
    .key9_msg = ""
};

const ChatMenuMessages chat_menu_timing_message_defaults{
    .key1_msg = "Damage Amp respawning soon!",
    .key2_msg = "Fusion is respawning soon!",
    .key3_msg = "Super Armor is respawning soon!",
    .key4_msg = "Super Health is respawning soon!",
    .key5_msg = "Invulnerability is respawning soon!",
    .key6_msg = "Rail is respawning soon!",
    .key7_msg = "",
    .key8_msg = "",
    .key9_msg = ""
};

const ChatMenuMessages chat_menu_powerup_message_defaults{
    .key1_msg = "Damage Amp is up!",
    .key2_msg = "Fusion is up!",
    .key3_msg = "Super Armor is up!",
    .key4_msg = "Super Health is up!",
    .key5_msg = "Invulnerability is up!",
    .key6_msg = "Rail is up!",
    .key7_msg = "",
    .key8_msg = "",
    .key9_msg = ""
};

ChatMenuMessages g_chat_menu_taunt_messages;
ChatMenuMessages g_chat_menu_command_messages;
rf::TimestampRealtime g_taunt_timer;

const ChatMenuMessages chat_menu_taunt_message_defaults{
    .key1_msg = "Rest in pieces!",
    .key2_msg = "You make a nice target!",
    .key3_msg = "Squeegee time!",
    .key4_msg = "Nice catch!",
    .key5_msg = "Goodbye Mr. Gibs!",
    .key6_msg = "Me red, you dead!",
    .key7_msg = "Look! A jigsaw puzzle!",
    .key8_msg = "Damn, I'm good.",
    .key9_msg = "Sucks to be you!"
};

const ChatMenuMessages chat_menu_command_message_defaults{
    .key1_msg = "/vote next",
    .key2_msg = "/vote previous",
    .key3_msg = "/vote restart",
    .key4_msg = "/vote extend",
    .key5_msg = "/stats",
    .key6_msg = "/nextmap",
    .key7_msg = "/whosready",
    .key8_msg = "/matchinfo",
    .key9_msg = "/info"
};

namespace rf
{
    auto& hud_miniflag_red_bmh = addr_as_ref<int>(0x0059DF48);
    auto& hud_miniflag_blue_bmh = addr_as_ref<int>(0x0059DF4C);
    auto& hud_miniflag_hilight_bmh = addr_as_ref<int>(0x0059DF50);
    auto& hud_flag_red_bmh = addr_as_ref<int>(0x0059DF54);
    auto& hud_flag_blue_bmh = addr_as_ref<int>(0x0059DF58);
    auto& hud_flag_gr_mode = addr_as_ref<rf::gr::Mode>(0x01775B30);
}

void hud_render_team_scores()
{
    int clip_h = rf::gr::clip_height();
    rf::gr::set_color(0, 0, 0, 150);
    int box_w = g_big_team_scores_hud ? 370 : 185;
    int box_h = g_big_team_scores_hud ? 80 : 55;
    int box_x = 10;
    int box_y = clip_h - box_h - 10; // clip_h - 65
    int miniflag_x = box_x + 7; // 17
    int miniflag_label_x = box_x + (g_big_team_scores_hud ? 45 : 33); // 43
    int max_miniflag_label_w = box_w - (g_big_team_scores_hud ? 80 : 55);
    int red_miniflag_y = box_y + 4; // clip_h - 61
    int blue_miniflag_y = box_y + (g_big_team_scores_hud ? 42 : 30); // clip_h - 35
    int red_miniflag_label_y = red_miniflag_y + 4; // clip_h - 57
    int blue_miniflag_label_y = blue_miniflag_y + 4; // clip_h - 31
    int flag_x = g_big_team_scores_hud ? 410 : 205;
    float flag_scale = g_big_team_scores_hud ? 1.5f : 1.0f;

    rf::gr::rect(10, clip_h - box_h - 10, box_w, box_h);
    auto game_type = rf::multi_get_game_type();
    int font_id = hud_get_default_font();

    if (game_type == rf::NG_TYPE_CTF) {
        static float hud_flag_alpha = 255.0f;
        static bool hud_flag_pulse_dir = false;
        float delta_alpha = rf::frametime * 500.0f;
        if (hud_flag_pulse_dir) {
            hud_flag_alpha -= delta_alpha;
            if (hud_flag_alpha <= 50.0f) {
                hud_flag_alpha = 50.0f;
                hud_flag_pulse_dir = false;
            }
        }
        else {
            hud_flag_alpha += delta_alpha;
            if (hud_flag_alpha >= 255.0f) {
                hud_flag_alpha = 255.0f;
                hud_flag_pulse_dir = true;
            }
        }
        rf::gr::set_color(53, 207, 22, 255);
        rf::Player* red_flag_player = g_debug_team_scores_hud ? rf::local_player : rf::multi_ctf_get_red_flag_player();
        if (red_flag_player) {
            const char* name = red_flag_player->name;
            std::string fitting_name = hud_fit_string(name, max_miniflag_label_w, nullptr, font_id);
            rf::gr::string(miniflag_label_x, red_miniflag_label_y, fitting_name.c_str(), font_id);

            if (red_flag_player == rf::local_player) {
                rf::gr::set_color(255, 255, 255, static_cast<int>(hud_flag_alpha));
                hud_scaled_bitmap(rf::hud_flag_red_bmh, flag_x, box_y, flag_scale, rf::hud_flag_gr_mode);
            }
        }
        else if (rf::multi_ctf_is_red_flag_in_base()) {
            rf::gr::string(miniflag_label_x, red_miniflag_label_y, "at base", font_id);
        }
        else {
            rf::gr::string(miniflag_label_x, red_miniflag_label_y, "missing", font_id);
        }
        rf::gr::set_color(53, 207, 22, 255);
        rf::Player* blue_flag_player = rf::multi_ctf_get_blue_flag_player();
        if (blue_flag_player) {
            const char* name = blue_flag_player->name;
            std::string fitting_name = hud_fit_string(name, max_miniflag_label_w, nullptr, font_id);
            rf::gr::string(miniflag_label_x, blue_miniflag_label_y, fitting_name.c_str(), font_id);

            if (blue_flag_player == rf::local_player) {
                rf::gr::set_color(255, 255, 255, static_cast<int>(hud_flag_alpha));
                hud_scaled_bitmap(rf::hud_flag_blue_bmh, flag_x, box_y, flag_scale, rf::hud_flag_gr_mode);
            }
        }
        else if (rf::multi_ctf_is_blue_flag_in_base()) {
            rf::gr::string(miniflag_label_x, blue_miniflag_label_y, "at base", font_id);
        }
        else {
            rf::gr::string(miniflag_label_x, blue_miniflag_label_y, "missing", font_id);
        }
    }

    if (game_type == rf::NG_TYPE_CTF || game_type == rf::NG_TYPE_TEAMDM) {
        float miniflag_scale = g_big_team_scores_hud ? 1.5f : 1.0f;
        rf::gr::set_color(255, 255, 255, 255);
        if (rf::local_player) {
            int miniflag_hilight_y;
            if (rf::local_player->team == rf::TEAM_RED) {
                miniflag_hilight_y = red_miniflag_y;
            }
            else {
                miniflag_hilight_y = blue_miniflag_y;
            }
            hud_scaled_bitmap(rf::hud_miniflag_hilight_bmh, miniflag_x, miniflag_hilight_y, miniflag_scale, rf::hud_flag_gr_mode);
        }
        hud_scaled_bitmap(rf::hud_miniflag_red_bmh, miniflag_x, red_miniflag_y, miniflag_scale, rf::hud_flag_gr_mode);
        hud_scaled_bitmap(rf::hud_miniflag_blue_bmh, miniflag_x, blue_miniflag_y, miniflag_scale, rf::hud_flag_gr_mode);
    }

    int red_score, blue_score;
    if (g_debug_team_scores_hud) {
        red_score = 15;
        blue_score = 15;
    }
    else if (game_type == rf::NG_TYPE_CTF) {
        red_score = rf::multi_ctf_get_red_team_score();
        blue_score = rf::multi_ctf_get_blue_team_score();
    }
    else if (game_type == rf::NG_TYPE_TEAMDM) {
        rf::gr::set_color(53, 207, 22, 255);
        red_score = rf::multi_tdm_get_red_team_score();
        blue_score = rf::multi_tdm_get_blue_team_score();
    }
    else {
        red_score = 0;
        blue_score = 0;
    }
    auto red_score_str = std::to_string(red_score);
    auto blue_score_str = std::to_string(blue_score);
    int str_w, str_h;
    rf::gr::get_string_size(&str_w, &str_h, red_score_str.c_str(), -1, font_id);
    rf::gr::string(box_x + box_w - 5 - str_w, red_miniflag_label_y, red_score_str.c_str(), font_id);
    rf::gr::get_string_size(&str_w, &str_h, blue_score_str.c_str(), -1, font_id);
    rf::gr::string(box_x + box_w - 5 - str_w, blue_miniflag_label_y, blue_score_str.c_str(), font_id);
}

CallHook<void(int, int, int, rf::gr::Mode)> hud_render_power_ups_gr_bitmap_hook{
    {
        0x0047FF2F,
        0x0047FF96,
        0x0047FFFD,
    },
    [](int bm_handle, int x, int y, rf::gr::Mode mode) {
        float scale = g_game_config.big_hud ? 2.0f : 1.0f;
        x = hud_transform_value(x, 640, rf::gr::clip_width());
        x = hud_scale_value(x, rf::gr::clip_width(), scale);
        y = hud_scale_value(y, rf::gr::clip_height(), scale);
        hud_scaled_bitmap(bm_handle, x, y, scale, mode);
    },
};

FunHook<void()> render_level_info_hook{
    0x00477180,
    []() {
        gr_font_run_with_default(hud_get_default_font(), [&]() {
            render_level_info_hook.call_target();
        });
    },
};

FunHook<void()> multi_hud_init_hook{
    0x00476AD0,
    []() {
        // Change font for Time Left text
        static int time_left_font = rf::gr::load_font("rfpc-large.vf");
        if (time_left_font >= 0) {
            write_mem<i8>(0x00477157 + 1, time_left_font);
        }
        multi_hud_init_hook.call_target();
    },
};

void hud_render_ready_notification()
{
    std::string ready_key_text =
        get_action_bind_name(get_af_control(rf::AlpineControlConfigAction::AF_ACTION_READY));

    std::string ready_notification_text =
        "Press " + ready_key_text + " to ready up for the match";

    rf::gr::set_color(255, 255, 255, 225);
    int center_x = rf::gr::screen_width() / 2;
    int notification_y = static_cast<int>(rf::gr::screen_height() * 0.25f);
    rf::gr::string_aligned(rf::gr::ALIGN_CENTER, center_x, notification_y, ready_notification_text.c_str(), 0);
}

void draw_hud_ready_notification(bool draw)
{
    draw ? g_draw_ready_notification = true : g_draw_ready_notification = false;
}

void set_local_pre_match_active(bool set_active) {
    set_active ? g_pre_match_active = true : g_pre_match_active = false;

    draw_hud_ready_notification(set_active);
}

void hud_render_vote_notification()
{
    std::string vote_yes_key_text =
        get_action_bind_name(get_af_control(rf::AlpineControlConfigAction::AF_ACTION_VOTE_YES));

    std::string vote_no_key_text =
        get_action_bind_name(get_af_control(rf::AlpineControlConfigAction::AF_ACTION_VOTE_NO));

    std::string vote_notification_text =
        "ACTIVE QUESTION: \n" + g_active_vote_type + "\n\n" + vote_yes_key_text + " to vote yes\n" + vote_no_key_text + " to vote no";

    rf::gr::set_color(255, 255, 255, 225);
    int notification_y = static_cast<int>(rf::gr::screen_height() * 0.25f);
    rf::gr::string_aligned(rf::gr::ALIGN_LEFT, 8, notification_y, vote_notification_text.c_str(), 0);
}

void draw_hud_vote_notification(std::string vote_type)
{
    if (!vote_type.empty()) {
        g_draw_vote_notification = true;
        g_active_vote_type = vote_type;
    }
}

void remove_hud_vote_notification()
{
    g_draw_vote_notification = false;
    g_active_vote_type = "";
}

CodeInjection hud_render_patch_alpine {
    0x00476D9D,
    [](auto& regs) {
        if (g_draw_vote_notification) {
            hud_render_vote_notification();
        }

        if (g_draw_ready_notification) {
            hud_render_ready_notification();
        }

        if (g_chat_menu_active != ChatMenuType::None) {
            hud_render_draw_chat_menu();

            if (!g_chat_menu_timer.valid() || g_chat_menu_timer.elapsed()) {
                toggle_chat_menu(ChatMenuType::None);
            }
        }
    }
};

void build_chat_menu_comms_messages() {
    // CTF gametype messages
    static const std::array<std::pair<AlpineLevelInfoID, std::string ChatMenuMessages::*>, 9> ctf_keys = {{
        {AlpineLevelInfoID::ChatCTF1, &ChatMenuMessages::key1_msg},
        {AlpineLevelInfoID::ChatCTF2, &ChatMenuMessages::key2_msg},
        {AlpineLevelInfoID::ChatCTF3, &ChatMenuMessages::key3_msg},
        {AlpineLevelInfoID::ChatCTF4, &ChatMenuMessages::key4_msg},
        {AlpineLevelInfoID::ChatCTF5, &ChatMenuMessages::key5_msg},
        {AlpineLevelInfoID::ChatCTF6, &ChatMenuMessages::key6_msg},
        {AlpineLevelInfoID::ChatCTF7, &ChatMenuMessages::key7_msg},
        {AlpineLevelInfoID::ChatCTF8, &ChatMenuMessages::key8_msg},
        {AlpineLevelInfoID::ChatCTF9, &ChatMenuMessages::key9_msg}
    }};

    static const std::array<std::string ChatMenuMessages::*, 9> ctf_short_keys = {{
        &ChatMenuMessages::short_key1_msg, &ChatMenuMessages::short_key2_msg, &ChatMenuMessages::short_key3_msg,
        &ChatMenuMessages::short_key4_msg, &ChatMenuMessages::short_key5_msg, &ChatMenuMessages::short_key6_msg,
        &ChatMenuMessages::short_key7_msg, &ChatMenuMessages::short_key8_msg, &ChatMenuMessages::short_key9_msg
    }};

    for (size_t i = 0; i < ctf_keys.size(); ++i) {
        g_chat_menu_ctf_messages.*ctf_keys[i].second =
            g_alpine_level_info_config.is_option_loaded(rf::level.filename, ctf_keys[i].first)
            ? get_level_info_value<std::string>(rf::level.filename, ctf_keys[i].first)
            : chat_menu_ctf_message_defaults.*ctf_keys[i].second;

        // Copy short messages from defaults (if they exist)
        g_chat_menu_ctf_messages.*ctf_short_keys[i] = chat_menu_ctf_message_defaults.*ctf_short_keys[i];
    }

    // TeamDM gametype messages
    static const std::array<std::pair<AlpineLevelInfoID, std::string ChatMenuMessages::*>, 9> teamdm_keys = {{
        {AlpineLevelInfoID::ChatTeamDM1, &ChatMenuMessages::key1_msg},
        {AlpineLevelInfoID::ChatTeamDM2, &ChatMenuMessages::key2_msg},
        {AlpineLevelInfoID::ChatTeamDM3, &ChatMenuMessages::key3_msg},
        {AlpineLevelInfoID::ChatTeamDM4, &ChatMenuMessages::key4_msg},
        {AlpineLevelInfoID::ChatTeamDM5, &ChatMenuMessages::key5_msg},
        {AlpineLevelInfoID::ChatTeamDM6, &ChatMenuMessages::key6_msg},
        {AlpineLevelInfoID::ChatTeamDM7, &ChatMenuMessages::key7_msg},
        {AlpineLevelInfoID::ChatTeamDM8, &ChatMenuMessages::key8_msg},
        {AlpineLevelInfoID::ChatTeamDM9, &ChatMenuMessages::key9_msg}
    }};

    static const std::array<std::string ChatMenuMessages::*, 9> tdm_short_keys = {{
        &ChatMenuMessages::short_key1_msg, &ChatMenuMessages::short_key2_msg, &ChatMenuMessages::short_key3_msg,
        &ChatMenuMessages::short_key4_msg, &ChatMenuMessages::short_key5_msg, &ChatMenuMessages::short_key6_msg,
        &ChatMenuMessages::short_key7_msg, &ChatMenuMessages::short_key8_msg, &ChatMenuMessages::short_key9_msg
    }};

    for (size_t i = 0; i < teamdm_keys.size(); ++i) {
        g_chat_menu_tdm_messages.*teamdm_keys[i].second =
            g_alpine_level_info_config.is_option_loaded(rf::level.filename, teamdm_keys[i].first)
            ? get_level_info_value<std::string>(rf::level.filename, teamdm_keys[i].first)
            : chat_menu_tdm_message_defaults.*teamdm_keys[i].second;

        // Copy short messages from defaults (if they exist)
        g_chat_menu_tdm_messages.*tdm_short_keys[i] = chat_menu_tdm_message_defaults.*tdm_short_keys[i];
    }
}

void build_chat_menu_clientside_messages() {
    // List of message types and their defaults
    struct MessageType {
        ChatMenuMessages& messages;
        const ChatMenuMessages& defaults;
    };

    std::array<MessageType, 5> message_types = {{
        {g_chat_menu_defense_messages, chat_menu_defense_message_defaults},
        {g_chat_menu_offense_messages, chat_menu_offense_message_defaults},
        {g_chat_menu_timing_messages, chat_menu_timing_message_defaults},
        {g_chat_menu_powerup_messages, chat_menu_powerup_message_defaults},
        {g_chat_menu_taunt_messages, chat_menu_taunt_message_defaults}
    }};

    // Loop through each message type
    for (auto& [messages, defaults] : message_types) {
        static const std::array<std::string ChatMenuMessages::*, 9> keys = {{
            &ChatMenuMessages::key1_msg, &ChatMenuMessages::key2_msg, &ChatMenuMessages::key3_msg,
            &ChatMenuMessages::key4_msg, &ChatMenuMessages::key5_msg, &ChatMenuMessages::key6_msg,
            &ChatMenuMessages::key7_msg, &ChatMenuMessages::key8_msg, &ChatMenuMessages::key9_msg
        }};

        static const std::array<std::string ChatMenuMessages::*, 9> short_keys = {{
            &ChatMenuMessages::short_key1_msg, &ChatMenuMessages::short_key2_msg, &ChatMenuMessages::short_key3_msg,
            &ChatMenuMessages::short_key4_msg, &ChatMenuMessages::short_key5_msg, &ChatMenuMessages::short_key6_msg,
            &ChatMenuMessages::short_key7_msg, &ChatMenuMessages::short_key8_msg, &ChatMenuMessages::short_key9_msg
        }};

        for (size_t i = 0; i < keys.size(); ++i) {
            messages.*keys[i] = defaults.*keys[i];
            messages.*short_keys[i] = defaults.*short_keys[i]; // Load short messages
        }
    }

    // Chat commands
    static const std::array<std::pair<AlpineOptionID, std::string ChatMenuMessages::*>, 9> command_keys = {{
        {AlpineOptionID::ChatCommand1, &ChatMenuMessages::key1_msg},
        {AlpineOptionID::ChatCommand2, &ChatMenuMessages::key2_msg},
        {AlpineOptionID::ChatCommand3, &ChatMenuMessages::key3_msg},
        {AlpineOptionID::ChatCommand4, &ChatMenuMessages::key4_msg},
        {AlpineOptionID::ChatCommand5, &ChatMenuMessages::key5_msg},
        {AlpineOptionID::ChatCommand6, &ChatMenuMessages::key6_msg},
        {AlpineOptionID::ChatCommand7, &ChatMenuMessages::key7_msg},
        {AlpineOptionID::ChatCommand8, &ChatMenuMessages::key8_msg},
        {AlpineOptionID::ChatCommand9, &ChatMenuMessages::key9_msg}
    }};

    static const std::array<std::string ChatMenuMessages::*, 9> command_short_keys = {{
        &ChatMenuMessages::short_key1_msg, &ChatMenuMessages::short_key2_msg, &ChatMenuMessages::short_key3_msg,
        &ChatMenuMessages::short_key4_msg, &ChatMenuMessages::short_key5_msg, &ChatMenuMessages::short_key6_msg,
        &ChatMenuMessages::short_key7_msg, &ChatMenuMessages::short_key8_msg, &ChatMenuMessages::short_key9_msg
    }};

    for (size_t i = 0; i < command_keys.size(); ++i) {
        const auto& [id, key_ptr] = command_keys[i];

        g_chat_menu_command_messages.*key_ptr =
            g_alpine_options_config.is_option_loaded(id)
            ? get_option_value<std::string>(id)
            : chat_menu_command_message_defaults.*key_ptr;

        // Copy short messages from defaults (if they exist)
        g_chat_menu_command_messages.*command_short_keys[i] = chat_menu_command_message_defaults.*command_short_keys[i];
    }
}

void draw_chat_menu_text(int x, int y)
{
    // Determine the menu title and relevant message source
    std::string main_string;
    const ChatMenuMessages* message_source = nullptr;

    if (g_chat_menu_active == ChatMenuType::Comms) {
        if (g_chat_menu_level == ChatMenuLevel::Base) {
            // Base menu with sub-options
            main_string = "TEAM COMMUNICATION\n\n";
            main_string += (rf::multi_get_game_type() == rf::NG_TYPE_TEAMDM) ? "1: TEAM DEATHMATCH\n" : "1: CAPTURE THE FLAG\n";
            main_string += "2: DEFENSE\n";
            main_string += "3: OFFENSE\n";
            main_string += "4: TIMING\n";
            main_string += "5: POWERUPS\n";
        }
        else {
            // Submenu handling
            switch (g_chat_menu_level) {
                case ChatMenuLevel::Gametype:
                    main_string = (rf::multi_get_game_type() == rf::NG_TYPE_TEAMDM) ? "TEAM DEATHMATCH\n\n" : "CAPTURE THE FLAG\n\n";
                    message_source = (rf::multi_get_game_type() == rf::NG_TYPE_TEAMDM) ? &g_chat_menu_tdm_messages : &g_chat_menu_ctf_messages;
                    break;
                case ChatMenuLevel::Defense:
                    main_string = "DEFENSE\n\n";
                    message_source = &g_chat_menu_defense_messages;
                    break;
                case ChatMenuLevel::Offense:
                    main_string = "OFFENSE\n\n";
                    message_source = &g_chat_menu_offense_messages;
                    break;
                case ChatMenuLevel::Timing:
                    main_string = "TIMING\n\n";
                    message_source = &g_chat_menu_timing_messages;
                    break;
                case ChatMenuLevel::Powerup:
                    main_string = "POWERUP\n\n";
                    message_source = &g_chat_menu_powerup_messages;
                    break;
                default:
                    xlog::warn("Invalid Comms sub-menu level: {}", static_cast<int>(g_chat_menu_level));
                    return;
            }
        }
    }
    else {
        // Taunts or Commands (flat menu)
        switch (g_chat_menu_active) {
            case ChatMenuType::Taunts:
                main_string = "TAUNTS\n\n";
                message_source = &g_chat_menu_taunt_messages;
                break;
            case ChatMenuType::Commands:
                main_string = "COMMANDS\n\n";
                message_source = &g_chat_menu_command_messages;
                break;
            default:
                xlog::warn("Invalid chat menu state: {}", static_cast<int>(g_chat_menu_active));
                return;
        }
    }

    // Add messages if applicable
    if (message_source) {
        const auto& messages = *message_source;
        std::vector<std::pair<std::string_view, std::string_view>> message_list = {
            {messages.key1_msg, messages.short_key1_msg},
            {messages.key2_msg, messages.short_key2_msg},
            {messages.key3_msg, messages.short_key3_msg},
            {messages.key4_msg, messages.short_key4_msg},
            {messages.key5_msg, messages.short_key5_msg},
            {messages.key6_msg, messages.short_key6_msg},
            {messages.key7_msg, messages.short_key7_msg},
            {messages.key8_msg, messages.short_key8_msg},
            {messages.key9_msg, messages.short_key9_msg}
        };

        std::ostringstream main_string_stream;
        int line_number = 1;
        constexpr size_t max_length = 18; // Truncate long messages

        for (const auto& [long_msg, short_msg] : message_list) {
            if (!long_msg.empty()) {
                std::string display_msg = (!short_msg.empty()) ? std::string(short_msg) : std::string(long_msg);
                if (display_msg.length() > max_length) {
                    display_msg = display_msg.substr(0, max_length) + "...";
                }

                main_string_stream << line_number << ": " << display_msg << '\n';
                ++line_number;
            } else {
                main_string_stream << '\n';
            }
        }
        main_string += main_string_stream.str();
    }

    main_string += "\n0: EXIT\n";

    //xlog::warn("Final menu string:\n{}", main_string);

    // Draw the menu
    int str_x = x + 4;
    int str_y = y + 4;
    rf::gr::set_color(255, 255, 180, 0xCC);
    rf::gr::string_aligned(rf::gr::ALIGN_LEFT, str_x, str_y, main_string.c_str(), 1);
}

void hud_render_draw_chat_menu() {
    int w = static_cast<int>(200);
    int h = static_cast<int>(166);
    int x = static_cast<int>(10);
    int y = (static_cast<int>(rf::gr::screen_height()) - h) / 2;
    rf::gr::set_color(0, 0, 0, 0x80);
    rf::gr::rect(x, y, w, h);
    rf::gr::set_color(79, 216, 255, 0x80);
    rf::gr::rect_border(x, y, w, h);

    draw_chat_menu_text(x, y);
}

void set_chat_menu_state(ChatMenuType state) {
    g_chat_menu_active = state;
    g_chat_menu_level = ChatMenuLevel::Base;

    if (g_chat_menu_active == ChatMenuType::None) {
        g_chat_menu_timer.invalidate();
    }
}

void toggle_chat_menu(ChatMenuType state) {
    if (g_chat_menu_active == state) {
        set_chat_menu_state(ChatMenuType::None);
    }
    else {
        if (state == ChatMenuType::Comms && rf::multi_get_game_type() == rf::NG_TYPE_DM) {
            set_chat_menu_state(ChatMenuType::None); // no team comms menu in DM
        }
        else {
            g_chat_menu_timer.set(5000); // 5 seconds timeout
            set_chat_menu_state(state);
        }
    }
}

bool get_chat_menu_is_active() {
    return g_chat_menu_active != ChatMenuType::None;
}

int get_chat_menu_level() {
    return static_cast<int>(g_chat_menu_level);
}

// call only when chat menu is active // disappearing base menu on 0 idk
void chat_menu_action_handler(rf::Key key) {
    g_chat_menu_timer.set(5000); // 5 seconds timeout

    if (g_chat_menu_active == ChatMenuType::Comms) {
        // Handle Comms menu navigation
        if (g_chat_menu_level == ChatMenuLevel::Base) {
            static const std::unordered_map<rf::Key, ChatMenuLevel> levelMapping = {
                {rf::KEY_1, ChatMenuLevel::Gametype},
                {rf::KEY_2, ChatMenuLevel::Defense},
                {rf::KEY_3, ChatMenuLevel::Offense},
                {rf::KEY_4, ChatMenuLevel::Timing},
                {rf::KEY_5, ChatMenuLevel::Powerup}
            };

            if (auto it = levelMapping.find(key); it != levelMapping.end()) {
                g_chat_menu_level = it->second;
            }
            return;
        }
    }

    // Determine which message set to use
    const ChatMenuMessages* menu_messages = nullptr;
    volatile bool use_team_chat = false;

    if (g_chat_menu_active == ChatMenuType::Comms) {
        switch (g_chat_menu_level) {
            case ChatMenuLevel::Gametype:
            menu_messages =
                (rf::multi_get_game_type() == rf::NG_TYPE_TEAMDM) ? &g_chat_menu_tdm_messages : &g_chat_menu_ctf_messages;
                break;
            case ChatMenuLevel::Defense:
                menu_messages = &g_chat_menu_defense_messages;
                break;
            case ChatMenuLevel::Offense:
                menu_messages = &g_chat_menu_offense_messages;
                break;
            case ChatMenuLevel::Timing:
                menu_messages = &g_chat_menu_timing_messages;
                break;
            case ChatMenuLevel::Powerup:
                menu_messages = &g_chat_menu_powerup_messages;
                break;
            default:
                return;
        }
        use_team_chat = true;
    }
    else if (g_chat_menu_active == ChatMenuType::Taunts) {
        menu_messages = &g_chat_menu_taunt_messages;
        use_team_chat = false;
    }
    else if (g_chat_menu_active == ChatMenuType::Commands) {
        menu_messages = &g_chat_menu_command_messages;
        use_team_chat = false;
    }

    if (menu_messages) {
        static const std::unordered_map<rf::Key, std::string ChatMenuMessages::*> key_map = {
            {rf::KEY_1, &ChatMenuMessages::key1_msg},
            {rf::KEY_2, &ChatMenuMessages::key2_msg},
            {rf::KEY_3, &ChatMenuMessages::key3_msg},
            {rf::KEY_4, &ChatMenuMessages::key4_msg},
            {rf::KEY_5, &ChatMenuMessages::key5_msg},
            {rf::KEY_6, &ChatMenuMessages::key6_msg},
            {rf::KEY_7, &ChatMenuMessages::key7_msg},
            {rf::KEY_8, &ChatMenuMessages::key8_msg},
            {rf::KEY_9, &ChatMenuMessages::key9_msg}
        };

        if (auto it = key_map.find(key); it != key_map.end()) {
            if (g_chat_menu_active == ChatMenuType::Taunts) {
                if (!g_taunt_timer.valid() || g_taunt_timer.elapsed()) {
                    g_taunt_timer.set(10000); // 10 seconds between taunts
                    const std::string& msg = "[Taunt] " + menu_messages->*it->second;
                    if (!msg.empty()) {
                        rf::multi_chat_say(msg.c_str(), use_team_chat);
                    }
                }
                else {
                    rf::String msg{"You must wait a little while between taunts"};
                    rf::String prefix;
                    rf::multi_chat_print(msg, rf::ChatMsgColor::white_white, prefix);
                }
            }
            else {
                const std::string& msg = menu_messages->*it->second;
                if (!msg.empty()) {
                    rf::multi_chat_say(msg.c_str(), use_team_chat);
                    rf::snd_play(4, 0, 0.0f, 1.0f);
                }
            }
            
        }
    }

    // close window after doing
    toggle_chat_menu(ChatMenuType::None);
}


void multi_hud_apply_patches()
{
    hud_render_patch_alpine.install();
    AsmWriter{0x00477790}.jmp(hud_render_team_scores);
    hud_render_power_ups_gr_bitmap_hook.install();
    render_level_info_hook.install();
    multi_hud_init_hook.install();

    // Change position of Time Left text
    write_mem<i8>(0x0047715F + 2, 21);
    write_mem<i32>(0x00477168 + 1, 154);
}

void multi_hud_set_big(bool is_big)
{
    g_big_team_scores_hud = is_big;
}
