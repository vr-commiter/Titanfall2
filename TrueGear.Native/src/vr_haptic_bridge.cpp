#include "vr_haptic_bridge.hpp"
#include "vr_haptic_state.hpp"
#include "vr_haptic_source.hpp"
#include "vr_rumble_bridge.hpp"
#include "vr_custom_haptic_bridge.hpp"

#include <Windows.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <intrin.h>

namespace tg::haptic_bridge_detail {
struct ActionInfo {
    std::int32_t type;
    std::uint32_t padding;
    const void* next;
    std::uint64_t action;
    std::uint64_t path;
};
struct Vibration {
    std::int32_t type;
    std::uint32_t padding;
    const void* next;
    std::int64_t duration;
    float frequency;
    float amplitude;
};
static_assert(sizeof(void*) == 8 && sizeof(ActionInfo) == 32 && sizeof(Vibration) == 32);
static_assert(offsetof(ActionInfo, action) == 16 && offsetof(ActionInfo, path) == 24);
static_assert(offsetof(Vibration, duration) == 16 && offsetof(Vibration, amplitude) == 28);
using Apply = std::int32_t (WINAPI*)(std::uint64_t, const ActionInfo*, const Vibration*);
using Stop = std::int32_t (WINAPI*)(std::uint64_t, const ActionInfo*);

struct Site { std::size_t offset; const char* bytes; };
constexpr Site Sites[] = {
    {0xd9390, "405356574881ecb0000000488b05de3d0b004833c448898424a8000000498bf0"},
    {0xda3f0, "48895c2418574881ecb0000000488b057c2d0b004833c448898424a800000048"},
    {0xebf3e, "e8ed020000488b104889134885d20f85a50200000f57c0f30f7f45c74533f64c"},
    {0xec257, "488d05020b1e004883c428c3488d0dfe0a1e00e87d570200833df20a1e00ff75"},
    {0xfcf0a, "4c8d83a8010000488bcf488d155df20200ffd64c8d83b0010000488bcf488d15"},
    {0xa9d3b, "4c8d442420488d1581cc0b00488b8930020000e81d0703008bd885c00f88840e"},
    {0xa9d8b, "488b44242048890348894b084c8d4424404c8d2de5cb0b00498bd5498b8f3002"},
    {0xcb1c, "488b5710c745d83b0000000f57c00f1145e0488b4258488945e8418bc6488b8c"},
    {0xcb68, "c745f80d00000033c00f11450048894510f30f1175144c8965088945104c8d45"},
    {0xec2d0, "4883ec28e857ffffff488b0848c700000000004885c9740f488b01ba01000000"},
    {0xec61a, "e811fcffff498b164533ff4d893e488b084889104885c9740b488b01ba010000"},
    {0xe9f91, "b9f8010000e8c5720200488bd84885c0741233d241b8f8010000488bc8e87387"},
    // Original weapon-tag exclusion, published split amplitudes and final XR
    // source selection must agree before reading their fixed data offsets.
    {0x1ca5e, "488b055b0c3000488bcb4805a0895500c6057b0a2b0001ffd0c605720a2b0000"},
    {0x1cbc5, "0fb60d24092b00488d057d082b00880c03488bc3488b5c24304883c4205fc3cc"},
    {0x1d15f, "0f1003488d1577032b00488bcbf30f1015f4fe14000f290555032b00e850f5ff"},
    {0x1dfc4, "f30f10752c488d3580f42a000fb634374084f675120f28d6488d0dedf42a0048"},
    {0x1d3a8, "0f2ffe760e488d0d1c012b00e857f2ffffeb030f57c0488b0d9b812a00660f7e"},
    {0xc915, "8b87c0070000894424608b87c407000089442470448b8fcc07000044894c245c"},
    {0xca10, "4585f6751b0fb687d407000084c0741041b7018b87c807000066440f6ec0eb07"},
    {0xca37, "f30f108cf7d80700000f2fcf760ef30f1084f7dc0700000f2fc7770a4584ff75"},
    {0xca7b, "418b957821000083fa037510450f2fad90210000762f410f28f6eb2c83fa0275"},
    {0xcaed, "450fa3f17306410f28f2eb04410f28f10f2ff7771aeb030f28f7418bc6f30f10"},
    {0x207b2, "488d1de7502a0048895c2438488d05e74d2a0048894424308b05d04d2a008944"},
    {0x24a28, "c78778210000050000006689879d21000088879f2100004889b7a021000089b7"},
    {0xd93ca, "1b488b4424384c8b4848498b81a80100004c8bc6488bd7488bcbffd0eb05b8fe"},
};

struct Generation { void* module{}; void* instance{}; void* table{}; };
struct BridgeState {
    std::mutex installMutex;
    std::mutex observerMutex;
    std::atomic<void*> module{};
    std::atomic<Apply> originalApply{};
    std::atomic<Stop> originalStop{};
    std::atomic<std::uint64_t> epoch{};
    VrHapticObserver observer;
    Generation activeGeneration;
    bool capturing = false;
    float lastAmplitude = 0;
    std::uint64_t lastPopEpoch = 0;
    Generation lastPopGeneration;
    bool pinned = false;
};

// XR cleanup can call our pinned callbacks during other DLLs' CRT teardown.
// Keep synchronization and original forwarding addresses alive until exit.
BridgeState& State() { static auto* const state = new BridgeState; return *state; }

bool Copy(void* out, const void* in, std::size_t size) noexcept {
    if (!out || !in) return false;
    __try { std::memcpy(out, in, size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
const void* At(const void* p, std::size_t offset) noexcept {
    return reinterpret_cast<const void*>(reinterpret_cast<std::uintptr_t>(p) + offset);
}
template<class T> bool Read(const void* p, std::size_t offset, T& out) noexcept {
    return p && Copy(&out, At(p, offset), sizeof(out));
}
bool Executable(const void* p) noexcept {
    MEMORY_BASIC_INFORMATION info{};
    if (!p || !VirtualQuery(p, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const DWORD access = info.Protect & 0xff;
    return access == PAGE_EXECUTE || access == PAGE_EXECUTE_READ ||
           access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY;
}
bool WritableSlot(const void* p) noexcept {
    MEMORY_BASIC_INFORMATION info{};
    const auto address = reinterpret_cast<std::uintptr_t>(p);
    if (!p || address % sizeof(void*) || !VirtualQuery(p, &info, sizeof(info)) ||
        info.State != MEM_COMMIT || info.Type != MEM_PRIVATE ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const DWORD access = info.Protect & 0xff;
    const auto end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    return (access == PAGE_READWRITE || access == PAGE_WRITECOPY ||
            access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY) &&
           address <= end && end - address >= sizeof(void*);
}
unsigned Hex(char c) noexcept { return c <= '9' ? c - '0' : c - 'a' + 10; }
bool Signatures(void* module) noexcept {
    for (const auto& site : Sites) {
        std::array<unsigned char, 32> actual{}, expected{};
        // The mapped-source bridge owns the following E8 displacement. Check
        // its stable prefix here; that bridge checks original/owned call bytes.
        const std::size_t count = site.offset == 0x1d15f ? 28 : expected.size();
        for (std::size_t i = 0; i < count; ++i)
            expected[i] = static_cast<unsigned char>((Hex(site.bytes[i * 2]) << 4) | Hex(site.bytes[i * 2 + 1]));
        const auto* address = At(module, site.offset);
        if (!Executable(address) || !Copy(actual.data(), address, count) || std::memcmp(actual.data(), expected.data(), count)) return false;
    }
    return true;
}
bool Current(void* module, Generation& generation) noexcept {
    generation.module = module;
    return Read(module, 0x2ccd60, generation.instance) && generation.instance &&
           Read(generation.instance, 0x48, generation.table) && generation.table;
}
bool StillCurrent(const Generation& generation) noexcept {
    Generation now;
    return Current(generation.module, now) && now.instance == generation.instance && now.table == generation.table;
}
// No application lock makes arbitrary concurrent XR destruction safe. Check
// the normal VR lifecycle immediately before CAS and never write a known old table.
bool SwapCurrent(const Generation& generation, std::size_t offset, void* expected, void* replacement) noexcept {
    __try {
        auto* base = static_cast<unsigned char*>(generation.module);
        const auto instance = *reinterpret_cast<void* volatile*>(base + 0x2ccd60);
        if (instance != generation.instance ||
            *reinterpret_cast<void* volatile*>(static_cast<unsigned char*>(instance) + 0x48) != generation.table) return false;
        auto* slot = reinterpret_cast<void* volatile*>(static_cast<unsigned char*>(generation.table) + offset);
        return InterlockedCompareExchangePointer(slot, replacement, expected) == expected;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

#ifdef TG_VR_HAPTIC_BRIDGE_TESTING
std::atomic<std::uint64_t> testNow{1000};
std::size_t testApplyCallerRva = 0xd93e6;
bool testMappedFilterRequired = false;
bool testCustomFilterRequired = false;
void (*testAfterApplyInstall)() = nullptr;
void (*testBeforePublish)() = nullptr;
#endif
std::uint64_t Now() noexcept {
#ifdef TG_VR_HAPTIC_BRIDGE_TESTING
    return testNow.load();
#else
    return GetTickCount64();
#endif
}
std::int32_t WINAPI BridgeApply(std::uint64_t, const ActionInfo*, const Vibration*);
std::int32_t WINAPI BridgeStop(std::uint64_t, const ActionInfo*);
bool Same(const Generation& a, const Generation& b) noexcept {
    return a.module == b.module && a.instance == b.instance && a.table == b.table;
}
bool Owned(const Generation& generation) noexcept {
    void *apply{}, *stop{};
    return StillCurrent(generation) && Read(generation.table, 0x1a8, apply) && Read(generation.table, 0x1b0, stop) &&
        apply == reinterpret_cast<void*>(&BridgeApply) && stop == reinterpret_cast<void*>(&BridgeStop) && StillCurrent(generation);
}
void Capture(const Generation* generation) {
    auto& state = State();
    std::lock_guard lock(state.observerMutex);
    if ((!generation && !state.capturing) ||
        (generation && state.capturing && Same(state.activeGeneration, *generation))) return;
    ++state.epoch;
    state.observer.Reset();
    state.lastAmplitude = 0;
    state.lastPopEpoch = 0;
    state.lastPopGeneration = {};
    state.capturing = generation != nullptr;
    state.activeGeneration = generation ? *generation : Generation{};
}
bool Refuse() {
    Capture(nullptr);
    return false;
}
int Side(std::uint64_t session, const ActionInfo* info, Generation& generation, void** observedInput = nullptr) noexcept {
    ActionInfo action{};
    void *input{}, *context{};
    std::uint64_t expectedSession{}, expectedAction{}, left{}, right{};
    auto* module = State().module.load(std::memory_order_acquire);
    if (!module || !Copy(&action, info, sizeof(action)) || action.type != 59 || action.next ||
        !Current(module, generation) || !Read(module, 0x2c5560, input) || !input ||
        !Read(input, 0x10, context) || !context ||
        !Read(context, 0x240, expectedSession) || !expectedSession || expectedSession != session ||
        !Read(context, 0x58, expectedAction) || !expectedAction || expectedAction != action.action ||
        !Read(context, 0x200, left) || !Read(context, 0x208, right) || !left || !right || left == right) return -1;
    const int side = action.path == left ? 0 : action.path == right ? 1 : -1;
    if (side >= 0 && observedInput) *observedInput = input;
    return side;
}

bool ReadSources(const Generation& generation, void* input, int side, VrHapticSourceSnapshot& source) noexcept {
    struct Fields {
        float all, nonWeapon, auxiliaryLeft;
        std::uint32_t gunMask, pending;
        std::uint8_t auxiliaryLeftActive, padding[3];
        float leftRemaining, leftAmplitude, rightRemaining, rightAmplitude;
    };
    static_assert(sizeof(Fields) == 0x28 && offsetof(Fields, leftRemaining) == 0x18);
    Fields first{}, second{};
    int firstMode{}, secondMode{};
    void *before{}, *after{};
    if (!StillCurrent(generation) || !Read(generation.module, 0x2c5560, before) || before != input ||
        !Read(generation.module, 0x2c7a18, firstMode) || !Read(input, 0x7c0, first) ||
        !Read(input, 0x7c0, second) || !Read(generation.module, 0x2c7a18, secondMode) ||
        !Read(generation.module, 0x2c5560, after) || after != input || !StillCurrent(generation) ||
        firstMode != secondMode || std::memcmp(&first, &second, sizeof(first)) != 0) return false;
    source.mode = firstMode;
    source.gunHandMask = first.gunMask;
    source.nativeAll = first.all;
    source.nativeNonWeapon = first.nonWeapon;
    source.customRemaining = side == 0 ? first.leftRemaining : first.rightRemaining;
    source.customAmplitude = side == 0 ? first.leftAmplitude : first.rightAmplitude;
    source.auxiliaryLeftAmplitude = first.auxiliaryLeft;
    source.auxiliaryLeftActive = first.auxiliaryLeftActive != 0;
#ifdef TG_VR_HAPTIC_BRIDGE_TESTING
    source.mappedFilterRequired = testMappedFilterRequired;
    source.customFilterRequired = testCustomFilterRequired;
#else
    source.mappedFilterRequired = true;
    source.customFilterRequired = true;
#endif
    if (source.mappedFilterRequired)
        source.mappedFilterValid = ReadMappedRumble(generation.module, first.all, first.nonWeapon, source.nativeUnmapped);
    if (source.customFilterRequired)
        source.customFilterValid = ReadFilteredCustomHaptic(generation.module, input, side,
            source.customRemaining, source.customAmplitude, source.unmappedCustomRemaining, source.unmappedCustomAmplitude);
    return true;
}

std::int32_t WINAPI BridgeApply(std::uint64_t session, const ActionInfo* info, const Vibration* vibration) {
    auto& state = State();
    const auto original = state.originalApply.load(std::memory_order_acquire);
    if (!original) return -2; // Never installed without a valid original.
    const auto epoch = state.epoch.load(std::memory_order_acquire);
    Generation generation;
    Vibration sample{};
    void* input{};
    const int side = Side(session, info, generation, &input);
    const bool observed = side >= 0 && Copy(&sample, vibration, sizeof(sample)) && sample.type == 13 && !sample.next;
    VrHapticSelection selection;
    VrHapticSourceSnapshot source;
#ifdef TG_VR_HAPTIC_BRIDGE_TESTING
    const bool knownCaller = testApplyCallerRva == 0xd93e6;
#else
    const bool knownCaller = observed && _ReturnAddress() == At(generation.module, 0xd93e6);
#endif
    // Snapshot and classify before calling into the XR runtime. Only the
    // additional TrueGear copy loses weapon rumble; original arguments below
    // remain identical. Unknown/racing sources cannot create a duplicate shot.
    if (observed && knownCaller && ReadSources(generation, input, side, source))
        selection = SelectNonWeaponHaptic(side, sample.amplitude, source);
    const auto result = original(session, info, vibration);
    if (selection.valid && result == 0) {
        try {
            std::lock_guard lock(state.observerMutex);
            if (state.capturing && Same(state.activeGeneration, generation) && state.epoch.load() == epoch && Owned(generation))
                state.observer.Observe(side, selection.amplitude, sample.duration, sample.frequency, result, Now());
        } catch (...) { /* Observation must never change original output. */ }
    }
    return result;
}
std::int32_t WINAPI BridgeStop(std::uint64_t session, const ActionInfo* info) {
    auto& state = State();
    const auto original = state.originalStop.load(std::memory_order_acquire);
    if (!original) return -2;
    const auto epoch = state.epoch.load(std::memory_order_acquire);
    Generation generation;
    const int side = Side(session, info, generation);
    const auto result = original(session, info);
    if (side >= 0 && result == 0) {
        try {
            std::lock_guard lock(state.observerMutex);
            if (state.capturing && Same(state.activeGeneration, generation) && state.epoch.load() == epoch && Owned(generation))
                state.observer.Stop(side, result);
        } catch (...) { }
    }
    return result;
}
} // namespace tg::haptic_bridge_detail

namespace tg {
bool InstallVrHapticBridge(void* validatedVrModule) {
    using namespace haptic_bridge_detail;
    try {
        auto& state = State();
        std::lock_guard lock(state.installMutex);
        if (!validatedVrModule || !Signatures(validatedVrModule)) return Refuse();
        if (state.module.load() && state.module.load() != validatedVrModule) return Refuse();
        Generation generation;
        if (!Current(validatedVrModule, generation)) return Refuse();
        auto* applySlot = const_cast<void*>(At(generation.table, 0x1a8));
        auto* stopSlot = const_cast<void*>(At(generation.table, 0x1b0));
        void *apply{}, *stop{};
        if (!WritableSlot(applySlot) || !WritableSlot(stopSlot) || !Copy(&apply, applySlot, sizeof(apply)) ||
            !Copy(&stop, stopSlot, sizeof(stop)) || !StillCurrent(generation)) return Refuse();
        const auto oursApply = reinterpret_cast<void*>(&BridgeApply);
        const auto oursStop = reinterpret_cast<void*>(&BridgeStop);
        if (apply == oursApply && stop == oursStop && state.originalApply.load() && state.originalStop.load()) {
            Capture(&generation);
            return true;
        }
        if (apply == oursApply || apply == oursStop || stop == oursApply || stop == oursStop ||
            !Executable(apply) || !Executable(stop)) {
            return Refuse();
        }
        const auto savedApply = state.originalApply.load();
        const auto savedStop = state.originalStop.load();
        if ((savedApply && apply != reinterpret_cast<void*>(savedApply)) ||
            (savedStop && stop != reinterpret_cast<void*>(savedStop))) {
            return Refuse();
        }
        if (!state.pinned) {
            HMODULE self{};
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&BridgeApply), &self)) return Refuse();
            state.pinned = true;
        }
        // Immutable originals also serve any previously entered callback after a reset/recreation.
        state.originalApply.store(reinterpret_cast<Apply>(apply), std::memory_order_release);
        state.originalStop.store(reinterpret_cast<Stop>(stop), std::memory_order_release);
        state.module.store(validatedVrModule, std::memory_order_release);
        Capture(nullptr);
        if (!SwapCurrent(generation, 0x1a8, apply, oursApply)) return Refuse();
#ifdef TG_VR_HAPTIC_BRIDGE_TESTING
        if (testAfterApplyInstall) testAfterApplyInstall();
#endif
        if (!SwapCurrent(generation, 0x1b0, stop, oursStop)) {
            SwapCurrent(generation, 0x1a8, oursApply, apply);
            return Refuse();
        }
        if (!Owned(generation)) return Refuse();
        Capture(&generation);
        return true;
    } catch (...) { return false; }
}
int PopVrHapticEvent() {
    using namespace haptic_bridge_detail;
    VrHapticPulse pulse;
    std::uint64_t epoch{};
    try {
        auto& state = State();
        {
            std::lock_guard lock(state.observerMutex);
            state.lastAmplitude = 0;
            state.lastPopEpoch = 0;
            state.lastPopGeneration = {};
            if (!state.capturing || !Owned(state.activeGeneration)) {
                ++state.epoch;
                state.capturing = false;
                state.observer.Reset();
                return -1;
            }
            if (!state.observer.Pop(Now(), pulse)) return -1;
            epoch = state.epoch.load();
        }
#ifdef TG_VR_HAPTIC_BRIDGE_TESTING
        if (testBeforePublish) testBeforePublish();
#endif
        {
            std::lock_guard lock(state.observerMutex);
            if (!state.capturing || state.epoch.load() != epoch || !Owned(state.activeGeneration)) return -1;
            state.lastAmplitude = pulse.amplitude;
            state.lastPopEpoch = epoch;
            state.lastPopGeneration = state.activeGeneration;
        }
        return pulse.side;
    } catch (...) { return -1; }
}
float LastVrHapticAmplitude() {
    using namespace haptic_bridge_detail;
    try {
        auto& state = State();
        std::lock_guard lock(state.observerMutex);
        if (!state.capturing || state.lastPopEpoch != state.epoch.load() ||
            !Same(state.lastPopGeneration, state.activeGeneration) || !Owned(state.activeGeneration)) {
            state.lastAmplitude = 0;
            state.lastPopEpoch = 0;
            state.lastPopGeneration = {};
            return 0;
        }
        return state.lastAmplitude;
    } catch (...) { return 0; }
}
void ResetVrHapticEvents() {
    ResetMappedRumble();
    ResetCustomHapticBridge();
    auto& state = haptic_bridge_detail::State();
    try {
        std::lock_guard lock(state.observerMutex);
        ++state.epoch;
        state.observer.Reset();
        state.lastAmplitude = 0;
        state.lastPopEpoch = 0;
        state.lastPopGeneration = {};
        state.capturing = false;
        state.activeGeneration = {};
    } catch (...) { }
}
} // namespace tg
