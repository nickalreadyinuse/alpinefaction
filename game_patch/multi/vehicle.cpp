#include <algorithm>
#include <cstring>
#include <cmath>
#include <patch_common/CodeInjection.h>
#include <patch_common/FunHook.h>
#include <patch_common/AsmWriter.h>
#include <common/utils/list-utils.h>
#include <xlog/xlog.h>
#include "../rf/entity.h"
#include "../rf/multi.h"
#include "../rf/player/player.h"
#include "../rf/object.h"
#include "../rf/os/timer.h"
#include "../rf/vmesh.h"
#include "alpine_packets.h"
#include "network.h"
#include "vehicle.h"
#include "../misc/player.h"

// Thiscall wrapper for Entity::attach_leech(int leech_obj_handle, int interface_tag_handle)
static auto entity_attach_leech = reinterpret_cast<bool(__thiscall*)(rf::Entity*, int, int)>(0x00427240);

// FUN_0042cdd0: returns true if entity_types[index].use_function == 1 (boardable vehicle)
static auto entity_type_has_vehicle_use = reinterpret_cast<bool(__cdecl*)(int type_index)>(0x0042cdd0);


// ObjInterp::set_next_pos_orient - properly updates entity interpolation state for rendering
// Thiscall on ObjInterp: this=obj_interp, then (entity, pos, phb, eye_phb, vel, move, tick, flags)
static auto obj_interp_set_next_pos_orient = reinterpret_cast<void(__thiscall*)(
    rf::ObjInterp*, rf::Entity*, rf::Vector3*, rf::Vector3*, rf::Vector3*, rf::Vector3*, rf::Vector3*, uint16_t, int)>(0x00483360);

// Extract pitch/heading/bank from an orient matrix
static rf::Vector3 orient_to_phb(const rf::Matrix3& orient)
{
    rf::Vector3 phb;
    phb.x = std::asin(-orient.fvec.y);                      // pitch
    phb.y = std::atan2(orient.fvec.x, orient.fvec.z);       // heading
    phb.z = std::atan2(orient.rvec.y, orient.uvec.y);       // bank
    return phb;
}

// ============================================================================
// Packet send/receive
// ============================================================================

// Helper: find entity by UID (for client-side vehicle lookup since remote handles don't map)
static rf::Entity* entity_find_by_uid(int uid)
{
    for (auto& entity : DoublyLinkedList{rf::entity_list}) {
        if (entity.uid == uid) {
            return &entity;
        }
    }
    return nullptr;
}

static void send_vehicle_state_packet_to_all(rf::Entity* entity, rf::Entity* vehicle, uint8_t action)
{
    af_vehicle_state_packet packet{};
    packet.header.type = static_cast<uint8_t>(af_packet_type::af_vehicle_state);
    packet.header.size = sizeof(packet) - sizeof(packet.header);
    packet.entity_handle = entity->handle;
    packet.vehicle_handle = vehicle->handle;
    packet.action = action;
    packet.host_tag_handle = entity->host_tag_handle;
    packet.vehicle_uid = vehicle->uid;

    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        if (&player == rf::local_player) {
            continue;
        }
        if (!is_player_minimum_af_client_version(&player, 1, 3, 0)) {
            continue;
        }
        af_send_packet(&player, &packet, sizeof(packet), true);
    }
}

void af_send_vehicle_state_packet(rf::Player* player, rf::Entity* entity, rf::Entity* vehicle, uint8_t action)
{
    af_vehicle_state_packet packet{};
    packet.header.type = static_cast<uint8_t>(af_packet_type::af_vehicle_state);
    packet.header.size = sizeof(packet) - sizeof(packet.header);
    packet.entity_handle = entity->handle;
    packet.vehicle_handle = vehicle->handle;
    packet.action = action;
    packet.host_tag_handle = entity->host_tag_handle;
    packet.vehicle_uid = vehicle->uid;

    af_send_packet(player, &packet, sizeof(packet), true);
}

void af_send_vehicle_create_packet(rf::Player* player, rf::Entity* vehicle)
{
    af_vehicle_create_packet packet{};
    packet.header.type = static_cast<uint8_t>(af_packet_type::af_vehicle_create);
    packet.header.size = sizeof(packet) - sizeof(packet.header);
    packet.entity_type_index = vehicle->info_index;
    packet.uid = vehicle->uid;
    packet.pos.x = vehicle->pos.x;
    packet.pos.y = vehicle->pos.y;
    packet.pos.z = vehicle->pos.z;
    std::memcpy(packet.orient, &vehicle->orient, sizeof(packet.orient));

    af_send_packet(player, &packet, sizeof(packet), true);
}

