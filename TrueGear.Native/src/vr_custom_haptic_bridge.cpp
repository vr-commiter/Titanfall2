#include "vr_custom_haptic_bridge.hpp"
#include "vr_rumble_filter.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

namespace tg::custom_bridge_detail {
using AmmoFunction = void (*)(void*, std::int32_t, std::int32_t, const char*, const char*, std::uint32_t, std::uint32_t);
using ScriptFunction = std::int32_t (*)(void*);
struct PatchSite {
    std::size_t base, entry, resume;
    unsigned length;
    std::uint64_t original;
};
// Each published entry fits inside ONE aligned CAS64 word. The setter's last
// original byte is bypassed; the consumer's following MOV bytes stay intact.
constexpr std::array<PatchSite, 6> Sites{{
    {0x39e98, 0x39e99, 0x39ea1, 7, 0x0007d0890944f0c1ull},
    {0x40920, 0x40921, 0x40928, 7, 0x000007d08109f006ull},
    {0x411e8, 0x411e9, 0x411f0, 7, 0x000007d08109f005ull},
    {0x5dab8, 0x5dab8, 0x5dac0, 8, 0x000007d0900941f0ull},
    // Replace the single-entry SERE block's input MOV, rebuild its side/kind
    // mask in the observer, and resume at the complete epilogue instruction.
    // 9b8bc itself crosses an atomic-word boundary and is never patched.
    {0x9b8a0, 0x9b8a1, 0x9b8c3, 7, 0x00229cb80d8b4829ull},
    {0xc938, 0xc938, 0xc93e, 6, 0x5489000007d09787ull},
}};
constexpr std::size_t AmmoCall = 0x5800f;
constexpr std::int32_t AmmoOriginalDisplacement = 0x5a5c;
struct Anchor { std::size_t rva; const char* hex; };
constexpr Anchor Anchors[]{
    {0x39e80, "4533c9f6c2017404450fabc1f6c2027408418d4004410fabc1"},
    {0x40905, "488b0d544c2800418bc7f6c20174040fbae802f6c20274040fbae806"},
    {0x411cd, "488b0d8c432800418bc7f6c20174040fbae801f6c20274040fbae805"},
    {0x5da70, "443bc20f84e701000048896c241841564883ec20"},
    {0x5da84, "4c8b05d57a260033d2f644245801498be948895c24304c8bf148897424388b74246074030fabf2f64424580274068d46040fabc2"},
    {0x9b838, "8bd88d50ff83fa02765f"},
    {0x9b8a8, "33c0f6c30174040fbae802f6c30274040fbae806f00981d0070000"},
    {0x9b8c3, "488b5c243033c04883c4205fc3"},
    {0xc929, "448b8fcc07000044894c245c418bd7"},
    {0xc940, "2468488b8760080000"},
    {0x57fef, "4c8d0d82e80e00448b865c010000660f6fc7660f73d808660f7ec2488d4c2460"},
};
struct SavedRegisters {
    std::uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    std::uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax, flags;
};
static_assert(sizeof(SavedRegisters) == 128);
struct StateData {
    std::mutex install;
    std::atomic<void*> vr{}, client{};
    std::atomic<bool> ready{false};
    std::atomic<std::uint64_t> epoch{1};
    std::atomic<std::uint32_t> activationPolicyMask{0};
    std::array<void*, 6> thunks{};
    std::array<std::uint64_t, 6> replacement{};
    void* ammoThunk{};
    std::int32_t ammoDisplacement{};
    AmmoFunction originalAmmo{};
    ScriptFunction originalScript{};
    bool pinned = false;
    // Only the original XR consumer thread reads or mutates these fields.
    CustomPulseTracker tracker;
    void* input{};
    std::uint64_t consumedEpoch{};
    std::uint32_t consumedPolicyMask{};
    DWORD consumerThread{};
};
StateData& State() { static auto* const state = new StateData; return *state; }
thread_local CustomHapticSource source = CustomHapticSource::Unknown;
struct SourceScope {
    CustomHapticSource prior;
    explicit SourceScope(CustomHapticSource next) noexcept : prior(source) { source = next; }
    ~SourceScope() { source = prior; }
};

#ifdef TG_VR_CUSTOM_HAPTIC_BRIDGE_TESTING
AmmoFunction testAmmo{};
ScriptFunction testScript{};
std::array<void*, 6> testResumes{};
int testFailPatch = -1;
bool testFailThunk = false;
#endif

const void* At(const void* p, std::size_t offset) noexcept {
    return reinterpret_cast<const void*>(reinterpret_cast<std::uintptr_t>(p) + offset);
}
bool Copy(void* dst, const void* src, std::size_t count) noexcept {
    if (!dst || !src) return false;
    __try { std::memcpy(dst, src, count); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template<class T> bool Read(const void* p, std::size_t offset, T& result) noexcept {
    return p && Copy(&result, At(p, offset), sizeof(result));
}
bool MatchesClient(void* vr, void* client) noexcept {
    void* actual{};
    return vr && client && Read(vr, 0x31d6c0, actual) && actual == client;
}
bool Thread(void* vr, std::size_t offset) noexcept {
    DWORD id{};
    return Read(vr, offset, id) && id && id == GetCurrentThreadId();
}
bool Owner(std::size_t i, bool original = false) noexcept {
    auto& s = State();
    std::uint64_t actual{};
    return Read(s.vr.load(std::memory_order_acquire), Sites[i].base, actual) &&
        (actual == s.replacement[i] || (original && actual == Sites[i].original));
}
bool AmmoOwner(bool original = false) noexcept {
    auto& s = State();
    std::uint8_t opcode{};
    std::int32_t displacement{};
    auto* vr = s.vr.load(std::memory_order_acquire);
    return Read(vr, AmmoCall, opcode) && opcode == 0xe8 && Read(vr, AmmoCall + 1, displacement) &&
        (displacement == s.ammoDisplacement || (original && displacement == AmmoOriginalDisplacement));
}
bool Owned() noexcept {
    for (std::size_t i = 0; i < Sites.size(); ++i) if (!Owner(i)) return false;
    return AmmoOwner();
}
bool Enabled() noexcept {
    auto& s = State();
    return s.ready.load(std::memory_order_acquire) &&
        MatchesClient(s.vr.load(std::memory_order_acquire), s.client.load(std::memory_order_acquire)) && Owned();
}
unsigned Hex(char c) noexcept { return c <= '9' ? static_cast<unsigned>(c - '0') : static_cast<unsigned>(c - 'a' + 10); }
bool Identity(void* vr) noexcept {
    for (const auto& anchor : Anchors) {
        const std::size_t count = std::strlen(anchor.hex) / 2;
        std::array<unsigned char, 96> actual{};
        if (count > actual.size() || !Copy(actual.data(), At(vr, anchor.rva), count)) return false;
        for (std::size_t i = 0; i < count; ++i)
            if (actual[i] != ((Hex(anchor.hex[i * 2]) << 4) | Hex(anchor.hex[i * 2 + 1]))) return false;
    }
    return true;
}
void LogicalFlags(SavedRegisters& regs, std::uint32_t result) noexcept {
    // Reproduce OR's defined flags for the ORIGINAL low-byte pending word.
    // AF is undefined for OR; retain its prior value rather than inventing it.
    constexpr std::uint64_t mask = 0x800 | 0x80 | 0x40 | 0x04 | 0x01;
    regs.flags &= ~mask;
    if (result == 0) regs.flags |= 0x40;
    if (result & 0x80000000u) regs.flags |= 0x80;
    unsigned parity = result & 0xffu;
    parity ^= parity >> 4; parity ^= parity >> 2; parity ^= parity >> 1;
    if ((parity & 1u) == 0) regs.flags |= 0x04;
}
void Produce(SavedRegisters& regs, void* input, std::uint32_t mask, CustomHapticSource kind) noexcept {
    const bool excluded = Enabled() && ExcludeCustomHapticSource(kind);
    const auto packed = EncodeCustomPending(mask, excluded);
    auto* pending = reinterpret_cast<volatile LONG*>(const_cast<void*>(At(input, 0x7d0)));
    const auto previous = static_cast<std::uint32_t>(InterlockedOr(pending, static_cast<LONG>(packed)));
    LogicalFlags(regs, (previous & 0xffu) | (mask & 0xffu));
}
void Observe(SavedRegisters* regs, std::uint32_t index, const void* xmm) noexcept {
    if (!regs || index >= Sites.size()) return;
    auto& s = State();
    if (index == 0) {
        Produce(*regs, reinterpret_cast<void*>(regs->rcx), static_cast<std::uint32_t>(regs->r9), CustomHapticSource::Unknown);
    } else if (index == 1 || index == 2) {
        Produce(*regs, reinterpret_cast<void*>(regs->rcx), static_cast<std::uint32_t>(regs->rax), CustomHapticSource::Acquisition);
    } else if (index == 3) {
        const auto kind = source == CustomHapticSource::AmmoPickup ? source : CustomHapticSource::Unknown;
        Produce(*regs, reinterpret_cast<void*>(regs->r8), static_cast<std::uint32_t>(regs->rdx), kind);
    } else if (index == 4) {
        void* input{};
        // Equivalent to the single-entry original MOV / XOR / TEST / BTS block.
        if (!Read(s.vr.load(std::memory_order_acquire), 0x2c5560, input)) return;
        const auto side = static_cast<std::uint32_t>(regs->rbx) & 3u;
        const std::uint32_t mask = ((side & 1u) ? 4u : 0u) | ((side & 2u) ? 64u : 0u);
        regs->rcx = reinterpret_cast<std::uintptr_t>(input);
        regs->rax = mask;
        const auto kind = source == CustomHapticSource::ItemGrab || source == CustomHapticSource::ItemRelease ?
            source : CustomHapticSource::Unknown;
        Produce(*regs, input, mask, kind);
    } else {
        auto* input = reinterpret_cast<void*>(regs->rdi);
        auto* pending = reinterpret_cast<volatile LONG*>(const_cast<void*>(At(input, 0x7d0)));
        const auto packed = static_cast<std::uint32_t>(InterlockedExchange(pending, static_cast<LONG>(regs->rdx)));
        regs->rdx = packed & 0xffu; // Original decoder sees exactly its original bits.
        auto* vr = s.vr.load(std::memory_order_acquire);
        if (!Enabled() || !Thread(vr, 0x2c558c)) {
            s.epoch.fetch_add(1, std::memory_order_acq_rel);
            return;
        }
        void* current{};
        std::array<CustomPulseState, 2> before{};
        float dt{};
        if (!Read(vr, 0x2c5560, current) || current != input ||
            !Copy(before.data(), At(input, 0x7d8), sizeof(before)) ||
            !Copy(&dt, At(xmm, 11 * 16), sizeof(dt))) {
            s.epoch.fetch_add(1, std::memory_order_acq_rel);
            return;
        }
        const auto epoch = s.epoch.load(std::memory_order_acquire);
        const auto policyMask = CustomHapticPolicyMask();
        const bool generationChanged = s.consumedEpoch != epoch || s.input != input || s.consumerThread != GetCurrentThreadId();
        // Zero is a legitimate disabled policy. Compare a complete mask, not a
        // truthiness guard or the unrelated dynamic rumble-graph revision.
        const auto previousPolicy = s.consumedEpoch ? s.consumedPolicyMask : s.activationPolicyMask.load(std::memory_order_acquire);
        const bool policyChanged = previousPolicy != policyMask;
        if (generationChanged || policyChanged) s.tracker.Reset();
        // Pending classifications from a previous reset/policy are conservative.
        // A first observation still accepts newly classified pending sources;
        // its already-active baseline is initialized as unknown by the tracker.
        const bool oldGeneration = s.consumedEpoch && generationChanged;
        const auto attributed = oldGeneration || policyChanged ? EncodeCustomPending(packed & 0xffu, false) : packed;
        s.input = input; s.consumerThread = GetCurrentThreadId();
        s.consumedEpoch = epoch; s.consumedPolicyMask = policyMask;
        s.tracker.Consume(attributed, dt, before);
    }
}
void AmmoPickup(void* pair, std::int32_t oldCount, std::int32_t newCount, const char* stage,
                const char* sound, std::uint32_t side, std::uint32_t kind) {
    auto& s = State();
    SourceScope scope(Enabled() ? CustomHapticSource::AmmoPickup : CustomHapticSource::Unknown);
    s.originalAmmo(pair, oldCount, newCount, stage, sound, side, kind);
}

class Code {
    std::vector<unsigned char> bytes_;
public:
    void Byte(unsigned char x) { bytes_.push_back(x); }
    void Bytes(std::initializer_list<unsigned char> values) { bytes_.insert(bytes_.end(), values); }
    template<class T> void Value(T value) {
        const auto* p = reinterpret_cast<const unsigned char*>(&value);
        bytes_.insert(bytes_.end(), p, p + sizeof(value));
    }
    void AbsoluteJump(const void* target) { Bytes({0xff,0x25,0,0,0,0}); Value(reinterpret_cast<std::uintptr_t>(target)); }
    void Xmm(bool load, unsigned reg) {
        Byte(0xf3); if (reg >= 8) Byte(0x44);
        Bytes({0x0f, static_cast<unsigned char>(load ? 0x6f : 0x7f), static_cast<unsigned char>(0x84u | ((reg & 7u) << 3)), 0x24});
        Value(std::uint32_t{0x20u + reg * 16u});
    }
    const std::vector<unsigned char>& Data() const { return bytes_; }
};
Code ObserverCode(std::uint32_t index, const void* resume) {
    Code c;
    c.Bytes({0x9c,0x50,0x51,0x52,0x53,0x55,0x56,0x57}); // FLAGS, RAX..RDI
    for (unsigned i = 0; i < 8; ++i) c.Bytes({0x41, static_cast<unsigned char>(0x50u + i)});
    c.Bytes({0x4c,0x8b,0xdc,0x48,0x83,0xe4,0xf0,0x48,0x81,0xec}); c.Value(std::uint32_t{0x130});
    c.Bytes({0x4c,0x89,0x9c,0x24}); c.Value(std::uint32_t{0x128});
    c.Bytes({0x0f,0xae,0x9c,0x24}); c.Value(std::uint32_t{0x120}); // STMXCSR
    for (unsigned i = 0; i < 16; ++i) c.Xmm(false, i);
    c.Bytes({0x4c,0x89,0xd9,0xba}); c.Value(index); // RCX = saved GPR frame, EDX = site
    c.Bytes({0x4c,0x8d,0x44,0x24,0x20,0x48,0xb8}); c.Value(reinterpret_cast<std::uintptr_t>(&Observe));
    c.Bytes({0xff,0xd0});
    for (unsigned i = 0; i < 16; ++i) c.Xmm(true, i);
    c.Bytes({0x0f,0xae,0x94,0x24}); c.Value(std::uint32_t{0x120}); // LDMXCSR
    c.Bytes({0x48,0x8b,0xa4,0x24}); c.Value(std::uint32_t{0x128});
    for (int i = 7; i >= 0; --i) c.Bytes({0x41, static_cast<unsigned char>(0x58 + i)});
    c.Bytes({0x5f,0x5e,0x5d,0x5b,0x5a,0x59,0x58,0x9d});
    c.AbsoluteJump(resume);
    return c;
}
void* NearPage(void* vr) noexcept {
    SYSTEM_INFO info{}; GetSystemInfo(&info);
    const auto granularity = static_cast<std::uintptr_t>(info.dwAllocationGranularity);
    const auto origin = reinterpret_cast<std::uintptr_t>(vr) & ~(granularity - 1);
    for (std::uintptr_t distance = granularity; distance < 0x7fff0000u; distance += granularity) {
        for (int direction : {-1, 1}) {
            if (direction < 0 && origin < distance) continue;
            const auto address = direction < 0 ? origin - distance : origin + distance;
            if (auto* p = VirtualAlloc(reinterpret_cast<void*>(address), 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) return p;
        }
    }
    return nullptr;
}
bool Relative(const void* target, const void* after, std::int32_t& result) noexcept {
    const auto delta = reinterpret_cast<std::intptr_t>(target) - reinterpret_cast<std::intptr_t>(after);
    if (delta < (std::numeric_limits<std::int32_t>::min)() || delta > (std::numeric_limits<std::int32_t>::max)()) return false;
    result = static_cast<std::int32_t>(delta); return true;
}
bool MakeThunks(void* vr) {
    auto& s = State();
    if (s.thunks[0]) return true;
#ifdef TG_VR_CUSTOM_HAPTIC_BRIDGE_TESTING
    if (testFailThunk) return false;
#endif
    auto* page = static_cast<unsigned char*>(NearPage(vr));
    if (!page) return false;
    std::size_t offset = 0;
    bool valid = true;
    for (std::size_t i = 0; i < Sites.size(); ++i) {
        const void* resume = At(vr, Sites[i].resume);
#ifdef TG_VR_CUSTOM_HAPTIC_BRIDGE_TESTING
        if (testResumes[i]) resume = testResumes[i];
#endif
        const auto code = ObserverCode(static_cast<std::uint32_t>(i), resume);
        const auto& bytes = code.Data();
        if (offset + bytes.size() >= 0x1f00) { valid = false; break; }
        s.thunks[i] = page + offset;
        std::memcpy(page + offset, bytes.data(), bytes.size());
        offset = (offset + bytes.size() + 15) & ~std::size_t{15};
        auto replacement = Sites[i].original;
        auto* destination = reinterpret_cast<unsigned char*>(&replacement) + (Sites[i].entry - Sites[i].base);
        std::int32_t delta{};
        if (!Relative(s.thunks[i], At(vr, Sites[i].entry + 5), delta)) { valid = false; break; }
        destination[0] = 0xe9; std::memcpy(destination + 1, &delta, sizeof(delta));
        for (unsigned j = 5; j < Sites[i].length; ++j) destination[j] = 0x90;
        s.replacement[i] = replacement;
    }
    if (valid) {
        s.ammoThunk = page + offset;
        Code ammo; ammo.AbsoluteJump(reinterpret_cast<void*>(&AmmoPickup));
        std::memcpy(s.ammoThunk, ammo.Data().data(), ammo.Data().size());
        valid = Relative(s.ammoThunk, At(vr, AmmoCall + 5), s.ammoDisplacement);
    }
    DWORD previous{};
    if (!valid || !VirtualProtect(page, 0x2000, PAGE_EXECUTE_READ, &previous) ||
        !FlushInstructionCache(GetCurrentProcess(), page, 0x2000)) {
        s.thunks = {}; s.ammoThunk = nullptr;
        VirtualFree(page, 0, MEM_RELEASE); return false;
    }
    return true; // Never release code once an original instruction can reach it.
}
bool Patch(std::size_t index) noexcept {
    auto& s = State();
    if (Owner(index)) return true;
#ifdef TG_VR_CUSTOM_HAPTIC_BRIDGE_TESTING
    if (testFailPatch == static_cast<int>(index)) return false;
#endif
    auto* address = const_cast<void*>(At(s.vr.load(), Sites[index].base));
    DWORD previous{};
    if (!VirtualProtect(address, 8, PAGE_EXECUTE_READWRITE, &previous)) return false;
    const auto old = static_cast<std::uint64_t>(InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(address),
        static_cast<LONG64>(s.replacement[index]), static_cast<LONG64>(Sites[index].original)));
    const bool flushed = FlushInstructionCache(GetCurrentProcess(), address, 8) != FALSE;
    DWORD ignored{};
    const bool restored = VirtualProtect(address, 8, previous, &ignored) != FALSE;
    return old == Sites[index].original && flushed && restored;
}
bool PatchAmmo() noexcept {
    auto& s = State();
    if (AmmoOwner()) return true;
#ifdef TG_VR_CUSTOM_HAPTIC_BRIDGE_TESTING
    if (testFailPatch == 6) return false;
#endif
    auto* address = const_cast<void*>(At(s.vr.load(), AmmoCall + 1));
    DWORD previous{};
    if (!VirtualProtect(address, 4, PAGE_EXECUTE_READWRITE, &previous)) return false;
    const LONG old = InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(address), s.ammoDisplacement, AmmoOriginalDisplacement);
    const bool flushed = FlushInstructionCache(GetCurrentProcess(), address, 4) != FALSE;
    DWORD ignored{};
    const bool restored = VirtualProtect(address, 4, previous, &ignored) != FALSE;
    return old == AmmoOriginalDisplacement && flushed && restored;
}
} // namespace tg::custom_bridge_detail

namespace tg {
bool InstallCustomHapticBridge(void* vr, void* client) {
    using namespace custom_bridge_detail;
    auto& s = State();
    std::lock_guard lock(s.install);
    if (!MatchesClient(vr, client) || !Thread(vr, 0x2c7e90) || !Identity(vr)) return false;
    if (s.vr.load() && (s.vr.load() != vr || s.client.load() != client)) return false;
    if (s.ready.load(std::memory_order_acquire)) return Owned();
    s.vr.store(vr, std::memory_order_release); s.client.store(client, std::memory_order_release);
    if (!s.pinned) {
        HMODULE self{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&Observe), &self)) return false;
        s.pinned = true;
    }
    s.originalAmmo = reinterpret_cast<AmmoFunction>(const_cast<void*>(At(vr, 0x5da70)));
    s.originalScript = reinterpret_cast<ScriptFunction>(const_cast<void*>(At(vr, 0x9b7a0)));
#ifdef TG_VR_CUSTOM_HAPTIC_BRIDGE_TESTING
    s.originalAmmo = testAmmo; s.originalScript = testScript;
#endif
    if (!s.originalAmmo || !s.originalScript || !MakeThunks(vr)) return false;
    for (std::size_t i = 0; i < Sites.size(); ++i) if (!Owner(i, true)) return false;
    if (!AmmoOwner(true)) return false;
    // Existing low-byte requests predate provenance and must remain unknown.
    void* input{};
    if (!Read(vr, 0x2c5560, input) || !input) return false;
    auto* pending = reinterpret_cast<volatile LONG*>(const_cast<void*>(At(input, 0x7d0)));
    const auto existing = static_cast<std::uint32_t>(InterlockedCompareExchange(pending, 0, 0)) & 0xffu;
    InterlockedOr(pending, static_cast<LONG>(existing << 8));
    for (std::size_t i = 0; i < Sites.size() - 1; ++i) if (!Patch(i)) return false;
    if (!PatchAmmo() || !Patch(Sites.size() - 1)) return false;
    // A not-yet-patched writer could have run after the first seed. All writers
    // now encode unknown until ready is published, so seal that final gap too.
    const auto finalExisting = static_cast<std::uint32_t>(InterlockedCompareExchange(pending, 0, 0)) & 0xffu;
    InterlockedOr(pending, static_cast<LONG>(finalExisting << 8));
    s.activationPolicyMask.store(CustomHapticPolicyMask(), std::memory_order_release);
    s.ready.store(Owned(), std::memory_order_release);
    return s.ready.load(std::memory_order_acquire);
}
void ResetCustomHapticBridge() noexcept {
    custom_bridge_detail::State().epoch.fetch_add(1, std::memory_order_acq_rel);
}
bool ReadFilteredCustomHaptic(void* vr, void* input, int side, float remaining, float amplitude,
                             float& filteredRemaining, float& filteredAmplitude) noexcept {
    using namespace custom_bridge_detail;
    filteredRemaining = filteredAmplitude = 0;
    auto& s = State();
    // The tracker belongs to the single original XR thread. Check its current
    // ID before touching any consumer-owned non-atomic state.
    if (!Enabled() || s.vr.load(std::memory_order_acquire) != vr || !Thread(vr, 0x2c558c)) return false;
    void* current{};
    if (!Read(vr, 0x2c5560, current) || input != current || input != s.input ||
        s.consumerThread != GetCurrentThreadId() || s.consumedEpoch != s.epoch.load(std::memory_order_acquire) ||
        s.consumedPolicyMask != CustomHapticPolicyMask()) return false;
    const auto epoch = s.consumedEpoch;
    const auto policy = s.consumedPolicyMask;
    if (!s.tracker.Read(side, remaining, amplitude, filteredRemaining, filteredAmplitude)) return false;
    if (epoch != s.epoch.load(std::memory_order_acquire) || policy != CustomHapticPolicyMask()) {
        filteredRemaining = filteredAmplitude = 0;
        return false;
    }
    return true;
}
bool TryMappedScriptPulse(void* vr, void* rawVm, CustomHapticSource kind, std::int32_t& result) {
    using namespace custom_bridge_detail;
    auto& s = State();
    if (!rawVm || (kind != CustomHapticSource::ItemGrab && kind != CustomHapticSource::ItemRelease) ||
        !Enabled() || s.vr.load(std::memory_order_acquire) != vr || !Thread(vr, 0x2c7e90) || !s.originalScript) return false;
    SourceScope scope(kind);
    result = s.originalScript(rawVm);
    return true;
}
} // namespace tg
