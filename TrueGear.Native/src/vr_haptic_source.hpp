#pragma once

#include <algorithm>
#include <cmath>

namespace tg {

// Inputs sampled from the original controller haptic mixer. Gun ownership uses
// physical controller bits: bit 0 is left, bit 1 is right.
struct VrHapticSourceSnapshot {
    int mode = 5;
    unsigned gunHandMask = 0;
    float nativeAll = 0;
    float nativeNonWeapon = 0;
    float customRemaining = 0;
    float customAmplitude = 0;
    float auxiliaryLeftAmplitude = 0;
    bool auxiliaryLeftActive = false;
    bool mappedFilterRequired = false;
    bool mappedFilterValid = false;
    float nativeUnmapped = 0;
    bool customFilterRequired = false;
    bool customFilterValid = false;
    float unmappedCustomRemaining = 0;
    float unmappedCustomAmplitude = 0;
};

struct VrHapticSelection {
    bool valid = false;
    float amplitude = 0;
};

// Reconstruct the mixer branch that produced raw before removing the weapon
// contribution. Exact equality is intentional: a changed source snapshot must
// not attribute a pulse from a different update. Zero is a valid selection; the
// observer owns its existing minimum-amplitude threshold.
inline VrHapticSelection SelectNonWeaponHaptic(
        int side, float raw, const VrHapticSourceSnapshot& source) noexcept {
    const auto validAmplitude = [](float value) noexcept {
        return std::isfinite(value) && value >= 0 && value <= 1;
    };
    if (side < 0 || side > 1 || !validAmplitude(raw)) return {};

    // These native override modes do not use the weapon/native source mixer.
    if (source.mode >= 1 && source.mode <= 4) return {true, raw};

    if (!std::isfinite(source.customRemaining)) return {};
    const bool customTimed = source.customRemaining > 0;
    if (customTimed && !validAmplitude(source.customAmplitude)) return {};
    const bool auxiliaryActive = side == 0 && source.auxiliaryLeftActive;
    if (auxiliaryActive && !validAmplitude(source.auxiliaryLeftAmplitude)) return {};
    const bool customActive = (customTimed && source.customAmplitude > 0) || auxiliaryActive;
    if (customActive) {
        const float expected = std::max(customTimed ? source.customAmplitude : 0.0f,
                                       auxiliaryActive ? source.auxiliaryLeftAmplitude : 0.0f);
        if (expected != raw) return {};
        if (!source.customFilterRequired || !source.customFilterValid) return {true, raw};
        if (!std::isfinite(source.unmappedCustomRemaining) || source.unmappedCustomRemaining < 0 ||
            !validAmplitude(source.unmappedCustomAmplitude)) return {true, raw};
        const float remainingCustom = source.unmappedCustomRemaining > 0 ? source.unmappedCustomAmplitude : 0.0f;
        const float remainingAuxiliary = auxiliaryActive ? source.auxiliaryLeftAmplitude : 0.0f;
        // Preserve original custom-over-native precedence. Removing a mapped
        // custom pulse must not expose an unrelated native rumble underneath.
        return {true, std::min(raw, std::max(remainingCustom, remainingAuxiliary))};
    }

    const bool gunSide = (source.gunHandMask & (1u << side)) != 0;
    const float expected = gunSide ? source.nativeAll : source.nativeNonWeapon;
    if (!validAmplitude(expected) || expected != raw) return {};
    if (source.mappedFilterRequired) {
        if (!source.mappedFilterValid || !validAmplitude(source.nativeUnmapped) ||
            !validAmplitude(source.nativeNonWeapon) || source.nativeUnmapped > source.nativeNonWeapon)
            return {};
        return {true, std::min(raw, source.nativeUnmapped)};
    }
    if (!gunSide) return {true, raw};

    if (!validAmplitude(source.nativeNonWeapon) || source.nativeNonWeapon > source.nativeAll)
        return {};
    return {true, std::min(raw, source.nativeNonWeapon)};
}

} // namespace tg
