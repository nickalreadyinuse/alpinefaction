#include <cassert>
#include <xlog/xlog.h>
#include <patch_common/FunHook.h>
#include <patch_common/CallHook.h>
#include <patch_common/AsmWriter.h>
#include <patch_common/CodeInjection.h>
#include <common/utils/list-utils.h>
#include "../misc/misc.h"
#include "../rf/gr/gr_light.h"
#include "../rf/object.h"
#include "../rf/event.h"
#include "../rf/mover.h"
#include "../rf/item.h"
#include "../rf/level.h"
#include "../rf/clutter.h"
#include "../rf/gr/gr.h"
#include "../rf/multi.h"
#include "../rf/crt.h"
#include "../os/console.h"
#include "../main/main.h"
#include "../multi/multi.h"

void gr_light_use_static(bool use_static);

bool server_side_restrict_disable_muzzle_flash = false;

// support fullbright character meshes
static float g_character_ambient_light_r = 1.0f;
static float g_character_ambient_light_g = 1.0f;
static float g_character_ambient_light_b = 1.0f;

void obj_mesh_lighting_alloc_one(rf::Object *objp)
{
    // Note: ObjDeleteMesh frees mesh_lighting_data
    assert(objp->mesh_lighting_data == nullptr);
    auto size = rf::vmesh_calc_lighting_data_size(objp->vmesh);
    objp->mesh_lighting_data = rf::rf_malloc(size);
}

void obj_mesh_lighting_free_one(rf::Object *objp)
{
    if (objp->mesh_lighting_data) {
        rf::rf_free(objp->mesh_lighting_data);
        objp->mesh_lighting_data = nullptr;
    }
}

void obj_mesh_lighting_update_one(rf::Object *objp)
{
    gr_light_use_static(true);
    rf::vmesh_update_lighting_data(objp->vmesh, objp->room, objp->pos, objp->orient, objp->mesh_lighting_data);
    gr_light_use_static(false);
}

static bool obj_should_be_lit(rf::Object *objp)
{
    if (!g_game_config.mesh_static_lighting) {
        return false;
    }
    if (!objp->vmesh || rf::vmesh_get_type(objp->vmesh) != rf::MESH_TYPE_STATIC) {
        return false;
    }
    // Clutter object always use static lighting
    return objp->type == rf::OT_CLUTTER || objp->type == rf::OT_ITEM;
}

void obj_mesh_lighting_maybe_update(rf::Object *objp)
{
    if (!objp->mesh_lighting_data && obj_should_be_lit(objp)) {
        obj_mesh_lighting_alloc_one(objp);
        obj_mesh_lighting_update_one(objp);
    }
}

void recalc_mesh_static_lighting()
{
    rf::obj_light_free();
    rf::obj_light_alloc();
    rf::obj_light_calculate();
}

void evaluate_fullbright_meshes()
{
    if (!rf::LEVEL_LOADED)
        return;

    bool should_fullbright = false;

    if (g_game_config.try_mesh_fullbright) {
        bool server_side_restrict_fb_mesh =
            rf::is_multi && !rf::is_server && get_df_server_info() && !get_df_server_info()->allow_fb_mesh;

        if (server_side_restrict_fb_mesh) {
            rf::console::print("This server does not allow you to force fullbright meshes!");
        }
        else {
            should_fullbright = true;
        }
    }

    // Use fullbright (1.0) for each channel if selected and allowed, otherwise use level ambient light
    if (should_fullbright) {
        g_character_ambient_light_r = 1.0f;
        g_character_ambient_light_g = 1.0f;
        g_character_ambient_light_b = 1.0f;
    }
    else {
        // sets all 3 g_character_ambient_light floats
        std::memcpy(&g_character_ambient_light_r, reinterpret_cast<const void*>(0x005A38D4), sizeof(float) * 3);
    }
}

FunHook<void()> obj_light_calculate_hook{
    0x0048B0E0,
    []() {
        xlog::trace("update_mesh_lighting_hook");
        // Init transform for lighting calculation
        rf::gr::view_matrix.make_identity();
        rf::gr::view_pos.zero();
        rf::gr::light_matrix.make_identity();
        rf::gr::light_base.zero();

        if (g_game_config.mesh_static_lighting) {
            // Enable static lights
            gr_light_use_static(true);
            // Calculate lighting for meshes now
            obj_light_calculate_hook.call_target();
            // Switch back to dynamic lights
            gr_light_use_static(false);
        }
        else {
            obj_light_calculate_hook.call_target();
        }
    },
};

