#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include <common/utils/list-utils.h>
#include <xlog/xlog.h>
#include <unordered_map>
#include <vector>
#include "projectile_lag_comp.h"
#include "server_internal.h"
#include "../rf/entity.h"
#include "../rf/weapon.h"
#include "../rf/object.h"
#include "../rf/physics.h"
#include "../rf/collide.h"
#include "../rf/multi.h"
#include "../rf/player/player.h"
#include "../rf/os/timer.h"

// --- Weapon classification ---

static bool is_projectile_weapon(const rf::Weapon* wp)
{
    if (!wp || !wp->info)
        return false;
    const auto* info = wp->info;
    // Melee and detonators are not projectiles
    if (info->flags & (rf::WTF_MELEE | rf::WTF_DETONATOR))
        return false;
    // Projectile weapons have meaningful speed and lifetime
    if (info->max_speed > 0.0f && info->lifetime_seconds > 0.0f)
        return true;
    // Continuous fire weapons (flamethrower)
    if (info->flags & rf::WTF_CONTINUOUS_FIRE)
        return true;
    return false;
}

// --- Per-weapon half-ping storage ---

static std::unordered_map<int, float> g_weapon_fire_half_ping; // weapon handle -> half_ping_sec

// --- Entity position history ---

struct PosRecord
{
    rf::Vector3 pos;
    int timestamp_ms;
};

constexpr int HISTORY_SIZE = 32;

struct PosHistory
{
    PosRecord records[HISTORY_SIZE];
    int write_idx = 0;
    int count = 0;

    void push(const rf::Vector3& p, int time_ms)
    {
        records[write_idx] = {p, time_ms};
        write_idx = (write_idx + 1) % HISTORY_SIZE;
        if (count < HISTORY_SIZE)
            ++count;
    }

    // Get record at logical index (0 = oldest)
    const PosRecord& at(int i) const
    {
        int start = (write_idx - count + HISTORY_SIZE) % HISTORY_SIZE;
        return records[(start + i) % HISTORY_SIZE];
    }
};

static std::unordered_map<int, PosHistory> g_entity_pos_history; // entity handle -> history

static rf::Vector3 interpolate_position(const PosHistory& history, int target_time_ms)
{
    if (history.count == 0)
        return {};

    // If target time is before all records, use oldest
    if (target_time_ms <= history.at(0).timestamp_ms)
        return history.at(0).pos;

    // If target time is after all records, use newest
    if (target_time_ms >= history.at(history.count - 1).timestamp_ms)
        return history.at(history.count - 1).pos;

    // Find the two records bracketing target_time_ms
    for (int i = 0; i < history.count - 1; ++i) {
        const auto& a = history.at(i);
        const auto& b = history.at(i + 1);
        if (target_time_ms >= a.timestamp_ms && target_time_ms <= b.timestamp_ms) {
            int dt = b.timestamp_ms - a.timestamp_ms;
            if (dt <= 0)
                return a.pos;
            float t = static_cast<float>(target_time_ms - a.timestamp_ms) / static_cast<float>(dt);
            rf::Vector3 result;
            result.x = a.pos.x + (b.pos.x - a.pos.x) * t;
            result.y = a.pos.y + (b.pos.y - a.pos.y) * t;
            result.z = a.pos.z + (b.pos.z - a.pos.z) * t;
            return result;
        }
    }

    // Fallback: return newest
    return history.at(history.count - 1).pos;
}

// --- Entity rewind/restore ---

struct SavedEntityPos
{
    int handle;
    rf::Vector3 original_pos;
    rf::Vector3 original_p_data_pos;
};

static std::vector<SavedEntityPos> g_rewound_entities;

static float get_half_ping_for_entity_handle(int entity_handle)
{
    rf::Player* pp = rf::player_from_entity_handle(entity_handle);
    if (!pp || !pp->net_data)
        return 0.0f;

    float half_ping_ms = static_cast<float>(pp->net_data->ping) / 2.0f;
    float max_ms = static_cast<float>(g_alpine_server_config.projectile_lag_comp_max_ms);
    if (half_ping_ms > max_ms)
        half_ping_ms = max_ms;

    return half_ping_ms;
}

