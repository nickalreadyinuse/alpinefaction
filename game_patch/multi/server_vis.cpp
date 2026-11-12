#include "server_vis.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>
#include <common/utils/list-utils.h>
#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include <xlog/xlog.h>
#include <common/rfproto.h>
#include "../rf/collide.h"
#include "../rf/entity.h"
#include "../rf/geometry.h"
#include "../rf/level.h"
#include "../rf/math/vector.h"
#include "../rf/multi.h"
#include "../rf/physics.h"
#include "../rf/player/player.h"
#include "../rf/weapon.h"
#include "../rf/os/console.h"
#include "../misc/player.h"
#include "../os/console.h"
#include "../os/os.h"
#include "alpine_packets.h"
#include "bagman.h"
#include "gametype.h"
#include "multi.h"
#include "salvage.h"
#include "server_internal.h"

// Server-side visibility culling. The server already holds every player's position, velocity and
// eye position, so viewer->target line of sight is a plain level raycast.
// The client-reported view angle is never consulted: the scanners are treated as spheres.
//
// Every failure path resolves to visible. Hiding someone who can actually be seen is the failure
// mode that ruins the game; sending an extra update for a tick is not.

namespace
{

// Stock rail scanner range: len_sq < 900.0f at 0x005A0B94 (multiplayer), see player_fpgun_render_for_rail_gun.
constexpr float rail_scanner_range = 30.0f;
// Stock rocket launcher / shoulder cannon IR range: 13.0f at 0x005898DC, minus entity radius.
constexpr float ir_scanner_range = 13.0f;
// Bot perception intervals: a blocked pair is re-tested sooner so reveals never lag a corner peek.
constexpr int recheck_visible_ms = 250;
constexpr int recheck_blocked_ms = 33; // one server tick at the default netfps
// RF players top out at 9 m/s; the probe ring radius is this times the lookahead.
constexpr float max_player_speed = 9.0f;
constexpr float min_lateral_offset = 0.6f;
constexpr int stale_sweep_ms = 5000;
// See-thru faces and glass do not block sight; invisible (clip) faces do not either.
constexpr int los_trace_flags = rf::CF_ANY_HIT | rf::CF_SKIP_SEE_THRU | rf::CF_SKIP_DESTRUCTIBLE_GLASS;

// One raycast result per unordered entity pair; eye-to-eye sight is symmetric.
struct RayState
{
    bool clear = true;
    int64_t last_check_ms = 0;
    int64_t last_clear_ms = 0;
};

// What one recipient currently believes about one entity, to drive hide/reveal packets.
struct PairState
{
    rf::Player* viewer = nullptr;
    int target_handle = -1;
    bool visible = true;
    bool fire_on_sent = false; // the last full record told the recipient the on/off weapon is firing
    // First 27 bytes of the last full record sent: handle, flags, tick, pos, pitch, yaw, state,
    // vel dir x/y, speed. Replayed (minus OUF_FIRE, velocity zeroed) as the fire-off stub.
    static constexpr int record_pos_part = 27;
    uint8_t last_record[record_pos_part] = {};
    bool has_last_record = false;
};

std::unordered_map<uint64_t, RayState> g_ray_cache;
std::unordered_map<uint64_t, PairState> g_pair_state;
int64_t g_last_stale_sweep_ms = 0;

const VisibilityCullingConfig& cfg()
{
    return g_alpine_server_config.visibility_culling_config;
}

uint64_t pair_key(const rf::Player* viewer, int target_handle)
{
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(viewer)) << 32 | static_cast<uint32_t>(target_handle);
}

uint64_t ray_key(int handle_a, int handle_b)
{
    const uint32_t lo = static_cast<uint32_t>(handle_a < handle_b ? handle_a : handle_b);
    const uint32_t hi = static_cast<uint32_t>(handle_a < handle_b ? handle_b : handle_a);
    return static_cast<uint64_t>(hi) << 32 | lo;
}

bool is_alpine_client(const rf::Player* player)
{
    return player->version_info.software == ClientSoftware::AlpineFaction
        || player->version_info.software == ClientSoftware::Observer;
}

