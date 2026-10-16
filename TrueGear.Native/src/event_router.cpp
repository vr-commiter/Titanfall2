#include "event_router.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <set>
#include <stdexcept>
#include <string_view>

namespace tg {
namespace {
[[noreturn]] void Invalid(const std::string& field) {
    throw std::invalid_argument("Invalid TrueGear configuration: " + field);
}

void Keys(const Json& object, std::initializer_list<std::string_view> allowed,
          const std::string& path) {
    if (!object.is_object()) Invalid(path);
    for (const auto& [key, value] : object.items()) {
        (void)value;
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
            Invalid(path + "." + key + " (unrecognized field)");
    }
}

bool IsEventName(const std::string& name) {
    if (name.empty() || name.size() > 256) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_';
    });
}

void Identifier(const std::string& value, const std::string& field) {
    if (!IsEventName(value)) Invalid(field + " (use 1-256 English letters, digits, or underscores)");
}

void Number(const Json& object, const char* key, double minimum, double maximum) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number()) Invalid(key);
    const double value = found->get<double>();
    if (!std::isfinite(value) || value < minimum || value > maximum ||
        std::floor(value) != value) Invalid(key);
}

void Boolean(const Json& object, const char* key) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_boolean()) Invalid(key);
}

std::string Text(const Json& object, const char* key) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string()) Invalid(key);
    return found->get<std::string>();
}

bool IsTrue(const Json& object, const char* key) {
    const auto found = object.find(key);
    return found != object.end() && found->is_boolean() && found->get<bool>();
}

std::string EventText(const Json& object, const char* key) {
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : "";
}

void Validate(const Json& config) {
    if (!config.is_object() || Text(config, "schema") != "truegear.native/2")
        Invalid("schema (requires truegear.native/2 event-name configuration)");
    Keys(config, {"schema", "version", "event_names_note", "enabled", "dry_run", "app_id",
        "api_key", "max_event_age_ms", "max_queue", "reconnect_ms", "allow_inferred_hands",
        "preload_events", "routes", "log_events", "body_slot_effects", "controller_haptic_max_duration_ms"}, "config");
    if (config.contains("version")) (void)Text(config, "version");
    if (config.contains("event_names_note")) (void)Text(config, "event_names_note");
    Boolean(config, "enabled");
    Boolean(config, "dry_run");
    if (config.contains("log_events")) Boolean(config, "log_events");
    // Accepted only for old configuration compatibility; playback never trims
    // effect timing, regardless of this obsolete setting's value.
    if (config.contains("controller_haptic_max_duration_ms")) Number(config, "controller_haptic_max_duration_ms", 0, 1000);
    Boolean(config, "allow_inferred_hands");
    for (const char* key : {"app_id", "api_key"}) {
        const auto value = Text(config, key);
        if (value.empty() || value.size() > 256 || value.find(';') != std::string::npos ||
            value.find('\0') != std::string::npos) Invalid(key);
        try { (void)Json(value).dump(); }
        catch (const Json::exception&) { Invalid(std::string(key) + " (invalid UTF-8)"); }
    }
    Number(config, "max_event_age_ms", 1, 1000);
    Number(config, "max_queue", 1, 4096);
    Number(config, "reconnect_ms", 250, 60000);
    if (!config.contains("preload_events") || !config["preload_events"].is_array() ||
        config["preload_events"].size() > 512) Invalid("preload_events");
    std::set<std::string> preloads;
    for (const auto& item : config["preload_events"]) {
        if (!item.is_string()) Invalid("preload_events item");
        const auto uuid = item.get<std::string>();
        Identifier(uuid, "preload_events item");
        if (!preloads.insert(uuid).second) Invalid("duplicate preload_events item");
    }
    if (config.contains("body_slot_effects")) {
        const auto& effects = config["body_slot_effects"];
        if (!effects.is_object() || effects.size() > 64) Invalid("body_slot_effects");
        for (const auto& [uuid, effect] : effects.items()) {
            const auto path = "body_slot_effects." + uuid;
            Identifier(uuid, path);
            if (!effect.is_object() || Text(effect, "name") != uuid || Text(effect, "uuid") != uuid)
                Invalid(path + " (key, name and uuid must match)");
            if (!effect.contains("tracks") || !effect["tracks"].is_array() || effect["tracks"].empty())
                Invalid(path + ".tracks");
            for (const auto& track : effect["tracks"]) {
                if (!track.is_object() || Text(track, "action_type") != "Shake")
                    Invalid(path + ".tracks (body slot effects must use Shake only)");
                if (!track.contains("index") || !track["index"].is_array() || track["index"].empty())
                    Invalid(path + ".tracks.index");
                for (const auto& index : track["index"]) {
                    if (!index.is_number_integer()) Invalid(path + ".tracks.index");
                    const double value = index.get<double>();
                    if (!((value >= 0 && value <= 19) || (value >= 100 && value <= 119)))
                        Invalid(path + ".tracks.index (requires a physical vest channel)");
                }
            }
        }
    }
    if (!config.contains("routes") || !config["routes"].is_object() || config["routes"].size() > 512)
        Invalid("routes");
    for (const auto& [name, route] : config["routes"].items()) {
        const auto path = "routes." + name;
        if (!IsEventName(name)) Invalid(path);
        Keys(route, {"enabled", "cooldown_ms", "uuid", "hands"}, path);
        Boolean(route, "enabled");
        Number(route, "cooldown_ms", 0, 60000);
        if (route.contains("uuid") == route.contains("hands"))
            Invalid(path + " (choose exactly one of uuid or hands)");
        if (route.contains("uuid")) Identifier(Text(route, "uuid"), path + ".uuid");
        else {
            const auto& hands = route["hands"];
            Keys(hands, {"left_uuid", "right_uuid", "titan_uuid"}, path + ".hands");
            for (const char* key : {"left_uuid", "right_uuid"})
                Identifier(Text(hands, key), path + ".hands." + key);
            if (hands.contains("titan_uuid"))
                Identifier(Text(hands, "titan_uuid"), path + ".hands.titan_uuid");
        }
    }
}

