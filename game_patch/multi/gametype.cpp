#include <xlog/xlog.h>
#include <unordered_set>
#include <patch_common/FunHook.h>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include <common/utils/list-utils.h>
#include <common/version/version.h>
#include "gametype.h"
#include "multi.h"
#include "alpine_packets.h"
#include "../object/event_alpine.h"
#include "../rf/gameseq.h"
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
        //xlog::warn("GameType[{}]: {} (slot={}, name_ptr={})", i, name, static_cast<const void*>(slot), static_cast<const void*>(*slot));
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

bool multi_game_type_is_team_type(rf::NetGameType game_type)
{
    switch (game_type) {
        case rf::NG_TYPE_CTF:
        case rf::NG_TYPE_TEAMDM:
        case rf::NG_TYPE_KOTH:
            return true;
        default: // DM
            return false;
    }
}

bool multi_game_type_has_hills(rf::NetGameType game_type)
{
    switch (game_type) {
        case rf::NG_TYPE_KOTH:
            return true;
        default: // DM, CTF, TDM
            return false;
    }
}

bool multi_is_team_game_type()
{
    return multi_game_type_is_team_type(rf::multi_get_game_type());
}

int multi_koth_get_red_team_score()
{
    return g_koth_info.red_team_score;
}

int multi_koth_get_blue_team_score()
{
    return g_koth_info.blue_team_score;
}

void multi_koth_set_red_team_score(int score)
{
    g_koth_info.red_team_score = score;
    return;
}

void multi_koth_set_blue_team_score(int score)
{
    g_koth_info.blue_team_score = score;
    return;
}

void multi_koth_reset_scores()
{
    g_koth_info.red_team_score = 0;
    g_koth_info.blue_team_score = 0;
}

bool gt_is_koth()
{
    return rf::multi_get_game_type() == rf::NetGameType::NG_TYPE_KOTH;
}

HillInfo* koth_find_hill_by_uid(uint8_t uid)
{
    for (auto& h : g_koth_info.hills)
        if (h.hill_uid == static_cast<int>(uid))
            return &h;
    return nullptr;
}

float cylinder_radius_for_box_inscribed(const rf::Trigger* t)
{
    const float hx = 0.5f * std::fabs(t->box_size.x);
    const float hz = 0.5f * std::fabs(t->box_size.z);
    return std::min(hx, hz);
}

static inline bool trigger_oriented_cylinder_check_if_activated(const rf::Trigger* tp, const rf::Object* objp, float radius,
    float half_height, const rf::Vector3& axis, const rf::Vector3& rvec, const rf::Vector3& fvec
)
{
    if (!tp || !objp)
        return false;

    const rf::Vector3 d = objp->pos - tp->pos;
    const float y = d.dot_prod(axis);
    if (y < -half_height || y > half_height)
        return false;

    // radial distance in the boxs local XZ plane
    const float x = d.dot_prod(rvec);
    const float z = d.dot_prod(fvec);
    return (x * x + z * z) <= (radius * radius);
}

static inline bool trigger_sphere_as_full_cylinder_check_if_activated(const rf::Trigger* tp, const rf::Object* objp, bool use_trigger_up
)
{
    if (!tp || !objp)
        return false;

    const float r = tp->radius;
    if (r <= 0.0f)
        return false;

    rf::Vector3 axis = use_trigger_up ? tp->orient.uvec : rf::Vector3{0.f, 1.f, 0.f};
    axis.normalize_safe();

    // d projected onto axis gives height from center
    const rf::Vector3 d = objp->pos - tp->pos;
    const float y = d.dot_prod(axis);
    if (y < -r || y > r)
        return false;

    const rf::Vector3 radial = d - axis * y;
    return radial.len_sq() <= (r * r);
}

bool obj_inside_trigger(const rf::Trigger* t, const rf::Object* o)
{
    if (!t || !o)
        return false;
    if (t->type == 0)
        return rf::trigger_inside_bounding_sphere(const_cast<rf::Trigger*>(t), const_cast<rf::Object*>(o));
    if (t->type == 1)
        return rf::trigger_inside_bounding_box(const_cast<rf::Trigger*>(t), const_cast<rf::Object*>(o));
    return false;
}

