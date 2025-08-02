#include <string_view>
#include <map>
#include <set>
#include <ctime>
#include <format>
#include "../rf/player/player.h"
#include "../rf/level.h"
#include "../rf/multi.h"
#include "../rf/gameseq.h"
#include "../rf/misc.h"
#include "../os/console.h"
#include "../misc/player.h"
#include "../main/main.h"
#include <common/utils/list-utils.h>
#include <xlog/xlog.h>
#include "server_internal.h"
#include "multi.h"

MatchInfo g_match_info;

bool ends_with(const rf::String& str, const std::string& suffix)
{
    std::string name_str = str.c_str();
    if (name_str.length() >= suffix.length()) {
        return (name_str.compare(name_str.length() - suffix.length(), suffix.length(), suffix) == 0);
    }
    return false;
}

enum class VoteType
{    
    Kick,
    Level,
    Restart,
    Extend,
    Next,
    Random,
    Previous,
    Match,
    CancelMatch,
    Unknown
};

struct Vote
{
private:
    std::time_t start_time = 0;
    bool reminder_sent = false;
    std::map<rf::Player*, bool> players_who_voted;
    rf::Player* owner;

public:
    virtual ~Vote() = default;

    virtual VoteType get_type() const = 0;    

    bool start(std::string_view arg, rf::Player* source)
    {
        if (!process_vote_arg(arg, source)) {
            return false;
        }
        owner = source;
        send_vote_starting_msg(source);

        start_time = std::time(nullptr);

        players_who_voted.insert({source, true});

        return check_for_early_vote_finish();
    }

    virtual bool on_player_leave(rf::Player* player)
    {        
        if (player == owner) {
            send_chat_line_packet("\xA6 Vote canceled: owner left the game!", nullptr);
            return false;
        }
        players_who_voted.erase(player);
        return check_for_early_vote_finish();
    }

    [[nodiscard]] virtual bool is_allowed_in_limbo_state() const
    {
        return true;
    }

    bool add_player_vote(bool is_yes_vote, rf::Player* source)
    {
        if (players_who_voted.count(source) == 1) {
            send_chat_line_packet("You already voted!", source);
        }
        else {
            players_who_voted[source] = is_yes_vote;

            const auto current_player_list = get_current_player_list(false);

            auto yes_votes = std::count_if(players_who_voted.begin(), players_who_voted.end(), [](const auto& pair) {
                return pair.second;
            });
            auto no_votes = players_who_voted.size() - yes_votes;

            auto msg = std::format("\xA6 Vote status: Yes: {} No: {} Waiting: {}", yes_votes, no_votes,
                                   current_player_list.size() - players_who_voted.size());
            send_chat_line_packet(msg.c_str(), nullptr);
            return check_for_early_vote_finish();
        }
        return true;
    }

    bool do_frame()
    {
        const auto& vote_config = get_config();
        std::time_t passed_time_sec = std::time(nullptr) - start_time;
        if (passed_time_sec >= vote_config.time_limit_seconds) {
            send_chat_line_packet("\xA6 Vote timed out!", nullptr);
            return false;
        }
        if (passed_time_sec >= vote_config.time_limit_seconds / 2 && !reminder_sent) {
            const auto current_player_list = get_current_player_list(false);

            for (rf::Player* player : current_player_list) {
                if (players_who_voted.find(player) == players_who_voted.end()) {
                    if (!get_player_additional_data(player).is_alpine) { // don't send reminder pings to alpine clients
                        send_chat_line_packet("\xA6 Send message \"/vote yes\" or \"/vote no\" to vote.", player);
                    }
                }
            }
            reminder_sent = true;
        }
        return true;
    }

    bool try_cancel_vote(rf::Player* source)
    {
        if (owner != source) {
            send_chat_line_packet("You cannot cancel a vote you didn't start!", source);
            return false;
        }

        send_chat_line_packet("\xA6 Vote canceled!", nullptr);
        return true;
    }

protected:
    [[nodiscard]] virtual std::string get_title() const = 0;
    [[nodiscard]] virtual const VoteConfig& get_config() const = 0;

