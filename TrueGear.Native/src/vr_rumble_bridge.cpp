#include "vr_rumble_bridge.hpp"
#include "vr_rumble_filter.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string_view>

namespace tg::rumble_bridge_detail {
using Mix = void (*)(float*, const float*, float);
using Contribution = void (*)(const void*, const float*);
using Maximum = float (*)(const float*);
constexpr std::array<std::size_t, 3> CallSites{0x1d17b, 0xd8ee7, 0x1d31b};
constexpr std::array<std::size_t, 3> OriginalTargets{0x1c6d0, 0x1dec0, 0x1c610};
constexpr std::array<std::size_t, 3> Returns{0x1d180, 0xd8eec, 0x1d320};
constexpr std::array<std::int32_t, 3> OriginalDisplacements{-0xab0, -0xbb02c, -0xd10};
struct Anchor { std::size_t offset; const char* hex; };
constexpr Anchor Anchors[]{
    {0x1d15f, "0f1003488d1577032b00488bcbf30f1015f4fe14000f290555032b00e850f5ff"},
    {0xd8ec2, "f3440f11642420f3440f11442424f3440f114c2428f3440f1154242c488d4bd8"},
    {0x1d2f0, "f3440f108424d0000000488d4c2420f3440f1144242cf3440f114c2420f3440f"},
};
struct StateData {
    std::mutex install;
    std::atomic<void*> vr{}, client{};
    std::atomic<bool> ready{false};
    std::array<void*, 3> thunks{};
    std::array<std::int32_t, 3> replacements{};
    Mix originalMix{};
    Contribution originalContribution{};
    Maximum originalMaximum{};
    bool pinned = false;
    std::atomic<std::uint64_t> sequence{0}, revision{0}, epoch{0}, publishedEpoch{0};
    std::atomic<std::uint32_t> publishedAll{0}, publishedNonWeapon{0}, publishedFiltered{0};
    std::atomic<bool> publishedValid{false};
};
StateData& State() { static auto* const state = new StateData; return *state; }
struct FrameData {
    bool active = false, valid = false;
    void* vr{};
    std::uint64_t revision{}, epoch{};
    std::array<float, 4> expected{}, filtered{};
};
thread_local FrameData frame;

#ifdef TG_VR_RUMBLE_BRIDGE_TESTING
Mix testMix{};
Contribution testContribution{};
Maximum testMaximum{};
std::array<std::size_t, 3> testReturns = Returns;
int testFailPatch = -1;
bool testFailThunk = false;
#endif

bool Copy(void* dst, const void* src, std::size_t count) noexcept {
    if (!dst || !src) return false;
    __try { std::memcpy(dst, src, count); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
const void* At(const void* pointer, std::size_t offset) noexcept {
    return reinterpret_cast<const void*>(reinterpret_cast<std::uintptr_t>(pointer) + offset);
}
template<class T> bool Read(const void* pointer, std::size_t offset, T& result) noexcept {
    return pointer && Copy(&result, At(pointer, offset), sizeof(result));
}
std::uint32_t Bits(float value) noexcept { std::uint32_t bits; std::memcpy(&bits, &value, 4); return bits; }
float Float(std::uint32_t bits) noexcept { float value; std::memcpy(&value, &bits, 4); return value; }
bool Amplitude(float value) noexcept { return std::isfinite(value) && value >= 0 && value <= 1; }
bool Vector(const std::array<float, 4>& values) noexcept {
    for (const float value : values) if (!Amplitude(value)) return false;
    return true;
}
float Max(const std::array<float, 4>& values) noexcept {
    return std::min(1.0f, *std::max_element(values.begin(), values.end()));
}
bool MatchesClient(void* vr, void* client) noexcept {
    void* actual{};
    return vr && client && Read(vr, 0x31d6c0, actual) && actual == client;
}
bool Owner(std::size_t index, bool allowOriginal = false) noexcept {
    auto& state = State();
    unsigned char opcode{};
    std::int32_t displacement{};
    const auto* vr = state.vr.load(std::memory_order_acquire);
    return Read(vr, CallSites[index], opcode) && opcode == 0xe8 &&
        Read(vr, CallSites[index] + 1, displacement) &&
        (displacement == state.replacements[index] || (allowOriginal && displacement == OriginalDisplacements[index]));
}
bool Owned() noexcept { return Owner(0) && Owner(1) && Owner(2); }
bool MainThread(void* vr, unsigned expectedActive) noexcept {
    DWORD thread{};
    unsigned char active{};
    return Read(vr, 0x2c7e90, thread) && thread && thread == GetCurrentThreadId() &&
        Read(vr, 0x2cd4f1, active) && active == expectedActive;
}
void InvalidatePublished() noexcept {
    auto& state = State();
    state.epoch.fetch_add(1, std::memory_order_acq_rel);
    state.publishedValid.store(false, std::memory_order_relaxed);
}
bool Caller(std::size_t index, const void* caller) noexcept {
#ifdef TG_VR_RUMBLE_BRIDGE_TESTING
    (void)caller;
    return testReturns[index] == Returns[index];
#else
    return caller == At(State().vr.load(std::memory_order_acquire), Returns[index]);
#endif
}
bool Add(std::array<float, 4>& dst, const std::array<float, 4>& src, float gain) noexcept {
    if (!std::isfinite(gain) || gain < 0) return false;
    for (std::size_t i = 0; i < 4; ++i) {
        if (!std::isfinite(src[i]) || src[i] < 0) return false;
        // Original SSE mixer rounds the product before addition (no FMA).
        volatile float product = gain * src[i];
        const float sum = dst[i] + product;
        if (!std::isfinite(sum)) return false;
        dst[i] = std::clamp(sum, 0.0f, 1.0f);
    }
    return true;
}

void Begin(float* dst, const float* source, float gain) {
    auto& state = State();
    auto* vr = state.vr.load(std::memory_order_acquire);
    frame = {};
    InvalidatePublished();
    if (state.ready.load(std::memory_order_acquire) && Caller(0, _ReturnAddress()) &&
        MatchesClient(vr, state.client.load()) && MainThread(vr, 0) &&
        source == At(vr, 0x2cd4e0) && dst && gain == 1.0f &&
        Copy(frame.expected.data(), At(vr, 0x2cd4d0), sizeof(frame.expected)) && Vector(frame.expected)) {
        frame.active = frame.valid = true;
        frame.vr = vr;
        frame.revision = MappedRumblePolicyRevision();
        frame.epoch = state.epoch.load(std::memory_order_acquire);
        frame.filtered = frame.expected;
    }
    state.originalMix(dst, source, gain);
}
void ObserveContribution(const void* slot, const float* values) {
    auto& state = State();
    if (frame.active && frame.valid) {
        std::array<float, 4> channels{};
        float gain{};
        std::uint8_t fire{};
        std::uintptr_t name{};
        std::array<char, 192> text{};
        const auto client = reinterpret_cast<std::uintptr_t>(state.client.load());
        const auto address = reinterpret_cast<std::uintptr_t>(slot);
        const auto first = client + 0x2a90710;
        const bool slotValid = address >= first && address - first < 128 * 0x68 && (address - first) % 0x68 == 0;
        const auto index = slotValid ? (address - first) / 0x68 : 0;
        if (!Caller(1, _ReturnAddress()) || !MatchesClient(frame.vr, reinterpret_cast<void*>(client)) ||
            !MainThread(frame.vr, 1) || !slotValid ||
            !Copy(channels.data(), values, sizeof(channels)) || !Read(slot, 0x2c, gain) ||
            !Read(frame.vr, 0x2cd450 + index, fire)) frame.valid = false;
        else if (!fire) {
            // CUtlString metadata can describe capacity or include/exclude its
            // terminator. The name pointer and a bounded C-string read suffice.
            std::size_t length = 0;
            bool terminated = false;
            if (Read(slot, 0, name) && name && name <= (std::numeric_limits<std::uintptr_t>::max)() - text.size()) {
                for (; length < text.size(); ++length) {
                    if (!Copy(&text[length], reinterpret_cast<const void*>(name + length), 1)) break;
                    if (text[length] == '\0') { terminated = true; break; }
                }
            }
            if (!terminated || !Add(frame.expected, channels, gain)) frame.valid = false;
            else if (!ExcludeMappedRumble(std::string_view(text.data(), length)) &&
                     !Add(frame.filtered, channels, gain)) frame.valid = false;
        }
    }
    state.originalContribution(slot, values);
}
float End(const float* values) {
    auto& state = State();
    const float original = state.originalMaximum(values);
    std::array<float, 4> actual{};
    if (frame.active && frame.valid && Caller(2, _ReturnAddress()) && MainThread(frame.vr, 0) &&
        state.ready.load(std::memory_order_acquire) && frame.vr == state.vr.load() &&
        MatchesClient(frame.vr, state.client.load()) && frame.revision == MappedRumblePolicyRevision() &&
        frame.epoch == state.epoch.load(std::memory_order_acquire) &&
        Amplitude(original) && Copy(actual.data(), At(frame.vr, 0x2cd4d0), sizeof(actual)) &&
        Vector(actual) && std::memcmp(actual.data(), frame.expected.data(), sizeof(actual)) == 0) {
        const float nonWeapon = original > 0 ? Max(actual) : 0;
        const float filtered = original > 0 ? Max(frame.filtered) : 0;
        // The original all-mix adds its base after slot accumulation, whereas
        // nonweapon starts from that base. Their roundoff can differ by an ULP.
        if (filtered <= nonWeapon) {
            state.sequence.fetch_add(1, std::memory_order_acq_rel);
            state.publishedAll.store(Bits(original), std::memory_order_relaxed);
            state.publishedNonWeapon.store(Bits(nonWeapon), std::memory_order_relaxed);
            state.publishedFiltered.store(Bits(filtered), std::memory_order_relaxed);
            state.revision.store(frame.revision, std::memory_order_relaxed);
            state.publishedEpoch.store(frame.epoch, std::memory_order_relaxed);
            state.publishedValid.store(true, std::memory_order_relaxed);
            state.sequence.fetch_add(1, std::memory_order_release);
        }
    }
    frame = {};
    return original;
}

unsigned Hex(char c) noexcept { return c <= '9' ? c - '0' : c - 'a' + 10; }
bool Identity(void* vr) noexcept {
    for (const auto& anchor : Anchors) {
        std::array<unsigned char, 32> actual{}, expected{};
        // Exclude each nearby call instruction so repeated installation can
        // validate the stable surrounding code after our own displacement CAS.
        const std::size_t count = anchor.offset == 0x1d2f0 ? 27 : 28;
        for (std::size_t i = 0; i < count; ++i)
            expected[i] = static_cast<unsigned char>((Hex(anchor.hex[i * 2]) << 4) | Hex(anchor.hex[i * 2 + 1]));
        if (!Copy(actual.data(), At(vr, anchor.offset), count) || std::memcmp(actual.data(), expected.data(), count)) return false;
    }
    return true;
}
void* NearPage(void* vr) noexcept {
    SYSTEM_INFO info{}; GetSystemInfo(&info);
    const auto granularity = static_cast<std::uintptr_t>(info.dwAllocationGranularity);
    const auto origin = reinterpret_cast<std::uintptr_t>(vr) & ~(granularity - 1);
    for (std::uintptr_t distance = granularity; distance < 0x7ff00000; distance += granularity) {
        for (int direction : {-1, 1}) {
            if (direction < 0 && origin < distance) continue;
            const auto address = direction < 0 ? origin - distance : origin + distance;
            auto* allocation = VirtualAlloc(reinterpret_cast<void*>(address), 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (allocation) return allocation;
        }
    }
    return nullptr;
}
bool MakeThunks(void* vr) noexcept {
    auto& state = State();
    if (state.thunks[0]) return true;
#ifdef TG_VR_RUMBLE_BRIDGE_TESTING
    if (testFailThunk) return false;
#endif
    auto* allocation = static_cast<unsigned char*>(NearPage(vr));
    if (!allocation) return false;
    const std::array<void*, 3> callbacks{reinterpret_cast<void*>(&Begin), reinterpret_cast<void*>(&ObserveContribution), reinterpret_cast<void*>(&End)};
    for (std::size_t i = 0; i < 3; ++i) {
        auto* thunk = allocation + i * 16;
        const unsigned char jump[]{0xff, 0x25, 0, 0, 0, 0};
        std::memcpy(thunk, jump, sizeof(jump));
        std::memcpy(thunk + 6, &callbacks[i], sizeof(void*));
        const auto delta = reinterpret_cast<std::intptr_t>(thunk) -
            reinterpret_cast<std::intptr_t>(At(vr, CallSites[i] + 5));
        if (delta < (std::numeric_limits<std::int32_t>::min)() || delta > (std::numeric_limits<std::int32_t>::max)()) {
            state.thunks = {}; VirtualFree(allocation, 0, MEM_RELEASE); return false;
        }
        state.thunks[i] = thunk;
        state.replacements[i] = static_cast<std::int32_t>(delta);
    }
    DWORD previous{};
    if (!VirtualProtect(allocation, 0x1000, PAGE_EXECUTE_READ, &previous) ||
        !FlushInstructionCache(GetCurrentProcess(), allocation, 0x1000)) {
        state.thunks = {}; VirtualFree(allocation, 0, MEM_RELEASE); return false;
    }
    return true; // Retained permanently once a call site can reference it.
}
bool Patch(std::size_t index) noexcept {
    auto& state = State();
    if (Owner(index)) return true;
#ifdef TG_VR_RUMBLE_BRIDGE_TESTING
    if (testFailPatch == static_cast<int>(index)) return false;
#endif
    auto* address = const_cast<void*>(At(state.vr.load(), CallSites[index] + 1));
    DWORD previous{};
    if (!VirtualProtect(address, sizeof(LONG), PAGE_EXECUTE_READWRITE, &previous)) return false;
    const LONG actual = InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(address),
        state.replacements[index], OriginalDisplacements[index]);
    const bool flushed = FlushInstructionCache(GetCurrentProcess(), address, sizeof(LONG)) != FALSE;
    DWORD ignored{};
    const bool restored = VirtualProtect(address, sizeof(LONG), previous, &ignored) != FALSE;
    return actual == OriginalDisplacements[index] && flushed && restored;
}
} // namespace tg::rumble_bridge_detail

namespace tg {
bool InstallMappedRumbleObservers(void* vr, void* client) {
    using namespace rumble_bridge_detail;
    auto& state = State();
    std::lock_guard lock(state.install);
    if (!MatchesClient(vr, client) || !MainThread(vr, 0) || !Identity(vr)) return false;
    if (state.vr.load() && (state.vr.load() != vr || state.client.load() != client)) return false;
    if (state.ready.load()) return Owned();
    state.vr.store(vr, std::memory_order_release); state.client.store(client, std::memory_order_release);
    if (!state.pinned) {
        HMODULE self{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&Begin), &self)) return false;
        state.pinned = true;
    }
    if (!state.originalMix) {
        state.originalMix = reinterpret_cast<Mix>(const_cast<void*>(At(vr, OriginalTargets[0])));
        state.originalContribution = reinterpret_cast<Contribution>(const_cast<void*>(At(vr, OriginalTargets[1])));
        state.originalMaximum = reinterpret_cast<Maximum>(const_cast<void*>(At(vr, OriginalTargets[2])));
#ifdef TG_VR_RUMBLE_BRIDGE_TESTING
        state.originalMix = testMix; state.originalContribution = testContribution; state.originalMaximum = testMaximum;
#endif
    }
    if (!state.originalMix || !state.originalContribution || !state.originalMaximum) return false;
    if (!MakeThunks(vr)) return false;
    for (std::size_t i = 0; i < 3; ++i) if (!Owner(i, true)) return false;
    for (std::size_t i = 0; i < 3; ++i) if (!Patch(i)) return false;
    state.ready.store(Owned(), std::memory_order_release);
    return state.ready.load();
}
bool ReadMappedRumble(void* vr, float all, float nonWeapon, float& filtered) noexcept {
    using namespace rumble_bridge_detail;
    auto& state = State();
    if (!Amplitude(all) || !Amplitude(nonWeapon) || !state.ready.load(std::memory_order_acquire) ||
        state.vr.load(std::memory_order_acquire) != vr || !MatchesClient(vr, state.client.load()) || !Owned()) return false;
    const auto before = state.sequence.load(std::memory_order_acquire);
    if (before & 1) return false;
    const bool valid = state.publishedValid.load(std::memory_order_relaxed);
    const auto storedAll = state.publishedAll.load(std::memory_order_relaxed);
    const auto storedNonWeapon = state.publishedNonWeapon.load(std::memory_order_relaxed);
    const auto storedFiltered = state.publishedFiltered.load(std::memory_order_relaxed);
    const auto revision = state.revision.load(std::memory_order_relaxed);
    const auto epoch = state.publishedEpoch.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    const auto after = state.sequence.load(std::memory_order_acquire);
    if (before != after || !valid || epoch != state.epoch.load(std::memory_order_acquire) ||
        revision != MappedRumblePolicyRevision() || storedAll != Bits(all) ||
        storedNonWeapon != Bits(nonWeapon)) return false;
    filtered = Float(storedFiltered);
    return true;
}
void ResetMappedRumble() noexcept {
    rumble_bridge_detail::InvalidatePublished();
    rumble_bridge_detail::frame = {};
}
} // namespace tg
