#pragma once

#include "event_router.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace tg {
enum class CustomHapticSource : std::uint8_t;

// Writer-side operations. Source declarations replace the previous dynamic
// set, and only routes enabled in the latest configuration are excluded.
// Configure can throw if the bounded process-lifetime policy store is full.
void ConfigureMappedRumbleFilter(const Json& config);
bool SetMappedRumbleSources(const Json& sources, std::string& error);

// Callback-side operations: bounded, allocation-free and lock-free reads.
bool ExcludeMappedRumble(std::string_view name) noexcept;
bool ExcludeCustomHapticSource(CustomHapticSource source) noexcept;
std::uint32_t CustomHapticPolicyMask() noexcept;
std::uint64_t MappedRumblePolicyRevision() noexcept;

} // namespace tg
