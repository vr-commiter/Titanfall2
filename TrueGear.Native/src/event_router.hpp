#pragma once

#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace tg {
using Json = nlohmann::json;

Json DefaultConfig();

// Selects an event identifier; runtime chooses named Player playback or an
// explicitly configured body-slot preset. Identifiers use English letters,
// digits, and underscores. Call on the output worker only.
class EventRouter {
public:
    explicit EventRouter(const Json& config);
    std::vector<std::string> Map(const Json& event, std::int64_t now_ms);
    void Reset();

private:
    Json config_;
    std::unordered_map<std::string, std::int64_t> last_play_;
};
} // namespace tg