void af_process_vehicle_create_packet(const void* data, size_t len, [[maybe_unused]] const rf::NetAddr& addr)
{
    if (rf::is_server) {
        return;
    }

    if (len < sizeof(af_vehicle_create_packet)) {
        xlog::warn("vehicle_create packet too short: {} < {}", len, sizeof(af_vehicle_create_packet));
        return;
    }

    af_vehicle_create_packet packet{};
    std::memcpy(&packet, data, sizeof(packet));

    if (packet.entity_type_index < 0 || packet.entity_type_index >= rf::num_entity_types) {
        xlog::warn("vehicle_create: invalid type index {}", packet.entity_type_index);
        return;
    }

    rf::Vector3 pos{packet.pos.x, packet.pos.y, packet.pos.z};
    rf::Matrix3 orient{};
    std::memcpy(&orient, packet.orient, sizeof(orient));

    rf::Entity* entity = rf::entity_create(
        packet.entity_type_index,
        rf::entity_types[packet.entity_type_index].name,
        -1,     // no parent
        pos,
        orient,
        0,      // flags
        -1      // mp_character
    );

    if (entity) {
        entity->uid = packet.uid;

        bool had_room = entity->room != nullptr;

        // Force room assignment. entity_create may leave room=NULL for vehicle types;
        // without a valid room the collision system has no geometry to test against
        // and the vehicle falls through the world. move() triggers GSolid::find_new_room.
        entity->move(&pos);

        // Set mass from entity info (needed for collision response)
        if (entity->p_data.mass <= 0.0f) {
            entity->p_data.mass = entity->info->mass > 0.0f ? entity->info->mass : 10000.0f;
        }

        // Populate collision spheres if entity_create left them empty
        float r = entity->radius;
        if (entity->p_data.cspheres.size() == 0 && entity->vmesh) {
            int num_cspheres = rf::vmesh_get_num_cspheres(entity->vmesh);
            if (num_cspheres > 0) {
                for (int i = 0; i < num_cspheres; i++) {
                    rf::PCollisionSphere sphere{};
                    rf::vmesh_get_csphere(entity->vmesh, i, &sphere.center, &sphere.radius);
                    sphere.spring_const = -1.0f;
                    entity->p_data.cspheres.add(sphere);
                }
            }
            else {
                rf::PCollisionSphere sphere{};
                sphere.center = {0.0f, 0.0f, 0.0f};
                sphere.radius = r;
                sphere.spring_const = -1.0f;
                entity->p_data.cspheres.add(sphere);
            }
            entity->p_data.radius = r;
        }

        xlog::info("Vehicle created on client: type='{}' uid={} pos=({:.1f},{:.1f},{:.1f}) "
                   "phys_flags=0x{:x} obj_flags=0x{:x} cspheres={} room={}",
                   entity->info->name, entity->uid, pos.x, pos.y, pos.z, entity->p_data.flags,
                   static_cast<int>(entity->obj_flags),
                   entity->p_data.cspheres.size(), entity->room ? 1 : 0);
    }
    else {
        xlog::warn("vehicle_create: entity_create failed for type_idx={}", packet.entity_type_index);
    }
}