bool obj_inside_uid_area(int check_uid, const rf::Object* o)
{
    if (check_uid < 0 || !o)
        return false;

    if (rf::Object* chk = rf::obj_lookup_from_uid(check_uid)) {
        if (chk->type == rf::ObjectType::OT_TRIGGER) {
            return obj_inside_trigger(static_cast<rf::Trigger*>(chk), o);
        }
    }

    return false;
}

bool player_inside_hill_trigger(const HillInfo& h, const rf::Player& p)
{
    auto ent = rf::entity_from_handle(p.entity_handle);
    if (!ent || !h.trigger)
        return false;

    if (h.trigger->type == 0 && h.handler->sphere_to_cylinder) {
        return trigger_sphere_as_full_cylinder_check_if_activated(h.trigger, ent, g_koth_info.rules.cyl_use_trigger_up);
    }

    if (h.trigger->type == 1 && h.handler->sphere_to_cylinder) {
        const rf::Trigger* t = h.trigger;
        const float radius = cylinder_radius_for_box_inscribed(t);
        const float half_h = 0.5f * std::fabs(t->box_size.y);
        const rf::Vector3 u = t->orient.uvec;
        const rf::Vector3 r = t->orient.rvec;
        const rf::Vector3 f = t->orient.fvec;
        return trigger_oriented_cylinder_check_if_activated(t, ent, radius, half_h, u, r, f);
    }

    // fallback to stock checks
    return obj_inside_trigger(h.trigger, ent);
}

bool player_is_countable(rf::Player& p)
{
    auto ent = rf::entity_from_handle(p.entity_handle);
    if (!ent)
        return false; // invalid entity
    if (rf::entity_is_dying(ent))
        return false; // dying
    if (ent->life <= 0)
        return false; // dead
    return true;
}

Presence sample_presence(const HillInfo& h)
{
    Presence out;
    auto plist = SinglyLinkedList{rf::player_list};
    for (auto& pl : plist) {
        if (!player_is_countable(pl))
            continue;
        if (!player_inside_hill_trigger(h, pl))
            continue;

        if (pl.team == 0)
            ++out.red;
        else if (pl.team == 1)
            ++out.blue;
    }
    return out;
}

static std::vector<uint8_t> on_capture_collect_player_ids_on_hill_for_team(const HillInfo& h, HillOwner team)
{
    std::vector<uint8_t> ids;
    auto plist = SinglyLinkedList{rf::player_list};
    for (auto& pl : plist) {
        if (!player_is_countable(pl))
            continue;
        if ((team == HillOwner::HO_Red && pl.team != 0) || (team == HillOwner::HO_Blue && pl.team != 1))
            continue;

        if (!player_inside_hill_trigger(h, pl))
            continue;
        if (pl.net_data) {
            ids.push_back(pl.net_data->player_id);
            rf::player_add_score(&pl, 3);
            //xlog::warn("added 3 score to {} for being one of the cappers", pl.name);
        }
    }
    return ids;
}

rf::Trigger* koth_resolve_trigger_from_uid(int uid)
{
    if (uid < 0)
        return nullptr;
    if (auto* o = rf::obj_lookup_from_uid(uid)) {
        if (o->type == rf::ObjectType::OT_TRIGGER)
            return static_cast<rf::Trigger*>(o);
    }
    return nullptr;
}

inline bool local_player_on_owner_team(HillOwner owner)
{
    if (!rf::local_player)
        return false;

    const bool local_is_blue = rf::local_player->team != 0;
    switch (owner) {
        case HillOwner::HO_Red:
            return !local_is_blue;
        case HillOwner::HO_Blue:
            return local_is_blue;
        default:
            return false;
    }
}

inline int capture_sfx_for_local(HillOwner owner)
{
    if (owner == HillOwner::HO_Neutral)
        return -1; // do not play a sound
    return local_player_on_owner_team(owner) ? 67 : 65;
}

