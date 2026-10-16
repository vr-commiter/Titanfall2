#pragma once

#include "vr_custom_haptic_state.hpp"

#include <cstdint>

namespace tg {

// Takes over the original pending OR/XCHG instructions in validated VR memory.
// Original controller output is unchanged; only the extra TrueGear copy reads
// the source-aware shadow decoder. No waiting or playback occurs here.
bool InstallCustomHapticBridge(void* validatedVr, void* validatedClient);
void ResetCustomHapticBridge() noexcept;
bool ReadFilteredCustomHaptic(void* vr, void* input, int side,
                             float remaining, float amplitude,
                             float& filteredRemaining, float& filteredAmplitude) noexcept;

// Called synchronously by the added script native wrapper. A true result means
// the original PulseHands function has already run exactly once, including when
// its return value reports failure. Do not fall back and invoke it a second time.
bool TryMappedScriptPulse(void* vr, void* rawVm, CustomHapticSource source,
                          std::int32_t& originalResult);

} // namespace tg
