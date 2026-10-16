#pragma once

#include "vr_slot_state.hpp"

#include <optional>
#include <vector>

namespace tg {

// Pure state observer. The caller supplies a fresh request classification and
// independently verified storage evidence; clearing a hand never proves store.
class VrGrenadeSlotObserver {
    struct Acquisition {
        int hand;
        int ticket;
        int sourceKind;
    };
    struct Holder {
        int hand;
        int ticket;
    };

    bool baseline_ = false;
    int observedTicket_ = 0;
    std::optional<Acquisition> pending_;
    std::optional<Holder> holder_;

    static bool Valid(const VrHandState& state, int actionHand, int sourceKind) noexcept {
        // The native release path can leave grenadeConfirmed true after the
        // hand becomes -1. Only a confirmed physical hand establishes holding.
        return state.grenadeHand >= -1 && state.grenadeHand <= 1 &&
            actionHand >= -1 && actionHand <= 1 && state.grenadeTicket >= 0 &&
            sourceKind >= -2 && sourceKind <= 4;
    }

    static std::optional<Acquisition> PendingAcquisition(const VrHandState& state, int actionHand,
                                                       int sourceKind, bool requireActionHand) {
        if (state.grenadeConfirmed || !IsVrPhysicalHand(state.grenadeHand) ||
            state.grenadeTicket <= 0 || (requireActionHand && actionHand != state.grenadeHand))
            return std::nullopt;
        // Preserve the first classification even when it is world/unknown.
        // An ACK or later hand movement cannot turn it into a chest request.
        return Acquisition{state.grenadeHand, state.grenadeTicket, sourceKind};
    }

    void RememberHolder(const VrHandState& state) {
        if (state.grenadeConfirmed && IsVrPhysicalHand(state.grenadeHand))
            holder_ = Holder{state.grenadeHand, state.grenadeTicket};
        else if (holder_ && IsVrPhysicalHand(state.grenadeHand) && state.grenadeHand != holder_->hand)
            holder_.reset();
    }

public:
    void Reset() noexcept {
        baseline_ = false;
        observedTicket_ = 0;
        pending_.reset();
        holder_.reset();
    }

    std::vector<VrSlotEvent> Observe(const VrHandState& state, int grenadeActionHand,
                                   int sourceKind = -2, bool confirmedStore = false) {
        std::vector<VrSlotEvent> events;
        if (!Valid(state, grenadeActionHand, sourceKind)) {
            Reset();
            return events;
        }
        if (!baseline_) {
            baseline_ = true;
            observedTicket_ = state.grenadeTicket;
            pending_ = PendingAcquisition(state, grenadeActionHand, sourceKind, true);
            RememberHolder(state);
            return events;
        }

        if (state.grenadeTicket != observedTicket_) {
            observedTicket_ = state.grenadeTicket;
            // A new positive acquisition ticket survives a cleared transient
            // action hand. Replacements discard the previous storage baseline.
            pending_ = PendingAcquisition(state, grenadeActionHand, sourceKind, false);
            holder_.reset();
        }

        constexpr int chest = static_cast<int>(VrSlotLocation::Chest);
        if (holder_ && state.grenadeHand == -1) {
            if (confirmedStore && grenadeActionHand == holder_->hand && state.grenadeTicket == holder_->ticket)
                events.push_back({holder_->hand, 4, holder_->ticket, true, chest});
            holder_.reset();
        }

        if (pending_ && state.grenadeConfirmed) {
            if (state.grenadeHand == pending_->hand && state.grenadeTicket == pending_->ticket &&
                pending_->sourceKind != -2 && pending_->sourceKind != 1)
                events.push_back({pending_->hand, 4, pending_->ticket, false, chest});
            pending_.reset();
        } else if (pending_ && state.grenadeHand != pending_->hand) {
            pending_.reset();
        }

        RememberHolder(state);
        return events;
    }
};

} // namespace tg
