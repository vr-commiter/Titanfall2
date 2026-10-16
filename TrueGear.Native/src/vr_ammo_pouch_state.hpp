#pragma once

#include "vr_slot_state.hpp"

#include <cstdint>
#include <vector>

namespace tg {

struct VrAmmoPouchSample {
    std::uintptr_t context = 0;
    int pickupCounter = 0;
    int hand = -1;
    int location = -1;
    std::uintptr_t metadata = 0;
};

// Observe the independently verified successful-pickup counter only. The
// caller supplies physical hand and shoulder evidence; this class never
// reconstructs either from a weapon holder, ejection counter or empty pouch.
class VrAmmoPouchObserver {
    std::uintptr_t context_ = 0;
    std::uintptr_t metadata_ = 0;
    int pickupCounter_ = 0;

public:
    void Reset() noexcept {
        context_ = 0;
        metadata_ = 0;
        pickupCounter_ = 0;
    }

    std::vector<VrSlotEvent> Observe(const VrAmmoPouchSample& sample) {
        // The native current-object pointer is cleared during each update.
        // Its transient absence is not an object or lifecycle boundary.
        if (!sample.context) return {};
        if (sample.pickupCounter < 0) {
            Reset();
            return {};
        }
        const bool increased = sample.context == context_ && sample.metadata == metadata_ &&
            sample.pickupCounter > pickupCounter_;
        // Always consume the observed counter. Unknown attribution cannot be
        // replayed later, and object changes or counter resets stay silent.
        context_ = sample.context;
        metadata_ = sample.metadata;
        pickupCounter_ = sample.pickupCounter;
        const bool knownLocation = IsVrShoulder(sample.location) ||
            sample.location == static_cast<int>(VrSlotLocation::Shoulders);
        if (!increased || !IsVrPhysicalHand(sample.hand) || !knownLocation) return {};
        // Several missed increments still represent only the latest observed
        // pickup. Retain its full ticket; queue serialization owns bit wrapping.
        return {{sample.hand, 5, sample.pickupCounter, false, sample.location}};
    }
};

} // namespace tg