bool weapon_is_ir(int weapon_type)
{
    return weapon_type >= 0 && weapon_type < rf::num_weapon_types
        && (rf::weapon_types[weapon_type].flags & rf::WTF_INFRARED) != 0;
}

bool is_objective_carrier(const rf::Player* player)
{
    if (rf::multi_ctf_get_red_flag_player() == player || rf::multi_ctf_get_blue_flag_player() == player) {
        return true;
    }
    if (g_bagman_info.carrier == player) {
        return true;
    }
    return gt_is_salvage() && salvage_get_carrier() == player;
}

bool ray_clear(const rf::Vector3& from, const rf::Vector3& to)
{
    rf::Vector3 p0 = from;
    rf::Vector3 p1 = to;
    bool hit;
    if (cfg().movers_occlude) {
        // level solid + mover brushes (doors, platforms)
        rf::PCollisionOut collision{};
        collision.obj_handle = -1;
        hit = rf::collide_linesegment_world(p0, p1, los_trace_flags, &collision);
    }
    else {
        rf::GCollisionOutput collision{};
        hit = rf::collide_linesegment_level_solid(p0, p1, los_trace_flags, &collision);
    }
    return !hit;
}

// eye_pos is written by entity_set_eye_pos in entity_process_post, so a just-spawned entity can
// still carry a stale or zero value. Fall back to the object position when it is not near it.
rf::Vector3 eye_of(const rf::Entity& ep)
{
    return (ep.eye_pos - ep.pos).len_sq() < 4.0f ? ep.eye_pos : ep.pos;
}

// Where could either entity be by the time the recipient renders this tick? A player tops out at
// max_player_speed, so probe a ring of that radius times the lookahead around both heads (a corner
// peek by either side), plus the velocity-extrapolated pair. Early exit on the first clear ray.
bool compute_los(const rf::Entity& viewer_ep, const rf::Entity& target_ep, int ping_ms)
{
    // A peek is seen by the server half a round trip late and the reveal takes the other half to
    // arrive, so the pair's ping is dead time on top of the base lookahead.
    const int ping_lookahead = std::clamp(ping_ms, 0, cfg().ping_lookahead_cap_ms);
    const float lookahead = static_cast<float>(cfg().lookahead_ms + ping_lookahead) / 1000.0f;
    const int max_rays = cfg().max_rays_per_pair > 0 ? cfg().max_rays_per_pair : 1;
    const float reach = std::max(min_lateral_offset, max_player_speed * lookahead);

    const rf::Vector3 viewer_eye = eye_of(viewer_ep);
    const rf::Vector3 target_eye = eye_of(target_ep);
    const rf::Vector3 viewer_shift = viewer_ep.p_data.vel * lookahead;
    const rf::Vector3 target_shift = target_ep.p_data.vel * lookahead;

    rf::Vector3 chest = target_ep.pos;
    chest.y = target_ep.pos.y + (target_eye.y - target_ep.pos.y) * 0.6f;

    // horizontal unit vector perpendicular to the sight line
    rf::Vector3 side{};
    {
        rf::Vector3 dir = target_eye - viewer_eye;
        dir.y = 0.0f;
        if (dir.len_sq() > 0.01f) {
            dir.normalize();
            side = rf::Vector3{-dir.z, 0.0f, dir.x} * reach;
        }
    }

    struct Ray
    {
        rf::Vector3 from;
        rf::Vector3 to;
    };
    const Ray rays[8] = {
        {viewer_eye, target_eye},
        {viewer_eye, chest},
        {viewer_eye + viewer_shift, target_eye + target_shift},
        {viewer_eye, target_eye + side},
        {viewer_eye, target_eye - side},
        {viewer_eye + side, target_eye},
        {viewer_eye - side, target_eye},
        {viewer_eye + viewer_shift, chest + target_shift},
    };

    for (int i = 0; i < 8 && i < max_rays; ++i) {
        if (ray_clear(rays[i].from, rays[i].to)) {
            return true;
        }
    }
    return false;
}