    virtual bool process_vote_arg([[maybe_unused]] std::string_view arg, [[maybe_unused]] rf::Player* source)
    {
        return true;
    }

    virtual void on_accepted()
    {
        send_chat_line_packet("\xA6 Vote passed!", nullptr);
    }

    virtual void on_rejected()
    {
        send_chat_line_packet("\xA6 Vote failed!", nullptr);
    }

    void send_vote_starting_msg(rf::Player* source)
    {
        if (!source) {
            return; // should never happen
        }

        auto title = get_title();
        std::string base_msg = std::format("{} vote started by {}.\n", title, source->name);

        // Notify the player who started the vote
        send_chat_line_packet(base_msg.c_str(), source);

        // Prepare messages for other players
        std::string msg_non_alpine = "\n=============== VOTE STARTING ===============\n" + base_msg +
                                     "Send message \"/vote yes\" or \"/vote no\" to participate.";

        std::string msg_alpine = "\n=============== VOTE STARTING ===============\n" + base_msg;

        // Send the message to other players
        for (rf::Player* player : get_current_player_list(false)) {
            if (!player || player == source) {
                continue; // skip the player who started the vote
            }

            const std::string& message_to_send =
                get_player_additional_data(player).is_alpine ? msg_alpine : msg_non_alpine;

            send_chat_line_packet(message_to_send.c_str(), player);
        }
    }

    void finish_vote(bool is_accepted)
    {
        if (is_accepted) {
            on_accepted();
        }
        else {
            on_rejected();
        }
    }

    bool check_for_early_vote_finish()
    {
        const auto current_player_list = get_current_player_list(false);

        // cancel the vote if the server clears out
        if (current_player_list.empty()) {
            return false;
        }

        const int remaining_players_count =
            std::count_if(current_player_list.begin(), current_player_list.end(), [this](rf::Player* player) {
                return players_who_voted.find(player) == players_who_voted.end();
            });

        const int yes_votes = std::count_if(players_who_voted.begin(), players_who_voted.end(), [](const auto& pair) {
            return pair.second;
        });

        const int no_votes =
            players_who_voted.size() - yes_votes;

        // success
        if (yes_votes > no_votes + remaining_players_count) {
            finish_vote(true);
            return false;
        }

        // failure
        if (no_votes > yes_votes + remaining_players_count) {
            finish_vote(false);
            return false;
        }

        // tie (failure)
        if (remaining_players_count == 0) {
            finish_vote(yes_votes > no_votes);
            return false;
        }

        return true;
    }
};

std::tuple<int, bool, std::string> parse_match_vote_info(std::string_view arg)
{
    // validate match param
    if (arg.length() < 3 || arg[1] != 'v' || arg[0] < '1' || arg[0] > '8' || arg[2] != arg[0]) {
        return {-1, false, ""};
    }

    int a_size = arg[0] - '0';

    // no level param
    if (arg[3] != ' ') {
        return {a_size, false, ""};
    }

    std::string level_name;
    bool valid_level;

    level_name = arg.substr(4);
    std::tie(valid_level, level_name) = is_level_name_valid(level_name); 

    return {a_size, valid_level, level_name};
}

struct VoteMatch : public Vote
{
    VoteType get_type() const override
    {
        return VoteType::Match;
    }

    bool process_vote_arg(std::string_view arg, rf::Player* source) override
    {
        if (rf::multi_get_game_type() == rf::NG_TYPE_DM) {
            send_chat_line_packet("\xA6 Match system is not available in deathmatch!", source);
            return false;
        }

        if (arg.empty()) {
            send_chat_line_packet("\xA6 You must specify a match size. Supported sizes are 1v1 - 8v8.", source);
            return false;
        }

        //g_match_info.team_size = parse_match_team_size(arg);
        auto [team_size, valid_level, match_level_name] = parse_match_vote_info(arg);
        g_match_info.team_size = team_size;        

        if (valid_level) {
            g_match_info.match_level_name = match_level_name;
        }
        else if (match_level_name == "") {
            g_match_info.match_level_name = rf::level.filename.c_str();
        }
        else {
            send_chat_line_packet("\xA6 Invalid level specified! Try again, or omit level filename to use the current level.", source);
            return false;
        }

        //rf::File::find(g_match_info.match_level_name.c_str());

        if (g_match_info.team_size == -1) {
            send_chat_line_packet("\xA6 Invalid match size! Supported sizes are 1v1 up to 8v8.", source);
            return false;
        }

        return true;
    }