void koth_local_announce_hill_captured(const HillInfo* h, HillOwner new_owner, const uint8_t* ids, size_t ids_len)
{
    if (!h)
        return;

    // Build captor names from IDs
    std::vector<std::string> names;
    names.reserve(ids_len);
    for (size_t i = 0; i < ids_len; ++i) {
        const uint8_t pid = ids[i];
        if (rf::Player* p = rf::multi_find_player_by_id(pid)) {
            names.emplace_back(p->name ? p->name : std::string{"Player "} + std::to_string(pid));
        }
    }

    std::string names_csv;
    if (!names.empty()) {
        names_csv = names[0];
        for (size_t i = 1; i < names.size(); ++i) {
            names_csv += ", ";
            names_csv += names[i];
        }
    }
    else {
        names_csv = "unknown";
    }

    const char* team_name = (new_owner == HillOwner::HO_Red) ? "RED" : (new_owner == HillOwner::HO_Blue) ? "BLUE" : "NEUTRAL";

    const std::string msg_str =
        std::format("{} was captured by {} for the {} team", h->name.empty() ? "Hill" : h->name, names_csv, team_name);
    rf::String msg = msg_str.c_str();

    rf::ChatMsgColor color_id = rf::ChatMsgColor::white_white;
    if (new_owner == HillOwner::HO_Red)
        color_id = rf::ChatMsgColor::red_red;
    if (new_owner == HillOwner::HO_Blue)
        color_id = rf::ChatMsgColor::blue_blue;

    rf::multi_chat_print(msg, color_id, {});

    const int sfx = capture_sfx_for_local(new_owner);
    if (sfx >= 0) {
        rf::snd_play(sfx, 0, 0.0, 1.0);
    }
}

void koth_local_announce_hill_captured_vector(const HillInfo* h, HillOwner new_owner, const std::vector<uint8_t>& ids)
{
    koth_local_announce_hill_captured(h, new_owner, ids.data(), ids.size());
}

// --- logging helpers ---
static const char* to_string(HillOwner o)
{
    switch (o) {
    case HillOwner::HO_Red:
        return "RED";
    case HillOwner::HO_Blue:
        return "BLUE";
    default:
        return "NEUTRAL";
    }
}

static const char* to_string(HillState s)
{
    switch (s) {
    case HillState::HS_Idle:
        return "Idle";
    case HillState::HS_LeanRedGrowing:
        return "RedGrowing";
    case HillState::HS_LeanRedShrinking:
        return "RedShrinking";
    case HillState::HS_LeanBlueGrowing:
        return "BlueGrowing";
    case HillState::HS_LeanBlueShrinking:
        return "BlueShrinking";
    default:
        return "?";
    }
}

static void koth_apply_ownership(HillInfo& h, HillOwner new_owner, bool announce = true)
{
    if (h.ownership == new_owner)
        return;

    HillOwner old_owner = h.ownership;

    // Reset hill capture state
    h.ownership = new_owner;
    h.steal_dir = HillOwner::HO_Neutral;
    h.state = HillState::HS_Idle;
    h.capture_milli = 0;
    h.capture_progress = 0;
    h.hold_ms_accum = 0;

    //xlog::warn("[KOTH] {}: OWNER {} -> {}", h.name.c_str(), to_string(old_owner), to_string(new_owner));

    if (announce && rf::is_server) {
        auto ids = on_capture_collect_player_ids_on_hill_for_team(h, new_owner);
        const uint8_t uid8 = static_cast<uint8_t>(std::clamp(h.hill_uid, 0, 255));
        af_send_koth_hill_captured_packet_to_all(uid8, new_owner, ids);
    }
}

