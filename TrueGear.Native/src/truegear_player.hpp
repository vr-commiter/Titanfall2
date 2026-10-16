#pragma once

#include "truegear_transport.hpp"
#include <nlohmann/json.hpp>
#include <map>
#include <optional>
#include <set>
#include <string>

namespace tg {

// Pure validation/normalization shared by dry-run and live output paths.
bool PrepareEffectContent(const nlohmann::json& input, nlohmann::json& normalized, std::string& error);
bool PrepareAngleEffectContent(const nlohmann::json& input, float angle, float vertical,
                              nlohmann::json& normalized, std::string& error);
// Match the supplied C# clone: scale only existing start_intensity fields.
bool PreparePowerEffectContent(const nlohmann::json& input, float amplitude,
                              nlohmann::json& normalized, std::string& error);

// Native facade for the supplied managed SDK's effect functionality.
// All calls belong on one output worker; no game or UI thread waits on I/O.
class TrueGearPlayer {
public:
    TrueGearPlayer(std::string app_id, std::string api_key);
    bool Start();
    bool GetStatus();
    bool SendRegist();
    bool SendPlay(const std::string& effect_uuid);
    bool PlayAngle(const std::string& effect_uuid, float angle, float vertical);
    bool SendPlayEffectByContent(const nlohmann::json& effect);
    bool SendSeekEffectByContent(const std::string& effect_uuid);
    bool PreSeekEffect(const std::string& effect_uuid);
    std::optional<nlohmann::json> FindEffectByUuid(const std::string& effect_uuid);

    // Explicit worker service and lifecycle helpers for the game plugin.
    void Poll();
    bool Stop(const std::string& effect_uuid);
    void Disconnect();
    const std::string& last_error() const;

private:
    bool ValidName(const std::string& value);
    bool Sent(bool result);
    void ClearCache();
    std::string app_id_, api_key_, error_;
    TrueGearTransport transport_;
    std::set<std::string> preseek_;
    std::map<std::string, nlohmann::json> cached_;
};
} // namespace tg
