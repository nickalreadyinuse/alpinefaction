#pragma once

#include <cstdint>
#include <variant>
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
    af_ping_location_req = 0x50,        // Alpine 1.1
    af_ping_location = 0x51,            // Alpine 1.1
    af_damage_notify = 0x52,            // Alpine 1.1
    af_obj_update = 0x53,               // Alpine 1.1
    af_client_req = 0x55,               // Alpine 1.2
    af_just_spawned_info = 0x56,        // Alpine 1.2
};

struct af_ping_location_req_packet
{
    RF_GamePacketHeader header;
    RF_Vector pos;
};

struct af_ping_location_packet
{
    RF_GamePacketHeader header;
    uint8_t player_id;
    RF_Vector pos;
};

struct af_damage_notify_packet
{
    RF_GamePacketHeader header;
    uint8_t player_id;
    uint16_t damage;
    uint8_t flags;
};

struct af_obj_update // members of af_obj_update_packet
{
    uint32_t obj_handle;
    uint8_t current_primary_weapon;
    uint8_t ammo_type;
    uint16_t clip_ammo;
    uint16_t reserve_ammo;
};

struct af_obj_update_packet
{
    RF_GamePacketHeader header;
    af_obj_update objects[];
};

enum class af_client_req_type : uint8_t
{
    af_req_handicap = 0x00,
};

struct HandicapPayload
{
    uint8_t amount = 0;
};

using af_client_payload = std::variant<HandicapPayload>;

struct af_client_req_packet
{
    RF_GamePacketHeader header;
    af_client_req_type req_type;
    af_client_payload payload;
};

enum class af_just_spawned_info_type : uint8_t
{
    af_loadout = 0x00,
};

struct LoadoutEntry
{
    uint8_t weapon_index;
    uint32_t ammo;
};

struct af_just_spawned_info_packet
{
    RF_GamePacketHeader header;
    uint8_t info_type;  // af_just_spawned_info_type
    uint8_t data[];     // type-specific payload
};

#pragma pack(pop)

bool af_process_packet(const void* data, int len, const rf::NetAddr& addr, rf::Player* player);
void af_send_packet(rf::Player* player, const void* data, int len, bool is_reliable);

void af_send_ping_location_req_packet(rf::Vector3* pos);
static void af_process_ping_location_req_packet(const void* data, size_t len, const rf::NetAddr& addr);
void af_send_ping_location_packet_to_team(rf::Vector3* pos, uint8_t player_id, rf::ubyte team);
static void af_process_ping_location_packet(const void* data, size_t len, const rf::NetAddr& addr);
void af_send_damage_notify_packet(uint8_t player_id, float damage, bool died, rf::Player* player);
static void af_process_damage_notify_packet(const void* data, size_t len, const rf::NetAddr& addr);
void af_send_obj_update_packet(rf::Player* player);
static void af_process_obj_update_packet(const void* data, size_t len, const rf::NetAddr& addr);
void af_send_client_req_packet(const af_client_req_packet& packet);
static void af_process_client_req_packet(const void* data, size_t len, const rf::NetAddr& addr);
void af_send_just_spawned_loadout(rf::Player* to_player, std::vector<WeaponLoadoutEntry> loadout);
static void af_process_just_spawned_info_packet(const void* data, size_t len, const rf::NetAddr& addr);

// client requests
void af_send_handicap_request(uint8_t amount);