void update_hill_server(HillInfo& h, int dt_ms)
{
    const Presence pres = sample_presence(h);
    const bool red_only = (pres.red > 0 && pres.blue == 0);
    const bool blue_only = (pres.blue > 0 && pres.red == 0);
    const bool both = (pres.red > 0 && pres.blue > 0);
    const bool empty = (pres.red == 0 && pres.blue == 0);

    // milli-percent helpers: rate is %/sec, dt_ms is ms
    auto inc_rate = [&](int rate_per_sec) {
        h.capture_milli = std::min(100000, h.capture_milli + rate_per_sec * dt_ms);
        h.capture_progress = static_cast<uint8_t>(h.capture_milli / 1000);
    };
    auto dec_rate = [&](int rate_per_sec) {
        h.capture_milli = std::max(0, h.capture_milli - rate_per_sec * dt_ms);
        h.capture_progress = static_cast<uint8_t>(h.capture_milli / 1000);
        if (h.capture_milli == 0)
            h.steal_dir = HillOwner::HO_Neutral;
    };

    // handle scoring only during gameplay
    if (rf::gameseq_get_state() == rf::GameState::GS_GAMEPLAY) {
        if (h.ownership == HillOwner::HO_Red || h.ownership == HillOwner::HO_Blue) {
            const HillOwner opp = opposite(h.ownership);
            const bool opp_on = team_present(opp, pres);
            const bool hostile_progress = hostile_progress_present_server(h);

            if (!opp_on && !hostile_progress) { // if uncontested and no partial progress from opponent
                h.hold_ms_accum += dt_ms;
                while (h.hold_ms_accum >= g_koth_info.rules.ms_per_point) {
                    if (h.ownership == HillOwner::HO_Red) {
                        ++g_koth_info.red_team_score;
                        //xlog::warn("[KOTH] {}: RED +1 (total R={} B={})", h.name.c_str(), g_koth_info.red_team_score, g_koth_info.blue_team_score);
                    }
                    else {
                        ++g_koth_info.blue_team_score;
                        //xlog::warn("[KOTH] {}: BLUE +1 (total R={} B={})", h.name.c_str(), g_koth_info.red_team_score, g_koth_info.blue_team_score);
                    }
                    h.hold_ms_accum -= g_koth_info.rules.ms_per_point;
                }
            }
        }
    }

    // --- change-driven logging helper (compare to last tick) ---
    auto emit_change_logs = [&]() {
        /* if (h.dbg_last_red != pres.red || h.dbg_last_blue != pres.blue) {
            xlog::warn("[KOTH] {}: presence R={} B={}", h.name.c_str(), pres.red, pres.blue);
            h.dbg_last_red = pres.red;
            h.dbg_last_blue = pres.blue;
        }
        if (h.dbg_last_owner != h.ownership) {
            xlog::warn("[KOTH] {}: OWNER {} -> {}", h.name.c_str(), to_string(h.dbg_last_owner),
                       to_string(h.ownership));
            h.dbg_last_owner = h.ownership;
        }
        if (h.dbg_last_dir != h.steal_dir) {
            xlog::warn("[KOTH] {}: steal_dir -> {}", h.name.c_str(), to_string(h.steal_dir));
            h.dbg_last_dir = h.steal_dir;
        }
        if (h.dbg_last_state != h.state) {
            xlog::warn("[KOTH] {}: state {} -> {}", h.name.c_str(), to_string(h.dbg_last_state), to_string(h.state));
            h.dbg_last_state = h.state;
        }
        uint8_t bucket = static_cast<uint8_t>(h.capture_progress / 5);
        if (bucket != h.dbg_last_prog_bucket) {
            xlog::warn("[KOTH] {}: progress {}% (dir={}, owner={})", h.name.c_str(), int(h.capture_progress),
                       to_string(h.steal_dir), to_string(h.ownership));
            h.dbg_last_prog_bucket = bucket;
        }*/
    };

    // contested: freeze
    if (both) {
        h.state = HillState::HS_Idle;
        emit_change_logs();
        return;
    }

    // empty: bleed back to 0
    if (empty) {
        if (h.capture_milli > 0)
            dec_rate(g_koth_info.rules.drain_empty_rate);
        h.state = HillState::HS_Idle;
        emit_change_logs();
        return;
    }

    // pre-first-capture (neutral -> first owner)
    if (h.ownership == HillOwner::HO_Neutral) {
        const int grow = g_koth_info.rules.grow_rate;
        // reuse the defended drain as a "neutral flip clear" rate; add a separate rule if you want
        const int neutral_clear = g_koth_info.rules.drain_defended_rate;

        auto clear_then_grow = [&](HillOwner losing, HillOwner winning, HillState clear_state, HillState grow_state) {
            if (h.steal_dir == losing && h.capture_milli > 0) {
                // Clear the other team's partial progress first
                h.state = clear_state;
                dec_rate(neutral_clear); // dec_rate sets steal_dir = Neutral on reaching 0
                return;                  // keep clearing until 0
            }

            if (h.steal_dir == HillOwner::HO_Neutral) {
                h.steal_dir = winning; // adopt winning side only when cleared
            }

            if (h.steal_dir == winning) {
                h.state = grow_state;
                inc_rate(grow);
                if (h.capture_milli >= 100000) {
                    koth_apply_ownership(h, winning);
                }
            }
        };

        if (red_only) {
            clear_then_grow(HillOwner::HO_Blue, HillOwner::HO_Red, HillState::HS_LeanBlueShrinking, HillState::HS_LeanRedGrowing);
        }
        else if (blue_only) {
            clear_then_grow(HillOwner::HO_Red, HillOwner::HO_Blue, HillState::HS_LeanRedShrinking, HillState::HS_LeanBlueGrowing);
        }

        emit_change_logs();
        return;
    }

    // owned paths (never return to neutral)
    const HillOwner owner = h.ownership;
    const HillOwner attackers = opposite(owner);
    const bool attackers_only = team_present(attackers, pres) && !team_present(owner, pres);
    const bool owner_only = team_present(owner, pres) && !team_present(attackers, pres);

    if (attackers_only) {
        if (h.steal_dir != attackers && h.capture_milli > 0) {
            // drain hostile progress quickly
            h.state = (owner == HillOwner::HO_Red) ? HillState::HS_LeanRedShrinking : HillState::HS_LeanBlueShrinking;
            dec_rate(g_koth_info.rules.drain_defended_rate);
        }
        else {
            h.steal_dir = attackers;
            h.state = (attackers == HillOwner::HO_Red) ? HillState::HS_LeanRedGrowing : HillState::HS_LeanBlueGrowing;
            inc_rate(g_koth_info.rules.grow_rate);
            if (h.capture_milli >= 100000) {
                koth_apply_ownership(h, attackers);
            }
        }
        emit_change_logs();
        return;
    }

    if (owner_only) {
        if (h.capture_milli > 0) {
            h.state = (owner == HillOwner::HO_Red) ? HillState::HS_LeanBlueShrinking : HillState::HS_LeanRedShrinking;
            dec_rate(g_koth_info.rules.drain_defended_rate);
        }
        else {
            h.state = HillState::HS_Idle;
        }
        emit_change_logs();
        return;
    }

    // fallback
    h.state = HillState::HS_Idle;
    emit_change_logs();
}

