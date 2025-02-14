#include <cstddef>
#include <cstring>
#include <cassert>
#include <array>
//#include <common/config/BuildConfig.h>
#include <common/utils/list-utils.h>
#include <common/rfproto.h>
#include <xlog/xlog.h>
#include "../rf/multi.h"
#include "multi.h"
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
        rf::net_send(player->net_data->addr, data, len);
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
        case af_packet_type::teleport_player:
            af_process_teleport_packet(data, len, addr);
            break;
        default:
            return false; // ignore if unrecognized
    }
    return true;
}

void af_send_teleport_packet(rf::Entity* entity, rf::Vector3* pos, rf::Matrix3* orient, rf::Player* player)
{
    xlog::warn("sending tp packet {}, {}", entity->name,
               rf::player_from_entity_handle(entity->handle)->net_data->player_id);
    // to be sent from server to client
    assert(rf::is_server);

    std::byte packet_buf[rf::max_packet_size];
    af_teleport_player_packet teleport_packet{};
    teleport_packet.header.type = static_cast<uint8_t>(af_packet_type::teleport_player);
    teleport_packet.header.size = sizeof(teleport_packet) - sizeof(teleport_packet.header);
    //teleport_packet.entity_handle = entity->handle;
    teleport_packet.player_id = rf::player_from_entity_handle(entity->handle)->net_data->player_id;
    teleport_packet.pos.x = pos->x;
    teleport_packet.pos.y = pos->y;
    teleport_packet.pos.z = pos->z;
    teleport_packet.orient.data[0][0] = orient->rvec.x;
    teleport_packet.orient.data[0][1] = orient->rvec.y;
    teleport_packet.orient.data[0][2] = orient->rvec.z;
    teleport_packet.orient.data[1][0] = orient->uvec.x;
    teleport_packet.orient.data[1][1] = orient->uvec.y;
    teleport_packet.orient.data[1][2] = orient->uvec.z;
    teleport_packet.orient.data[2][0] = orient->fvec.x;
    teleport_packet.orient.data[2][1] = orient->fvec.y;
    teleport_packet.orient.data[2][2] = orient->fvec.z;
    
    std::memcpy(packet_buf, &teleport_packet, sizeof(teleport_packet));
    af_send_packet(player, packet_buf, sizeof(teleport_packet), true);
}

void af_send_teleport_packet_to_all(rf::Entity* entity, rf::Vector3* pos, rf::Matrix3* orient) {
    std::vector<rf::Player*> player_list;

    SinglyLinkedList<rf::Player> linked_player_list{rf::player_list};

    for (auto& player : linked_player_list) {
        if (&player != rf::local_player) {
            af_send_teleport_packet(entity, pos, orient, &player);
        }
    }
}

static void af_process_teleport_packet(const void* data, size_t len, const rf::NetAddr& addr)
{
    xlog::warn("receiving tp packet with length {}", len);
    // to be received from server as client
    if (!rf::is_multi || rf::is_server || rf::is_dedicated_server) {
        return;
    }

    af_teleport_player_packet teleport_packet{};
    /* if (len < sizeof(teleport_packet)) {
        xlog::warn("Invalid length in received teleport packet");
        return;
    }*/

    std::memcpy(&teleport_packet, data, sizeof(teleport_packet));

    xlog::warn("receiving tp packet raw entity handle {}", teleport_packet.player_id);

    rf::Player* player = rf::multi_find_player_by_id(teleport_packet.player_id);
    if (!player) {
        xlog::warn("Invalid player in received teleport packet");
        return;
    }


    rf::Entity* entity = rf::entity_from_handle(player->entity_handle);
    if (!entity) {
        xlog::warn("Invalid entity in received teleport packet");
        return;
    }

    rf::Vector3 pos = {
        pos.x = teleport_packet.pos.x,
        pos.y = teleport_packet.pos.y,
        pos.z = teleport_packet.pos.z
    };

    if (entity->obj_interp) {
        entity->obj_interp->Clear();
    }
    entity->move(&pos);
    
}
