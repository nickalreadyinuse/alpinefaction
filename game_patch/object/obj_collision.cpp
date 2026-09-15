#include <cstdint>
#include <new>
#include <algorithm>
#include <unordered_map>
#include <vector>
#include <xlog/xlog.h>
#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include "../rf/object.h"
#include "../rf/physics.h"
#include "../rf/geometry.h"
#include "../rf/vmesh.h"
#include "../os/console.h"
#include "../os/os.h"
#include "obj_collision.h"

constexpr int initial_chunk_nodes = 8192;
constexpr int growth_chunk_nodes = 4096;
constexpr int max_total_nodes = 1 << 20;

static int g_total_nodes = 0;

static bool collision_pairs_grow(int count)
{
    if (g_total_nodes + count > max_total_nodes) {
        static bool warned_cap = false;
        if (!warned_cap) {
            warned_cap = true;
            xlog::warn("Collision pair pool reached cap of {} nodes",
                max_total_nodes);
        }
        return false;
    }

    // never freed - the engine keeps pointers to pair nodes for the process lifetime
    auto* nodes = new (std::nothrow) rf::ObjCollisionPair[count]();
    if (!nodes) {
        static int64_t last_alloc_warn_ms = -5000;
        const int64_t now_ms = timer::get_i64(1000);
        if (now_ms - last_alloc_warn_ms >= 5000) {
            last_alloc_warn_ms = now_ms;
            xlog::warn("Collision pair pool allocation failed at {} nodes",
                g_total_nodes);
        }
        return false;
    }

    for (int i = 0; i < count; ++i) {
        rf::obj_collision_pair_free_list.push(&nodes[i]);
    }

    g_total_nodes += count;
    xlog::info("Collision pair pool grown to {} nodes", g_total_nodes);
    return true;
}

FunHook<void()> obj_collision_pairs_init_hook{
    0x0048C950,
    []() {
        collision_pairs_grow(initial_chunk_nodes);
    },
};

// Per-object index of active pair nodes. The stock engine keeps pairs in one global list and
// collide_stick2ground walks all of it for every trace (O(pairs) per entity per frame, ~17us on
// object-heavy maps). Insertions come from object_pairs_add (hooked below); every removal in the
// engine unlinks the node and pushes it onto the free list, so the free-list push is the single
// removal choke point.
// ponytail: linear erase per free; swap to intrusive per-object links if pair churn shows up in profiles
static std::unordered_map<rf::Object*, std::vector<rf::ObjCollisionPair*>> g_pairs_by_obj;

static void pair_index_remove(rf::Object* obj, rf::ObjCollisionPair* node)
{
    auto it = g_pairs_by_obj.find(obj);
    if (it == g_pairs_by_obj.end()) {
        return;
    }
    auto& nodes = it->second;
    auto pos = std::find(nodes.begin(), nodes.end(), node);
    if (pos != nodes.end()) {
        *pos = nodes.back();
        nodes.pop_back();
    }
    if (nodes.empty()) {
        g_pairs_by_obj.erase(it);
    }
}

// ObjCollisionPairList::push (thiscall). Free-list pushes are the only way an active pair dies.
FunHook<void __fastcall(rf::ObjCollisionPairList*, int, rf::ObjCollisionPair*)> obj_collision_pair_list_push_hook{
    0x0048CC70,
    [](rf::ObjCollisionPairList* list, int edx, rf::ObjCollisionPair* node) FASTCALL_LAMBDA {
        // fresh pool nodes arrive zeroed; only nodes that were active carry a/b
        if (list == &rf::obj_collision_pair_free_list && node->a) {
            pair_index_remove(node->a, node);
            if (node->b != node->a) {
                pair_index_remove(node->b, node);
            }
        }
        obj_collision_pair_list_push_hook.call_target(list, edx, node);
    },
};

FunHook<bool(rf::Object*, rf::Object*)> obj_collision_pair_create_hook{
    0x0048BD80,
    [](rf::Object* a, rf::Object* b) {
        // grown before the filter runs; at worst one chunk is allocated early
        if (!rf::obj_collision_pair_free_list.head) {
            collision_pairs_grow(growth_chunk_nodes);
        }
        bool created = obj_collision_pair_create_hook.call_target(a, b);
        if (created) {
            // object_pairs_add pushes the new node to the active list head, then fills a/b/flags
            rf::ObjCollisionPair* node = rf::obj_collision_pair_active_list.head;
            g_pairs_by_obj[a].push_back(node);
            if (b != a) {
                g_pairs_by_obj[b].push_back(node);
            }
        }
        return created;
    },
};

