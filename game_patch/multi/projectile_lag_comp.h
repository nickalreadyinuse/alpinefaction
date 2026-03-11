#pragma once

namespace rf
{
    struct Entity;
    struct Weapon;
}

void projectile_lag_comp_init();
void projectile_lag_comp_on_level_init();
void projectile_lag_comp_record_positions();
void projectile_lag_comp_advance_weapon(rf::Entity* shooter, rf::Weapon* wp);
bool projectile_lag_comp_enabled();
void rewind_entities_for_projectile(int killer_handle);
void restore_entities_after_projectile();