void af_process_vehicle_state_packet(const void* data, size_t len, [[maybe_unused]] const rf::NetAddr& addr)
{
    // Client-side only
    if (rf::is_server) {
        return;
    }

    if (len < sizeof(af_vehicle_state_packet)) {
        xlog::warn("vehicle_state packet too short: {} < {}", len, sizeof(af_vehicle_state_packet));
        return;
    }

    af_vehicle_state_packet packet{};
    std::memcpy(&packet, data, sizeof(packet));

    // Find player entity by remote handle (works because player entities use vanilla MP sync)
    rf::Object* entity_obj = rf::obj_from_remote_handle(packet.entity_handle);
    if (!entity_obj) {
        xlog::warn("vehicle_state: entity not found for remote handle 0x{:x}", packet.entity_handle);
        return;
    }
    auto* entity = static_cast<rf::Entity*>(entity_obj);

    // Find vehicle by UID (remote handles don't map for af_vehicle_create entities)
    rf::Entity* vehicle = entity_find_by_uid(packet.vehicle_uid);

    if (packet.action == 1) {
        // Enter vehicle
        if (!vehicle) {
            xlog::warn("vehicle_state: vehicle not found for uid {}", packet.vehicle_uid);
            return;
        }

        // Already in this vehicle
        if (entity->host_handle == vehicle->handle) {
            xlog::trace("vehicle_state: entity '{}' already in vehicle, skipping", entity->name);
            return;
        }

        xlog::info("Vehicle enter: entity '{}' -> vehicle '{}' iface_pts={}",
            entity->name, vehicle->name, vehicle->interface_points.size());

        // Turn off current weapon
        rf::entity_turn_weapon_off(entity->handle, entity->ai.current_primary_weapon);

        // Attach entity to vehicle
        bool attached = entity_attach_leech(vehicle, entity->handle, packet.host_tag_handle);
        if (!attached) {
            xlog::warn("vehicle_state: attach_leech failed (iface_pts={}, tag_handle=0x{:x})",
                vehicle->interface_points.size(), packet.host_tag_handle);
            return;
        }

        // --- Vanilla boarding initialization (matches server-side code at 0x004A1E76+) ---

        // Set AI mode to CATATONIC (critical for physics: physics_simulate_entity's
        // automobile input path FUN_0049e180 checks ai.mode to decide whether to
        // process driving input)
        rf::ai_set_mode(&vehicle->ai, rf::AI_MODE_CATATONIC, -1, -1);
        rf::ai_set_submode(&vehicle->ai, rf::AI_SUBMODE_NONE);

        // Set friendliness to neutral (prevents hostility issues)
        rf::obj_set_friendliness(vehicle, rf::OBJ_NEUTRAL);

        // Copy eye PHB limits from EntityInfo to the driver entity
        entity->min_rel_eye_phb = vehicle->info->min_rel_eye_phb;
        entity->max_rel_eye_phb = vehicle->info->max_rel_eye_phb;

        // Enable vehicle mouse control mode (offset 0x720) for all non-gunner vehicles.
        // entity_attach_leech only sets this for turrets; we need it for all vehicle types
        // so mouse input can drive vehicle orientation.
        // Note: for automobile-flagged entities, physics Branch 2 (0x4000) catches before
        // Branch 4 (0x720), so this is redundant for physics but may be used by other
        // systems (input routing, camera control).
        if (!rf::entity_is_jeep_gunner(entity)) {
            *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(vehicle) + 0x720) = 1;
        }

        // Set vehicle entity flags matching what the vanilla boarding code does at 0x004A1E76:
        // Set 0x10000 (entity is being driven), clear 0x20000 (autonomous/AI)
        auto& vflags = *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(vehicle) + 0x810);
        vflags = (vflags & ~0x20000u) | 0x10000u;

        xlog::info("Vehicle board: entity='{}' vehicle='{}' ai.mode={} p_data.flags=0x{:x}",
            entity->name, vehicle->name, static_cast<int>(vehicle->ai.mode), vehicle->p_data.flags);
    }
    else {
        // Exit vehicle
        if (!rf::entity_in_vehicle(entity)) {
            xlog::trace("vehicle_state: entity '{}' already not in vehicle, skipping", entity->name);
            return;
        }

        xlog::info("Vehicle exit: entity '{}'", entity->name);
        rf::entity_detach_from_host(entity);
    }
}

// ============================================================================
// Server-side hooks: detect vehicle entry/exit and broadcast
// ============================================================================

// After entity_attach_leech call in the vehicle entry path
CodeInjection vehicle_entry_broadcast_injection{
    0x004A1E46,
    [](auto& regs) {
        rf::Entity* entity = regs.edi;
        rf::Entity* vehicle = regs.esi;

        if (!entity || !vehicle) {
            return;
        }

        // Debug: log attach result on both server and client
        int attach_result = regs.eax;
        xlog::info("Vehicle entry: entity='{}' vehicle='{}' attach_result={} iface_pts={} host_handle=0x{:x}",
            entity->name, vehicle->name, attach_result,
            vehicle->interface_points.size(), entity->host_handle);

        // Enable vehicle mouse control mode for all non-gunner vehicles.
        // entity_attach_leech only sets this for turrets. We need it on BOTH
        // server and client for flight physics to process mouse input.
        if (rf::entity_in_vehicle(entity) && !rf::entity_is_jeep_gunner(entity)) {
            *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(vehicle) + 0x720) = 1;
        }

        if (!rf::is_server) {
            return;
        }

        // Server: broadcast vehicle state to all clients
        if (!rf::entity_in_vehicle(entity)) {
            return;
        }

        xlog::info("Server: player '{}' entered vehicle '{}'", entity->name, vehicle->name);
        send_vehicle_state_packet_to_all(entity, vehicle, 1);
    },
};

// Global hook on entity_detach_from_host to broadcast vehicle exit from ANY code path
// (Use key, player death, vehicle destruction, etc.)
// Note: entity_detach_from_host actually returns int (1=success, 0=failure) despite the
// void declaration in rf/entity.h. We hook it correctly here.
FunHook<int(rf::Entity*)> entity_detach_from_host_hook{
    0x004279D0,
    [](rf::Entity* ep) -> int {
        // Capture vehicle info before detach
        bool was_in_vehicle = ep && rf::entity_in_vehicle(ep);
        int vehicle_handle = was_in_vehicle ? ep->host_handle : -1;

        // Call original
        int result = entity_detach_from_host_hook.call_target(ep);

        // Broadcast exit if detach succeeded and entity was in a vehicle
        if (result && was_in_vehicle && rf::is_server) {
            rf::Entity* vehicle = rf::entity_from_handle(vehicle_handle);
            if (ep && vehicle) {
                xlog::info("Server: player '{}' exited vehicle '{}'", ep->name, vehicle->name);
                send_vehicle_state_packet_to_all(ep, vehicle, 0);
            }
        }

        return result;
    },
};

