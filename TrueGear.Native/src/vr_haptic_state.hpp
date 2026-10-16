#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>

namespace tg {

struct VrHapticPulse {
    int side = -1;
    float amplitude = 0;
    std::int64_t durationNs = 0;
    float frequency = 0;
    std::uint64_t sequence = 0;
    std::uint64_t capturedAt = 0;
};

// Observes successful original controller output; it never creates an effect
// or alters the original call. The caller serializes access to this object.
class VrHapticObserver {
    struct Window {
        bool active = false;
        std::uint64_t lastApply = 0;
        std::uint64_t durationMs = 0;
        std::uint64_t lastEmitted = 0;
    };

    static constexpr std::size_t Capacity = 64;
    static constexpr std::uint64_t MaxAgeMs = 250;
    static constexpr std::uint64_t ContinuousIntervalMs = 100;
    std::array<Window, 2> windows_{};
    std::deque<VrHapticPulse> pulses_;
    std::uint64_t sequence_ = 0;
    std::uint64_t lastNow_ = 0;
    bool haveClock_ = false;

    void AlignClock(std::uint64_t nowMs) noexcept {
        if (haveClock_ && nowMs < lastNow_) {
            // Old timestamps belong to the abandoned clock window. They must
            // not become future events that a later poll could replay.
            pulses_.clear();
            windows_ = {};
        }
        haveClock_ = true;
        lastNow_ = nowMs;
    }

    void DiscardExpired(std::uint64_t nowMs) noexcept {
        while (!pulses_.empty() && nowMs - pulses_.front().capturedAt > MaxAgeMs)
            pulses_.pop_front();
    }

public:
    // True means this Apply created a queued event. Every valid successful
    // Apply at amplitude >= 0.1 advances sequence, including merged calls.
    // Rejected low amplitudes cannot alter the clock or continuous window.
    bool Observe(int side, float amplitude, std::int64_t durationNs, float frequency,
                 std::int32_t xrResult, std::uint64_t nowMs) {
        if (xrResult != 0 || side < 0 || side > 1 ||
            !std::isfinite(amplitude) || amplitude < 0.1f || amplitude > 1 ||
            !std::isfinite(frequency) || frequency < 0 ||
            (durationNs <= 0 && durationNs != -1)) return false;

        AlignClock(nowMs);
        auto& window = windows_[side];
        const bool emit = !window.active || nowMs - window.lastApply >= window.durationMs ||
                          nowMs - window.lastEmitted >= ContinuousIntervalMs;
        const auto sequence = ++sequence_;
        window.active = true;
        window.lastApply = nowMs;
        // Divide before rounding, so INT64_MAX nanoseconds cannot overflow.
        // Store elapsed duration instead of an absolute deadline, so even
        // timestamps near UINT64_MAX retain the same expiration behavior.
        window.durationMs = durationNs == -1 ? 1 :
            static_cast<std::uint64_t>(durationNs / 1000000) + (durationNs % 1000000 != 0);
        if (!emit) return false;

        window.lastEmitted = nowMs;
        DiscardExpired(nowMs);
        if (pulses_.size() == Capacity) pulses_.pop_front();
        pulses_.push_back({side, amplitude, durationNs, frequency, sequence, nowMs});
        return true;
    }

    void Stop(int side, std::int32_t xrResult) noexcept {
        if (xrResult == 0 && side >= 0 && side <= 1) windows_[side] = {};
    }

    bool Pop(std::uint64_t nowMs, VrHapticPulse& pulse) noexcept {
        AlignClock(nowMs);
        DiscardExpired(nowMs);
        if (pulses_.empty()) return false;
        pulse = pulses_.front();
        pulses_.pop_front();
        return true;
    }

    void Reset() noexcept {
        pulses_.clear();
        windows_ = {};
        haveClock_ = false;
        lastNow_ = 0;
    }
};

} // namespace tg
