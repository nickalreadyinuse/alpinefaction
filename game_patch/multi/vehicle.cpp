#include <cstring>
#include <patch_common/CodeInjection.h>
#include <patch_common/FunHook.h>
#include <patch_common/AsmWriter.h>
#include <common/utils/list-utils.h>
#include <xlog/xlog.h>
#include "../rf/entity.h"
#include "../rf/multi.h"
#include "../rf/player/player.h"
#include "../rf/object.h"
#include "alpine_packets.h"
#include "network.h"
#include "vehicle.h"
#include "../misc/player.h"

// Thiscall wrapper for Entity::attach_leech(int leech_obj_handle, int interface_tag_handle)
static auto entity_attach_leech = reinterpret_cast<bool(__thiscall*)(rf::Entity*, int, int)>(0x00427240);

// FUN_0042cdd0: returns true if entity_types[index].use_function == 1 (boardable vehicle)
static auto entity_type_has_vehicle_use = reinterpret_cast<bool(__cdecl*)(int type_index)>(0x0042cdd0);

// ============================================================================
// Packet send/receive
// ============================================================================

static void send_vehicle_state_packet_to_all(rf::Entity* entity, rf::Entity* vehicle, uint8_t action)
{
    af_vehicle_state_packet packet{};
    packet.header.type = static_cast<uint8_t>(af_packet_type::af_vehicle_state);
    packet.header.size = sizeof(packet) - sizeof(packet.header);
    packet.entity_handle = entity->handle;
    packet.vehicle_handle = vehicle->handle;
    packet.action = action;
    packet.host_tag_handle = entity->host_tag_handle;

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

    rf::Object* entity_obj = rf::obj_from_remote_handle(packet.entity_handle);
    rf::Object* vehicle_obj = rf::obj_from_remote_handle(packet.vehicle_handle);

    if (!entity_obj) {
        xlog::warn("vehicle_state: entity not found for remote handle 0x{:x}", packet.entity_handle);
        return;
    }

    auto* entity = static_cast<rf::Entity*>(entity_obj);

    if (packet.action == 1) {
        // Enter vehicle
        if (!vehicle_obj) {
            xlog::warn("vehicle_state: vehicle not found for remote handle 0x{:x}", packet.vehicle_handle);
            return;
        }

        auto* vehicle = static_cast<rf::Entity*>(vehicle_obj);

        // Already in this vehicle (local player already processed this locally)
        if (entity->host_handle == vehicle->handle) {
            xlog::trace("vehicle_state: entity '{}' already in vehicle, skipping", entity->name);
            return;
        }

        xlog::info("Vehicle enter: entity '{}' -> vehicle '{}'", entity->name, vehicle->name);

        // Turn off current weapon
        rf::entity_turn_weapon_off(entity->handle, entity->ai.current_primary_weapon);

        // Attach entity to vehicle (same function the server used)
        bool attached = entity_attach_leech(vehicle, entity->handle, packet.host_tag_handle);
        if (!attached) {
            xlog::warn("vehicle_state: attach_leech failed");
            return;
        }
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

// After successful attach_leech call in the vehicle entry path
CodeInjection vehicle_entry_broadcast_injection{
    0x004A1E46,
    [](auto& regs) {
        if (!rf::is_server) {
            return;
        }

        rf::Entity* entity = regs.edi;
        rf::Entity* vehicle = regs.esi;

        if (!entity || !vehicle) {
            return;
        }

        // Verify the entity is actually now in the vehicle
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


    // --- Server-side hooks for network sync ---
    vehicle_entry_broadcast_injection.install();
    entity_detach_from_host_hook.install();
}
