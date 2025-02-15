#include <cstddef>
#include <cstring>
#include <cassert>
#include <array>
#include <common/utils/list-utils.h>
#include <common/rfproto.h>
#include <xlog/xlog.h>
#include "../rf/multi.h"
#include "multi.h"
#include "../hud/hud_world.h"
#include "alpine_packets.h"
#include "../misc/player.h"

void af_send_packet(rf::Player* player, const void* data, int len, bool is_reliable)
{
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
    if (!rf::is_server) {
        return;
    }

    // ignore ping_location_req packets if location pinging is configured off
    if (!get_df_server_info().has_value() || !get_df_server_info()->location_pinging) {
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

    add_location_ping_world_hud_sprite(pos, player->name);
}
