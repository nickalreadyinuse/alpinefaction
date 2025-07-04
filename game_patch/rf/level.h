#pragma once

#include "math/vector.h"
#include "math/matrix.h"
#include "os/string.h"
#include "os/array.h"
#include "gr/gr.h"

namespace rf
{
    struct GRoom;
    struct GSolid;
    struct ParticleEmitter;
    struct BoltEmitter;
    struct LevelLight;
    struct GeoRegion;

    struct ClimbRegion
    {
        int type;
        Vector3 pos;
        Matrix3 orient;
        Vector3 extents;
    };
    static_assert(sizeof(ClimbRegion) == 0x40);

    struct CutsceneCamera
    {
        int uid;
        Vector3 pos;
        Matrix3 orient;
    };
    static_assert(sizeof(CutsceneCamera) == 0x34);

    struct PushRegion
    {
        int shape;
        int uid;
        uint flags_and_turbulence;
        Vector3 pos;
        Matrix3 orient;
        float radius_pow2;
        Vector3 aabb_min;
        Vector3 aabb_max;
        Vector3 vExtents;
        float strength;
        bool is_enabled;
    };
    static_assert(sizeof(PushRegion) == 0x6C); // original is 0x69 due to 1 byte bool

    struct EmitterPair
    {
        ParticleEmitter *pemitter;
        int sound_emitter_handle;
        Vector3 emitting_pos;
        EmitterPair *next;
        EmitterPair *prev;
        GRoom *room;
    };
    static_assert(sizeof(EmitterPair) == 0x20);

    struct EmitterPairSet
    {
        EmitterPair head;
    };
    static_assert(sizeof(EmitterPairSet) == 0x20);

    struct LevelInfo
    {
        int version;
        String name;
        String filename;
        String author;
        String level_date;
        int level_timestamp;
        int default_rock_texture;
        int default_rock_hardness;
        Color ambient_light;
        Color distance_fog_color;
        float distance_fog_near_clip;
        float distance_fog_far_clip;
        float old_distance_fog_far_clip;
        bool has_mirrored_faces;
        bool has_skyroom;
        char skybox_rotation_axis;
        float skybox_rotation_velocity;
        float skybox_current_rotation;
        Matrix3 skybox_current_orientation;
        Matrix3 skybox_inverse_orientation;
        unsigned checksum;
        String next_level_filename;
        bool hard_level_break;
        VArray<ParticleEmitter*> pemitters;
        VArray<BoltEmitter*> bemitters;
        VArray<LevelLight*> lights;
        VArray<GeoRegion*> regions;
        VArray<ClimbRegion*> ladders;
        VArray<PushRegion*> pushers;
        EmitterPairSet ep_set;
        GSolid *geometry;
        int flags;
        float time;
        float global_time;
        float time_left;
        Vector3 player_start_pos;
        Matrix3 player_start_orient;
    };
    static_assert(sizeof(LevelInfo) == 0x154);

    enum LevelFlags {
        LEVEL_LOADED = 1,
    };

    static auto& add_liquid_depth_update =
        addr_as_ref<void(GRoom* room, float target_liquid_depth, float duration)>(0x0045E640);
    static auto& level_room_from_uid = addr_as_ref<GRoom*(int uid)>(0x0045E7C0);

    static auto& level = addr_as_ref<LevelInfo>(0x00645FD8);
    static auto& level_filename_to_load = addr_as_ref<String>(0x00646140);
    static auto& level_get_push_region_from_uid = addr_as_ref<PushRegion*(int uid)>(0x0045D6D0);

    static auto& level_set_level_to_load = addr_as_ref<void(String filename, String state_filename)>(0x0045E2E0);
    static auto& game_new_game = addr_as_ref<void()>(0x00436950);

}