FunHook<void()> obj_light_alloc_hook{
    0x0048B1D0,
    []() {
        for (auto& item: DoublyLinkedList{rf::item_list}) {
            if (item.vmesh && !(item.obj_flags & rf::OF_DELAYED_DELETE)
                && rf::vmesh_get_type(item.vmesh) == rf::MESH_TYPE_STATIC) {
                auto size = rf::vmesh_calc_lighting_data_size(item.vmesh);
                item.mesh_lighting_data = rf::rf_malloc(size);
            }
        }
        for (auto& clutter: DoublyLinkedList{rf::clutter_list}) {
            if (clutter.vmesh && !(clutter.obj_flags & rf::OF_DELAYED_DELETE)
                && rf::vmesh_get_type(clutter.vmesh) == rf::MESH_TYPE_STATIC) {
                auto size = rf::vmesh_calc_lighting_data_size(clutter.vmesh);
                clutter.mesh_lighting_data = rf::rf_malloc(size);
            }
        }
    },
};

FunHook<void()> obj_light_free_hook{
    0x0048B370,
    []() {
        for (auto& item: DoublyLinkedList{rf::item_list}) {
            rf::rf_free(item.mesh_lighting_data);
            item.mesh_lighting_data = nullptr;
        }
        for (auto& clutter: DoublyLinkedList{rf::clutter_list}) {
            rf::rf_free(clutter.mesh_lighting_data);
            clutter.mesh_lighting_data = nullptr;
        }
    },
};

ConsoleCommand2 mesh_static_lighting_cmd{
    "r_meshlighting",
    []() {
        g_game_config.mesh_static_lighting = !g_game_config.mesh_static_lighting;
        g_game_config.save();
        recalc_mesh_static_lighting();
        rf::console::print("Mesh static lighting is {}", g_game_config.mesh_static_lighting ? "enabled" : "disabled");
    },
    "Toggle mesh static lighting calculation",
};

CallHook<void(rf::Entity&)> entity_update_muzzle_flash_light_hook{
    0x0041E814,
    [](rf::Entity& ep) {
        if (g_game_config.try_disable_muzzle_flash && !server_side_restrict_disable_muzzle_flash) {
            return;
        }
        entity_update_muzzle_flash_light_hook.call_target(ep);
    },
};

void evaluate_restrict_disable_muzzle_flash()
{
    server_side_restrict_disable_muzzle_flash =
        rf::is_multi && !rf::is_server && get_df_server_info() && !get_df_server_info()->allow_no_mf;

    if (server_side_restrict_disable_muzzle_flash) {
        if (g_game_config.try_disable_muzzle_flash) {
            rf::console::print("This server does not allow you to disable muzzle flash lights!");
        }
    }
}

ConsoleCommand2 muzzle_flash_cmd{
    "r_muzzleflash",
    []() {
        g_game_config.try_disable_muzzle_flash = !g_game_config.try_disable_muzzle_flash;
        g_game_config.save();

        evaluate_restrict_disable_muzzle_flash();

        rf::console::print("Muzzle flash lights are {}",
                           g_game_config.try_disable_muzzle_flash
                               ? "disabled. In multiplayer, this will only apply if the server allows it."
                               : "enabled.");
    },
    "Toggle muzzle flash lights. In multiplayer, this is only applied if the server allows it.",
};

ConsoleCommand2 fullbright_models_cmd{
    "r_fullbright",
    []() {
        g_game_config.try_mesh_fullbright = !g_game_config.try_mesh_fullbright;
        g_game_config.save();

        evaluate_fullbright_meshes();    
                
        rf::console::print("Fullbright character meshes are {}", g_game_config.try_mesh_fullbright ?
			"enabled. In multiplayer, this will only apply if the server allows it." : "disabled.");
    },
    "Toggle fullbright character meshes. In multiplayer, this is only available if the server allows it.",
};

CodeInjection dynamic_light_load_patch{
    0x0045F500,
    [](auto& regs) {
        // Note will crash dedicated servers
        if (!rf::is_dedicated_server && af_rfl_version(rf::level.version)) {
            regs.eip = 0x0045F507;
        }
    }
};

void obj_light_apply_patch()
{
    // Support fullbright character meshes
    AsmWriter{0x0052DB3E}.fld<float>(AsmRegMem(&g_character_ambient_light_r));
    AsmWriter{0x0052DB50}.fld<float>(AsmRegMem(&g_character_ambient_light_g));
    AsmWriter{0x0052DB62}.fld<float>(AsmRegMem(&g_character_ambient_light_b));

    // Allow dynamic lights in levels
    dynamic_light_load_patch.install(); // in LevelLight__load

    // Fix/improve items and clutters static lighting calculation: fix matrices being zero and use static lights
    obj_light_calculate_hook.install();
    obj_light_alloc_hook.install();
    obj_light_free_hook.install();

    // Fix invalid vertex offset in mesh lighting calculation
    write_mem<int8_t>(0x005042F0 + 2, sizeof(rf::Vector3));

    // Don't create muzzle flash lights
    entity_update_muzzle_flash_light_hook.install();

    // Commands
    mesh_static_lighting_cmd.register_cmd();
    muzzle_flash_cmd.register_cmd();
    fullbright_models_cmd.register_cmd();
}
