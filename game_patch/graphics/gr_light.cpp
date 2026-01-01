#include <xlog/xlog.h>
#include "../rf/gr/gr.h"
#include "../rf/os/frametime.h"
#include "../rf/vmesh.h"
#include "gr_internal.h"

bool is_find_static_lights = false;
std::vector<ExplosionFlashLight> g_active_explosion_flash_lights;
std::vector<ExplosionFlashVclipEntry> g_explosion_flash_vclip_entries_weapon;
std::vector<ExplosionFlashVclipEntry> g_explosion_flash_vclip_entries_env;

void explosion_flash_lights_init()
{
    g_explosion_flash_vclip_entries_weapon.clear();
    for (const auto& vclip_spec : k_explosion_flash_vclip_specs_weapon) {
        const int vclip_id = rf::vclip_lookup(vclip_spec.name);
        if (vclip_id >= 0) {
            g_explosion_flash_vclip_entries_weapon.push_back({
                vclip_id,
                vclip_spec.radius_scale,
                vclip_spec.intensity,
                vclip_spec.red,
                vclip_spec.green,
                vclip_spec.blue,
                vclip_spec.duration_ms
            });
        }
    }

    g_explosion_flash_vclip_entries_env.clear();
    for (const auto& vclip_spec : k_explosion_flash_vclip_specs_env) {
        const int vclip_id = rf::vclip_lookup(vclip_spec.name);
        if (vclip_id >= 0) {
            g_explosion_flash_vclip_entries_env.push_back({
                vclip_id,
                vclip_spec.radius_scale,
                vclip_spec.intensity,
                vclip_spec.red,
                vclip_spec.green,
                vclip_spec.blue,
                vclip_spec.duration_ms
            });
        }
    }
}

void explosion_flash_lights_level_init()
{
    g_active_explosion_flash_lights.clear();
}

bool explosion_flash_vclip_lookup(bool from_weapon, int vclip_id, float* radius_scale, float* intensity, float* r, float* g, float* b, int* duration_ms)
{
    const auto& entries = from_weapon
        ? g_explosion_flash_vclip_entries_weapon
        : g_explosion_flash_vclip_entries_env;

    const auto it = std::find_if(entries.begin(), entries.end(),
        [vclip_id](const ExplosionFlashVclipEntry& entry) {
            return entry.vclip_id == vclip_id;
        });

    if (it == entries.end()) {
        return false;
    }

    const ExplosionFlashVclipEntry& e = *it;
    if (radius_scale)
        *radius_scale = e.radius_scale;
    if (intensity)
        *intensity = e.intensity;
    if (r)
        *r = e.red;
    if (g)
        *g = e.green;
    if (b)
        *b = e.blue;
    if (duration_ms)
        *duration_ms = e.duration_ms;

    return true;
}

bool vclip_should_do_explosion_flash(bool from_weapon, int vclip_id, float* radius_scale, float* intensity, float* r, float* g, float* b, int* duration_ms)
{
    return explosion_flash_vclip_lookup(from_weapon, vclip_id, radius_scale, intensity, r, g, b, duration_ms);
}

float explosion_flash_light_intensity(const ExplosionFlashLight& light)
{
    if (light.duration_ms <= 0) {
        return 0.0f;
    }

    const float progress = std::clamp(light.elapsed_ms / static_cast<float>(light.duration_ms), 0.0f, 1.0f);
    constexpr float ramp_up_portion = 0.2f;
    if (progress < ramp_up_portion) {
        const float ramp = progress / ramp_up_portion;
        return light.max_intensity * (0.3f + 0.7f * ramp);
    }

    const float fade = (progress - ramp_up_portion) / (1.0f - ramp_up_portion);
    return light.max_intensity * (1.0f - fade);
}

void explosion_flash_light_create(const rf::Vector3& pos, float radius, float intensity, float r, float g, float b, int duration_ms)
{
    g_active_explosion_flash_lights.emplace_back(ExplosionFlashLight{
        .pos = pos,
        .radius = radius,
        .max_intensity = intensity,
        .red = r,
        .green = g,
        .blue = b,
        .duration_ms = duration_ms,
    });
}

void explosion_flash_light_create_offset(const rf::Vector3& pos, const rf::Vector3& dir, float offset_meters, float radius,
    float intensity, float r, float g, float b, int duration_ms)
{
    rf::Vector3 offset_pos = pos;
    if (offset_meters != 0.0f) {
        rf::Vector3 normalized_dir = dir;
        const float len = normalized_dir.len();
        if (len > 0.0001f) {
            normalized_dir /= len;
            offset_pos += normalized_dir * offset_meters;
        }
    }

    explosion_flash_light_create(offset_pos, radius, intensity, r, g, b, duration_ms);
}

void explosion_flash_lights_do_frame()
{
    if (g_active_explosion_flash_lights.empty()) {
        return;
    }

    const float delta_ms = rf::frametime * 1000.0f;
    for (auto& light : g_active_explosion_flash_lights) {
        if (light.light_handle > -1) {
            rf::gr::light_delete(light.light_handle, 0);
            light.light_handle = -1;
        }

        const float intensity = explosion_flash_light_intensity(light);
        if (intensity > 0.0f) {
            light.light_handle = rf::gr::light_create_point(
                &light.pos,
                light.radius,
                intensity,
                light.red,
                light.green,
                light.blue,
                true,
                light.shadow_condition,
                light.attenuation_algorithm
            );
        }

        light.elapsed_ms += delta_ms;
    }

    g_active_explosion_flash_lights.erase(
        std::remove_if(
            g_active_explosion_flash_lights.begin(),
            g_active_explosion_flash_lights.end(),
            [](ExplosionFlashLight& light) {
                if (light.elapsed_ms < light.duration_ms) {
                    return false;
                }
                if (light.light_handle > -1) {
                    rf::gr::light_delete(light.light_handle, 0);
                    light.light_handle = -1;
                }
                return true;
            }),
        g_active_explosion_flash_lights.end());
}

void gr_light_use_static(bool use_static)
{
    // Enable some experimental flag that causes static lights to be included in computations
    //auto& experimental_alloc_and_lighting = addr_as_ref<bool>(0x00879AF8);
    auto& gr_light_state = addr_as_ref<int>(0x00C96874);
    //experimental_alloc_and_lighting = use_static;
    is_find_static_lights = use_static;
    // Increment light cache key to trigger cache invalidation
    gr_light_state++;
}

void gr_light_apply_patch()
{
    // Change the variable that is used to determine what light list is searched (by default is_editor
    // variable determines that). It is needed for static mesh lighting.
    write_mem_ptr(0x004D96CD + 1, &is_find_static_lights); // gr_light_find_all_in_room
    write_mem_ptr(0x004D9761 + 1, &is_find_static_lights); // gr_light_find_all_in_room
    write_mem_ptr(0x004D98B3 + 1, &is_find_static_lights); // gr_light_find_all_by_gsolid
}
