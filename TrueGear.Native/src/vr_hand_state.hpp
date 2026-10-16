#pragma once

namespace tg {

// A read-only snapshot of CircuitLord's physical hand and confirmed grip state.
// -1 means no hand or an unknown gun slot; 0/1 mean left/right for hand fields.
struct VrHandState {
    int gunHand = -1;
    int grenadeHand = -1;
    bool gunConfirmed = false;
    bool grenadeConfirmed = false;
    bool supportHeld = false;
    int gunSlot = -1;
    int gunTicket = 0;
    int grenadeTicket = 0;
};

constexpr bool IsVrPhysicalHand(int side) noexcept {
    return side == 0 || side == 1;
}

constexpr int ConfirmedWeaponGripMask(const VrHandState& state) noexcept {
    if (!state.gunConfirmed || !IsVrPhysicalHand(state.gunHand)) return 0;
    return state.supportHeld ? 3 : (1 << state.gunHand);
}

constexpr int ConfirmedGrenadeGripMask(const VrHandState& state) noexcept {
    if (!state.grenadeConfirmed || !IsVrPhysicalHand(state.grenadeHand)) return 0;
    return 1 << state.grenadeHand;
}

// Keep the result positive for Squirrel's signed integer representation.
// [1:0] gun side + 1; [3:2] grenade side + 1; [4] support grip;
// [5] confirmed gun; [6] confirmed grenade; [8:7] gun slot code;
// [15] ready; [29:16] gun acknowledgement ticket modulo 16384.
constexpr int PackVrHandState(const VrHandState& state) noexcept {
    if (state.gunHand < -1 || state.gunHand > 1 ||
        state.grenadeHand < -1 || state.grenadeHand > 1 ||
        state.gunTicket < 0 || state.grenadeTicket < 0) return -1;

    int slotCode = 0;
    switch (state.gunSlot) {
    // CircuitLord uses 2 while a world pickup is awaiting its ACK. The hand
    // and pending/confirmed bits remain valid; the eventual holster slot is
    // not known yet. Do not discard the snapshot and reset the observer.
    case -1: case 2: break;
    case 0: slotCode = 1; break;
    case 1: slotCode = 2; break;
    case 3: slotCode = 3; break;
    default: return -1;
    }

    return (state.gunHand + 1) | ((state.grenadeHand + 1) << 2) |
        (state.supportHeld ? (1 << 4) : 0) |
        (state.gunConfirmed ? (1 << 5) : 0) |
        (state.grenadeConfirmed ? (1 << 6) : 0) |
        (slotCode << 7) | (1 << 15) |
        ((state.gunTicket & 0x3fff) << 16);
}

} // namespace tg
