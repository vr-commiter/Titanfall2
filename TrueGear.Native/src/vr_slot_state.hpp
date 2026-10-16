#pragma once

#include "vr_hand_state.hpp"

#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

namespace tg {

enum class VrSlotEventType { BodyDraw, BodyStore };

enum class VrSlotLocation : int {
    LeftShoulder = 0, RightShoulder = 1, LeftHip = 2, RightHip = 3,
    Chest = 4, Shoulders = 5
};

struct VrSlotEvent {
    int hand = -1;
    int slot = -1;
    int ticket = 0;
    bool stored = false;
    int location = static_cast<int>(VrSlotLocation::Shoulders);
};

constexpr bool IsVrGunBodySlot(int slot) noexcept {
    return slot == 0 || slot == 1 || slot == 3;
}

constexpr bool IsVrShoulder(int location) noexcept {
    return location == static_cast<int>(VrSlotLocation::LeftShoulder) ||
           location == static_cast<int>(VrSlotLocation::RightShoulder);
}

// Native world coordinates use inches. Keep a 1 cm distance-difference margin
// as an explicit classification policy so a nearly central hand stays unknown.
// Positive infinity permits an unrestricted nearest-shoulder draw classification.
inline int ResolveShoulderLocation(const std::array<float, 3>& position,
                                   const std::array<float, 3>& leftAnchor,
                                   const std::array<float, 3>& rightAnchor,
                                   float maxDistance = std::numeric_limits<float>::infinity()) noexcept {
    if (std::isnan(maxDistance) || maxDistance < 0.0f) return -1;
    double leftSquared = 0.0, rightSquared = 0.0;
    for (std::size_t axis = 0; axis < position.size(); ++axis) {
        if (!std::isfinite(position[axis]) || !std::isfinite(leftAnchor[axis]) ||
            !std::isfinite(rightAnchor[axis])) return -1;
        const double left = static_cast<double>(position[axis]) - leftAnchor[axis];
        const double right = static_cast<double>(position[axis]) - rightAnchor[axis];
        leftSquared += left * left;
        rightSquared += right * right;
    }
    const double maximumSquared = static_cast<double>(maxDistance) * maxDistance;
    const double nearestSquared = leftSquared < rightSquared ? leftSquared : rightSquared;
    if (nearestSquared > maximumSquared ||
        std::abs(std::sqrt(leftSquared) - std::sqrt(rightSquared)) <= 0.01 / 0.0254)
        return -1;
    return leftSquared < rightSquared ? static_cast<int>(VrSlotLocation::LeftShoulder) :
                                      static_cast<int>(VrSlotLocation::RightShoulder);
}

// Feed raw, validated native snapshots once per frame. The native gun slot is
// the request source before its ACK: 2 is a world pickup, whereas 0/1/3 are
// body holsters. An ACK can subsequently replace 2 with the equipped gun slot.
// gunActionHand is the per-update event hand at RVA 0x317cc8; it is reset to -1
// by the native grip helper and must not be reconstructed from a held hand.
class VrSlotObserver {
    struct Acquisition {
        int side;
        int sourceSlot;
        int ticket;
        int location;
    };
    struct Holder {
        int side;
        int slot;
        int ticket;
        int location;
    };

    bool baseline_ = false;
    int observedTicket_ = 0;
    std::optional<Acquisition> pending_;
    std::optional<Holder> holder_;

    static bool Valid(const VrHandState& state, int actionHand) noexcept {
        if (state.gunHand < -1 || state.gunHand > 1 ||
            actionHand < -1 || actionHand > 1 || state.gunTicket < 0 ||
            (state.gunSlot != -1 && state.gunSlot != 2 && !IsVrGunBodySlot(state.gunSlot)))
            return false;
        if (state.gunConfirmed && !IsVrPhysicalHand(state.gunHand)) return false;
        // A held/pending gun with an unknown slot cannot identify a body action.
        return !IsVrPhysicalHand(state.gunHand) || state.gunSlot != -1;
    }