    [[nodiscard]] std::string get_title() const override
    {
        return std::format("START {}v{} MATCH on {}",
            g_match_info.team_size, g_match_info.team_size, g_match_info.match_level_name);      
    }

    void on_accepted() override
    {
        auto msg = std::format("\xA6 Vote passed. {}.",
            g_match_info.match_level_name == rf::level.filename.c_str()
            ? "Entering pre-match ready up phase" : "Changing to match level, then entering pre-match ready up phase");
        send_chat_line_packet(msg.c_str(), nullptr);

        g_match_info.pre_match_queued = true;

        if (g_match_info.match_level_name == rf::level.filename.c_str()) {
            start_pre_match();
        }
        else if (!g_match_info.match_level_name.empty()) {
            rf::multi_change_level(g_match_info.match_level_name.c_str());
        }      
    }

    bool on_player_leave(rf::Player* player) override
    {        
        return Vote::on_player_leave(player);
    }

    [[nodiscard]] const VoteConfig& get_config() const override
    {
        return g_alpine_server_config.alpine_restricted_config.vote_match;
    }
};

struct VoteCancelMatch : public Vote
{
    VoteType get_type() const override
    {
        return VoteType::CancelMatch;
    }

    [[nodiscard]] std::string get_title() const override
    {
        return "CANCEL CURRENT MATCH";
    }

    bool process_vote_arg(std::string_view arg, rf::Player* source) override
    {
        if (!g_match_info.match_active && !g_match_info.pre_match_active) {
            send_chat_line_packet("\xA6 No active or queued match to cancel.", source);
            return false;
        }

        return true;
    }

    void on_accepted() override
    {
        send_chat_line_packet("\xA6 Vote passed: The match has been canceled.", nullptr);

        cancel_match();
    }

    [[nodiscard]] const VoteConfig& get_config() const override
    {
        return g_alpine_server_config.alpine_restricted_config.vote_match;
    }
};


struct VoteKick : public Vote
{
    rf::Player* m_target_player;

    VoteType get_type() const override
    {
        return VoteType::Kick;
    }

    bool process_vote_arg(std::string_view arg, [[ maybe_unused ]] rf::Player* source) override
    {
        std::string player_name{arg};
        m_target_player = find_best_matching_player(player_name.c_str());
        return m_target_player != nullptr;
    }

    [[nodiscard]] std::string get_title() const override
    {
        return std::format("KICK PLAYER '{}'", m_target_player->name);
    }

    void on_accepted() override
    {
        send_chat_line_packet("\xA6 Vote passed: kicking player", nullptr);
        rf::multi_kick_player(m_target_player);
    }

    bool on_player_leave(rf::Player* player) override
    {
        if (m_target_player == player) {
            return false;
        }
        return Vote::on_player_leave(player);
    }

    [[nodiscard]] const VoteConfig& get_config() const override
    {
        return g_additional_server_config.vote_kick;
    }
};

struct VoteExtend : public Vote
{
    rf::Player* m_target_player;

    VoteType get_type() const override
    {
        return VoteType::Extend;
    }

    [[nodiscard]] std::string get_title() const override
    {
        return "EXTEND ROUND BY 5 MINUTES";
    }

    void on_accepted() override
    {
        send_chat_line_packet("\xA6 Vote passed: extending round", nullptr);
        extend_round_time(5);
    }

    [[nodiscard]] bool is_allowed_in_limbo_state() const override
    {
        return false;
    }

    [[nodiscard]] const VoteConfig& get_config() const override
    {
        return g_additional_server_config.vote_extend;
    }
};

struct VoteLevel : public Vote
{
    std::string m_level_name;