bool NeedsActorHand(std::string_view event) {
	return event == "vr_item_grabbed" || event.starts_with("vr_slot_") ||
        event.starts_with("vr_reload_") || event.starts_with("weapon_reload_") ||
        event.starts_with("vr_grenade_");
}

Json Route(const char* name, int cooldown, bool hands = false, bool titan = false) {
    Json route = {{"enabled", true}, {"cooldown_ms", cooldown}};
    if (!hands) route["uuid"] = name;
    else {
        route["hands"] = {{"left_uuid", std::string(name) + "_left"},
                           {"right_uuid", std::string(name) + "_right"}};
        if (titan) route["hands"]["titan_uuid"] = std::string(name) + "_titan";
    }
    return route;
}

Json BodySlotEffect(const std::string& uuid, const Json& indices, int duration) {
    return {{"name", uuid}, {"uuid", uuid}, {"keep", false},
        {"tracks", Json::array({{
            {"action_type", "Shake"}, {"intensity_mode", "Const"}, {"index", indices},
            {"start_time", 0}, {"end_time", duration}, {"start_intensity", 30}, {"end_intensity", 30},
            {"once", false}, {"interval", 0}, {"stop_name", ""}
        }})}};
}
} // namespace

Json DefaultConfig() {
    Json config = {
        {"schema", "truegear.native/2"},
        {"event_names_note", "Use English letters, digits, and underscores. Suggested Player event names are editable; matching Player events are not confirmed."},
        {"enabled", true}, {"dry_run", false}, {"log_events", true}, {"app_id", "1237970"}, {"api_key", "Titanfall2"},
        {"max_event_age_ms", 250}, {"max_queue", 256}, {"reconnect_ms", 2000},
        {"allow_inferred_hands", true}, {"preload_events", Json::array()}, {"routes", Json::object()},
        {"body_slot_effects", Json::object()}
    };
    auto& routes = config["routes"];
    routes["weapon_shot_inferred"] = Route("weapon_shot_inferred", 45, true, true);
    for (const auto* category : {"pistol", "rifle", "smg", "lmg", "shotgun", "sniper", "special", "anti_titan"}) {
        const auto name = std::string("weapon_shot_") + category;
        routes[name] = Route(name.c_str(), 45, true);
    }
    routes["vr_item_grabbed"] = Route("vr_item_grabbed", 90, true);
    routes["vr_controller_haptic_left"] = Route("vr_controller_haptic_left", 100);
    routes["vr_controller_haptic_right"] = Route("vr_controller_haptic_right", 100);
    // Body location is independent of the hand that took or stored the item.
    // These additive vest pulses never replace existing grab/release effects.
    const Json slot_locations = {
        {"left_shoulder", Json::array({100, 101})}, {"right_shoulder", Json::array({102, 103})},
        {"left_hip", Json::array({112, 116})}, {"right_hip", Json::array({115, 119})},
        {"grenade", Json::array({1, 2, 5, 6})}, {"shoulders", Json::array({100, 101, 102, 103})}
    };
    for (const auto& [location, indices] : slot_locations.items()) {
        for (const auto* action : {"taken", "stored"}) {
            const auto name = std::string("vr_slot_item_") + action + "_" + location;
            routes[name] = Route(name.c_str(), 90);
            config["body_slot_effects"][name] = BodySlotEffect(name, indices, std::string_view(action) == "taken" ? 100 : 150);
        }
    }
    for (const auto* location : {"left_shoulder", "right_shoulder", "shoulders"}) {
        const auto name = std::string("vr_ammo_pouch_taken_") + location;
        routes[name] = Route(name.c_str(), 90);
        config["body_slot_effects"][name] = BodySlotEffect(name, slot_locations[location], 100);
    }
    routes["vr_grenade_thrown_predicted"] = Route("vr_grenade_thrown_predicted", 120, true);
    routes["combat_damage_taken"] = Route("combat_damage_taken", 40);
    routes["combat_damage_taken_nondirectional"] = Route("combat_damage_taken_nondirectional", 40);
    routes["movement_jump"] = Route("movement_jump", 100);
    routes["movement_double_jump"] = Route("movement_double_jump", 100);
    routes["movement_land"] = Route("movement_land", 110);
    routes["movement_dash"] = Route("movement_dash", 100);
    routes["movement_slide_start"] = Route("movement_slide_start", 200);
    routes["health_low"] = Route("health_low", 1000);
    routes["health_regen_start"] = Route("health_regen_start", 1000);
    routes["shield_broken"] = Route("shield_broken", 200);
    routes["shield_restored"] = Route("shield_restored", 500);
    routes["player_death"] = Route("player_death", 0);
    routes["player_phase_shift_start"] = Route("player_phase_shift_start", 400);
    routes["player_execution_start"] = Route("player_execution_start", 400);
    routes["titan_embark_complete"] = Route("titan_embark_complete", 600);
    routes["titan_disembark_complete"] = Route("titan_disembark_complete", 600);
    routes["titan_doomed"] = Route("titan_doomed", 600);
    routes["titan_destroyed"] = Route("titan_destroyed", 600);
    routes["titan_footstep_inferred"] = Route("titan_footstep_inferred", 120);
    routes["titan_core_ready"] = Route("titan_core_ready", 500);
    routes["titan_core_active_start"] = Route("titan_core_active_start", 500);
    routes["titan_core_active_end"] = Route("titan_core_active_end", 500);
    routes["titan_vortex_shield_impact"] = Route("titan_vortex_shield_impact", 40);
    routes["titan_vortex_shield_reflect"] = Route("titan_vortex_shield_reflect", 40);
    routes["titan_heat_shield_impact"] = Route("titan_heat_shield_impact", 40);
    routes["titan_particle_wall_impact"] = Route("titan_particle_wall_impact", 80);
    routes["titan_sword_block_impact"] = Route("titan_sword_block_impact", 40);
    routes["titan_gun_shield_impact"] = Route("titan_gun_shield_impact", 40);
    return config;
}

