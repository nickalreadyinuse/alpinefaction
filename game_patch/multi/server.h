#pragma once

// Forward declarations
namespace rf
{
    struct Player;
    struct Entity;
}

void server_init();
void server_do_frame();
bool check_server_chat_command(const char* msg, rf::Player* sender);
bool server_is_saving_enabled();
bool server_allow_fullbright_meshes();
bool server_allow_lightmaps_only();
bool server_allow_disable_screenshake();
bool server_no_player_collide();
void server_reliable_socket_ready(rf::Player* player);
bool server_weapon_items_give_full_ammo();
bool server_weapon_infinite_magazines();
void server_add_player_weapon(rf::Player* player, int weapon_type, bool full_ammo);
void multi_reload_weapon_server_side(rf::Player* pp, int weapon_type);
void multi_update_gungame_weapon(rf::Player* player, bool force_notification);
void gungame_on_player_spawn(rf::Player* player);
void send_sound_packet_throwaway(rf::Player* target, int sound_id);
const char* get_rand_level_filename();
void shuffle_level_array();void process_queued_spawn_points_from_items();