bool cached_los(const rf::Entity& viewer_ep, const rf::Entity& target_ep, int ping_ms, int64_t now)
{
    RayState& state = g_ray_cache[ray_key(viewer_ep.handle, target_ep.handle)];
    const int interval = state.clear ? recheck_visible_ms : recheck_blocked_ms;
    if (state.last_check_ms == 0 || now - state.last_check_ms >= interval) {
        state.last_check_ms = now;
        state.clear = compute_los(viewer_ep, target_ep, ping_ms);
        if (state.clear) {
            state.last_clear_ms = now;
        }
    }
    return state.clear || (now - state.last_clear_ms) < cfg().hysteresis_ms;
}

// Rules that never need a raycast. Returns true when the target must be sent.
bool exempt(rf::Player* viewer, rf::Entity* viewer_ep, rf::Entity* target, rf::Player* target_player)
{
    if (cfg().exempt_bots && viewer->is_bot) {
        return true;
    }
    if (viewer->is_observer()) {
        return true; // demos must record what a real client would see, but never less
    }
    if (multi_is_team_game_type() && viewer->team == target_player->team) {
        return true;
    }
    if (is_objective_carrier(target_player)) {
        return true; // outlined through walls anyway
    }

    const float dist = (target->pos - viewer_ep->pos).len();
    if (dist < cfg().min_dist) {
        return true; // footsteps are computed client-side from the position
    }

    const int weapon = viewer_ep->ai.current_primary_weapon;
    if (weapon_is_ir(weapon) && dist < ir_scanner_range + target->radius + cfg().scanner_margin) {
        return true;
    }
    if (rf::weapon_has_scanner(weapon)) {
        // Stock and Dash clients never transmit scanning_for_target, so assume it is engaged.
        const bool scanning = viewer->fpgun_data.scanning_for_target || !is_alpine_client(viewer);
        if (scanning && dist < rail_scanner_range + cfg().scanner_margin) {
            return true;
        }
    }
    return false;
}

void send_hidden(rf::Player* viewer, rf::Entity* target, bool hidden)
{
    if (is_player_minimum_af_client_version(viewer, 1, 5, 0)) {
        af_send_entity_hidden(viewer, static_cast<uint32_t>(target->handle), hidden);
        if (!hidden) {
            af_send_teleport_entity_to_player(viewer, static_cast<uint32_t>(target->handle), target->pos,
                                              target->orient, target->p_data.vel);
        }
    }
    else if (!hidden) {
        // Legacy clients cannot hide; the entity froze at its last seen spot. Snap it into place
        // the way the /load teleport does.
        rf::send_entity_create_packet(target, viewer);
    }
}

void update_pair(rf::Player* viewer, rf::Entity* target, bool visible)
{
    const uint64_t key = pair_key(viewer, target->handle);
    auto [it, inserted] = g_pair_state.try_emplace(key, PairState{viewer, target->handle, true});
    if (it->second.visible != visible) {
        it->second.visible = visible;
        send_hidden(viewer, target, !visible);
    }
}

bool entity_fire_on(const rf::Entity& ep)
{
    const int weapon = ep.ai.current_primary_weapon;
    return (rf::weapon_is_on_off_weapon(weapon, false) || rf::weapon_is_on_off_weapon(weapon, true))
        && rf::entity_weapon_is_on(ep.handle, weapon);
}

// The stock client handler feeds every record into the interpolation ring, position field or
// not, so a stub must carry a real position: the last one this recipient already saw. Only
// OUF_POS_ROT_ANIM is set (no OUF_FIRE, which turns the on/off weapon off), the tick is the
// entity's newest so the ring accepts it, and the velocity is zeroed so it stands still.
int write_fire_off_record(PairState& state, const rf::Entity& target, uint8_t* out)
{
    if (!state.has_last_record) {
        return 0;
    }
    std::memcpy(out, state.last_record, PairState::record_pos_part);
    out[4] = 0x01; // OUF_POS_ROT_ANIM
    if (target.obj_interp && target.obj_interp->num_frames() > 0) {
        const uint16_t tick = target.obj_interp->newest_frame_time();
        std::memcpy(out + 5, &tick, sizeof(tick));
    }
    out[24] = 0; // vel dir x
    out[25] = 0; // vel dir y
    out[26] = 0; // speed + z sign
    state.fire_on_sent = false;
    return PairState::record_pos_part;
}