// ============================================================================
// Late-joiner sync: send vehicle state for all boarded entities
// ============================================================================

void vehicle_send_state_to_player(rf::Player* player)
{
    if (!rf::is_server || !player) {
        return;
    }

    if (!is_player_minimum_af_client_version(player, 1, 3, 0)) {
        return;
    }

    // First: send vehicle entity creation packets so the client has the entities
    for (auto& entity : DoublyLinkedList{rf::entity_list}) {
        if (entity_type_has_vehicle_use(entity.info_index)) {
            af_send_vehicle_create_packet(player, &entity);
        }
    }

    // Then: send vehicle boarding state for any entities currently in vehicles
    for (auto& entity : DoublyLinkedList{rf::entity_list}) {
        if (rf::entity_in_vehicle(&entity)) {
            rf::Entity* vehicle = rf::entity_from_handle(entity.host_handle);
            if (vehicle) {
                af_send_vehicle_state_packet(player, &entity, vehicle, 1);
            }
        }
    }
}

// ============================================================================
// Fix: skip AI path initialization for vehicle entities in MP
// ============================================================================

// FUN_004270f0 (entity_init_path) sets up path-following for an entity. It
// calls FUN_0040a9b0 which accesses GSolid path node VArrays that can be empty
// in the MP context, causing crashes. Vehicles don't need patrol paths in MP,
// so we skip the entire function for vehicle entity types when in multiplayer.
//
// Fix: FUN_004ae0d0 (player_fpgun_load_weapon_mesh) crashes with "Failed to load
// weapon mesh!" if the mesh handle is 0 after loading. In MP, vehicle weapon fpgun
// meshes aren't pre-loaded. Instead of crashing, return 0 (no fpgun) gracefully.
// At 0x004AE1F3: MOV EAX, [ESI+0x34] loads the mesh handle; if 0, error loop follows.
CodeInjection fpgun_mesh_null_guard{
    0x004AE1F3,
    [](auto& regs) {
        int mesh = *reinterpret_cast<int*>(regs.esi + 0x34);
        if (mesh == 0) {
            regs.eax = 0;
            regs.eip = 0x004AE0FA; // return 0 path (XOR EAX,EAX; POP; RET)
        }
    },
};

// FUN_0040a9b0 processes entity path/navmesh data via GSolid VArrays. In MP these
// VArrays can have NULL data pointers, causing crashes. This function has 6 callers
// and the crash site (0x0040AA42) is a 2-byte instruction that can't fit a 5-byte
// JMP for CodeInjection. Solution: FunHook at the function entry (7-byte instruction
// sequence = room for JMP). Path/patrol processing isn't needed in MP.
FunHook<bool(int)> entity_path_process_hook{
    0x0040A9B0,
    [](int entity_handle) -> bool {
        if (rf::is_multi) {
            return false;
        }
        return entity_path_process_hook.call_target(entity_handle);
    },
};

// ============================================================================
// Vehicle position sync: driving client -> server -> other clients
// ============================================================================

// Track remote vehicle state with interpolation data
struct RemoteVehicleState {
    int uid = -1;
    rf::Vector3 pos_prev{};
    rf::Vector3 pos_target{};
    rf::Matrix3 orient_prev{};
    rf::Matrix3 orient_target{};
    int update_time_ms = 0;        // when target was received
    int prev_update_time_ms = 0;   // when prev was received
    float update_interval_ms = 50.0f; // rolling estimate of packet interval
    bool active = false;
    bool has_prev = false;         // false until 2nd packet received
    uint8_t fire_flags = 0;        // bit 0 = primary, bit 1 = alt fire
    uint8_t prev_fire_flags = 0;   // previous frame's fire flags for edge detection
    int weapon_type = 0;           // current weapon index
};
static constexpr int MAX_REMOTE_VEHICLES = 8;
static RemoteVehicleState remote_vehicles[MAX_REMOTE_VEHICLES];

