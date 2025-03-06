#include <cstddef>
#include <cstring>
#include <cassert>
#include <array>
#include <common/utils/list-utils.h>
#include <common/rfproto.h>
#include <xlog/xlog.h>
#include "../rf/multi.h"
#include "../rf/player/player.h"
#include "../rf/weapon.h"
#include "multi.h"
#include "server_internal.h"
#include "../hud/hud_world.h"
#include "alpine_packets.h"
#include "../misc/player.h"

void af_send_packet(rf::Player* player, const void* data, int len, bool is_reliable)
{
    if (!player || !player->net_data) {
        xlog::error("af_send_packet: Attempted to send to invalid player");
        return;
    }
    if (len <= 0 || len > rf::max_packet_size) {
        xlog::error("af_send_packet: Packet size {} exceeds max {}", len, rf::max_packet_size);
        return;
    }

    //xlog::info("Sending packet: player={}, size={}, reliable={}", player->name, len, is_reliable);

    if (is_reliable) {
        rf::multi_io_send_buffered_reliable_packets(player);
        rf::multi_io_send_reliable(player, data, len, 0);
        rf::multi_io_send_buffered_reliable_packets(player);
    }
    else {
        //rf::net_send(player->net_data->addr, data, len);
        rf::multi_io_send(player, data, len);
    }
}

bool af_process_packet(const void* data, int len, const rf::NetAddr& addr, rf::Player* player)
{
    RF_GamePacketHeader header{};
    if (len < static_cast<int>(sizeof(header))) {
        return false;
    }

    std::memcpy(&header, data, sizeof(header));
    auto packet_type = static_cast<af_packet_type>(header.type);

    switch (packet_type)
    {
        case af_packet_type::af_ping_location_req:
            af_process_ping_location_req_packet(data, len, addr);
            break;
        case af_packet_type::af_ping_location:
            af_process_ping_location_packet(data, len, addr);
            break;
        case af_packet_type::af_damage_notify:
            af_process_damage_notify_packet(data, len, addr);
            break;
        case af_packet_type::af_obj_update:
            af_process_obj_update_packet(data, len, addr);
            break;
        default:
            return false; // ignore if unrecognized
    }
    return true;
}

void af_send_ping_location_req_packet(rf::Vector3* pos)
{
    // Send: client -> server
    if (!rf::is_multi || rf::is_server || rf::is_dedicated_server) {
        return;
    }

    std::byte packet_buf[rf::max_packet_size];
    af_ping_location_req_packet ping_location_req_packet{};
    ping_location_req_packet.header.type = static_cast<uint8_t>(af_packet_type::af_ping_location_req);
    ping_location_req_packet.header.size = sizeof(ping_location_req_packet) - sizeof(ping_location_req_packet.header);
    ping_location_req_packet.pos.x = pos->x;
    ping_location_req_packet.pos.y = pos->y;
    ping_location_req_packet.pos.z = pos->z;
    std::memcpy(packet_buf, &ping_location_req_packet, sizeof(ping_location_req_packet));
    af_send_packet(rf::local_player, packet_buf, sizeof(ping_location_req_packet), false);
}

static void af_process_ping_location_req_packet(const void* data, size_t len, const rf::NetAddr& addr)
{
    // Receive: server <- client
    if (!rf::is_dedicated_server) {
        return;
    }

    // ignore ping_location_req packets if location pinging is configured off
    if (!server_get_df_config().location_pinging) {
        return;
    }

    af_ping_location_req_packet ping_location_req_packet{};
    ping_location_req_packet.header.type = static_cast<uint8_t>(af_packet_type::af_ping_location_req);
    ping_location_req_packet.header.size = sizeof(ping_location_req_packet) - sizeof(ping_location_req_packet.header);

    int packet_len = sizeof(ping_location_req_packet);

    rf::Player* player = rf::multi_find_player_by_addr(addr);

    std::memcpy(&ping_location_req_packet, data, sizeof(ping_location_req_packet));

    rf::Vector3 pos = {
        pos.x = ping_location_req_packet.pos.x,
        pos.y = ping_location_req_packet.pos.y,
        pos.z = ping_location_req_packet.pos.z
    };
    af_send_ping_location_packet_to_team(&pos, player->net_data->player_id, player->team);
}

void af_send_ping_location_packet(rf::Vector3* pos, uint8_t player_id, rf::Player* player)
{
    // Send: server -> client
    assert(rf::is_server);

    std::byte packet_buf[rf::max_packet_size];
    af_ping_location_packet ping_location_packet{};
    ping_location_packet.header.type = static_cast<uint8_t>(af_packet_type::af_ping_location);
    ping_location_packet.header.size = sizeof(ping_location_packet) - sizeof(ping_location_packet.header);
    ping_location_packet.pos.x = pos->x;
    ping_location_packet.pos.y = pos->y;
    ping_location_packet.pos.z = pos->z;
    ping_location_packet.player_id = player_id;

    std::memcpy(packet_buf, &ping_location_packet, sizeof(ping_location_packet));

    if (!player) {
        xlog::error("af_ping_location_packet: Attempted to send to a null player");
        return;
    }
    af_send_packet(player, packet_buf, sizeof(ping_location_packet), false);
}

