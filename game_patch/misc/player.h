#include <functional>
#include <map>
#include <optional>
#include <string>
#include <common/utils/string-utils.h>
#include "../rf/math/vector.h"
#include "../rf/math/matrix.h"
#include "../rf/os/timestamp.h"
#include "../purefaction/pf_packets.h"

// Forward declarations
namespace rf
{
    struct Player;
}

struct PlayerNetGameSaveData
{
    rf::Vector3 pos;
    rf::Matrix3 orient;
};

struct PlayerAdditionalData
{
    std::optional<pf_pure_status> received_ac_status{};
    bool is_browser = false;
    bool is_muted = false;
    int last_hitsound_sent_ms = 0;
    int last_critsound_sent_ms = 0;
    std::map<std::string, PlayerNetGameSaveData> saves;
    rf::Vector3 last_teleport_pos;
    rf::TimestampRealtime last_teleport_timestamp;
    std::optional<int> last_spawn_point_index;
    bool is_alpine = false;
    int last_activity_ms = 0;
    rf::TimestampRealtime idle_check_timestamp;
    rf::TimestampRealtime idle_kick_timestamp;
    uint8_t alpine_version_major = 0;
    uint8_t alpine_version_minor = 0;
    uint8_t alpine_version_type = 0;
    uint32_t max_rfl_version = 200;
};

void find_player(const StringMatcher& query, std::function<void(rf::Player*)> consumer);
void reset_player_additional_data(const rf::Player* player);
PlayerAdditionalData& get_player_additional_data(rf::Player* player);
void update_player_flashlight();