// --- Public API ---

bool projectile_lag_comp_enabled()
{
    return g_alpine_server_config.projectile_lag_comp;
}

void projectile_lag_comp_record_positions()
{
    if (!rf::is_server || !projectile_lag_comp_enabled())
        return;

    int now = rf::timer_get_milliseconds();

    for (auto& entity : DoublyLinkedList{rf::entity_list}) {
        g_entity_pos_history[entity.handle].push(entity.pos, now);
    }
}

void projectile_lag_comp_advance_weapon(rf::Entity* shooter, rf::Weapon* wp)
{
    if (!rf::is_server || !projectile_lag_comp_enabled())
        return;

    if (!shooter || !wp)
        return;

    if (!is_projectile_weapon(wp))
        return;

    rf::Player* pp = rf::player_from_entity_handle(shooter->handle);
    if (!pp || !pp->net_data)
        return;

    float half_ping_sec = static_cast<float>(pp->net_data->ping) / 2000.0f;
    float max_sec = static_cast<float>(g_alpine_server_config.projectile_lag_comp_max_ms) / 1000.0f;
    if (half_ping_sec > max_sec)
        half_ping_sec = max_sec;

    if (half_ping_sec <= 0.0f)
        return;

    // Copy velocity and apply gravity if needed
    rf::Vector3 vel = wp->p_data.vel;
    if (wp->p_data.flags & rf::PF_GRAVITY) {
        vel.y -= 9.8f * half_ping_sec;
    }

    // Compute new position
    rf::Vector3 new_pos;
    new_pos.x = wp->pos.x + vel.x * half_ping_sec;
    new_pos.y = wp->pos.y + vel.y * half_ping_sec;
    new_pos.z = wp->pos.z + vel.z * half_ping_sec;

    // Collision check along the path
    rf::LevelCollisionOut col_out{};
    bool hit = rf::collide_linesegment_level_for_multi(
        wp->pos, new_pos, wp, nullptr, &col_out,
        wp->info->collision_radius, false, 1.0f);

    if (hit) {
        // Place slightly before the hit point (back off along velocity direction)
        float speed = std::sqrt(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z);
        if (speed > 0.001f) {
            float backoff = 0.01f; // small backoff distance
            new_pos.x = col_out.hit_point.x - (vel.x / speed) * backoff;
            new_pos.y = col_out.hit_point.y - (vel.y / speed) * backoff;
            new_pos.z = col_out.hit_point.z - (vel.z / speed) * backoff;
        }
        else {
            new_pos = col_out.hit_point;
        }
    }

    // Update weapon position
    wp->pos = new_pos;
    wp->p_data.pos = new_pos;
    wp->last_pos = new_pos;

    // Update velocity if gravity was applied
    if (wp->p_data.flags & rf::PF_GRAVITY) {
        wp->p_data.vel = vel;
    }

    // Reduce remaining lifetime
    wp->lifeleft_seconds -= half_ping_sec;
    if (wp->lifeleft_seconds < 0.0f)
        wp->lifeleft_seconds = 0.0f;

    // Store half-ping for this weapon for later use during explosion
    g_weapon_fire_half_ping[wp->handle] = half_ping_sec;

    xlog::trace("Projectile lag comp: advanced weapon {} by {:.1f}ms", wp->handle, half_ping_sec * 1000.0f);
}

