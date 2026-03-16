#pragma once

namespace rf { struct Player; }

void vehicle_apply_patches();
void vehicle_send_state_to_player(rf::Player* player);
void vehicle_do_frame();