// Stand-alone obj_update packet holding one fire-off stub record.
void send_fire_off_packet(rf::Player* viewer, PairState& state, const rf::Entity& target)
{
    uint8_t buf[3 + PairState::record_pos_part + 4];
    const int record_size = write_fire_off_record(state, target, buf + 3);
    if (record_size == 0) {
        return;
    }
    buf[0] = RF_GPT_OBJECT_UPDATE;
    const uint16_t payload = static_cast<uint16_t>(record_size + 4);
    std::memcpy(buf + 1, &payload, sizeof(payload));
    const uint32_t end = 0xFFFFFFFFu;
    std::memcpy(buf + 3 + record_size, &end, sizeof(end));
    rf::multi_io_send(viewer, buf, 3 + record_size + 4);
}

} // namespace

int server_vis_withheld_record(rf::Player* viewer, rf::Entity* target, void* data)
{
    auto it = g_pair_state.find(pair_key(viewer, target->handle));
    if (it == g_pair_state.end() || !it->second.fire_on_sent) {
        return 0;
    }
    return write_fire_off_record(it->second, *target, static_cast<uint8_t*>(data));
}

void server_vis_record_sent(rf::Player* viewer, rf::Entity* target, const void* data, int size)
{
    if (!cfg().enabled || viewer->entity_handle == target->handle) {
        return;
    }
    auto it = g_pair_state.find(pair_key(viewer, target->handle));
    if (it == g_pair_state.end()) {
        return;
    }
    PairState& state = it->second;
    state.fire_on_sent = entity_fire_on(*target);
    const auto* bytes = static_cast<const uint8_t*>(data);
    if (size >= PairState::record_pos_part && (bytes[4] & 0x01)) {
        std::memcpy(state.last_record, bytes, PairState::record_pos_part);
        state.has_last_record = true;
    }
}

void server_vis_on_entity_killed(rf::Entity* ep)
{
    if (!rf::is_server || !ep) {
        return;
    }
    for (auto it = g_pair_state.begin(); it != g_pair_state.end();) {
        if (it->second.target_handle != ep->handle) {
            ++it;
            continue;
        }
        PairState& state = it->second;
        if (!state.visible) {
            // reliable, so it is ordered ahead of the obj_kill that follows
            af_send_entity_hidden(state.viewer, static_cast<uint32_t>(ep->handle), false);
        }
        if (state.fire_on_sent) {
            send_fire_off_packet(state.viewer, state, *ep);
        }
        it = g_pair_state.erase(it);
    }
}

bool server_vis_wants_footsteps()
{
    return rf::is_server && rf::is_multi && cfg().enabled;
}

bool server_vis_can_see(rf::Player* viewer, rf::Entity* target)
{
    if (!cfg().enabled || !rf::is_server || !viewer || !target) {
        return true;
    }
    if (viewer->entity_handle == target->handle) {
        return true;
    }
    rf::Player* target_player = rf::player_from_entity_handle(target->handle);
    if (!target_player || target_player->entity_handle != target->handle) {
        return true; // not a player entity
    }
    rf::Entity* viewer_ep = rf::entity_from_handle(viewer->entity_handle);
    // Dead or spectating viewers need the full stream. Still go through update_pair so anything
    // hidden from them while alive is revealed, or the client keeps OF_HIDDEN on a moving entity.
    bool visible = true;
    if (viewer_ep && !rf::player_is_dead(viewer) && !rf::entity_is_dying(viewer_ep) && rf::level.geometry
        && !exempt(viewer, viewer_ep, target, target_player)) {
        // The ray cache is shared by both directions of a pair, so use the higher ping of the two.
        const int viewer_ping = viewer->net_data ? viewer->net_data->ping : 0;
        const int target_ping = target_player->net_data ? target_player->net_data->ping : 0;
        visible = cached_los(*viewer_ep, *target, std::max(viewer_ping, target_ping), timer::get_i64(1000));
    }
    update_pair(viewer, target, visible);
    return visible;
}