void rewind_entities_for_projectile(int killer_handle)
{
    if (!rf::is_server || !projectile_lag_comp_enabled())
        return;

    float half_ping_ms = get_half_ping_for_entity_handle(killer_handle);
    if (half_ping_ms <= 0.0f)
        return;

    int now = rf::timer_get_milliseconds();
    int target_time = now - static_cast<int>(half_ping_ms);

    for (auto& entity : DoublyLinkedList{rf::entity_list}) {
        // Don't rewind the shooter
        if (entity.handle == killer_handle)
            continue;

        auto it = g_entity_pos_history.find(entity.handle);
        if (it == g_entity_pos_history.end())
            continue;

        rf::Vector3 rewound_pos = interpolate_position(it->second, target_time);

        // Save original position
        g_rewound_entities.push_back({entity.handle, entity.pos, entity.p_data.pos});

        // Apply rewound position
        entity.pos = rewound_pos;
        entity.p_data.pos = rewound_pos;
    }

    xlog::trace("Projectile lag comp: rewound {} entities by {:.1f}ms for killer {}",
        g_rewound_entities.size(), half_ping_ms, killer_handle);
}

void restore_entities_after_projectile()
{
    for (auto& saved : g_rewound_entities) {
        rf::Object* obj = rf::obj_from_handle(saved.handle);
        if (obj) {
            obj->pos = saved.original_pos;
            obj->p_data.pos = saved.original_p_data_pos;
        }
    }
    g_rewound_entities.clear();
}

// --- Hooks: apply_radius_damage call sites ---
// Cannot use FunHook on apply_radius_damage (0x00488DC0) because its prologue starts with
// FLD [ESP+0x8] — a stack-relative FPU load that breaks when relocated to a trampoline.
// Instead, hook each call site individually with CallHooks.

// Shared rewind/restore wrapper for all apply_radius_damage call sites
static void radius_damage_with_rewind(
    CallHook<void(rf::Vector3&, float, float, int, int)>& hook,
    rf::Vector3& epicenter, float damage, float radius, int killer_handle, int damage_type)
{
    bool rewound = false;
    if (rf::is_server && projectile_lag_comp_enabled() && killer_handle > 0) {
        rf::Object* killer_obj = rf::obj_from_handle(killer_handle);
        if (killer_obj && killer_obj->type == rf::OT_ENTITY) {
            rf::Player* pp = rf::player_from_entity_handle(killer_handle);
            if (pp) {
                rewind_entities_for_projectile(killer_handle);
                rewound = true;
            }
        }
    }
    hook.call_target(epicenter, damage, radius, killer_handle, damage_type);
    if (rewound) {
        restore_entities_after_projectile();
    }
}

// Call site in weapon_hit_entity (0x004C62F5)
CallHook<void(rf::Vector3&, float, float, int, int)> weapon_hit_entity_radius_damage_hook{
    0x004C62F5,
    [](rf::Vector3& epicenter, float damage, float radius, int killer_handle, int damage_type) {
        radius_damage_with_rewind(weapon_hit_entity_radius_damage_hook,
            epicenter, damage, radius, killer_handle, damage_type);
    },
};

// Call site in weapon_move_one / grenade+RC explosion (0x004C6C94)
CallHook<void(rf::Vector3&, float, float, int, int)> weapon_explode_radius_damage_hook{
    0x004C6C94,
    [](rf::Vector3& epicenter, float damage, float radius, int killer_handle, int damage_type) {
        radius_damage_with_rewind(weapon_explode_radius_damage_hook,
            epicenter, damage, radius, killer_handle, damage_type);
    },
};

// Call site in misc explosion function (0x0043660D)
CallHook<void(rf::Vector3&, float, float, int, int)> misc_explosion_radius_damage_hook{
    0x0043660D,
    [](rf::Vector3& epicenter, float damage, float radius, int killer_handle, int damage_type) {
        radius_damage_with_rewind(misc_explosion_radius_damage_hook,
            epicenter, damage, radius, killer_handle, damage_type);
    },
};

// --- Init / cleanup ---

void projectile_lag_comp_init()
{
    weapon_hit_entity_radius_damage_hook.install();
    weapon_explode_radius_damage_hook.install();
    misc_explosion_radius_damage_hook.install();
}

void projectile_lag_comp_on_level_init()
{
    g_weapon_fire_half_ping.clear();
    g_entity_pos_history.clear();
    g_rewound_entities.clear();
}
