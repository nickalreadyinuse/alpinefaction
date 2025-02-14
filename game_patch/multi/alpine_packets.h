#pragma once

#include <cstdint>
#include <common/rfproto.h>
#include "../rf/multi.h"

#pragma pack(push, 1)

// Forward declarations
namespace rf
{
struct Object;
struct Player;
struct Vector3;
struct Matrix3;
struct Entity;
}

enum class af_packet_type : uint8_t
{
    teleport_player = 0x50,
};

// turns out this isn't needed
struct af_teleport_player_packet
{
    RF_GamePacketHeader header;
    uint8_t player_id;
    struct RF_Vector pos;
    struct RF_Matrix orient;
};

bool af_process_packet(const void* data, int len, const rf::NetAddr& addr, rf::Player* player);
void af_send_packet(rf::Player* player, const void* data, int len, bool is_reliable);

void af_send_teleport_packet(rf::Entity* entity, rf::Vector3* pos, rf::Matrix3* orient, rf::Player* player);
void af_send_teleport_packet_to_all(rf::Entity* entity, rf::Vector3* pos, rf::Matrix3* orient);
static void af_process_teleport_packet(const void* data, size_t len, const rf::NetAddr& addr);

#pragma pack(pop)