static void server_maybe_broadcast_state(HillInfo& h, const Presence& pres)
{
    const uint8_t prog_bucket = static_cast<uint8_t>(h.capture_progress / 5);

    bool changed = false;
    changed |= (h.net_last_red != static_cast<uint8_t>(std::clamp(pres.red, 0, 255)));
    changed |= (h.net_last_blue != static_cast<uint8_t>(std::clamp(pres.blue, 0, 255)));
    changed |= (h.net_last_state != h.state);
    changed |= (h.net_last_dir != h.steal_dir);
    changed |= (h.net_last_prog_bucket != prog_bucket);

    if (!changed)
        return;

    af_send_koth_hill_state_packet_to_all(h, pres);

    // snapshot what we sent
    h.net_last_red = static_cast<uint8_t>(std::clamp(pres.red, 0, 255));
    h.net_last_blue = static_cast<uint8_t>(std::clamp(pres.blue, 0, 255));
    h.net_last_state = h.state;
    h.net_last_dir = h.steal_dir;
    h.net_last_prog_bucket = prog_bucket;
}

static void update_hill_client_predict(HillInfo& h, int dt_ms)
{
    const Presence pres = sample_presence(h);

    auto inc_rate = [&](int rate_per_sec) {
        h.capture_milli = std::min(100000, h.capture_milli + rate_per_sec * dt_ms);
        h.capture_progress = static_cast<uint8_t>(h.capture_milli / 1000);
    };
    auto dec_rate = [&](int rate_per_sec) {
        h.capture_milli = std::max(0, h.capture_milli - rate_per_sec * dt_ms);
        h.capture_progress = static_cast<uint8_t>(h.capture_milli / 1000);
        if (h.capture_milli == 0)
            h.steal_dir = HillOwner::HO_Neutral;
    };

    const bool red_only = (pres.red > 0 && pres.blue == 0);
    const bool blue_only = (pres.blue > 0 && pres.red == 0);
    const bool both = (pres.red > 0 && pres.blue > 0);
    const bool empty = (pres.red == 0 && pres.blue == 0);

    if (both) {
        h.state = HillState::HS_Idle;
        return;
    }
    if (empty) {
        if (h.capture_milli > 0)
            dec_rate(g_koth_info.rules.drain_empty_rate);
        h.state = HillState::HS_Idle;
        return;
    }

    // Neutral: clear old side’s partial first, then grow new side
    if (h.ownership == HillOwner::HO_Neutral) {
        const int grow = g_koth_info.rules.grow_rate;
        const int neutral_clear = g_koth_info.rules.drain_defended_rate;

        auto clear_then_grow = [&](HillOwner losing, HillOwner winning, HillState clear_state, HillState grow_state) {
            if (h.steal_dir == losing && h.capture_milli > 0) {
                h.state = clear_state;
                dec_rate(neutral_clear);
                return;
            }
            if (h.steal_dir == HillOwner::HO_Neutral)
                h.steal_dir = winning;

            if (h.steal_dir == winning) {
                h.state = grow_state;
                inc_rate(grow);
                if (h.capture_milli >= 100000) {
                    // Local flip; no announce
                    koth_apply_ownership(h, winning, /*announce=*/false);
                }
            }
        };

        if (red_only) {
            clear_then_grow(HillOwner::HO_Blue, HillOwner::HO_Red, HillState::HS_LeanBlueShrinking, HillState::HS_LeanRedGrowing);
        }
        else if (blue_only) {
            clear_then_grow(HillOwner::HO_Red, HillOwner::HO_Blue, HillState::HS_LeanRedShrinking, HillState::HS_LeanBlueGrowing);
        }
        return;
    }

    // Owned: attackers steal, owner defends
    const HillOwner owner = h.ownership;
    const HillOwner attackers = opposite(owner);
    const bool attackers_only =
        (attackers == HillOwner::HO_Red) ? (pres.red > 0 && pres.blue == 0) : (pres.blue > 0 && pres.red == 0);
    const bool owner_only =
        (owner == HillOwner::HO_Red) ? (pres.red > 0 && pres.blue == 0) : (pres.blue > 0 && pres.red == 0);

    if (attackers_only) {
        if (h.steal_dir != attackers && h.capture_milli > 0) {
            h.state = (owner == HillOwner::HO_Red) ? HillState::HS_LeanRedShrinking : HillState::HS_LeanBlueShrinking;
            dec_rate(g_koth_info.rules.drain_defended_rate);
        }
        else {
            h.steal_dir = attackers;
            h.state = (attackers == HillOwner::HO_Red) ? HillState::HS_LeanRedGrowing : HillState::HS_LeanBlueGrowing;
            inc_rate(g_koth_info.rules.grow_rate);
            if (h.capture_milli >= 100000) {
                koth_apply_ownership(h, attackers, /*announce=*/false);
            }
        }
        return;
    }

    if (owner_only) {
        if (h.capture_milli > 0) {
            h.state = (owner == HillOwner::HO_Red) ? HillState::HS_LeanBlueShrinking : HillState::HS_LeanRedShrinking;
            dec_rate(g_koth_info.rules.drain_defended_rate);
        }
        else {
            h.state = HillState::HS_Idle;
        }
        return;
    }

    // fallback (shouldn't hit due to early returns)
    h.state = HillState::HS_Idle;
}

