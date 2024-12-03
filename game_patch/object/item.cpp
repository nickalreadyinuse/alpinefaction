#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include <xlog/xlog.h>
#include "../rf/event.h"
#include "../rf/item.h"
#include "../rf/misc.h"
#include "../rf/entity.h"
#include "../rf/multi.h"
#include "../rf/weapon.h"
#include "../rf/player/player.h"
#include "../multi/server.h"

FunHook<int(int, int, int, int)> item_touch_weapon_hook{
    0x0045A6D0,
    [](int entity_handle, int item_handle, int weapon_type, int count) {
        if (server_weapon_items_give_full_ammo() && weapon_type != rf::shoulder_cannon_weapon_type) {
            rf::WeaponInfo& winfo = rf::weapon_types[weapon_type];
            count = winfo.max_ammo + winfo.clip_size;
        }
        return item_touch_weapon_hook.call_target(entity_handle, item_handle, weapon_type, count);
    },
};

CodeInjection item_create_sort_injection{
    0x004593AC,
    [](auto& regs) {
        rf::Item* item = regs.esi;
        rf::VMesh* vmesh = item->vmesh;
        const char* mesh_name = vmesh ? rf::vmesh_get_name(vmesh) : nullptr;
        if (!mesh_name) {
            // Sometimes on level change some objects can stay and have only vmesh destroyed
            return;
        }
        std::string_view mesh_name_sv = mesh_name;

        // HACKFIX: enable alpha sorting for Invulnerability Powerup and Riot Shield
        // Note: material used for alpha-blending is flare_blue1.tga - it uses non-alpha texture
        // so information about alpha-blending cannot be taken from material alone - it must be read from VFX
        static const char* force_alpha_mesh_names[] = {
            "powerup_invuln.vfx",
            "Weapon_RiotShield.V3D",
        };
        for (const char* alpha_mesh_name : force_alpha_mesh_names) {
            if (mesh_name_sv == alpha_mesh_name) {
                item->obj_flags |= rf::OF_HAS_ALPHA;
                break;
            }
        }

        rf::Item* current = rf::item_list.next;
        while (current != &rf::item_list) {
            rf::VMesh* current_anim_mesh = current->vmesh;
            const char* current_mesh_name = current_anim_mesh ? rf::vmesh_get_name(current_anim_mesh) : nullptr;
            if (current_mesh_name && mesh_name_sv == current_mesh_name) {
                break;
            }
            current = current->next;
        }
        item->next = current;
        item->prev = current->prev;
        item->next->prev = item;
        item->prev->next = item;
        // Set up needed registers
        regs.ecx = regs.esp + 0xC0 - 0xB0; // create_info
        regs.eip = 0x004593D1;
    },
};

CodeInjection game_level_init_pre_patch{
    0x00435BDC, [](auto& regs) {

        if (!rf::is_multi) {
            rf::multi_powerup_destroy_all();
        }
    }
};

FunHook<void(rf::Item*, rf::Entity*, bool, int)> item_pickup_hook{
    0x00459560,
    [](rf::Item* item, rf::Entity* ep, bool do_los_check, int allow_multi) {
        // can't reliably work in multi
        if (!rf::is_multi && ep == rf::local_player_entity) {
            xlog::warn("player tried to pick up item {}, uid {}, handle {}", item->name, item->uid, item->handle);
            rf::activate_all_events_of_type(rf::EventType::When_Picked_Up, item->handle, -1, true);
        } // todo: verify this doesnt trigger every frame if player can't pick up item

        item_pickup_hook.call_target(item, ep, do_los_check, allow_multi);
    },
};

void item_do_patch()
{
    item_pickup_hook.install();

    // allow picking up powerups in SP // todo: restrict to Alpine levels
    AsmWriter(0x0045AAFD).jmp(0x0045AB11); // allow item_touch_multi_amp in SP
    AsmWriter(0x0048012B).jmp(0x00480135); // allow multi_powerup_add in SP
    game_level_init_pre_patch.install();   // initialize powerup vars in SP

    // Allow overriding weapon items count value
    item_touch_weapon_hook.install();

    // Sort objects by mesh name to improve rendering performance
    item_create_sort_injection.install();
}
