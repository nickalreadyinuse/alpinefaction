#pragma once

void hud_apply_patches();
int hud_get_small_font();
int hud_get_default_font();
int hud_get_large_font();
bool hud_weapons_is_double_ammo();
void draw_hud_vote_notification(std::string vote_type);
void remove_hud_vote_notification();
void draw_hud_ready_notification(bool draw);