// One pair of collide_stick2ground: swept-sphere test of objp against the other half of the pair.
// Mirrors stock 0x0049B900 exactly, including its use of pair flags without regard to which side they describe.
static bool stick2ground_test_pair(rf::Object* objp, rf::Object* other, uint32_t pair_flags, rf::Vector3* p1,
    rf::Vector3* p2, rf::PCollisionOut* out)
{
    rf::PhysicsData& pd = objp->p_data;
    rf::PhysicsData& opd = other->p_data;
    bool hit = false;
    if ((pair_flags & 0x18) && other->type == rf::OT_DEBRIS) {
        hit = rf::bbox_intersect(pd.bbox_min, pd.bbox_max, opd.bbox_min, opd.bbox_max)
            && rf::collide_spheres_solid(p1, p2, &pd, &other->pos, &other->orient,
                static_cast<rf::GSolid*>(static_cast<rf::Debris*>(other)->solid), out);
    }
    else if ((pair_flags & 0x6) && other->vmesh && rf::vmesh_get_type(other->vmesh) == rf::MESH_TYPE_STATIC) {
        hit = rf::bbox_intersect(pd.bbox_min, pd.bbox_max, opd.bbox_min, opd.bbox_max)
            && rf::collide_spheres_mesh(p1, p2, &pd, &other->pos, &other->orient, other->vmesh, out);
    }
    else {
        if (other->type != rf::OT_CLUTTER && other->type != rf::OT_DEBRIS) {
            return false;
        }
        if (other->parent_handle == objp->handle || opd.radius <= rf::collide_stick2ground_min_radius) {
            return false;
        }
        hit = rf::bbox_intersect(pd.bbox_min, pd.bbox_max, opd.bbox_min, opd.bbox_max)
            && rf::collide_spheres_spheres(p1, p2, &pd, &opd, out);
    }
    if (hit) {
        out->material = other->material;
        out->obj_handle = other->handle;
        out->vel = opd.vel;
    }
    return hit;
}

// Stock walks the whole global pair list; this walks only objp's own pairs. Order does not matter:
// every sweep keeps the closest hit (writes only when below out->hit_time).
FunHook<bool(rf::Object*, rf::Vector3*, rf::Vector3*, rf::PCollisionOut*, rf::Object*)> collide_stick2ground_hook{
    0x0049B900,
    [](rf::Object* objp, rf::Vector3* p1, rf::Vector3* p2, rf::PCollisionOut* out, rf::Object* ignored) -> bool {
        auto it = g_pairs_by_obj.find(objp);
        if (it == g_pairs_by_obj.end()) {
            return false;
        }
        bool hit = false;
        for (rf::ObjCollisionPair* pair : it->second) {
            rf::Object* other = pair->a == objp ? pair->b : pair->a;
            if (other == ignored) {
                continue;
            }
            hit |= stick2ground_test_pair(objp, other, pair->flags, p1, p2, out);
        }
        return hit;
    },
};

// weapon_create hitscan sweep (0x004C7F65-0x004C7FF1): stock walks the whole global pair list looking
// for the new weapon's pairs, bbox-tests each and runs the swept pair test; the first pair test that
// hits ends the sweep. Same loop over the weapon's own pairs. Stock list is head-pushed (newest first),
// so iterate the index newest-first to keep the same first-hit preference.
CodeInjection weapon_create_hitscan_pairs_injection{
    0x004C7F65,
    [](auto& regs) {
        rf::Object* weapon = regs.esi;
        rf::PhysicsData& pd = weapon->p_data;
        auto it = g_pairs_by_obj.find(weapon);
        if (it != g_pairs_by_obj.end()) {
            auto& nodes = it->second;
            for (auto rit = nodes.rbegin(); rit != nodes.rend(); ++rit) {
                rf::ObjCollisionPair* pair = *rit;
                rf::Object* other = pair->a == weapon ? pair->b : pair->a;
                if (!rf::bbox_intersect(pd.bbox_min, pd.bbox_max, other->p_data.bbox_min, other->p_data.bbox_max)) {
                    continue;
                }
                pair->a->p_data.collide_out.hit_time = 1.0f;
                pair->b->p_data.collide_out.hit_time = 1.0f;
                bool hit = (pair->flags & 0x6) ? rf::collide_object_object_mesh(weapon, other)
                                               : rf::collide_object_object_spheres(weapon, other);
                if (hit) {
                    regs.eip = 0x004C7FFE; // weapon hit object
                    return;
                }
            }
        }
        regs.eip = 0x004C7FF3; // no pair hit: collide_object_world
    },
};

ConsoleCommand2 collision_pairs_cmd{
    "dbg_collision_pairs",
    []() {
        rf::console::print("Collision pairs: active {}, free {}, allocated {} (hard cap {})",
            rf::obj_collision_pair_active_list.count, rf::obj_collision_pair_free_list.count, g_total_nodes, max_total_nodes);
        // index self-check: every active node must be indexed under both of its objects and nothing else
        int active = 0;
        int missing = 0;
        for (auto* node = rf::obj_collision_pair_active_list.head; node; node = node->next) {
            ++active;
            for (rf::Object* obj : {node->a, node->b}) {
                auto it = g_pairs_by_obj.find(obj);
                if (it == g_pairs_by_obj.end() || std::find(it->second.begin(), it->second.end(), node) == it->second.end()) {
                    ++missing;
                }
            }
        }
        size_t indexed = 0;
        for (auto& [obj, nodes] : g_pairs_by_obj) {
            indexed += nodes.size();
        }
        rf::console::print("Pair index: {} objects, {} entries (expected {}), {} missing", g_pairs_by_obj.size(), indexed,
            2 * active, missing);
    },
    "Prints object collision pair pool statistics",
};

void obj_collision_apply_patch()
{
    // Replace the fixed 8192 node pair pool with a growable one
    obj_collision_pairs_init_hook.install();
    obj_collision_pair_create_hook.install();

    // Per-object pair index so collide_stick2ground stops walking the whole pair list per trace
    obj_collision_pair_list_push_hook.install();
    collide_stick2ground_hook.install();
    weapon_create_hitscan_pairs_injection.install();

    collision_pairs_cmd.register_cmd();
}