void server_vis_do_frame()
{
    if (!rf::is_server) {
        return;
    }
    const int64_t now = timer::get_i64(1000);

    if (now - g_last_stale_sweep_ms < stale_sweep_ms) {
        return;
    }
    g_last_stale_sweep_ms = now;

    // A hidden entity that died or vanished never gets another obj_update record, so the engine
    // send loop never asks about it again. Reveal it (deaths are public) and forget the pair.
    for (auto it = g_pair_state.begin(); it != g_pair_state.end();) {
        rf::Entity* target = rf::entity_from_handle(it->second.target_handle);
        if (!target || rf::entity_is_dying(target)) {
            if (target && !it->second.visible) {
                send_hidden(it->second.viewer, target, false);
            }
            it = g_pair_state.erase(it);
        }
        else {
            ++it;
        }
    }
    for (auto it = g_ray_cache.begin(); it != g_ray_cache.end();) {
        const int lo = static_cast<int>(it->first & 0xFFFFFFFFu);
        const int hi = static_cast<int>(it->first >> 32);
        if (!rf::entity_from_handle(lo) || !rf::entity_from_handle(hi)) {
            it = g_ray_cache.erase(it);
        }
        else {
            ++it;
        }
    }
}

void server_vis_level_init()
{
    g_ray_cache.clear();
    g_pair_state.clear();
}

void server_vis_on_player_destroy(rf::Player* player)
{
    for (auto it = g_pair_state.begin(); it != g_pair_state.end();) {
        if (it->second.viewer == player) {
            it = g_pair_state.erase(it);
        }
        else {
            ++it;
        }
    }
}

namespace
{

// send_weapon_fire_packet (0x0047D430) relays a client's shot to every other client with one
// multi_io_send per recipient at 0x0047D693; the packet carries the shooter's position. Drop it
// for recipients that cannot see the shooter, but only for bullet weapons: a rocket or grenade
// is a visible projectile the recipient must still spawn, or it vanishes around the corner.
// Layout: [0] type 0x30, [1..2] size, [3] weapon, [4] flags, [5] shooter player id.
CallHook<void(rf::Player*, const void*, int)> weapon_fire_relay_send_hook{
    0x0047D693,
    [](rf::Player* recipient, const void* data, int len) {
        if (cfg().enabled && len >= 6) {
            const auto* bytes = static_cast<const uint8_t*>(data);
            const int weapon = bytes[3];
            const bool hitscan = weapon >= 0 && weapon < rf::num_weapon_types
                && (rf::weapon_types[weapon].flags2 & rf::WTF2_STOCK_LAG_COMP) != 0;
            if (hitscan) {
                rf::Player* shooter = rf::multi_find_player_by_id(bytes[5]);
                rf::Entity* shooter_ep = shooter ? rf::entity_from_handle(shooter->entity_handle) : nullptr;
                if (shooter_ep && !server_vis_can_see(recipient, shooter_ep)) {
                    return;
                }
            }
        }
        weapon_fire_relay_send_hook.call_target(recipient, data, len);
    },
};

// Spawn (multi_spawn_player_server_side) and the /load teleport broadcast entity_create with the
// position to everyone. Every recipient now knows the position, so reset their belief to visible
// and immediately re-evaluate: whoever cannot see the entity gets the hide packet right behind
// the create on the same reliable stream.
FunHook<void(rf::Entity*)> send_entity_create_packet_to_all_hook{
    0x00475110,
    [](rf::Entity* ep) {
        send_entity_create_packet_to_all_hook.call_target(ep);
        if (!cfg().enabled || !rf::is_server || !ep) {
            return;
        }
        for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
            if (&player == rf::local_player || !player.net_data) {
                continue;
            }
            auto it = g_pair_state.find(pair_key(&player, ep->handle));
            if (it != g_pair_state.end()) {
                it->second.visible = true;
            }
            server_vis_can_see(&player, ep);
        }
    },
};