void af_send_ping_location_packet_to_team(rf::Vector3* pos, uint8_t player_id, rf::ubyte team)
{
    SinglyLinkedList<rf::Player> linked_player_list{rf::player_list};

    for (auto& player : linked_player_list) {
        if (!&player) continue;  // Skip invalid player
        if (&player == rf::local_player) continue;  // Skip the local player
        if (player.net_data->player_id == player_id) continue; // Skip the sender
        if (player.team != team) continue; // Skip if not on the correct team

        af_send_ping_location_packet(pos, player_id, &player);
    }
}

static void af_process_ping_location_packet(const void* data, size_t len, const rf::NetAddr& addr)
{
    // Receive: client <- server
    if (!rf::is_multi || rf::is_server || rf::is_dedicated_server) {
        return;
    }

    af_ping_location_packet ping_location_packet{};
    if (len < sizeof(ping_location_packet)) {
        return;
    }

    std::memcpy(&ping_location_packet, data, sizeof(ping_location_packet));

    rf::Player* player = rf::multi_find_player_by_id(ping_location_packet.player_id);
    if (!player) {
        return;
    }

    rf::Vector3 pos = {
        pos.x = ping_location_packet.pos.x,
        pos.y = ping_location_packet.pos.y,
        pos.z = ping_location_packet.pos.z
    };

    add_location_ping_world_hud_sprite(pos, player->name, ping_location_packet.player_id);
}

void af_send_damage_notify_packet(uint8_t player_id, float damage, bool died, rf::Player* player)
{
    // Send: server -> client
    assert(rf::is_server);

    std::byte packet_buf[rf::max_packet_size];
    af_damage_notify_packet damage_notify_packet{};
    damage_notify_packet.header.type = static_cast<uint8_t>(af_packet_type::af_damage_notify);
    damage_notify_packet.header.size = sizeof(damage_notify_packet) - sizeof(damage_notify_packet.header);
    damage_notify_packet.player_id = player_id;
    int rounded_damage = std::round(damage);
    damage_notify_packet.damage = static_cast<uint16_t>(std::max(1, rounded_damage)); // round damage with minimum 1

    damage_notify_packet.flags = 0; // init flags
    damage_notify_packet.flags = (damage_notify_packet.flags & ~0x01) | (died << 0);

    std::memcpy(packet_buf, &damage_notify_packet, sizeof(damage_notify_packet));

    if (!player) {
        xlog::error("af_damage_notify_packet: Attempted to send to an invalid player");
        return;
    }
    af_send_packet(player, packet_buf, sizeof(damage_notify_packet), false);
}

static void af_process_damage_notify_packet(const void* data, size_t len, const rf::NetAddr& addr)
{
    // Receive: client <- server
    if (!rf::is_multi || rf::is_server || rf::is_dedicated_server) {
        return;
    }

    af_damage_notify_packet damage_notify_packet{};
    if (len < sizeof(damage_notify_packet)) {
        return;
    }

    std::memcpy(&damage_notify_packet, data, sizeof(damage_notify_packet));

    rf::Player* player = rf::multi_find_player_by_id(damage_notify_packet.player_id);
    if (!player) {
        return;
    }

    rf::Entity* entity = rf::entity_from_handle(player->entity_handle);
    if (!entity) {
        return;
    }

    bool died = static_cast<bool>(damage_notify_packet.flags & 0x01);
    add_damage_notify_world_hud_string(entity->pos, damage_notify_packet.player_id, damage_notify_packet.damage, died);
    play_local_hit_sound(died);
}