static void set_remote_vehicle_state(int uid, const rf::Vector3& pos, const float* orient_data,
                                     uint8_t fire_flags, int weapon_type)
{
    int now_ms = rf::timer_get_milliseconds();

    // Find existing or free slot
    int free_slot = -1;
    for (int i = 0; i < MAX_REMOTE_VEHICLES; i++) {
        if (remote_vehicles[i].uid == uid) {
            auto& rv = remote_vehicles[i];
            // Shift current target -> prev
            rv.pos_prev = rv.pos_target;
            rv.orient_prev = rv.orient_target;
            rv.prev_update_time_ms = rv.update_time_ms;

            // Store new target
            rv.pos_target = pos;
            std::memcpy(&rv.orient_target, orient_data, sizeof(rf::Matrix3));
            rv.update_time_ms = now_ms;

            // Update rolling average of packet interval
            if (rv.has_prev && rv.prev_update_time_ms > 0) {
                float interval = static_cast<float>(now_ms - rv.prev_update_time_ms);
                if (interval > 0.0f && interval < 500.0f) {
                    rv.update_interval_ms = rv.update_interval_ms * 0.7f + interval * 0.3f;
                }
            }
            rv.has_prev = true;

            // Weapon fire state
            rv.fire_flags = fire_flags;
            rv.weapon_type = weapon_type;
            return;
        }
        if (free_slot < 0 && !remote_vehicles[i].active) free_slot = i;
    }
    if (free_slot >= 0) {
        auto& rv = remote_vehicles[free_slot];
        rv.uid = uid;
        rv.pos_target = pos;
        std::memcpy(&rv.orient_target, orient_data, sizeof(rf::Matrix3));
        rv.pos_prev = pos;
        std::memcpy(&rv.orient_prev, orient_data, sizeof(rf::Matrix3));
        rv.update_time_ms = now_ms;
        rv.prev_update_time_ms = 0;
        rv.update_interval_ms = 50.0f;
        rv.has_prev = false;
        rv.active = true;
        rv.fire_flags = fire_flags;
        rv.prev_fire_flags = 0;
        rv.weapon_type = weapon_type;
    }
}

static void clear_remote_vehicle_state(int uid)
{
    for (auto& rv : remote_vehicles) {
        if (rv.uid == uid) { rv.active = false; rv.uid = -1; }
    }
}

// Normalize a Matrix3's rows to prevent drift from linear interpolation
static void matrix3_orthonormalize(rf::Matrix3& m)
{
    // Normalize fvec
    m.fvec.normalize_safe();
    // Re-derive rvec = uvec x fvec, then normalize
    m.rvec = m.uvec.cross(m.fvec);
    m.rvec.normalize_safe();
    // Re-derive uvec = fvec x rvec
    m.uvec = m.fvec.cross(m.rvec);
    m.uvec.normalize_safe();
}

// Apply stored remote vehicle states with interpolation. Called AFTER gameplay_sim_frame
// (which runs physics/ObjInterp and overwrites orient) but BEFORE rendering reads it.
static void apply_remote_vehicle_states()
{
    if (rf::is_server || !rf::is_multi) return;

    int now_ms = rf::timer_get_milliseconds();

    for (auto& rv : remote_vehicles) {
        if (!rv.active) continue;
        rf::Entity* vehicle = entity_find_by_uid(rv.uid);
        if (!vehicle) { rv.active = false; continue; }

        // Don't override if local player is driving this vehicle
        if (rf::local_player) {
            rf::Entity* local_entity = rf::entity_from_handle(rf::local_player->entity_handle);
            if (local_entity && rf::entity_in_vehicle(local_entity) &&
                local_entity->host_handle == vehicle->handle) {
                continue;
            }
        }

        // Compute interpolation factor
        rf::Vector3 lerped_pos;
        rf::Matrix3 lerped_orient;

        if (rv.has_prev && rv.update_interval_ms > 0.0f) {
            float elapsed = static_cast<float>(now_ms - rv.update_time_ms);
            float t = std::clamp(elapsed / rv.update_interval_ms, 0.0f, 1.0f);

            // Lerp position
            lerped_pos = rv.pos_prev + (rv.pos_target - rv.pos_prev) * t;

            // Component-wise lerp of orientation matrix (sufficient for small angular deltas)
            float* prev = reinterpret_cast<float*>(&rv.orient_prev);
            float* tgt = reinterpret_cast<float*>(&rv.orient_target);
            float* out = reinterpret_cast<float*>(&lerped_orient);
            for (int i = 0; i < 9; i++) {
                out[i] = prev[i] + (tgt[i] - prev[i]) * t;
            }
            matrix3_orthonormalize(lerped_orient);
        }
        else {
            // No previous state yet — snap to target
            lerped_pos = rv.pos_target;
            lerped_orient = rv.orient_target;
        }

        // Write orient directly — the sim already ran, so this won't be overwritten before render
        std::memcpy(&vehicle->orient, &lerped_orient, sizeof(rf::Matrix3));

        // Update position via move() (handles room/cell update)
        vehicle->move(&lerped_pos);

        // Weapon fire edge detection
        if (rv.fire_flags != rv.prev_fire_flags) {
            bool primary_now = (rv.fire_flags & 0x1) != 0;
            bool primary_was = (rv.prev_fire_flags & 0x1) != 0;
            bool alt_now = (rv.fire_flags & 0x2) != 0;
            bool alt_was = (rv.prev_fire_flags & 0x2) != 0;

            if (primary_now && !primary_was) {
                rf::entity_turn_weapon_on(vehicle->handle, rv.weapon_type, false);
            }
            else if (!primary_now && primary_was) {
                rf::entity_turn_weapon_off(vehicle->handle, rv.weapon_type);
            }

            if (alt_now && !alt_was) {
                rf::entity_turn_weapon_on(vehicle->handle, rv.weapon_type, true);
            }
            else if (!alt_now && alt_was) {
                rf::entity_turn_weapon_off(vehicle->handle, rv.weapon_type);
            }

            rv.prev_fire_flags = rv.fire_flags;
        }
    }
}