// A hidden entity does not animate on the client, so its footsteps fall silent there. The server
// still runs entity_footsteps_do_frame (0x0042F940) for every entity; when a mesh trigger fires
// it calls object_emit_sound (0x0048A930) with the entity, the sound position and a sounds.tbl
// id array. Relay that as a stock positional sound packet (0x34, stock clients included) to every
// recipient that has the entity hidden and is inside footstep_relay_dist.
int relay_footstep(rf::Object* obj, const rf::Vector3& pos, int* sound_ids, int num_sounds)
{
    if (!server_vis_wants_footsteps() || !obj || !sound_ids || num_sounds <= 0
        || cfg().footstep_relay_dist <= 0.0f) {
        return 0;
    }
    rf::Player* owner = rf::player_from_entity_handle(obj->handle);
    if (!owner) {
        return 0;
    }
    RF_SoundPacket packet{};
    packet.header.type = RF_GPT_SOUND;
    packet.header.size = sizeof(packet) - sizeof(packet.header);
    packet.sound_id = static_cast<uint16_t>(sound_ids[0]);
    packet.pos.x = pos.x;
    packet.pos.y = pos.y;
    packet.pos.z = pos.z;

    const float max_dist_sq = cfg().footstep_relay_dist * cfg().footstep_relay_dist;
    int sent = 0;
    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        if (&player == rf::local_player || &player == owner || !player.net_data) {
            continue;
        }
        auto it = g_pair_state.find(pair_key(&player, obj->handle));
        if (it == g_pair_state.end() || it->second.visible) {
            continue; // the client animates it itself
        }
        rf::Entity* listener = rf::entity_from_handle(player.entity_handle);
        if (!listener || (listener->pos - pos).len_sq() > max_dist_sq) {
            continue;
        }
        rf::multi_io_send(&player, &packet, sizeof(packet));
        ++sent;
    }
    return sent;
}

using ObjEmitSoundFn = int(rf::Object*, rf::Vector3, int*, int, float, float);
CallHook<ObjEmitSoundFn> footstep_left_emit_sound_hook{
    0x0042FA3A,
    [](rf::Object* obj, rf::Vector3 pos, int* sound_ids, int num_sounds, float vol, float pan) {
        relay_footstep(obj, pos, sound_ids, num_sounds);
        return footstep_left_emit_sound_hook.call_target(obj, pos, sound_ids, num_sounds, vol, pan);
    },
};
CallHook<ObjEmitSoundFn> footstep_right_emit_sound_hook{
    0x0042FB0C,
    [](rf::Object* obj, rf::Vector3 pos, int* sound_ids, int num_sounds, float vol, float pan) {
        relay_footstep(obj, pos, sound_ids, num_sounds);
        return footstep_right_emit_sound_hook.call_target(obj, pos, sound_ids, num_sounds, vol, pan);
    },
};

ConsoleCommand2 viscull_cmd{
    "sv_viscull",
    [](std::optional<bool> enabled) {
        if (enabled) {
            g_alpine_server_config.visibility_culling_config.enabled = *enabled;
            if (!*enabled) {
                // reveal everything that is currently hidden
                for (auto& [key, state] : g_pair_state) {
                    if (!state.visible) {
                        if (rf::Entity* target = rf::entity_from_handle(state.target_handle)) {
                            send_hidden(state.viewer, target, false);
                        }
                    }
                }
                server_vis_level_init();
            }
            else {
                enforce_alpine_hard_reject_for_all_players_on_current_level();
            }
        }
        rf::console::print("Server-side visibility culling: {}", cfg().enabled);
    },
    "Enable or disable server-side visibility culling",
    "sv_viscull [0|1]",
};

} // namespace

void server_vis_apply_patch()
{
    viscull_cmd.register_cmd();
    weapon_fire_relay_send_hook.install();
    send_entity_create_packet_to_all_hook.install();
    footstep_left_emit_sound_hook.install();
    footstep_right_emit_sound_hook.install();
}