void af_send_obj_update_packet(rf::Player* player)
{
    // Send: server -> client
    assert(rf::is_server);

    std::vector<af_obj_update> obj_updates;
    auto player_list = SinglyLinkedList{rf::player_list};

    // loop through players to gather info
    for (auto& other_player : player_list) {
        //xlog::info("starting payer list loop");
        if (!&other_player) {
            continue; // player not valid
        }

        if (&other_player == player) {
            continue; // player is myself
        }

        if (rf::player_is_dead(&other_player)) {
            continue; // player is dead
        }

        rf::Entity* entity = rf::entity_from_handle(other_player.entity_handle);
        if (!entity) {
            continue; // player entity is invalid or not spawned
        }

        if (rf::entity_is_dying(entity)) {
            continue; // player entity is dying (dying entities have invalid info in ai)
        }

        af_obj_update obj_update{};
        obj_update.obj_handle = entity->handle;

        uint8_t current_primary_weapon = static_cast<uint8_t>(entity->ai.current_primary_weapon);
        if (current_primary_weapon > 63) {
            xlog::error("obj_update packet tried to process an invalid weapon type: {}", current_primary_weapon);
            continue; // reported weapon type is out of valid range
        }
        obj_update.current_primary_weapon = current_primary_weapon;

        uint8_t ammo_type = static_cast<uint8_t>(rf::weapon_types[current_primary_weapon].ammo_type);
        if (ammo_type > 31) {
            xlog::error("obj_update packet tried to process an invalid ammo type: {}", ammo_type);
            continue; // reported ammo type is out of valid range
        }
        obj_update.ammo_type = ammo_type;

        obj_update.clip_ammo = static_cast<uint16_t>(entity->ai.clip_ammo[current_primary_weapon]);
        obj_update.reserve_ammo = static_cast<uint16_t>(entity->ai.ammo[ammo_type]);

        /*xlog::warn("Adding player {}, weap {}, ammo {}, clip {}, reserve {}", 
                   entity->name, obj_update.current_primary_weapon, obj_update.ammo_type, 
                   obj_update.clip_ammo, obj_update.reserve_ammo);*/
        obj_updates.push_back(obj_update);
    }

    if (obj_updates.empty()) {
        return;  // No updates to send
    }

    // Calculate packet size
    size_t object_data_size = obj_updates.size() * sizeof(af_obj_update);
    size_t total_packet_size = sizeof(RF_GamePacketHeader) + object_data_size;

    if (total_packet_size > rf::max_packet_size) {
        xlog::error("af_send_obj_update_packet: Packet too large! Size: {}", total_packet_size);
        return;
    }

    // Allocate memory dynamically for the packet
    auto packet_buf = std::make_unique<std::byte[]>(total_packet_size);
    if (!packet_buf) {
        return; // could not allocate memory
    }

    // Fill packet header
    RF_GamePacketHeader header{};
    header.type = static_cast<uint8_t>(af_packet_type::af_obj_update);
    header.size = static_cast<uint16_t>(object_data_size);

    // Copy data to packet buffer
    std::memcpy(packet_buf.get(), &header, sizeof(header));
    if (!obj_updates.empty()) {
        std::memcpy(packet_buf.get() + sizeof(header), obj_updates.data(), object_data_size);
    }

    if (!player) {
        xlog::error("af_obj_update: Attempted to send to an invalid player");
        return;
    }
    af_send_packet(player, packet_buf.get(), total_packet_size, false);
}

static void af_process_obj_update_packet(const void* data, size_t len, const rf::NetAddr& addr)
{
    // Receive: client <- server
    if (!rf::is_multi || rf::is_server || rf::is_dedicated_server) {
        return;
    }

    if (len < sizeof(RF_GamePacketHeader)) {
        xlog::warn("Received malformed object update packet (too small)");
        return;
    }

    // Read the packet header
    RF_GamePacketHeader header;
    std::memcpy(&header, data, sizeof(header));

    // Calculate the expected object data size
    size_t object_data_size = header.size;
    if (len != sizeof(RF_GamePacketHeader) + object_data_size) {
        xlog::warn("Object update packet has unexpected size! Expected {}, got {}", 
                   sizeof(RF_GamePacketHeader) + object_data_size, len);
        return;
    }

    // Calculate number of objects being updated
    size_t num_objects = object_data_size / sizeof(af_obj_update);
    if (num_objects == 0) {
        return; // Nothing to process
    }

    // Copy object updates to a vector to avoid misaligned access
    std::vector<af_obj_update> obj_updates(num_objects);
    std::memcpy(obj_updates.data(), 
                static_cast<const std::byte*>(data) + sizeof(RF_GamePacketHeader), 
                object_data_size);

    // Process each object update safely
    for (const auto& obj_update : obj_updates) {
        // Validate the remote object
        rf::Object* remote_object = rf::obj_from_remote_handle(obj_update.obj_handle);
        if (!remote_object) {
            //xlog::warn("Received obj update for invalid remote handle: {:x}", obj_update.obj_handle);
            continue;
        }

        rf::Entity* entity = rf::entity_from_handle(remote_object->handle);
        if (!entity) {
            //xlog::warn("Received obj update for invalid entity handle: {:x}", obj_update.obj_handle);
            continue;
        }

        // Do not update the local player (shouldn't even be in the packet, but just to make sure
        if (entity == rf::local_player_entity) {
            continue;
        }

        // Only update ammo if the player's weapon matches the packet
        if (entity->ai.current_primary_weapon == obj_update.current_primary_weapon) {
            entity->ai.clip_ammo[obj_update.current_primary_weapon] = obj_update.clip_ammo;
            entity->ai.ammo[obj_update.ammo_type] = obj_update.reserve_ammo;

            /* xlog::warn("Updated player {}, weapon {}, ammo type {}, clip {}, reserve {}", 
                        entity->name, obj_update.current_primary_weapon, obj_update.ammo_type, 
                        obj_update.clip_ammo, obj_update.reserve_ammo);*/
        } else {
            //xlog::warn("Did not update player {} because packet weapon {}, their weapon {}", entity->name, obj_update.current_primary_weapon, entity->ai.current_primary_weapon);
        }
    }
}
