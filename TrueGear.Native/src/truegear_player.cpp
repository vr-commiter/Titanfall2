#include "truegear_player.hpp"

#include <utility>
#include <limits>
#include <cmath>

namespace tg {
TrueGearPlayer::TrueGearPlayer(std::string app_id, std::string api_key)
    : app_id_(std::move(app_id)), api_key_(std::move(api_key)) {}

bool TrueGearPlayer::ValidName(const std::string& value) {
    if (!IsTrueGearEventName(value)) {
        error_ = "Effect name/UUID must contain 1..256 ASCII letters, digits or underscores";
        return false;
    }
    return true;
}

void TrueGearPlayer::ClearCache() { cached_.clear(); }

bool TrueGearPlayer::Sent(bool result) {
    if (!result) {
        error_ = transport_.last_error();
        if (error_.empty()) error_ = "TrueGear local transport is not connected";
        if (!transport_.Connected()) ClearCache();
    }
    return result;
}

bool TrueGearPlayer::Start() {
    Poll();
    if (transport_.Connected()) return true;
    ClearCache();
    error_.clear();
    return Sent(transport_.Connect(app_id_, api_key_));
}

void TrueGearPlayer::Poll() {
    if (!transport_.Connected()) { ClearCache(); return; }
    for (const auto& response : transport_.DrainResponses()) {
        if (response.method == "register_app") {
            // The supplied SDK treats a response with this method as the
            // trigger for prefetching, without interpreting Result as a
            // hardware or authentication status code. Keep that distinction.
            ClearCache();
            for (const auto& uuid : preseek_) {
                if (!Sent(transport_.SeekByUuid(uuid))) return;
            }
        } else if (response.method == "seek_by_uuid") {
            try {
                auto depth_guard = [](int depth, nlohmann::json::parse_event_t, nlohmann::json&) {
                    if (depth > 24) throw std::invalid_argument("Effect lookup JSON too deeply nested");
                    return true;
                };
                const auto effect = nlohmann::json::parse(response.result, depth_guard);
                if (!effect.is_object() || !effect.contains("uuid") || !effect["uuid"].is_string()) continue;
                const auto uuid = effect["uuid"].get<std::string>();
                if (preseek_.find(uuid) != preseek_.end()) cached_[uuid] = effect;
            } catch (const std::exception&) {
                error_ = "Ignored malformed TrueGear effect lookup response";
            }
        }
    }
    if (!transport_.Connected()) ClearCache();
}

bool TrueGearPlayer::GetStatus() {
    Poll();
    return transport_.Connected();
}

bool TrueGearPlayer::SendRegist() {
    Poll();
    return Sent(transport_.Register());
}

bool TrueGearPlayer::SendPlay(const std::string& uuid) {
    if (!ValidName(uuid)) return false;
    Poll();
    return Sent(transport_.PlayByUuid(uuid));
}

bool PrepareEffectContent(const nlohmann::json& input, nlohmann::json& normalized, std::string& error) {
    try {
        if (!input.is_object() || !input.contains("name") || !input["name"].is_string())
            throw std::invalid_argument("Effect content requires a string name");
        const auto name = input["name"].get<std::string>();
        if (!IsTrueGearEventName(name))
            throw std::invalid_argument("Effect name/UUID must contain 1..256 ASCII letters, digits or underscores");
        auto effect = input;
        if (effect.contains("uuid") && (!effect["uuid"].is_string() || effect["uuid"] != name))
            throw std::invalid_argument("Effect UUID must match its name");
        // The supplied SDK writes the name into both wire fields.
        effect["uuid"] = name;
        if (!effect.contains("tracks") || !effect["tracks"].is_array())
            throw std::invalid_argument("Effect content requires a tracks array");
        auto normalize_boolean = [](nlohmann::json& object, const char* field, bool add_default) {
            if (!object.contains(field)) {
                if (add_default) object[field] = "False";
            } else if (object[field].is_boolean()) object[field] = object[field].get<bool>() ? "True" : "False";
            else if (!object[field].is_string() || (object[field] != "True" && object[field] != "False"))
                throw std::invalid_argument(std::string("Invalid effect boolean: ") + field);
        };
        normalize_boolean(effect, "keep", true);
        auto is_int32 = [](const nlohmann::json& value) {
            if (!value.is_number_integer()) return false;
            const auto number = value.get<double>();
            return number >= std::numeric_limits<std::int32_t>::min() && number <= std::numeric_limits<std::int32_t>::max();
        };
        for (auto& track : effect["tracks"]) {
            if (!track.is_object()) throw std::invalid_argument("Effect tracks must be objects");
            normalize_boolean(track, "once", false);
            if (track.contains("stop_name") && (!track["stop_name"].is_string() ||
                (!track["stop_name"].get<std::string>().empty() &&
                 !IsTrueGearEventName(track["stop_name"].get<std::string>()))))
                throw std::invalid_argument("Effect stop_name must be empty or a valid event name");
            if (track.contains("index")) {
                if (!track["index"].is_array()) throw std::invalid_argument("Effect index must be an array");
                for (const auto& index : track["index"])
                    if (!is_int32(index)) throw std::invalid_argument("Effect index entries must be Int32 values");
            }
            for (const char* field : {"start_time", "end_time", "start_intensity", "end_intensity", "interval"})
                if (track.contains(field) && !is_int32(track[field]))
                    throw std::invalid_argument(std::string("Effect field must be an Int32 value: ") + field);
            if (track.contains("action_type") && track["action_type"] != "Shake" &&
                track["action_type"] != "Electrical" && track["action_type"] != "Stop")
                throw std::invalid_argument("Invalid SDK action_type");
            if (track.contains("intensity_mode") && track["intensity_mode"] != "Const" &&
                track["intensity_mode"] != "Fade" && track["intensity_mode"] != "FadeInAndOut")
                throw std::invalid_argument("Invalid SDK intensity_mode");
        }
        if (effect.dump().size() > 65536) throw std::invalid_argument("Effect content exceeds limit");
        normalized = std::move(effect);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool TrueGearPlayer::SendPlayEffectByContent(const nlohmann::json& input) {
    nlohmann::json effect;
    if (!PrepareEffectContent(input, effect, error_)) return false;
    Poll();
    return Sent(transport_.PlayContent(effect.dump()));
}

bool PreparePowerEffectContent(const nlohmann::json& input, float amplitude,
                              nlohmann::json& normalized, std::string& error) {
    try {
        nlohmann::json effect;
        if (!PrepareEffectContent(input, effect, error)) return false;
        if (!std::isfinite(amplitude))
            throw std::invalid_argument("Power amplitude must be finite");
        if (amplitude < 0.1f)
            throw std::invalid_argument("Power amplitude below 0.1 is filtered");

        for (auto& track : effect["tracks"]) {
            if (!track.contains("start_intensity")) continue;
            // The supplied C# clone changes only the start intensity. Keep its
            // single-precision multiply followed by truncation: promoting the
            // multiply to double changes 100 * 0.7f from 70 to 69 after casting.
            const float scaled = static_cast<float>(track["start_intensity"].get<std::int32_t>()) * amplitude;
            // INT32_MAX rounds up to 2^31 as a float. Use an exclusive upper
            // boundary so no out-of-range float reaches the integer cast.
            if (!std::isfinite(scaled) || scaled < -2147483648.0f || scaled >= 2147483648.0f)
                throw std::invalid_argument("Scaled effect start_intensity is outside Int32");
            track["start_intensity"] = static_cast<std::int32_t>(scaled);
        }
        normalized = std::move(effect);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool PrepareAngleEffectContent(const nlohmann::json& input, float input_angle, float vertical,
                              nlohmann::json& normalized, std::string& error) {
    try {
        if (!std::isfinite(input_angle) || !std::isfinite(vertical))
            throw std::invalid_argument("Angle and vertical must be finite");
        nlohmann::json effect;
        if (!PrepareEffectContent(input, effect, error)) return false;
        // Camera-relative headings increase counterclockwise. Wrap before
        // sector selection so 0 and 360 share the same front direction.
        double heading = std::fmod(static_cast<double>(input_angle), 360.0);
        if (heading < 0.0) heading += 360.0;
        // Preserve the supplied C# branch, including its exact 22.5 boundary.
        // Treat front as 360 so the original 0-degree ninth-sector anomaly
        // cannot select one EMS arm while the equivalent 360 selects both.
        if (heading == 0.0) heading = 360.0;
        const auto sector_angle = heading > 22.5 ? heading - 22.5 : 360.0 - heading;
        const int horizontal = static_cast<int>(sector_angle / 45.0) + 1;
        const int offset = vertical > 0.1f ? -4 : vertical < -0.5f ? 8 : 0;
        for (auto& track : effect["tracks"]) {
            const auto action = track.value("action_type", "");
            if (action != "Shake" && action != "Electrical") continue;
            if (!track.contains("index") || !track["index"].is_array())
                throw std::invalid_argument("Directional Shake and Electrical tracks require an index array");
            if (action == "Electrical") {
                if (!track["index"].empty()) {
                    if (horizontal == 1 || horizontal == 4 || horizontal == 5 || horizontal == 8)
                        track["index"] = nlohmann::json::array({0, 100});
                    else
                        for (auto& index : track["index"]) index = horizontal <= 4 ? 0 : 100;
                }
                continue;
            }
            auto indices = nlohmann::json::array();
            for (const auto& index : track["index"]) {
                std::int64_t value = index.get<std::int64_t>();
                const bool front = value >= 0 && value <= 19;
                const bool back = value >= 100 && value <= 119;
                if (!front && !back) continue;
                // Clip vertical movement within its original panel before
                // rotation: an absent row must not wrap onto the other panel.
                value += offset;
                if ((front && (value < 0 || value > 19)) ||
                    (back && (value < 100 || value > 119))) continue;
                if (horizontal < 8) {
                    if (front) {
                        const auto remainder = value % 4;
                        if (horizontal <= remainder) value -= horizontal;
                        else if (horizontal <= remainder + 4) value = value - remainder + 99 + horizontal - remainder;
                        // Finish the eight-column circle on the same row.
                        // A fixed +2 only works for the sixth sector.
                        else value += 8 - horizontal;
                    } else {
                        const auto remainder = 3 - value % 4;
                        if (horizontal <= remainder) value += horizontal;
                        else if (horizontal <= remainder + 4) value = value + remainder - 99 - horizontal + remainder;
                        else value -= 8 - horizontal;
                    }
                }
                if ((value >= 0 && value <= 19) || (value >= 100 && value <= 119)) indices.push_back(value);
            }
            track["index"] = std::move(indices);
        }
        normalized = std::move(effect);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool TrueGearPlayer::PlayAngle(const std::string& uuid, float angle, float vertical) {
    if (!ValidName(uuid)) return false;
    if (!std::isfinite(angle) || !std::isfinite(vertical))
        return SendPlay(uuid);
    auto source = FindEffectByUuid(uuid);
    if (!source) {
        // Register before requesting so only this expected lookup can enter
        // the cache. One lazy request per UUID; registration replays it after
        // reconnect. The current hit uses named playback without waiting.
        if (preseek_.find(uuid) == preseek_.end() && PreSeekEffect(uuid))
            SendSeekEffectByContent(uuid);
        return SendPlay(uuid);
    }
    nlohmann::json effect;
    if (PrepareAngleEffectContent(*source, angle, vertical, effect, error_) && SendPlayEffectByContent(effect))
        return true;
    return SendPlay(uuid);
}

bool TrueGearPlayer::SendSeekEffectByContent(const std::string& uuid) {
    if (!ValidName(uuid)) return false;
    Poll();
    return Sent(transport_.SeekByUuid(uuid));
}

bool TrueGearPlayer::PreSeekEffect(const std::string& uuid) {
    if (!ValidName(uuid)) return false;
    if (preseek_.size() >= 512 && preseek_.find(uuid) == preseek_.end()) {
        error_ = "TrueGear prefetch list limit exceeded";
        return false;
    }
    preseek_.insert(uuid);
    return true;
}

std::optional<nlohmann::json> TrueGearPlayer::FindEffectByUuid(const std::string& uuid) {
    if (!ValidName(uuid)) return std::nullopt;
    Poll();
    const auto found = cached_.find(uuid);
    if (found == cached_.end()) return std::nullopt;
    return found->second;
}

bool TrueGearPlayer::Stop(const std::string& uuid) {
    if (!ValidName(uuid)) return false;
    Poll();
    return Sent(transport_.Stop(uuid));
}

void TrueGearPlayer::Disconnect() {
    transport_.Disconnect();
    ClearCache();
}

const std::string& TrueGearPlayer::last_error() const {
    return error_.empty() ? transport_.last_error() : error_;
}
} // namespace tg
