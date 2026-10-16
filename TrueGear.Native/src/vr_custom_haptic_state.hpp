#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace tg {

enum class CustomHapticSource : std::uint8_t {
    Unknown = 0,
    Acquisition = 1,
    AmmoPickup = 2,
    ItemGrab = 3,
    ItemRelease = 4,
};

// The original pending word uses only bits 0..7: four kinds per physical
// controller. Add provenance to that SAME atomic OR, rather than a separate
// sidecar write that could land on the other side of the consumer's XCHG.
// Bits 8..15 retain unknown sources; bits 16..23 identify excluded sources.
// Invalid original high bits never authorize exclusion of the remaining low
// bits. The consumer must give only the low byte to the original decoder.
inline std::uint32_t EncodeCustomPending(std::uint32_t originalMask, bool excluded) noexcept {
    const std::uint32_t low = originalMask & 0xffu;
    const bool knownMask = originalMask == low;
    return low | (low << (excluded && knownMask ? 16 : 8));
}

// A low bit with no provenance belongs to an uninstrumented/earlier writer and
// remains unknown. Unknown wins when both source classes set the SAME kind.
// Orphan high bits cannot create a new pulse. Reserved high bits invalidate
// exclusion for this batch, while retaining the original low-byte request.
inline std::uint8_t DecodeCustomUnknown(std::uint32_t packed) noexcept {
    const std::uint32_t low = packed & 0xffu;
    if (packed & 0xff000000u) return static_cast<std::uint8_t>(low);
    const std::uint32_t unknown = (packed >> 8) & 0xffu;
    const std::uint32_t mapped = (packed >> 16) & 0xffu;
    return static_cast<std::uint8_t>(low & (unknown | ~mapped));
}

struct CustomPulseState {
    float remaining = 0;
    float amplitude = 0;
};

namespace custom_haptic_state_detail {
inline bool Valid(const CustomPulseState& state) noexcept {
    return std::isfinite(state.remaining) && state.remaining >= 0 &&
        std::isfinite(state.amplitude) && state.amplitude >= 0 && state.amplitude <= 1;
}

inline bool Same(float a, float b) noexcept {
    // Do not accept an adjacent float as another frame's prediction. Signed
    // zero is also checked so the source snapshot is an exact identity check.
    std::uint32_t first{}, second{};
    std::memcpy(&first, &a, sizeof(first));
    std::memcpy(&second, &b, sizeof(second));
    return first == second;
}

inline bool Same(const CustomPulseState& a, const CustomPulseState& b) noexcept {
    return Same(a.remaining, b.remaining) && Same(a.amplitude, b.amplitude);
}
} // namespace custom_haptic_state_detail

// Replay the original single-controller decoder: float32 SUBSS, MAXSS with
// positive zero, then the highest pending kind (3 down to 0). Kind zero cannot
// replace an active pulse above its own 0.12 amplitude; other kinds replace it
// even if the previous pulse was stronger or longer. A zero remaining time
// retains the old amplitude field, exactly as the original consumer does.
// Invalid elapsed time/state is not guessed: leave state untouched and return
// false so the observer can withhold this additional TrueGear copy.
inline bool StepCustomPulse(CustomPulseState& state, std::uint8_t nibble, float dt) noexcept {
    using namespace custom_haptic_state_detail;
    if (!Valid(state) || !std::isfinite(dt) || dt < 0) return false;

    // Force the intermediate rounding to float32 even on compilers that permit
    // excess precision. The comparison also chooses +0 for a negative result.
    volatile float decremented = state.remaining - dt;
    state.remaining = decremented > 0.0f ? decremented : 0.0f;
    nibble &= 0x0fu;
    if (!nibble) return true;

    int kind = 3;
    while ((nibble & (1u << kind)) == 0) --kind;
    if (kind == 0 && state.remaining > 0.0f && state.amplitude > 0.12f) return true;

    constexpr std::array<float, 4> duration{0.012f, 0.04f, 0.14f, 0.5f};
    constexpr std::array<float, 4> amplitude{0.12f, 0.4f, 0.6f, 1.0f};
    state.remaining = duration[static_cast<std::size_t>(kind)];
    state.amplitude = amplitude[static_cast<std::size_t>(kind)];
    return true;
}

// Called once at each original pending XCHG, before its per-side decoder runs.
// The owner serializes Consume/Read and resets this object for a new input
// object/generation. No clocks, waiting, playback, allocation or locks live here.
class CustomPulseTracker {
    std::array<CustomPulseState, 2> expected_{};
    std::array<CustomPulseState, 2> filtered_{};
    bool valid_ = false;

public:
    void Reset() noexcept {
        expected_ = {};
        filtered_ = {};
        valid_ = false;
    }

    // before is the original pair of active fields immediately before this
    // frame's decrement. A new baseline or mismatch keeps those active fields
    // as unknown; source identity cannot be recovered from amplitude/duration.
    // Both original fields zero on both sides is an explicit native reset.
    bool Consume(std::uint32_t packed, float dt,
                 const std::array<CustomPulseState, 2>& before) noexcept {
        using namespace custom_haptic_state_detail;
        if (!std::isfinite(dt) || dt < 0 || !Valid(before[0]) || !Valid(before[1])) {
            Reset();
            return false;
        }

        const bool zero = before[0].remaining == 0 && before[0].amplitude == 0 &&
                          before[1].remaining == 0 && before[1].amplitude == 0;
        const bool matched = valid_ && Same(before[0], expected_[0]) && Same(before[1], expected_[1]);
        if (zero || !matched) filtered_ = before;
        expected_ = before;

        const auto original = static_cast<std::uint8_t>(packed & 0xffu);
        const auto unknown = DecodeCustomUnknown(packed);
        for (std::size_t side = 0; side < expected_.size(); ++side) {
            const auto shift = static_cast<unsigned>(side * 4);
            if (!StepCustomPulse(expected_[side], static_cast<std::uint8_t>(original >> shift), dt) ||
                !StepCustomPulse(filtered_[side], static_cast<std::uint8_t>(unknown >> shift), dt)) {
                Reset();
                return false;
            }
        }
        valid_ = true;
        return true;
    }

    // Read only against the actual ORIGINAL active fields after its decoder.
    // The caller still reconstructs the original custom-vs-native priority:
    // filtered custom plus left auxiliary may replace a custom-selected raw
    // pulse, capped to raw. A removed custom pulse must not reveal hidden native
    // rumble or prolong output after the original custom branch has ended.
    bool Read(int side, float actualRemaining, float actualAmplitude,
              float& outRemaining, float& outAmplitude) const noexcept {
        using namespace custom_haptic_state_detail;
        outRemaining = outAmplitude = 0;
        if (!valid_ || side < 0 || side > 1) return false;
        const auto index = static_cast<std::size_t>(side);
        const CustomPulseState actual{actualRemaining, actualAmplitude};
        if (!Valid(actual) || !Same(actual, expected_[index])) return false;
        outRemaining = filtered_[index].remaining;
        outAmplitude = filtered_[index].amplitude;
        return true;
    }
};

} // namespace tg