// ============================================================================
// Enable vehicle input for MP client
// ============================================================================
//
// gameplay_do_frame (0x00433520) handles BOTH SP/server AND MP client frames.
// For MP client (is_multi && !is_server && state==0xB), BL is set to 1 and the
// JNZ at 0x004335c9 skips both player_process_controls and gameplay_sim_frame,
// landing at 0x00433640. The SP/server path (BL=0) falls through normally.
//
// For vehicles, we need both input reading and physics simulation on the client.
// player_process_controls can't be used directly — it takes a state-2 shortcut
// for MP clients that skips vehicle input routing. Instead we call
// controls_read_internal directly with the vehicle's ai.ci.


// obj_should_sim_physics (0x00488030): called per-object in obj_move_all Pass 2.
// Returns 1 if the object should get physics simulation this frame, 0 to skip.
// Vehicle entities can be skipped due to several conditions:
// - obj_flags & 0x4000 (OF_NO_COLLIDE_SP) → always skip
// - entity_flags2 & 0x40 for OT_ENTITY → always skip
// - Not player-controlled + distance/state checks → skip if far away or idle
// We force return 1 for boarded vehicle entities so the full physics pipeline
// (physics_simulate_entity → mouselook/vehicle rotation → translation) runs.
FunHook<int(rf::Object*)> obj_should_sim_physics_hook{
    0x00488030,
    [](rf::Object* obj) -> int {
        if (rf::is_multi && obj->type == rf::OT_ENTITY) {
            auto* entity = static_cast<rf::Entity*>(obj);
            // Check if this is a boarded vehicle (has a leech attached)
            if (rf::entity_is_vehicle(entity) && rf::entity_get_first_leech(entity) != -1) {
                return 1;
            }
        }
        return obj_should_sim_physics_hook.call_target(obj);
    },
};

// controls_read_internal: reads raw mouse/keyboard input into a ControlInfo.
// Called via controls_read (0x00430720) which has an MP gate — we bypass it
// with the NOP at 0x0043074a, but we also call this directly for vehicle input.
static auto controls_read_internal =
    reinterpret_cast<void(__cdecl*)(rf::Player*, rf::ControlInfo*)>(0x00430760);

// mp_client_do_frame (0x0045B5E0) is the ACTUAL frame handler for MP clients
// (NOT gameplay_do_frame at 0x00433520 which is for SP/listen server).
// It forces camera to mode 2 (free cam), calls player_free_cam_update which
// consumes mouse input, then calls gameplay_sim_frame at 0x0045B877.
//
// We inject at TWO points:
// 1. After game_poll (0x0045B62B): capture mouse input into vehicle ci BEFORE
//    player_free_cam_update consumes it, then skip the free cam code
// 2. (gameplay_sim_frame already runs at 0x0045B877 with the NOP at 0x004332a8)

// Injection after game_poll in mp_client_do_frame — captures input for vehicles
// before the free cam system consumes mouse deltas.
// At 0x0045B62B: MOV ECX, [0x007c75d4] — first instruction after game_poll returns.
CodeInjection mp_client_vehicle_input_injection{
    0x0045B62B,
    [](auto& regs) {
        if (!rf::local_player) return;
        rf::Entity* entity = rf::entity_from_handle(rf::local_player->entity_handle);
        if (!entity || !rf::entity_in_vehicle(entity)) return;
        rf::Entity* vehicle = rf::entity_from_handle(entity->host_handle);
        if (!vehicle) return;

        // Read input into vehicle's ai.ci. This captures mouse deltas BEFORE
        // player_free_cam_update (called at 0x0045B64B) consumes them.
        controls_read_internal(rf::local_player, &vehicle->ai.ci);

        // Restore flight control flag (field_18 at ci+0x18 = entity+0x720).
        // controls_read_internal may reset it; physics_simulate_entity Branch 4
        // checks it for mouselook/flight rotation.
        if (!(vehicle->p_data.flags & rf::PF_AUTOMOBILE)) {
            vehicle->ai.ci.field_18 = true;
        }

        // Skip the free cam update and controls_process-with-entity-ci block
        // (0x0045B62B to 0x0045B67A). Jump directly to 0x0045B67A so mouse
        // input goes to the vehicle, not the camera.
        regs.eip = 0x0045B67A;
    },
};

