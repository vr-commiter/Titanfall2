#include "vr_rumble_filter.hpp"
#include "vr_custom_haptic_state.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace tg {
namespace {
constexpr std::size_t MaxSources = 32;
constexpr std::size_t MaxName = 191;
constexpr std::size_t MaxPolicies = 256;
constexpr std::array<std::string_view, 6> Events = {
    "combat_damage_taken", "movement_jump", "movement_double_jump",
    "movement_land", "movement_slide_start",
    "movement_dash"
};
constexpr std::array<std::string_view, 4> DamageGraphs = {
    "rumble_pilot_hurt", "titan_damaged_small", "titan_damaged_med", "titan_damaged_big"
};
// client+0x159362 supplies this exact graph to the native rumble slot player.
// Keep the cockpit source distinct from nearby Titan steps and other impacts.
constexpr std::string_view TitanFootstepGraph = "titan_cockpit_footstep";
// Keep the known raise pulse out of generic TrueGear playback when the
// dedicated heat-shield impact route is enabled: this mode emits hits only.
constexpr std::string_view TitanHeatShieldStartGraph = "rumble_titan_heatshield_start";

struct Policy {
    std::array<std::array<char, MaxName + 1>, MaxSources + DamageGraphs.size() + 2> names{};
    std::size_t count = 0;
    std::uint64_t revision = 0;
    std::uint32_t customSources = 0;
};
// Trivial process-lifetime storage is safe even during other DLLs' CRT teardown.
constinit Policy emptyPolicy;
constinit std::atomic<const Policy*> published{&emptyPolicy};
static_assert(std::atomic<const Policy*>::is_always_lock_free);

struct Options {
    bool enabled = true;
    std::array<bool, Events.size()> routes{};
    bool nondirectionalDamage = false;
    bool titanFootsteps = false;
    bool titanHeatShieldImpact = false;
    std::uint32_t customSources = 0;
};
struct Source {
    std::size_t event;
    std::string graph;
    bool operator==(const Source&) const = default;
};
struct WriterState {
    std::mutex mutex;
    Options options;
    std::vector<Source> sources;
    std::vector<std::unique_ptr<Policy>> records;
};
WriterState& Writer() {
    // Never destroy records while a pinned native callback may still read one.
    static auto* const state = new WriterState;
    return *state;
}

std::string_view Name(const Policy& policy, std::size_t index) noexcept {
    return policy.names[index].data();
}

void Publish(WriterState& state, const Options& options, const std::vector<Source>& sources) {
    std::vector<std::string_view> names;
    if (options.enabled) {
        if (options.routes[0] || options.nondirectionalDamage)
            names.insert(names.end(), DamageGraphs.begin(), DamageGraphs.end());
        if (options.titanFootsteps) names.push_back(TitanFootstepGraph);
        if (options.titanHeatShieldImpact) names.push_back(TitanHeatShieldStartGraph);
        for (const auto& source : sources)
            if (options.routes[source.event]) names.emplace_back(source.graph);
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    const Policy* previous = published.load(std::memory_order_acquire);
    const auto customSources = options.enabled ? options.customSources : 0;
    if (previous->count == names.size() && previous->customSources == customSources) {
        bool same = true;
        for (std::size_t i = 0; i < names.size(); ++i)
            if (Name(*previous, i) != names[i]) { same = false; break; }
        if (same) return;
    }
    if (state.records.size() >= MaxPolicies)
        throw std::runtime_error("Mapped rumble policy store limit reached.");
    auto next = std::make_unique<Policy>();
    next->count = names.size();
    next->revision = previous->revision + 1;
    next->customSources = customSources;
    for (std::size_t i = 0; i < names.size(); ++i)
        std::memcpy(next->names[i].data(), names[i].data(), names[i].size());
    const Policy* snapshot = next.get();
    state.records.push_back(std::move(next));
    published.store(snapshot, std::memory_order_release);
}

Options ReadOptions(const Json& config) {
    Options result;
    if (!config.is_object()) { result.enabled = false; return result; }
    const auto enabled = config.find("enabled");
    if (enabled != config.end()) result.enabled = enabled->is_boolean() && enabled->get<bool>();
    const auto routes = config.find("routes");
    if (routes == config.end() || !routes->is_object()) return result;
    for (std::size_t i = 0; i < Events.size(); ++i) {
        const auto route = routes->find(std::string(Events[i]));
        if (route == routes->end() || !route->is_object()) continue;
        const auto active = route->find("enabled");
        result.routes[i] = active != route->end() && active->is_boolean() && active->get<bool>();
    }
    const auto activeRoute = [&routes](const char* name) {
        const auto route = routes->find(name);
        if (route == routes->end() || !route->is_object()) return false;
        const auto active = route->find("enabled");
        return active != route->end() && active->is_boolean() && active->get<bool>();
    };
    result.nondirectionalDamage = activeRoute("combat_damage_taken_nondirectional");
    result.titanFootsteps = activeRoute("titan_footstep_inferred");
    result.titanHeatShieldImpact = activeRoute("titan_heat_shield_impact");
    const auto customBit = [](CustomHapticSource source) { return 1u << static_cast<unsigned>(source); };
    if (activeRoute("vr_item_grabbed"))
        result.customSources |= customBit(CustomHapticSource::Acquisition) | customBit(CustomHapticSource::ItemGrab);
    // The user removed release playback. Keep its confirmed SERE source out
    // of generic TrueGear haptics instead of reintroducing it as a fallback.
    // This tag changes only our copy; the original controller pulse remains.
    result.customSources |= customBit(CustomHapticSource::ItemRelease);
    // Location is resolved later by the existing body observer. When only
    // some pouch routes are enabled, preserve this source conservatively.
    if (activeRoute("vr_ammo_pouch_taken_left_shoulder") && activeRoute("vr_ammo_pouch_taken_right_shoulder") &&
        activeRoute("vr_ammo_pouch_taken_shoulders"))
        result.customSources |= customBit(CustomHapticSource::AmmoPickup);
    return result;
}

bool ParseSources(const Json& input, std::vector<Source>& result, std::string& error) {
    if (!input.is_array() || input.size() > MaxSources) {
        error = "Mapped rumble sources must be an array of at most 32 entries.";
        return false;
    }
    for (const auto& item : input) {
        if (!item.is_object() || item.size() != 2 || !item.contains("event") ||
            !item.contains("graph") || !item["event"].is_string() || !item["graph"].is_string()) {
            error = "Each mapped rumble source requires only string event and graph fields.";
            return false;
        }
        const auto& event = item["event"].get_ref<const std::string&>();
        // Older scripts/save state may still include removed zipline metadata.
        // Ignore it without rejecting the remaining action source declarations.
        if (event == "movement_zipline_start" || event == "movement_zipline_stop" || event == "movement_mantle") continue;
        const auto found = std::find(Events.begin() + 1, Events.end(), std::string_view(event));
        if (found == Events.end()) {
            error = "Mapped rumble source event is not an allowed movement event.";
            return false;
        }
        const auto& graph = item["graph"].get_ref<const std::string&>();
        if (graph.empty() || graph.size() > MaxName ||
            std::any_of(graph.begin(), graph.end(), [](unsigned char ch) { return ch < 0x21 || ch > 0x7e; })) {
            error = "Mapped rumble graph must contain 1 to 191 visible ASCII characters.";
            return false;
        }
        result.push_back({static_cast<std::size_t>(found - Events.begin()), graph});
    }
    std::sort(result.begin(), result.end(), [](const Source& a, const Source& b) {
        return a.event < b.event || (a.event == b.event && a.graph < b.graph);
    });
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return true;
}
} // namespace

void ConfigureMappedRumbleFilter(const Json& config) {
    const Options options = ReadOptions(config);
    auto& state = Writer();
    std::lock_guard lock(state.mutex);
    Publish(state, options, state.sources);
    state.options = options;
}

bool SetMappedRumbleSources(const Json& sources, std::string& error) {
    error.clear();
    try {
        std::vector<Source> parsed;
        if (!ParseSources(sources, parsed, error)) return false;
        auto& state = Writer();
        std::lock_guard lock(state.mutex);
        Publish(state, state.options, parsed);
        state.sources.swap(parsed);
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool ExcludeMappedRumble(std::string_view name) noexcept {
    if (name.empty() || name.size() > MaxName) return false;
    const Policy& policy = *published.load(std::memory_order_acquire);
    std::size_t first = 0, last = policy.count;
    while (first < last) {
        const std::size_t middle = first + (last - first) / 2;
        const auto candidate = Name(policy, middle);
        if (candidate < name) first = middle + 1;
        else last = middle;
    }
    return first < policy.count && Name(policy, first) == name;
}

std::uint64_t MappedRumblePolicyRevision() noexcept {
    return published.load(std::memory_order_acquire)->revision;
}

bool ExcludeCustomHapticSource(CustomHapticSource source) noexcept {
    const auto index = static_cast<unsigned>(source);
    return index > 0 && index <= static_cast<unsigned>(CustomHapticSource::ItemRelease) &&
        (published.load(std::memory_order_acquire)->customSources & (1u << index)) != 0;
}

std::uint32_t CustomHapticPolicyMask() noexcept {
    return published.load(std::memory_order_acquire)->customSources;
}

} // namespace tg
