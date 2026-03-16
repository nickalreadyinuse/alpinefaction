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
        xlog::info("Vehicle created on client: type='{}' uid={} pos=({:.1f},{:.1f},{:.1f})",
                   entity->info->name, entity->uid, pos.x, pos.y, pos.z);
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

        // Enable vehicle mouse control mode on the vehicle entity's control data.
        // Vanilla entity_attach_leech only sets this for turret entities (flag 0x2000),
        // but fighters/subs/etc. also need it for proper mouse-driven rotation.
        if (!rf::entity_is_jeep_gunner(entity)) {
            *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(vehicle) + 0x720) = 1;
        }

        // Set vehicle entity flags matching what the vanilla boarding code does at 0x004A1E76:
        // Set 0x10000 (entity is being driven), clear 0x20000 (autonomous/AI)
        auto& vflags = *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(vehicle) + 0x810);
        vflags = (vflags & ~0x20000u) | 0x10000u;

        xlog::info("Vehicle enter success: entity '{}' host_handle=0x{:x}", entity->name, entity->host_handle);
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

// Debug: log when player_handle_use_keypress_local is called (entry path)
// At 0x004A197F: MOV EAX, [EDI+0x200] - first logic instruction, EBX=player, EDI=entity
CodeInjection use_keypress_debug{
    0x004A197F,
    [](auto& regs) {
        if (rf::is_server) return;
        rf::Entity* entity = regs.edi;
        if (entity) {
            xlog::info("use_keypress_local: entity='{}' host_handle=0x{:x}",
                entity->name, entity->host_handle);
        }
    },
};

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

        if (!rf::is_server) {
            return;
        }

        // Server: verify the entity is actually now in the vehicle
        if (!rf::entity_in_vehicle(entity)) {
            return;
        }

        // Enable vehicle mouse control mode for all vehicle types (not just turrets)
        if (!rf::entity_is_jeep_gunner(entity)) {
            *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(vehicle) + 0x720) = 1;
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

// Track remote vehicle state for per-frame application (prevents entity sim from overwriting)
struct RemoteVehicleState {
    int uid = -1;
    rf::Vector3 pos{};
    rf::Matrix3 orient{};
    rf::Vector3 phb{};
    bool active = false;
};
static constexpr int MAX_REMOTE_VEHICLES = 8;
static RemoteVehicleState remote_vehicles[MAX_REMOTE_VEHICLES];

static void set_remote_vehicle_state(int uid, const rf::Vector3& pos, const float* orient_data, const rf::Vector3& phb)
{
    // Find existing or free slot
    int free_slot = -1;
    for (int i = 0; i < MAX_REMOTE_VEHICLES; i++) {
        if (remote_vehicles[i].uid == uid) {
            remote_vehicles[i].pos = pos;
            std::memcpy(&remote_vehicles[i].orient, orient_data, sizeof(rf::Matrix3));
            remote_vehicles[i].phb = phb;
            remote_vehicles[i].active = true;
            return;
        }
        if (free_slot < 0 && !remote_vehicles[i].active) free_slot = i;
    }
    if (free_slot >= 0) {
        remote_vehicles[free_slot].uid = uid;
        remote_vehicles[free_slot].pos = pos;
        std::memcpy(&remote_vehicles[free_slot].orient, orient_data, sizeof(rf::Matrix3));
        remote_vehicles[free_slot].phb = phb;
        remote_vehicles[free_slot].active = true;
    }
}

static void clear_remote_vehicle_state(int uid)
{
    for (auto& rv : remote_vehicles) {
        if (rv.uid == uid) { rv.active = false; rv.uid = -1; }
    }
}

// Apply stored remote vehicle states. Called AFTER gameplay_sim_frame (which runs
// physics/ObjInterp and overwrites orient) but BEFORE rendering reads it.
// This is the only reliable timing to override orient for remote vehicles.
static void apply_remote_vehicle_states()
{
    if (rf::is_server || !rf::is_multi) return;

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

        // Write orient directly — the sim already ran, so this won't be overwritten before render
        std::memcpy(&vehicle->orient, &rv.orient, sizeof(rf::Matrix3));

        // Update position via move() (handles room/cell update)
        vehicle->move(&rv.pos);
    }
}

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

        // Update vehicle position and orientation on the server
        static int recv_count = 0;
        if (++recv_count % 100 == 1) {  // log every 100th receive
            xlog::warn("vehicle_pos_recv: uid={} pos=({:.1f},{:.1f},{:.1f})",
                packet.vehicle_uid, packet.pos.x, packet.pos.y, packet.pos.z);
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

        // Store state for per-frame application via ObjInterp
        rf::Vector3 new_pos{packet.pos.x, packet.pos.y, packet.pos.z};
        rf::Vector3 phb{packet.phb.x, packet.phb.y, packet.phb.z};
        set_remote_vehicle_state(packet.vehicle_uid, new_pos, packet.orient, phb);
    }
}

// Client: send vehicle position to server when local player is driving
static int vehicle_pos_send_timestamp = 0;

static void vehicle_send_position_to_server()
{
    // Debug: log state when in MP to trace position sync
    static int dbg_count = 0;
    if (dbg_count < 5 && rf::is_multi && !rf::is_server && rf::local_player) {
        rf::Entity* dbg_entity = rf::entity_from_handle(rf::local_player->entity_handle);
        bool in_v = dbg_entity ? rf::entity_in_vehicle(dbg_entity) : false;
        if (in_v || dbg_count == 0) {
            dbg_count++;
            xlog::warn("veh_sync: entity={} in_vehicle={} host_handle=0x{:x}",
                dbg_entity ? 1 : 0, (int)in_v,
                dbg_entity ? dbg_entity->host_handle : -1);
        }
    }

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

    static int send_count = 0;
    if (++send_count % 100 == 1) {  // log every 100th send (~5 seconds)
        xlog::warn("vehicle_pos_send: uid={} pos=({:.1f},{:.1f},{:.1f}) fvec=({:.2f},{:.2f},{:.2f})",
            vehicle->uid, vehicle->pos.x, vehicle->pos.y, vehicle->pos.z,
            vehicle->orient.fvec.x, vehicle->orient.fvec.y, vehicle->orient.fvec.z);
    }

    af_send_packet(rf::local_player, &packet, sizeof(packet), false);
}

// Called every frame from rf_do_frame_hook in main.cpp
void vehicle_do_frame()
{
    vehicle_send_position_to_server();
    // Note: apply_remote_vehicle_states() is called from gameplay_sim_frame_hook
    // (after sim, before render) — NOT here, which runs outside the sim/render boundary.
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

    // --- Fix: skip path processing in MP (prevents GSolid VArray crashes) ---
    entity_path_process_hook.install();

    // --- Fix: handle missing fpgun meshes for vehicle weapons ---
    fpgun_mesh_null_guard.install();


    // --- Debug: trace local use-key processing on client ---
    use_keypress_debug.install();

    // --- Server-side hooks for network sync ---
    vehicle_entry_broadcast_injection.install();
    entity_detach_from_host_hook.install();

    // --- Client: apply remote vehicle orient after sim, before render ---
    gameplay_sim_frame_hook.install();
}