    VoteType get_type() const override
    {
        return VoteType::Level;
    }

    bool process_vote_arg([[maybe_unused]] std::string_view arg, rf::Player* source) override
    {
        auto [is_valid, level_name] = is_level_name_valid(arg);

        if (!is_valid) {
            auto msg = std::format("\xA6 Cannot start vote: level {} is not available on the server!", level_name);
            send_chat_line_packet(msg.c_str(), source);
            return false;
        }

        m_level_name = std::move(level_name);
        return true;
    }

    [[nodiscard]] std::string get_title() const override
    {
        return std::format("LOAD LEVEL '{}'", m_level_name);
    }

    void on_accepted() override
    {
        auto msg = std::format("\xA6 Vote passed: changing level to {}", m_level_name);
        send_chat_line_packet(msg.c_str(), nullptr);
        rf::multi_change_level(m_level_name.c_str());
    }

    [[nodiscard]] bool is_allowed_in_limbo_state() const override
    {
        return false;
    }

    [[nodiscard]] const VoteConfig& get_config() const override
    {
        return g_additional_server_config.vote_level;
    }
};

struct VoteRestart : public Vote
{

    VoteType get_type() const override
    {
        return VoteType::Restart;
    }

    [[nodiscard]] std::string get_title() const override
    {
        return "RESTART LEVEL";
    }

    void on_accepted() override
    {
        send_chat_line_packet("\xA6 Vote passed: restarting level", nullptr);
        restart_current_level();
    }

    [[nodiscard]] bool is_allowed_in_limbo_state() const override
    {
        return false;
    }

    [[nodiscard]] const VoteConfig& get_config() const override
    {
        return g_additional_server_config.vote_restart;
    }
};

struct VoteNext : public Vote
{
    VoteType get_type() const override
    {
        return VoteType::Next;
    }

    [[nodiscard]] std::string get_title() const override
    {
        return "LOAD NEXT LEVEL";
    }

    void on_accepted() override
    {
        send_chat_line_packet("\xA6 Vote passed: loading next level", nullptr);
        load_next_level();
    }

    [[nodiscard]] bool is_allowed_in_limbo_state() const override
    {
        return false;
    }

    [[nodiscard]] const VoteConfig& get_config() const override
    {
        return g_additional_server_config.vote_next;
    }
};

struct VoteRandom : public Vote
{
    VoteType get_type() const override
    {
        return VoteType::Random;
    }

    [[nodiscard]] std::string get_title() const override
    {
        return "LOAD RANDOM LEVEL";
    }

    void on_accepted() override
    {
        send_chat_line_packet("\xA6 Vote passed: loading random level from rotation", nullptr);

        // if dynamic rotation is on, just load the next level
        g_alpine_server_config.dynamic_rotation ? load_next_level() : load_rand_level();
    }

    [[nodiscard]] bool is_allowed_in_limbo_state() const override
    {
        return false;
    }

    [[nodiscard]] const VoteConfig& get_config() const override
    {
        return g_additional_server_config.vote_rand;
    }
};

struct VotePrevious : public Vote
{
    VoteType get_type() const override
    {
        return VoteType::Previous;
    }

    [[nodiscard]] std::string get_title() const override
    {
        return "LOAD PREV LEVEL";
    }

    void on_accepted() override
    {
        send_chat_line_packet("\xA6 Vote passed: loading previous level", nullptr);
        load_prev_level();
    }

    [[nodiscard]] bool is_allowed_in_limbo_state() const override
    {
        return false;
    }

    [[nodiscard]] const VoteConfig& get_config() const override
    {
        return g_additional_server_config.vote_previous;
    }
};