EventRouter::EventRouter(const Json& config) : config_(config) { Validate(config_); }

std::vector<std::string> EventRouter::Map(const Json& event, std::int64_t now_ms) {
    if (!config_["enabled"].get<bool>() || now_ms < 0 || !event.is_object() ||
        EventText(event, "schema") != "truegear.events/1" ||
        !event.contains("data") || !event["data"].is_object()) return {};
    const auto name = EventText(event, "event");
    if (name == "session_ready" || name == "player_despawn") return {};
    const bool resolved = IsTrue(event, "playback_name_resolved");
    std::string route_name = name;
    std::string resolved_side;
    auto route_at = config_["routes"].find(name);
    // Current hooks already selected a physical side in the event name.
    // Retain existing configured UUIDs/cooldowns without inspecting hand data.
    // An explicit full-name route takes precedence over a base-name route.
    if (resolved && route_at == config_["routes"].end()) {
        for (const auto* side : {"left", "right", "titan"}) {
            const std::string suffix = std::string("_") + side;
            if (!name.ends_with(suffix)) continue;
            const auto base = name.substr(0, name.size() - suffix.size());
            const auto candidate = config_["routes"].find(base);
            if (candidate != config_["routes"].end()) {
                route_name = base;
                resolved_side = side;
                route_at = candidate;
            }
            break;
        }
    }
    if (route_at == config_["routes"].end() || !(*route_at)["enabled"].get<bool>()) return {};
    const auto& route = *route_at;
    const auto& data = event["data"];
    std::string uuid;
    if (route.contains("uuid")) uuid = route["uuid"].get<std::string>();
    else if (resolved) {
        const auto field = resolved_side + "_uuid";
        if (resolved_side.empty() || !route["hands"].contains(field)) return {};
        uuid = route["hands"][field].get<std::string>();
    }
    else {
        const auto& hands = route["hands"];
        if (IsTrue(data, "titan")) {
            if (!hands.contains("titan_uuid")) return {};
            uuid = hands["titan_uuid"].get<std::string>();
        } else {
            if (!IsTrue(data, "hand_known")) return {};
            const auto hand = EventText(data, "hand");
            if (hand != "left" && hand != "right") return {};
            if (!config_["allow_inferred_hands"].get<bool>() && IsTrue(data, "hand_inferred")) return {};
            const auto role = EventText(data, "hand_role");
            if ((NeedsActorHand(name) && role != "actor") ||
                (name.starts_with("weapon_shot_") && role != "weapon")) return {};
            uuid = hands[hand == "left" ? "left_uuid" : "right_uuid"].get<std::string>();
        }
    }
    const auto identity = route_name + '\0' + uuid;
    const auto previous = last_play_.find(identity);
    const auto cooldown = route["cooldown_ms"].get<std::int64_t>();
    if (previous != last_play_.end() &&
        (now_ms < previous->second || now_ms - previous->second < cooldown)) return {};
    last_play_[identity] = now_ms;
    return {uuid};
}

void EventRouter::Reset() { last_play_.clear(); }
} // namespace tg