static void koth_client_predict_tick(int dt_ms)
{
    // client-only; server is authoritative
    if (!rf::is_multi || rf::is_server || rf::is_dedicated_server)
        return;

    for (auto& h : g_koth_info.hills) {
        if (h.ownership == HillOwner::HO_Neutral) {
            h.client_hold_ms_accum = 0;
            continue;
        }

        const bool opp_on = (h.ownership == HillOwner::HO_Red) ? (h.net_last_blue > 0) : (h.ownership == HillOwner::HO_Blue) ? (h.net_last_red > 0) : false;

        const HillOwner attackers = opposite(h.ownership);
        const bool hostile_progress = (h.steal_dir == attackers) && (h.capture_progress > 0);

        if (rf::gameseq_get_state() == rf::GameState::GS_GAMEPLAY) {
            if (!opp_on && !hostile_progress) {
                h.client_hold_ms_accum += dt_ms;
                while (h.client_hold_ms_accum >= g_koth_info.rules.ms_per_point) {
                    if (h.ownership == HillOwner::HO_Red)
                        ++g_koth_info.red_team_score;
                    else
                        ++g_koth_info.blue_team_score;
                    h.client_hold_ms_accum -= g_koth_info.rules.ms_per_point;
                }
            }
            else {
                h.client_hold_ms_accum = 0;
            }
        }
    }

    // predict capture percentage clientside
    for (auto& h : g_koth_info.hills) {
        update_hill_client_predict(h, dt_ms);
    }
}

