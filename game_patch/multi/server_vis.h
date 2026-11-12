#pragma once

// Server-side visibility culling. When enabled the server skips an entity's obj_update records
// for a recipient that has no plausible line of sight to it, saving bandwidth.

namespace rf
{
    struct Player;
    struct Entity;
}

// Whether `viewer` should receive `target`'s position this tick. Always true when the feature is
// off or on any failure path. Drives the hide/reveal packets on transitions, so it must be called
// from the per-recipient send path (it is idempotent within a frame).
bool server_vis_can_see(rf::Player* viewer, rf::Entity* target);
// pack_obj_update_data companions. When a record is withheld this may write a stub record that
// repeats the last position the recipient already saw, without OUF_FIRE, so a looping on/off
// weapon sound stops; returns the stub size or 0. After a full record is sent, record_sent keeps
// a copy of its position part and the fire state it carried.
int server_vis_withheld_record(rf::Player* viewer, rf::Entity* target, void* data);
void server_vis_record_sent(rf::Player* viewer, rf::Entity* target, const void* data, int size);
// Before obj_kill goes out: reveal the entity and silence its weapon on every recipient that had
// it hidden, then forget its pairs. Deaths are public.
void server_vis_on_entity_killed(rf::Entity* ep);

void server_vis_do_frame();
void server_vis_level_init();
void server_vis_on_player_destroy(rf::Player* player);
void server_vis_apply_patch();
// True when the server must run remote-entity footstep triggers so it can relay them (entity.cpp gate).
bool server_vis_wants_footsteps();