// Hook gameplay_sim_frame (0x00433260) to apply remote vehicle orient AFTER
// all simulation (physics, ObjInterp) but BEFORE rendering starts.
FunHook<void()> gameplay_sim_frame_hook{
    0x00433260,
    []() {
        gameplay_sim_frame_hook.call_target();
        // Now simulation is done, rendering hasn't started yet — safe to override orient
        apply_remote_vehicle_states();
    },
};

void af_process_vehicle_position_packet(const void* data, size_t len, [[maybe_unused]] const rf::NetAddr& addr)
{
    if (len < sizeof(af_vehicle_position_packet)) {
        return;
    }

    af_vehicle_position_packet packet{};
    std::memcpy(&packet, data, sizeof(packet));

    if (rf::is_server) {
        // Server: apply position to vehicle entity and forward to other clients
        rf::Entity* vehicle = entity_find_by_uid(packet.vehicle_uid);
        if (!vehicle) {
            xlog::trace("vehicle_position: vehicle uid {} not found on server", packet.vehicle_uid);
            return;
        }

        rf::Vector3 new_pos{packet.pos.x, packet.pos.y, packet.pos.z};
        vehicle->move(&new_pos);
        std::memcpy(&vehicle->orient, packet.orient, sizeof(packet.orient));

        // Forward to all other clients
        for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
            if (&player == rf::local_player) continue;
            if (!is_player_minimum_af_client_version(&player, 1, 3, 0)) continue;
            // Skip the player who sent this (their entity is driving the vehicle)
            rf::Entity* player_entity = rf::entity_from_handle(player.entity_handle);
            if (player_entity && rf::entity_in_vehicle(player_entity) &&
                player_entity->host_handle == vehicle->handle) {
                continue;
            }
            af_send_packet(&player, &packet, sizeof(packet), false); // unreliable
        }
    }
    else {
        // Client: apply position to local vehicle entity
        rf::Entity* vehicle = entity_find_by_uid(packet.vehicle_uid);
        if (!vehicle) return;

        // Don't override if local player is driving this vehicle
        if (rf::local_player) {
            rf::Entity* local_entity = rf::entity_from_handle(rf::local_player->entity_handle);
            if (local_entity && rf::entity_in_vehicle(local_entity) &&
                local_entity->host_handle == vehicle->handle) {
                return;
            }
        }

        // Store state for per-frame interpolation and fire sync
        rf::Vector3 new_pos{packet.pos.x, packet.pos.y, packet.pos.z};
        set_remote_vehicle_state(packet.vehicle_uid, new_pos, packet.orient,
                                 packet.fire_flags, packet.weapon_type);
    }
}

// Client: send vehicle position to server when local player is driving
static int vehicle_pos_send_timestamp = 0;

static void vehicle_send_position_to_server()
{
    if (rf::is_server || !rf::is_multi) return;
    if (!rf::local_player) return;

    rf::Entity* entity = rf::entity_from_handle(rf::local_player->entity_handle);
    if (!entity || !rf::entity_in_vehicle(entity)) return;

    // Rate limit: send every ~50ms (same rate as obj_update)
    int now = rf::timer_get(1000);
    if (vehicle_pos_send_timestamp != 0 && (now - vehicle_pos_send_timestamp) < 50) return;
    vehicle_pos_send_timestamp = now;
    if (rf::entity_is_jeep_gunner(entity)) return;

    rf::Entity* vehicle = rf::entity_from_handle(entity->host_handle);
    if (!vehicle) return;

    af_vehicle_position_packet packet{};
    packet.header.type = static_cast<uint8_t>(af_packet_type::af_vehicle_position);
    packet.header.size = sizeof(packet) - sizeof(packet.header);
    packet.vehicle_uid = vehicle->uid;
    packet.pos.x = vehicle->pos.x;
    packet.pos.y = vehicle->pos.y;
    packet.pos.z = vehicle->pos.z;
    std::memcpy(packet.orient, &vehicle->orient, sizeof(packet.orient));
    rf::Vector3 phb = orient_to_phb(vehicle->orient);
    packet.phb = {phb.x, phb.y, phb.z};

    // Include weapon fire state — vehicle entities own the weapon, not the driver
    int veh_weapon = vehicle->ai.current_primary_weapon;
    packet.weapon_type = static_cast<uint8_t>(veh_weapon);
    packet.fire_flags = 0;
    if (rf::entity_weapon_is_on(vehicle->handle, veh_weapon)) {
        packet.fire_flags |= 0x1;
    }
    // Alt fire check
    int veh_alt_weapon = vehicle->ai.current_secondary_weapon;
    if (veh_alt_weapon >= 0 && rf::entity_weapon_is_on(vehicle->handle, veh_alt_weapon)) {
        packet.fire_flags |= 0x2;
    }

    af_send_packet(rf::local_player, &packet, sizeof(packet), false);
}