void koth_do_frame() // fires every frame on both server and client
{
    if (!rf::is_multi || !gt_is_koth())
        return;

    // ---- SERVER TICK ----
    if (rf::is_dedicated_server || rf::is_server) {
        static int last_srv = rf::timer_get(1000);
        const int now_srv = rf::timer_get(1000);
        int dt_srv = now_srv - last_srv;
        if (dt_srv > 0) {
            last_srv = now_srv;
            const int dt_ms = std::clamp(dt_srv, 0, 250);

            for (auto& h : g_koth_info.hills) {
                update_hill_server(h, dt_ms);

                // After the authoritative update, broadcast state if something relevant changed
                const Presence pres = sample_presence(h);
                server_maybe_broadcast_state(h, pres);
            }
        }
    }

    // ---- CLIENT PREDICTION TICK ----
    if (!rf::is_server && !rf::is_dedicated_server) {
        static int last_cli = rf::timer_get(1000);
        const int now_cli = rf::timer_get(1000);
        int dt_cli = now_cli - last_cli;
        if (dt_cli > 0) {
            last_cli = now_cli;
            const int dt_ms = std::clamp(dt_cli, 0, 250);
            koth_client_predict_tick(dt_ms);
        }
    }
}

static int koth_build_hills_from_capture_point_events()
{
    g_koth_info.hills.clear();

    auto events = find_all_events_by_type(rf::EventType::Capture_Point_Handler);
    std::unordered_set<int> seen_uids;

    int idx = 0;
    for (rf::Event* e : events) {
        if (!e)
            continue;

        auto* cp = static_cast<rf::EventCapturePointHandler*>(e);

        // if trigger_uid not specified, try to infer from links
        if (cp->trigger_uid < 0 && !e->links.empty()) {
            for (int uid : e->links) {
                if (auto* o = rf::obj_lookup_from_uid(uid)) {
                    if (o->type == rf::ObjectType::OT_TRIGGER) {
                        cp->trigger_uid = uid;
                        break;
                    }
                }
            }
        }

        // resolve trigger pointer
        rf::Trigger* trig = koth_resolve_trigger_from_uid(cp->trigger_uid);
        if (!trig) {
            xlog::warn("KOTH: Capture point '{}' has invalid trigger UID {} — skipping", cp->name.c_str(),
                       cp->trigger_uid);
            continue;
        }

        // fix a case where a mapper specified the same trigger for multiple capture point handlers
        if (!seen_uids.insert(cp->trigger_uid).second) {
            xlog::warn("KOTH: Duplicate capture point for trigger UID {} — ignoring subsequent entries", cp->trigger_uid);
            continue;
        }

        HillInfo h{};
        h.hill_uid = ++idx;
        h.name = cp->name.empty() ? std::format("Hill {}", idx) : cp->name; // fallback name
        h.trigger_uid = cp->trigger_uid;
        h.trigger = trig;
        h.outline_offset = cp->outline_offset;
        h.handler = cp;
        h.ownership = static_cast<HillOwner>(cp->initial_owner);
        h.state = HillState::HS_Idle;
        h.capture_progress = 0;
        h.capture_milli = 0;
        h.hold_ms_accum = 0;

        g_koth_info.hills.push_back(std::move(h));
    }

    //xlog::warn("KOTH: discovered {} capture point(s)", int(g_koth_info.hills.size()));
    return int(g_koth_info.hills.size());
}

void koth_level_init()
{
    clear_koth_name_textures(); // clear hill labels

    if (!rf::is_multi || !gt_is_koth())
        return;

    //xlog::warn("initializing koth");

    multi_koth_reset_scores();

    const int n = koth_build_hills_from_capture_point_events();

    //xlog::warn("KOTH: {} capture points found in this map", n);
}