class VoteMgr
{
private:
    std::optional<std::unique_ptr<Vote>> active_vote;

public:
    template<typename T>
    bool StartVote(std::string_view arg, rf::Player* source)
    {
        if (active_vote) {
            send_chat_line_packet("Another vote is currently in progress!", source);
            return false;
        }

        auto vote = std::make_unique<T>();

        if (!vote->get_config().enabled) {
            send_chat_line_packet("This vote type is disabled!", source);
            return false;
        }

        if (!vote->is_allowed_in_limbo_state() && rf::gameseq_get_state() != rf::GS_GAMEPLAY) {
            send_chat_line_packet("Vote cannot be started now!", source);
            return false;
        }

        if (vote->get_type() == VoteType::Match && (g_match_info.pre_match_active || g_match_info.match_active)) {
            send_chat_line_packet(
                "A match is already queued or in progress. Finish it before starting a new one.", source);
            return false;
        }

        if (!vote->start(arg, source)) {
            return false;
        }

        active_vote = {std::move(vote)};
        return true;
    }    

    void on_player_leave(rf::Player* player)
    {
        if (active_vote && !active_vote.value()->on_player_leave(player)) {
            active_vote.reset();
        }
    }

    void OnLimboStateEnter()
    {
        if (active_vote && !active_vote.value()->is_allowed_in_limbo_state()) {
            send_chat_line_packet("\xA6 Vote canceled!", nullptr);
            active_vote.reset();
        }
    }

    void add_player_vote(bool is_yes_vote, rf::Player* source)
    {
        if (!active_vote) {
            send_chat_line_packet("No vote in progress!", source);
            return;
        }

        if (!active_vote.value()->add_player_vote(is_yes_vote, source)) {
            active_vote.reset();
        }
    }

    void try_cancel_vote(rf::Player* source)
    {
        if (!active_vote) {
            send_chat_line_packet("No vote in progress!", source);
            return;
        }

        if (active_vote.value()->try_cancel_vote(source)) {
            active_vote.reset();
        }
    }

    void do_frame()
    {
        if (!active_vote)
            return;

        if (!active_vote.value()->do_frame()) {
            active_vote.reset();
        }
    }
};

VoteMgr g_vote_mgr;

void handle_vote_command(std::string_view vote_name, std::string_view vote_arg, rf::Player* sender)
{
    if (get_player_additional_data(sender).is_browser || ends_with(sender->name, " (Bot)")) {
        send_chat_line_packet("Browsers and bots are not allowed to vote!", sender);
        return;
    }
    if (vote_name == "kick")
        g_vote_mgr.StartVote<VoteKick>(vote_arg, sender);
    else if (vote_name == "level" || vote_name == "map")
        g_vote_mgr.StartVote<VoteLevel>(vote_arg, sender);
    else if (vote_name == "extend" || vote_name == "ext")
        g_vote_mgr.StartVote<VoteExtend>(vote_arg, sender);
    else if (vote_name == "restart" || vote_name == "rest")
        g_vote_mgr.StartVote<VoteRestart>(vote_arg, sender);
    else if (vote_name == "next")
        g_vote_mgr.StartVote<VoteNext>(vote_arg, sender);
    else if (vote_name == "random" || vote_name == "rand")
        g_vote_mgr.StartVote<VoteRandom>(vote_arg, sender);
    else if (vote_name == "previous" || vote_name == "prev")
        g_vote_mgr.StartVote<VotePrevious>(vote_arg, sender);
    else if (vote_name == "match")
        g_vote_mgr.StartVote<VoteMatch>(vote_arg, sender);
    else if (vote_name == "cancelmatch" || vote_name == "nomatch")
        g_vote_mgr.StartVote<VoteCancelMatch>(vote_arg, sender);
    else if (vote_name == "yes" || vote_name == "y")
        g_vote_mgr.add_player_vote(true, sender);
    else if (vote_name == "no" || vote_name == "n")
        g_vote_mgr.add_player_vote(false, sender);
    else if (vote_name == "cancel")
        g_vote_mgr.try_cancel_vote(sender);
    else
        send_chat_line_packet("Unrecognized vote type!", sender);
}

void server_vote_do_frame()
{
    g_vote_mgr.do_frame();
}

void server_vote_on_player_leave(rf::Player* player)
{
    g_vote_mgr.on_player_leave(player);
}

void server_vote_on_limbo_state_enter()
{
    g_vote_mgr.OnLimboStateEnter();
}