// Called every frame from rf_do_frame_hook in main.cpp
void vehicle_do_frame()
{
    // Apply remote vehicle interpolation for MP clients. gameplay_sim_frame (and
    // its hook that calls apply_remote_vehicle_states) only runs when the local
    // player is driving a vehicle. For spectating other players' vehicles, we
    // need to apply interpolation here as well.
    if (rf::is_multi && !rf::is_server) {
        apply_remote_vehicle_states();
    }
    vehicle_send_position_to_server();
}

// ============================================================================
// Phase 1: Remove multiplayer vehicle boarding blocks
// ============================================================================

// The function player_handle_use_keypress_local (0x004A1970) has two net_game checks
// that prevent vehicle entry/exit in multiplayer. We NOP the conditional jumps.

void vehicle_apply_patches()
{
    // --- Level load: allow vehicle entity creation on the SERVER ---
    //
    // level_read_entities (0x00464010) filters out vehicle entity types
    // (use_function == 1) in MP. We NOP the 6-byte JNZ at 0x00464657.
    // Note: the client never calls level_read_entities in MP (level_load skips
    // the entity section for clients). Vehicle entities are synced to clients
    // via af_vehicle_create packets during state_info instead.
    AsmWriter(0x00464657).nop(6);

    // --- Exit block in player_handle_use_keypress_local ---
    // 004a198e: MOV CL, byte ptr [0x0064ecb9]  ; load rf::is_multi
    // 004a1994: TEST CL, CL
    // 004a1996: JNZ 0x004a1b6d                  ; if multiplayer, skip all exit code
    // NOP the 6-byte JNZ (near conditional jump: 0F 85 xx xx xx xx)
    AsmWriter(0x004A1996).nop(6);

    // --- Entry block in player_handle_use_keypress_local ---
    // 004a1cbf: CMP byte ptr [0x0064ecb9], 0x1  ; compare rf::is_multi to 1
    // 004a1cc6: JZ 0x004a1b36                   ; if multiplayer, skip all entry code
    // NOP the 7-byte CMP and 6-byte JZ
    AsmWriter(0x004A1CBF).nop(13);

    // --- Fix: bypass controls_read MP gate for vehicle input ---
    //
    // controls_read (0x00430720) has a gate at 0x0043074a that blocks input
    // reading for MP clients when net_data->flags & 0x20 is set. NOP the
    // 2-byte JNZ so player_process_controls can read input via controls_read.
    AsmWriter(0x0043074a).nop(2);

    // --- Fix: enable world collision for vehicle entities in MP ---
    //
    // The physics step function (FUN_00487770) has a multiplayer filter that only runs
    // world collision (FUN_0049bb70) for the local_player_entity and non-OT_ENTITY objects.
    // Vehicle entities are OT_ENTITY type but NOT the local player, so they're skipped.
    // In singleplayer, ALL entities with PF_COLLIDE_WORLD get world collision.
    //
    // At 0x00487893: JZ 0x004878bb (74 26) — skips collision when entity->type == 0.
    // NOP this to let vehicle entities (and all OT_ENTITY) get world collision in MP.
    // The PF_COLLIDE_WORLD check at 0x0048789f still gates the actual collision call.
    AsmWriter(0x00487893).nop(2);

    // gameplay_sim_frame (0x00433260) has an internal early-return for MP clients
    // when net_data->flags & 0x20 is set (JNZ at 0x004332a8, 6 bytes). NOP it so
    // the physics simulation runs when called from our vehicle input injection.
    AsmWriter(0x004332a8).nop(6);

    // --- Fix: skip path processing in MP (prevents GSolid VArray crashes) ---
    entity_path_process_hook.install();

    // --- Fix: handle missing fpgun meshes for vehicle weapons ---
    fpgun_mesh_null_guard.install();

    // --- Force physics active for boarded vehicles in MP ---
    obj_should_sim_physics_hook.install();

    // --- Client: capture vehicle input before free cam consumes it in mp_client_do_frame ---
    mp_client_vehicle_input_injection.install();

    // --- Server-side hooks for network sync ---
    vehicle_entry_broadcast_injection.install();
    entity_detach_from_host_hook.install();

    // --- Client: apply remote vehicle orient after sim, before render ---
    gameplay_sim_frame_hook.install();
}