// sync scores every 5ms during gameplay to account for clientside prediction errors
CodeInjection send_team_score_server_do_frame_patch{
    0x0046E5B4,
    [](auto& regs) {
        if (multi_is_team_game_type()) {
            regs.eip = 0x0046E5C3; // if any team mode, send team_scores packet
        }
    },
};

// sync hill state(s) and scores for late joiners
CodeInjection send_team_score_state_info_patch{
    0x0048183F,
    [](auto& regs) {
        auto game_type = rf::multi_get_game_type();
        if (multi_game_type_is_team_type(game_type)) {
            if (multi_game_type_has_hills(game_type)) { // send hill state packet on join
                rf::Player* pp = regs.edi;
                for (auto& h : g_koth_info.hills) {
                    const Presence pres = sample_presence(h);
                    af_send_koth_hill_state_packet(pp, h, pres);
                }
            }
            regs.eip = 0x00481859; // if any team mode, send team_scores packet
        }
    },
};

// sync scores on level change
CodeInjection send_team_score_change_level_patch{
    0x0047BF97,
    [](auto& regs) {
        if (multi_is_team_game_type()) {
            regs.eip = 0x0047BFA6; // if any team mode, send team_scores packet
        }
    },
};

// send new gametype scores in the stock team_scores packet
CodeInjection send_team_score_patch{
    0x00472151,
    [](auto& regs) {
        if (gt_is_koth()) {
            // both int16_t on the wire
            const uint16_t red_score = (uint16_t)std::clamp(multi_koth_get_red_team_score(), 0, 0xFFFF);
            const uint16_t blue_score = (uint16_t)std::clamp(multi_koth_get_blue_team_score(), 0, 0xFFFF);
            regs.si = red_score;
            regs.ax = blue_score;
            regs.eip = 0x00472176; // use stock game packet send
        }
    },
};

// receive new gametype scores in the stock team_scores packet
CodeInjection process_team_score_patch{
    0x0047221D,
    [](auto& regs) {
        if (gt_is_koth()) {
            // both int16_t on the wire
            int red_score = regs.esi;
            int blue_score = regs.edi;
            multi_koth_set_red_team_score(red_score);
            multi_koth_set_blue_team_score(blue_score);
        }
    },
};

CodeInjection player_fpgun_load_meshes_patch{
    0x004AE5CD,
    [](auto& regs) {
        if (multi_is_team_game_type()) {
            regs.eip = 0x004AE5E3; // set team for fpgun load
        }
    },
};

CodeInjection player_create_entity_team_skins_patch{
    0x004A44E1,
    [](auto& regs) {
        if (multi_is_team_game_type()) {
            regs.eip = 0x004A44F7; // set team for player entity create
        }
    },
};

CodeInjection multi_hud_render_target_name_color_patch{
    0x0047806F,
    [](auto& regs) {
        // base game logic path draws name green if mode != CTF AND != TDM
        if (multi_is_team_game_type()) {
            regs.eip = 0x0047807E; // draw team colored name
        }
    },
};

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

    // patch listen server create menu gametype select field to use new table
    const uintptr_t base = (uintptr_t)&g_af_gametype_names[0];
    const uintptr_t end = base + sizeof(g_af_gametype_names);
    const uintptr_t kMovBase = 0x004459B1;
    const uintptr_t kCmpEnd = 0x004459CE;
    write_mem<uint32_t>(kMovBase + 1, (uint32_t)base);
    write_mem<uint32_t>(kCmpEnd + 2, (uint32_t)end);

    // team_score packet expansion to support new team gametypes
    send_team_score_server_do_frame_patch.install();
    send_team_score_state_info_patch.install();
    send_team_score_change_level_patch.install();
    send_team_score_patch.install();
    process_team_score_patch.install();

    // load team coloured fpgun arms in new team gametypes
    player_fpgun_load_meshes_patch.install();

    // fix bug where team fpgun skins not loaded for listen server hosts if a
    // custom gamemode is the first loaded mode after game launch
    player_create_entity_team_skins_patch.install();

    // colour target player names in new team gametypes
    multi_hud_render_target_name_color_patch.install();
}