    static int BodyLocation(int slot, int shoulder = -1) noexcept {
        if (slot == 1) return static_cast<int>(VrSlotLocation::LeftHip);
        if (slot == 3) return static_cast<int>(VrSlotLocation::RightHip);
        return IsVrShoulder(shoulder) ? shoulder : static_cast<int>(VrSlotLocation::Shoulders);
    }

    static std::optional<Acquisition> PendingAcquisition(const VrHandState& state, int actionHand,
                                                       int takeShoulder, bool requireActionHand = true) {
        if (state.gunConfirmed || !IsVrPhysicalHand(state.gunHand) ||
            (requireActionHand && actionHand != state.gunHand) || state.gunTicket <= 0)
            return std::nullopt;
        return Acquisition{state.gunHand, state.gunSlot, state.gunTicket, BodyLocation(state.gunSlot, takeShoulder)};
    }

    void RememberHolder(const VrHandState& state) {
        if (state.gunConfirmed && IsVrPhysicalHand(state.gunHand)) {
            const int location = holder_ && holder_->ticket == state.gunTicket && holder_->slot == state.gunSlot ?
                holder_->location : BodyLocation(state.gunSlot);
            holder_ = Holder{state.gunHand, state.gunSlot, state.gunTicket, location};
        }
    }

public:
    // Call on unavailable samples, lifecycle changes, or an unsupported layout.
    // The next valid held snapshot becomes a silent baseline.
    void Reset() noexcept {
        baseline_ = false;
        observedTicket_ = 0;
        pending_.reset();
        holder_.reset();
    }

    std::vector<VrSlotEvent> Observe(const VrHandState& state, int gunActionHand,
                                   int takeShoulder = -1, int releaseShoulder = -1) {
        std::vector<VrSlotEvent> events;
        if (!Valid(state, gunActionHand)) {
            Reset();
            return events;
        }
        if (!baseline_) {
            baseline_ = true;
            observedTicket_ = state.gunTicket;
            pending_ = PendingAcquisition(state, gunActionHand, takeShoulder);
            RememberHolder(state);
            return events;
        }

        if (state.gunTicket != observedTicket_) {
            observedTicket_ = state.gunTicket;
            // A genuinely new ticket is written only for acquisition. Its
            // physical hand and pre-ACK slot survive longer than eventHand.
            // Initial baselines retain the stricter event-hand requirement.
            pending_ = PendingAcquisition(state, gunActionHand, takeShoulder, false);
            // A rejected replacement must not be reported as storing the old gun.
            holder_.reset();
        }

        if (holder_ && state.gunHand == -1) {
            if (gunActionHand == holder_->side && state.gunTicket == holder_->ticket &&
                IsVrGunBodySlot(holder_->slot)) {
                // A touched release shoulder overrides the original draw site.
                // Otherwise auto-holster reuses the holder's last draw location;
                // an unobserved/world primary acquisition remains shared.
                const int location = holder_->slot == 0 && IsVrShoulder(releaseShoulder) ?
                    releaseShoulder : holder_->location;
                events.push_back({holder_->side, holder_->slot, holder_->ticket, true, location});
            }
            holder_.reset();
        }

        if (pending_ && state.gunConfirmed) {
            if (state.gunTicket == pending_->ticket && state.gunHand == pending_->side &&
                IsVrGunBodySlot(pending_->sourceSlot) && state.gunSlot == pending_->sourceSlot) {
                events.push_back({pending_->side, pending_->sourceSlot, pending_->ticket, false, pending_->location});
                holder_ = Holder{pending_->side, pending_->sourceSlot, pending_->ticket, pending_->location};
            }
            pending_.reset();
        } else if (pending_ && state.gunHand == -1) {
            pending_.reset();
        }

        // Confirmation and support-hand changes do not themselves identify a
        // slot action. A transfer merely updates the physical primary holder.
        RememberHolder(state);
        return events;
    }
};

} // namespace tg
