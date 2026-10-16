#include "northstar_abi.hpp"
#include "runtime.hpp"
#include "event_router.hpp"
#include "vr_hand_state.hpp"
#include "vr_slot_state.hpp"
#include "vr_grenade_slot_state.hpp"
#include "vr_ammo_pouch_state.hpp"
#include "vr_haptic_bridge.hpp"
#include "vr_rumble_bridge.hpp"
#include "vr_custom_haptic_bridge.hpp"
#include "vr_custom_haptic_state.hpp"

#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <xmmintrin.h>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace
{
constexpr std::size_t MaxText = 32 * 1024;
constexpr std::size_t SignatureBytes = 32;
constexpr char RegisterSignature[] = "4883ec38450fb6c84533c0c644242000e89b2e00004883c438c3cccccccccccc";
constexpr char GetStringSignature[] = "488b41484c63c24d03c04a8b44c0084883c030c3cccccccccccccccccccccccc";
constexpr char RaiseErrorSignature[] = "48895c24084889742410574883ec30488b5950488bf14183c8ff488b8b484000";
constexpr char GetFunctionSignature[] = "48895c2418488954241055565741544155415641574883ec50488b59504c8be9";
constexpr char ServerInitSignature[] = "48895c240848896c24104889742418574883ec60488b5908488be94c8d0d1ead";
constexpr char PushIntegerSignature[] = "4883ec3833c048c7442420020000054889442428895424288b51688d4201448b";
constexpr char PushFloatSignature[] = "4883ec388b516833c048c74424200400000548894424288d4201448bc2f30f11";
static_assert(sizeof(RegisterSignature) == 2 * SignatureBytes + 1);
static_assert(sizeof(GetStringSignature) == 2 * SignatureBytes + 1);
static_assert(sizeof(RaiseErrorSignature) == 2 * SignatureBytes + 1);
static_assert(sizeof(GetFunctionSignature) == 2 * SignatureBytes + 1);
static_assert(sizeof(ServerInitSignature) == 2 * SignatureBytes + 1);
static_assert(sizeof(PushIntegerSignature) == 2 * SignatureBytes + 1);
static_assert(sizeof(PushFloatSignature) == 2 * SignatureBytes + 1);

struct GameBuild
{
	const wchar_t* moduleName;
	const char* sha256;
	DWORD timestamp;
	DWORD imageSize;
	std::size_t fileSize;
	DWORD registerRva;
	DWORD getStringRva;
	DWORD raiseErrorRva;
};

// RVAs are from official NorthstarLauncher, checked against these local game files.
// A different game build is deliberately unsupported until independently verified.
constexpr GameBuild Builds[2] = {
	{L"server.dll", "a5ca3a25c8ae56952a26141b0f6cdcb6c19086c8c39013f7cb345d5d723661af",
	 0x5a271255, 0x24ed000, 13680184, 0x1dd10, 0x60a0, 0x8440},
	{L"client.dll", "002b36487fec7c98882929fbccdb506d0146bbf3270cbb964a4d21c5edf7eebc",
	 0x5a271254, 0x3d4c000, 13587968, 0x108e0, 0x60c0, 0x8470}
};

struct Api
{
	HMODULE module{};
	northstar::RegisterFunction registerFunction{};
	northstar::GetString getString{};
	northstar::RaiseError raiseError{};
	northstar::GetFunction getFunction{};
	northstar::CallScriptInit callScriptInit{};
	northstar::PushInteger pushInteger{};
	northstar::PushFloat pushFloat{};
};
struct VmBinding
{
	northstar::ScriptVm* wrapper{};
	northstar::Sqvm* vm{};
	bool bootstrapAttempted{};
	ULONGLONG nextBootstrapLookup{};
	bool receivedEvent{};
};

HMODULE g_self{};
HMODULE g_northstar{};
std::atomic<northstar::System*> g_system{};
std::mutex g_stateMutex;
std::array<Api, 2> g_apis{};
std::array<VmBinding, 2> g_vms{};
HMODULE g_vrModule{};
bool g_vrChecked{};
std::mutex g_slotMutex;
tg::VrSlotObserver g_slotObserver;
tg::VrGrenadeSlotObserver g_grenadeSlotObserver;
tg::VrAmmoPouchObserver g_ammoPouchObserver;
std::deque<tg::VrSlotEvent> g_slotEvents;
struct BodyPoses
{
	std::array<std::array<float, 3>, 2> hands{};
	std::array<int, 2> pickupKinds{-1, -1};
	std::array<bool, 2> grenadeStoreEligible{};
	std::array<bool, 2> grenadeSlow{};
	std::array<std::array<std::array<float, 3>, 3>, 2> grenadeVelocities{};
	int grenadeHand = -1;
	int grenadeTicket = 0;
	bool grenadeConfirmed = false;
	ULONGLONG sampledAt{};
};
BodyPoses g_bodyPoses;
int g_projectileTicket = 0;
void CaptureGrenadeStoreInputs(BodyPoses& poses, bool scriptEligible);
void ObserveVrSlots(int projectileTicket = -1);

void ResetSlotObservation()
{
	std::lock_guard lock(g_slotMutex);
	g_slotObserver.Reset();
	g_grenadeSlotObserver.Reset();
	g_ammoPouchObserver.Reset();
	g_slotEvents.clear();
	g_bodyPoses = {};
	g_projectileTicket = 0;
}

// Script and native shoulder anchors are both in Source world coordinates.
// Raw tracking-space controller poses cannot be compared to these anchors.
bool UpdateBodyPoses(const std::string& text)
{
	const auto input = tg::Json::parse(text, nullptr, false);
	if (!input.is_object()) return false;
	BodyPoses poses;
	constexpr const char* names[] = {"left", "right"};
	constexpr const char* kinds[] = {"left_kind", "right_kind"};
	for (std::size_t side = 0; side < 2; ++side) {
		if (!input.contains(names[side]) || !input[names[side]].is_array() || input[names[side]].size() != 3 ||
			!input.contains(kinds[side]) || !input[kinds[side]].is_number_integer()) return false;
		const auto kind = input[kinds[side]].get<std::int64_t>();
		if (kind < -1 || kind > 4) return false;
		poses.pickupKinds[side] = static_cast<int>(kind);
		for (std::size_t axis = 0; axis < 3; ++axis) {
			if (!input[names[side]][axis].is_number()) return false;
			const auto value = input[names[side]][axis].get<double>();
			if (!std::isfinite(value) || std::abs(value) > 1.0e7) return false;
			poses.hands[side][axis] = static_cast<float>(value);
		}
	}
	if (input.contains("eligible") && !input["eligible"].is_boolean()) return false;
	int projectileTicket = -1;
	if (input.contains("projectile_ticket")) {
		if (!input["projectile_ticket"].is_number_integer()) return false;
		const auto ticket = input["projectile_ticket"].get<std::int64_t>();
		if (ticket < 0 || ticket > (std::numeric_limits<int>::max)()) return false;
		projectileTicket = static_cast<int>(ticket);
	}
	// Consume the preceding action with its original source/velocity packet.
	// The new packet's projectile ticket can positively exclude that throw.
	ObserveVrSlots(projectileTicket);
	CaptureGrenadeStoreInputs(poses, input.value("eligible", false));
	poses.sampledAt = GetTickCount64();
	std::lock_guard lock(g_slotMutex);
	g_bodyPoses = poses;
	return true;
}

struct PendingLog
{
	int level;
	std::string text;
};
std::mutex g_logMutex;
std::deque<PendingLog> g_pendingLogs;

void Log(int level, const char* message) noexcept
{
	// Runtime emits this record only after the effect send succeeds. Keep the
	// game logger limited to playback, even if a future caller adds telemetry.
	if (level != 0 || !message || std::strncmp(message, "PLAY event=", 11) != 0 || !message[11]) return;
	const auto length = strnlen_s(message, 8192);
	if (length == 8192 || std::memchr(message, '\r', length) || std::memchr(message, '\n', length)) return;
	try
	{
		// NSSys::Log looks up the caller in Northstar's plugin vector. That vector
		// is owned by the main thread, so output workers must not call it directly.
		std::lock_guard lock(g_logMutex);
		if (g_pendingLogs.size() >= 128) g_pendingLogs.pop_front();
		g_pendingLogs.push_back({level, std::string(message, length)});
	}
	catch (...) { }
}

void FlushLogs() noexcept
{
	try
	{
		std::deque<PendingLog> pending;
		{
			std::lock_guard lock(g_logMutex);
			pending.swap(g_pendingLogs);
		}
		for (auto& entry : pending)
		{
			if (auto* system = g_system.load())
				system->Log(0, static_cast<northstar::LogLevel>(entry.level), entry.text.data());
		}
	}
	catch (...) { }
}

// Keep SEH in small leaf functions; no C++ object is unwound by these guards.
bool SafeCopy(void* destination, const void* source, std::size_t bytes)
{
	if (!source || !destination) return false;
	__try { std::memcpy(destination, source, bytes); return true; }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Reproduce the native helper's float dot/normalize order with SSE. Its
// output speed below 1 m/s proves TryThrow takes the store branch; raw hand
// speed alone is insufficient because normalized directions can cancel.
bool SlowGrenadeMotion(const std::array<std::array<float, 3>, 3>& samples)
{
    const auto zero = _mm_setzero_ps();
    auto sum = zero;
    auto maximum = zero;
    const auto dot = [](__m128 vector, bool nativeLengthOrder) {
        const auto squared = _mm_mul_ps(vector, vector);
        const auto second = nativeLengthOrder ? _mm_shuffle_ps(squared, squared, _MM_SHUFFLE(2, 2, 2, 2)) :
            _mm_shuffle_ps(squared, squared, _MM_SHUFFLE(1, 1, 1, 1));
        const auto third = nativeLengthOrder ? _mm_shuffle_ps(squared, squared, _MM_SHUFFLE(1, 1, 1, 1)) :
            _mm_shuffle_ps(squared, squared, _MM_SHUFFLE(2, 2, 2, 2));
        return _mm_add_ss(_mm_add_ss(squared, second), third);
    };
    const auto normalize = [&](const __m128 vector) {
        const auto squared = dot(vector, false);
        const auto broadcast = _mm_shuffle_ps(squared, squared, 0);
        const auto norm = _mm_sqrt_ps(broadcast);
        auto direction = _mm_div_ps(vector, norm);
        direction = _mm_and_ps(direction, _mm_cmpneq_ps(norm, zero));
        const auto notInfinity = _mm_cmpneq_ps(broadcast, _mm_set1_ps(std::numeric_limits<float>::infinity()));
        return _mm_or_ps(_mm_and_ps(direction, notInfinity),
            _mm_andnot_ps(notInfinity, _mm_set1_ps(std::numeric_limits<float>::quiet_NaN())));
    };
    for (const auto& sample : samples) {
        if (std::any_of(sample.begin(), sample.end(), [](float v) { return !std::isfinite(v); })) return false;
        const auto vector = _mm_setr_ps(sample[0], sample[1], sample[2], 0);
        const auto squared = dot(vector, true);
        if (!std::isfinite(_mm_cvtss_f32(squared))) return false;
        maximum = _mm_max_ss(maximum, _mm_sqrt_ss(squared));
        sum = _mm_add_ps(sum, normalize(vector));
    }
    const auto velocity = _mm_mul_ps(normalize(sum), _mm_shuffle_ps(maximum, maximum, 0));
    const auto speed = _mm_cvtss_f32(_mm_sqrt_ss(dot(velocity, true)));
    return std::isfinite(speed) && speed < 1.0f;
}

// Capture BEFORE the upcoming native grip/throw update. These opaque native
// gates are copied literally from its eligibility branch, not reconstructed
// from hand disappearance or the absence of a later projectile.
void CaptureGrenadeStoreInputs(BodyPoses& poses, bool scriptEligible)
{
	if (!g_vrModule || !scriptEligible) return;
	const auto* base = reinterpret_cast<const std::byte*>(g_vrModule);
	unsigned char confirmed{};
	if (!SafeCopy(&poses.grenadeHand, base + 0x317d00, sizeof(poses.grenadeHand)) ||
		!SafeCopy(&poses.grenadeTicket, base + 0x317d58, sizeof(poses.grenadeTicket)) ||
		!SafeCopy(&confirmed, base + 0x317d65, sizeof(confirmed)) || confirmed > 1) return;
	poses.grenadeConfirmed = confirmed != 0;
	const std::byte *input{}, *entity{}, *entityState{}, *context{}, *view{};
	if (!SafeCopy(&input, base + 0x2c5560, sizeof(input)) || !input ||
		!SafeCopy(&context, base + 0x2cd7c8, sizeof(context)) || !context ||
		!SafeCopy(&entity, base + 0x2cd7d0, sizeof(entity)) || !entity ||
		!SafeCopy(&view, base + 0x2ce760, sizeof(view)) || !view ||
		!SafeCopy(&entityState, entity + 0x1208, sizeof(entityState)) || !entityState) return;
	unsigned char active{}, blocked[4]{};
	std::uint32_t hmdFlags{}, occupied{};
	if (!SafeCopy(&active, input + 0xb91, sizeof(active)) || !active ||
		!SafeCopy(&blocked[0], base + 0x31808c, 1) || !SafeCopy(&blocked[1], base + 0x2ca4e0, 1) ||
		!SafeCopy(&blocked[2], base + 0x2ca511, 1) || !SafeCopy(&blocked[3], base + 0x18d228, 1) ||
		!SafeCopy(&hmdFlags, input + 0xb68, sizeof(hmdFlags)) || (hmdFlags & 3) != 3 ||
		!SafeCopy(&occupied, base + 0x3185bc, sizeof(occupied)) ||
		std::any_of(std::begin(blocked), std::end(blocked), [](unsigned char flag) { return flag != 0; })) return;
	for (std::size_t side = 0; side < 2; ++side) {
		std::uint32_t handFlags{};
		if (!SafeCopy(&handFlags, input + (side == 0 ? 0x918 : 0xa40), sizeof(handFlags)) ||
			(handFlags & 3) != 3 || (occupied & (1u << side))) continue;
		poses.grenadeStoreEligible[side] = true;
		auto& velocities = poses.grenadeVelocities[side];
		if (SafeCopy(velocities.data(), input + 0x7f8 + side * 36, sizeof(velocities)))
			poses.grenadeSlow[side] = SlowGrenadeMotion(velocities);
	}
}

bool GrenadeStoreInputsStillMatch(const BodyPoses& captured, int side)
{
	if (!tg::IsVrPhysicalHand(side)) return false;
	BodyPoses current;
	CaptureGrenadeStoreInputs(current, true);
	return captured.grenadeStoreEligible[side] && captured.grenadeSlow[side] &&
		current.grenadeStoreEligible[side] && current.grenadeSlow[side] &&
		current.grenadeVelocities[side] == captured.grenadeVelocities[side];
}

bool SafeRegister(northstar::RegisterFunction function, northstar::ScriptVm* vm,
	northstar::FunctionRegistration* registration)
{
	__try { function(vm, registration, 1); return true; }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

const char* SafeGetString(northstar::GetString function, northstar::Sqvm* vm, int index = 1)
{
	__try { return function(vm, index); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

northstar::SqResult ScriptError(const Api& api, northstar::Sqvm* vm, const char* error)
{
	__try { return api.raiseError ? api.raiseError(vm, error) : northstar::SqError; }
	__except (EXCEPTION_EXECUTE_HANDLER) { return northstar::SqError; }
}

bool CopyBoundedText(const char* source, std::string& result)
{
	if (!source) return false;
	result.clear();
	std::array<char, 512> block{};
	while (result.size() <= MaxText)
	{
		MEMORY_BASIC_INFORMATION region{};
		if (!VirtualQuery(source, &region, sizeof(region)) || region.State != MEM_COMMIT ||
			(region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
		const auto available = static_cast<std::size_t>(
			static_cast<const char*>(region.BaseAddress) + region.RegionSize - source);
		const auto count = (std::min)({available, block.size(), MaxText + 1 - result.size()});
		if (!count || !SafeCopy(block.data(), source, count)) return false;
		const auto* terminator = static_cast<const char*>(std::memchr(block.data(), 0, count));
		if (terminator)
		{
			result.append(block.data(), terminator - block.data());
			return !result.empty() && result.size() <= MaxText;
		}
		result.append(block.data(), count);
		source += count;
	}
	return false;
}

unsigned HexDigit(char c) { return c <= '9' ? c - '0' : c - 'a' + 10; }

template<std::size_t N> std::array<unsigned char, N> HexBytes(const char* text)
{
	std::array<unsigned char, N> bytes{};
	for (std::size_t i = 0; i < N; ++i)
		bytes[i] = static_cast<unsigned char>((HexDigit(text[2 * i]) << 4) | HexDigit(text[2 * i + 1]));
	return bytes;
}

bool ReadModuleFile(HMODULE module, const GameBuild& build, std::vector<unsigned char>& bytes)
{
	std::wstring path(32768, L'\0');
	const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
	if (!length || length >= path.size()) return false;
	path.resize(length);
	HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER size{};
	if (!GetFileSizeEx(file, &size) || size.QuadPart != static_cast<LONGLONG>(build.fileSize))
	{
		CloseHandle(file);
		return false;
	}
	bytes.resize(build.fileSize);
	DWORD read{};
	const bool ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size();
	CloseHandle(file);
	return ok;
}

bool CheckHash(const std::vector<unsigned char>& bytes, const char* expected)
{
	BCRYPT_ALG_HANDLE algorithm{};
	if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
	std::array<unsigned char, 32> hash{};
	const auto status = BCryptHash(algorithm, nullptr, 0, const_cast<PUCHAR>(bytes.data()),
		static_cast<ULONG>(bytes.size()), hash.data(), static_cast<ULONG>(hash.size()));
	BCryptCloseAlgorithmProvider(algorithm, 0);
	return status >= 0 && hash == HexBytes<32>(expected);
}

bool ExecutableAddress(const void* address)
{
	MEMORY_BASIC_INFORMATION region{};
	if (!VirtualQuery(address, &region, sizeof(region)) || region.State != MEM_COMMIT ||
		(region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
	const DWORD access = region.Protect & 0xff;
	return access == PAGE_EXECUTE || access == PAGE_EXECUTE_READ ||
		access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY;
}

bool CheckDetouredEntry(const unsigned char* entry, const char* signature)
{
	std::array<unsigned char, SignatureBytes> original{};
	if (!SafeCopy(original.data(), entry, original.size())) return false;
	if (original == HexBytes<SignatureBytes>(signature)) return ExecutableAddress(entry);

	// Northstar installs a registration detour before invoking plugin callbacks.
	// Follow only the two branch forms used by its hook library, without altering them.
	const unsigned char* cursor = entry;
	for (unsigned hop = 0; hop < 4; ++hop)
	{
		std::array<unsigned char, 6> code{};
		if (!ExecutableAddress(cursor) || !SafeCopy(code.data(), cursor, code.size())) return false;
		if (code[0] == 0xe9)
		{
			std::int32_t displacement{};
			std::memcpy(&displacement, &code[1], sizeof(displacement));
			cursor += 5 + displacement;
		}
		else if (code[0] == 0xff && code[1] == 0x25)
		{
			std::int32_t displacement{};
			std::memcpy(&displacement, &code[2], sizeof(displacement));
			if (!SafeCopy(&cursor, cursor + 6 + displacement, sizeof(cursor))) return false;
		}
		else return false;

		HMODULE owner{};
		if (ExecutableAddress(cursor) && GetModuleHandleExW(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(cursor), &owner) && owner == g_northstar) return true;
	}
	return false;
}

bool CheckFunction(const std::vector<unsigned char>& file, const IMAGE_NT_HEADERS64& header,
	std::size_t ntOffset, HMODULE module, DWORD rva, const char* signature, bool registration)
{
	const auto sections = ntOffset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + header.FileHeader.SizeOfOptionalHeader;
	if (sections > file.size() || header.FileHeader.NumberOfSections > (file.size() - sections) / sizeof(IMAGE_SECTION_HEADER))
		return false;
	bool found = false;
	for (unsigned i = 0; i < header.FileHeader.NumberOfSections; ++i)
	{
		IMAGE_SECTION_HEADER section{};
		std::memcpy(&section, file.data() + sections + i * sizeof(section), sizeof(section));
		if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE) || rva < section.VirtualAddress) continue;
		const auto relative = static_cast<std::size_t>(rva - section.VirtualAddress);
		if (relative > section.SizeOfRawData || SignatureBytes > section.SizeOfRawData - relative) continue;
		const auto offset = static_cast<std::size_t>(section.PointerToRawData) + relative;
		if (offset > file.size() || SignatureBytes > file.size() - offset) return false;
		const auto expected = HexBytes<SignatureBytes>(signature);
		if (std::memcmp(file.data() + offset, expected.data(), expected.size())) return false;
		found = true;
		break;
	}
	if (!found || rva > header.OptionalHeader.SizeOfImage || SignatureBytes > header.OptionalHeader.SizeOfImage - rva) return false;
	const auto* entry = reinterpret_cast<const unsigned char*>(module) + rva;
	if (registration) return CheckDetouredEntry(entry, signature);
	std::array<unsigned char, SignatureBytes> memory{};
	return ExecutableAddress(entry) && SafeCopy(memory.data(), entry, memory.size()) && memory == HexBytes<SignatureBytes>(signature);
}

bool ValidateModule(HMODULE module, int context, Api& result)
{
	const auto& build = Builds[context];
	std::vector<unsigned char> file;
	if (!ReadModuleFile(module, build, file) || !CheckHash(file, build.sha256)) return false;
	IMAGE_DOS_HEADER dos{};
	std::memcpy(&dos, file.data(), sizeof(dos));
	if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < sizeof(dos) ||
		static_cast<std::size_t>(dos.e_lfanew) > file.size() - sizeof(IMAGE_NT_HEADERS64)) return false;
	IMAGE_NT_HEADERS64 nt{}, loaded{};
	std::memcpy(&nt, file.data() + dos.e_lfanew, sizeof(nt));
	if (!SafeCopy(&loaded, reinterpret_cast<const unsigned char*>(module) + dos.e_lfanew, sizeof(loaded))) return false;
	if (nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
		nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt.FileHeader.TimeDateStamp != build.timestamp ||
		nt.OptionalHeader.SizeOfImage != build.imageSize || loaded.Signature != nt.Signature ||
		loaded.FileHeader.TimeDateStamp != nt.FileHeader.TimeDateStamp || loaded.OptionalHeader.SizeOfImage != nt.OptionalHeader.SizeOfImage)
		return false;
	if (!CheckFunction(file, nt, dos.e_lfanew, module, build.registerRva, RegisterSignature, true) ||
		!CheckFunction(file, nt, dos.e_lfanew, module, build.getStringRva, GetStringSignature, false) ||
		!CheckFunction(file, nt, dos.e_lfanew, module, build.raiseErrorRva, RaiseErrorSignature, false) ||
		!CheckFunction(file, nt, dos.e_lfanew, module, 0x36a0, PushIntegerSignature, false) ||
		!CheckFunction(file, nt, dos.e_lfanew, module, 0x3800, PushFloatSignature, false)) return false;
	const auto base = reinterpret_cast<std::uintptr_t>(module);
	result = {module, reinterpret_cast<northstar::RegisterFunction>(base + build.registerRva),
		reinterpret_cast<northstar::GetString>(base + build.getStringRva),
		reinterpret_cast<northstar::RaiseError>(base + build.raiseErrorRva)};
	result.pushInteger = reinterpret_cast<northstar::PushInteger>(base + 0x36a0);
	result.pushFloat = reinterpret_cast<northstar::PushFloat>(base + 0x3800);
	if (context == 0) {
		// Official Northstar getfunction and CallScriptInitCallback entry points.
		// The latter has the same Northstar-owned detour validation as registration.
		if (!CheckFunction(file, nt, dos.e_lfanew, module, 0x6c80, GetFunctionSignature, false) ||
			!CheckFunction(file, nt, dos.e_lfanew, module, 0x1d5c0, ServerInitSignature, true)) return false;
		result.getFunction = reinterpret_cast<northstar::GetFunction>(base + 0x6c80);
		result.callScriptInit = reinterpret_cast<northstar::CallScriptInit>(base + 0x1d5c0);
	}
	else if (!CheckFunction(file, nt, dos.e_lfanew, module, 0x88900,
		"40534883ec204883b90812000000488bd97525488b41104883c110ff50404885", false)) return false;
	return true;
}

// Independently verified read-only layout of the installed CircuitLord build.
// Never read these RVAs in a different build. The haptic bridge separately
// validates its dispatch anchors before observing the writable XR function table.
bool ValidateVrModule(HMODULE module)
{
	constexpr GameBuild build = {L"Titanfall2VR.dll",
		"6b550331c17830999602f573c59307544bcb357838aad11ff00037cbe59523f3",
		0x6ac9b8bc, 0x32b000, 2972672, 0, 0, 0};
	std::vector<unsigned char> file;
	if (!ReadModuleFile(module, build, file) || !CheckHash(file, build.sha256)) return false;
	IMAGE_DOS_HEADER dos{};
	std::memcpy(&dos, file.data(), sizeof(dos));
	if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < sizeof(dos) ||
		static_cast<std::size_t>(dos.e_lfanew) > file.size() - sizeof(IMAGE_NT_HEADERS64)) return false;
	IMAGE_NT_HEADERS64 nt{}, loaded{};
	std::memcpy(&nt, file.data() + dos.e_lfanew, sizeof(nt));
	if (!SafeCopy(&loaded, reinterpret_cast<const unsigned char*>(module) + dos.e_lfanew, sizeof(loaded)) ||
		nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
		nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt.FileHeader.TimeDateStamp != build.timestamp ||
		nt.OptionalHeader.SizeOfImage != build.imageSize || loaded.Signature != nt.Signature ||
		loaded.FileHeader.TimeDateStamp != nt.FileHeader.TimeDateStamp ||
		loaded.OptionalHeader.SizeOfImage != nt.OptionalHeader.SizeOfImage) return false;
	return CheckFunction(file, nt, dos.e_lfanew, module, 0x3c9d0,
		"48895c241048897c2418488b7c2430498bd8c74108ffffffff4c8bdaf3410f10", false) &&
		CheckFunction(file, nt, dos.e_lfanew, module, 0x3ee54,
		"f20f1115bc8e2d000fc6d2aaf30f1115b88e2d000f28df0f54dc410f56d8410f", false) &&
		CheckFunction(file, nt, dos.e_lfanew, module, 0x3eea8,
		"f20f1115748e2d000fc6d2aaf30f1115708e2d000f28df0f54dd410f56d8410f", false) &&
		CheckFunction(file, nt, dos.e_lfanew, module, 0x3e492,
		"48833d2ef3280000744684db744280bf910b0000007439803ddc9b2d00007530", false) &&
		CheckFunction(file, nt, dos.e_lfanew, module, 0x3e4b2,
		"803d27c02800007527803d4fc0280000751e803d5ded1400007515488d8f580b", false) &&
		CheckFunction(file, nt, dos.e_lfanew, module, 0x3f1e6,
		"4584ed7417488d8f08090000e8b986fcff84c07407c644244001eb0ac6442440", false) &&
		CheckFunction(file, nt, dos.e_lfanew, module, 0x3f8d0,
		"8b0de68c2d0044387c24407409f6c1017504b001eb0232c08844244044387c24", false) &&
		CheckFunction(file, nt, dos.e_lfanew, module, 0x78b0,
		"0fb6411024033c030f94c0c3cccccccc0fb64110240f3c0f0f94c0c3cccccccc", false) &&
		CheckFunction(file, nt, dos.e_lfanew, module, 0x40bfa,
		"83fbfe0f8567030000486315fa702d00807c1440000f84dc010000803d49712d", false) &&
		CheckFunction(file, nt, dos.e_lfanew, module, 0x3dc86,
		"448bc6488d5530488b0dcc782800e8b7d2fcff488b15c0782800488d0cf6488d", false) &&
		CheckFunction(file, nt, dos.e_lfanew, module, 0x3dcf8,
		"0f2f0569f31200730732c0e9660600008b3d66a02d0085f60f94c34c8d4d304c", false) &&
		CheckFunction(file, nt, dos.e_lfanew, module, 0x3cb90,
		"4883ec7833c04889018941084889410c89411441f64010027435f30f100d1206", false) &&
		CheckFunction(file, nt, dos.e_lfanew, module, 0xc090,
		"41f68417b809000001751f4788b40f400800000f57c033c0410f1100410f1140", false) &&
		CheckFunction(file, nt, dos.e_lfanew, module, 0x50661,
		"ff875c010000837f2000c6470c01c6470a017d41488b0783780c017538488b80", false) &&
		CheckFunction(file, nt, dos.e_lfanew, module, 0x57afb,
		"48893576042c00488b542450486385400f0000488d0c4048c1e10448038a0810", false) &&
		CheckFunction(file, nt, dos.e_lfanew, module, 0x555be,
		"8b1d00272c00b80100000083fbff0f44d8895dc0448be0442be3448964244844", false) &&
		CheckFunction(file, nt, dos.e_lfanew, module, 0x57fef,
		"4c8d0d82e80e00448b865c010000660f6fc7660f73d808660f7ec2488d4c2460", false) &&
		CheckFunction(file, nt, dos.e_lfanew, module, 0x57d36,
		"4533c0488d542470488d0dd3ff2b00e8b64afeff8885200400004b8d3c2f4c8d", false) &&
		CheckFunction(file, nt, dos.e_lfanew, module, 0x99be3,
		"8b15dbe0270083faff0f44d34c8b05ca3a2800488bcf4981c0a036000041ffd0", false) &&
		CheckFunction(file, nt, dos.e_lfanew, module, 0x5873b,
			"488d0d5af52b00e88929ffff44384c24410f841703000044384c24420f850c03", false) &&
		CheckFunction(file, nt, dos.e_lfanew, module, 0x597cc,
			"488d15c9e42b00488bc8e8256ffbffc64424402c488d542440488bc8e8836cfb", false) &&
		CheckFunction(file, nt, dos.e_lfanew, module, 0x7bca1,
			"403835f4bf290075094d85e40f84940000000f10254ec129000f100557c12900", false);
}

bool ReadVrSnapshot(tg::VrHandState& state, int* actionHand = nullptr, int* grenadeActionHand = nullptr)
{
	const auto module = GetModuleHandleW(L"Titanfall2VR.dll");
	if (!module) return false;
	if (g_vrModule && g_vrModule != module) { g_vrModule = nullptr; g_vrChecked = false; }
	if (!g_vrChecked) {
		g_vrChecked = true;
		if (!ValidateVrModule(module)) return false;
		g_vrModule = module;
	}
	if (!g_vrModule) return false;
	const auto* base = reinterpret_cast<const std::byte*>(module);
	unsigned char support{}, confirmed[2]{};
	if (!SafeCopy(&state.gunHand, base + 0x317cc4, sizeof(state.gunHand)) ||
		!SafeCopy(&state.gunSlot, base + 0x317ccc, sizeof(state.gunSlot)) ||
		!SafeCopy(&state.grenadeHand, base + 0x317d00, sizeof(state.grenadeHand)) ||
		!SafeCopy(&state.gunTicket, base + 0x317d54, sizeof(state.gunTicket)) ||
		!SafeCopy(&state.grenadeTicket, base + 0x317d58, sizeof(state.grenadeTicket)) ||
		!SafeCopy(&support, base + 0x317c9c, sizeof(support)) ||
		!SafeCopy(confirmed, base + 0x317d64, sizeof(confirmed)) ||
		(actionHand && !SafeCopy(actionHand, base + 0x317cc8, sizeof(*actionHand))) ||
		(grenadeActionHand && !SafeCopy(grenadeActionHand, base + 0x317d04, sizeof(*grenadeActionHand)))) return false;
	state.supportHeld = support != 0;
	state.gunConfirmed = confirmed[0] != 0;
	state.grenadeConfirmed = confirmed[1] != 0;
	const int packed = support > 1 || confirmed[0] > 1 || confirmed[1] > 1 ? -1 : tg::PackVrHandState(state);
	return packed >= 0 && (!actionHand || (*actionHand >= -1 && *actionHand <= 1)) &&
		(!grenadeActionHand || (*grenadeActionHand >= -1 && *grenadeActionHand <= 1));
}

int ReadVrHandState()
{
	tg::VrHandState state;
	return ReadVrSnapshot(state) ? tg::PackVrHandState(state) : -1;
}

// Called under the slot mutex after the complete native layout is verified.
// This counter is incremented only by successful fresh-ammo pouch pickups,
// independently of ejecting an old magazine, racking rounds or an empty pouch.
void ObserveVrAmmoPouch(const tg::VrHandState& state, bool posesFresh)
{
	const auto* base = reinterpret_cast<const std::byte*>(g_vrModule);
	const std::byte *reload{}, *checkReload{};
	const void *metadata{}, *checkMetadata{};
	int pickups{}, checkPickups{}, empty{};
	unsigned char heldNew{}, heldOther{};
	// The original updater clears this pointer transiently before publishing
	// the active reload object. Missing samples must not erase its baseline.
	if (!SafeCopy(&reload, base + 0x317f78, sizeof(reload)) || !reload ||
		!SafeCopy(&metadata, reload, sizeof(metadata)) || !metadata ||
		!SafeCopy(&pickups, reload + 0x15c, sizeof(pickups)) || pickups < 0 ||
		!SafeCopy(&heldNew, reload + 0xa, sizeof(heldNew)) || heldNew > 1 ||
		!SafeCopy(&heldOther, reload + 0xb, sizeof(heldOther)) || heldOther > 1 ||
		!SafeCopy(&empty, reload + 0x17c, sizeof(empty)) ||
		!SafeCopy(&checkReload, base + 0x317f78, sizeof(checkReload)) || checkReload != reload ||
		!SafeCopy(&checkMetadata, reload, sizeof(checkMetadata)) || checkMetadata != metadata ||
		!SafeCopy(&checkPickups, reload + 0x15c, sizeof(checkPickups)) || checkPickups != pickups) return;
	// Reproduce the original reload updater's physical offhand selection,
	// including its explicit gunHand==-1 fallback, rather than a preference.
	const int hand = 1 - (state.gunHand == -1 ? 1 : state.gunHand);
	int location = static_cast<int>(tg::VrSlotLocation::Shoulders);
	if (posesFresh && tg::IsVrPhysicalHand(hand)) {
		std::array<float, 3> leftShoulder{}, rightShoulder{};
		if (SafeCopy(leftShoulder.data(), base + 0x317d18, sizeof(leftShoulder)) &&
			SafeCopy(rightShoulder.data(), base + 0x317d24, sizeof(rightShoulder))) {
			const int shoulder = tg::ResolveShoulderLocation(g_bodyPoses.hands[hand], leftShoulder, rightShoulder);
			if (tg::IsVrShoulder(shoulder)) location = shoulder;
		}
	}
	const tg::VrAmmoPouchSample sample{reinterpret_cast<std::uintptr_t>(reload), pickups, hand, location,
		reinterpret_cast<std::uintptr_t>(metadata)};
	for (const auto& event : g_ammoPouchObserver.Observe(sample)) {
		if (g_slotEvents.size() == 32) g_slotEvents.pop_front();
		g_slotEvents.push_back(event);
	}
}

// The input action side is cleared by the VR module every frame. Capture it
// here instead of asking the slower script poll to reconstruct slot actions.
void ObserveVrSlots(int projectileTicket)
{
	bool clientReady{};
	{
		std::lock_guard lock(g_stateMutex);
		clientReady = g_vms[1].receivedEvent;
	}
	if (!clientReady) { ResetSlotObservation(); return; }
	tg::VrHandState state;
	int actionHand = -1, grenadeActionHand = -1;
	if (!ReadVrSnapshot(state, &actionHand, &grenadeActionHand)) { ResetSlotObservation(); return; }
	std::lock_guard lock(g_slotMutex);
	if (projectileTicket >= 0) g_projectileTicket = projectileTicket;
	int takeShoulder = -1, releaseShoulder = -1;
	const auto now = GetTickCount64();
	const bool posesFresh = g_bodyPoses.sampledAt && now >= g_bodyPoses.sampledAt && now - g_bodyPoses.sampledAt <= 100;
	if (posesFresh) {
		std::array<float, 3> leftShoulder{}, rightShoulder{};
		const auto* base = reinterpret_cast<const std::byte*>(g_vrModule);
		if (SafeCopy(leftShoulder.data(), base + 0x317d18, sizeof(leftShoulder)) &&
			SafeCopy(rightShoulder.data(), base + 0x317d24, sizeof(rightShoulder))) {
			if (tg::IsVrPhysicalHand(state.gunHand))
				takeShoulder = tg::ResolveShoulderLocation(g_bodyPoses.hands[state.gunHand], leftShoulder, rightShoulder);
			// Auto-holster can occur away from the shoulders. Only a release
			// within 25 cm selects a new shoulder; otherwise reuse its draw site.
			if (tg::IsVrPhysicalHand(actionHand))
				releaseShoulder = tg::ResolveShoulderLocation(g_bodyPoses.hands[actionHand], leftShoulder, rightShoulder, 0.25f / 0.0254f);
		}
	}
	for (const auto& event : g_slotObserver.Observe(state, actionHand, takeShoulder, releaseShoulder)) {
		if (g_slotEvents.size() == 32) g_slotEvents.pop_front();
		g_slotEvents.push_back(event);
	}
	const int sourceKind = posesFresh && tg::IsVrPhysicalHand(state.grenadeHand) ?
		g_bodyPoses.pickupKinds[state.grenadeHand] : -2;
	int pendingThrow = -1;
	const bool stored = posesFresh && tg::IsVrPhysicalHand(grenadeActionHand) && g_bodyPoses.grenadeConfirmed &&
		g_bodyPoses.grenadeHand == grenadeActionHand && g_bodyPoses.grenadeTicket == state.grenadeTicket &&
		g_projectileTicket != state.grenadeTicket &&
		GrenadeStoreInputsStillMatch(g_bodyPoses, grenadeActionHand) &&
		SafeCopy(&pendingThrow, reinterpret_cast<const std::byte*>(g_vrModule) + 0x317d90, sizeof(pendingThrow)) && pendingThrow == 0;
	for (const auto& event : g_grenadeSlotObserver.Observe(state, grenadeActionHand, sourceKind, stored)) {
		if (g_slotEvents.size() == 32) g_slotEvents.pop_front();
		g_slotEvents.push_back(event);
	}
	ObserveVrAmmoPouch(state, posesFresh);
}

int PopVrSlotEvent()
{
	std::lock_guard lock(g_slotMutex);
	if (g_slotEvents.empty()) return -1;
	const auto event = g_slotEvents.front();
	g_slotEvents.pop_front();
	return 0x8000 | event.hand | (event.stored ? 2 : 0) | (event.slot << 2) | (event.location << 5) |
		((event.ticket & 0x3fff) << 16);
}

northstar::SqResult ReturnVrHandState(const Api& api, northstar::Sqvm* vm, int state)
{
	if (!api.pushInteger) return northstar::SqNull;
	__try { api.pushInteger(vm, state); return 1; }
	__except (EXCEPTION_EXECUTE_HANDLER) { return northstar::SqError; }
}

northstar::SqResult ReturnVrHapticAmplitude(const Api& api, northstar::Sqvm* vm, float amplitude)
{
	if (!api.pushFloat) return northstar::SqNull;
	__try { api.pushFloat(vm, amplitude); return 1; }
	__except (EXCEPTION_EXECUTE_HANDLER) { return northstar::SqError; }
}

bool LoadApi(HMODULE module, int context)
{
	if (!module) return false;
	{
		std::lock_guard lock(g_stateMutex);
		if (g_apis[context].module == module) return true;
	}
	Api api{};
	if (!ValidateModule(module, context, api)) return false;
	std::lock_guard lock(g_stateMutex);
	g_apis[context] = api;
	return true;
}

// Save restore can replace wrapper->sqvm without another OnSqvmCreated callback.
// Never dereference the root pointer cached before the save was restored.
northstar::Sqvm* CurrentRoot(northstar::ScriptVm* wrapper, int context)
{
	northstar::ScriptVm view{};
	void* sharedState{};
	if (!SafeCopy(&view, wrapper, sizeof(view)) || view.context != context || !view.sqvm ||
		!SafeCopy(&sharedState, reinterpret_cast<const std::byte*>(view.sqvm) +
			offsetof(northstar::SqvmPrefix, sharedState), sizeof(sharedState)) || !sharedState) return nullptr;
	return view.sqvm;
}

int FindFunction(const Api& api, northstar::Sqvm* vm, const char* name)
{
	northstar::ScriptObject object{};
	__try { return api.getFunction(vm, name, &object, nullptr); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return -2; }
}

bool CallBootstrap(const Api& api, northstar::ScriptVm* wrapper)
{
	__try { return api.callScriptInit(wrapper, "TrueGear_ServerBootstrap"); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Run only from Northstar's game-thread frame callback. A loaded save can skip
// CodeCallback_MapSpawn and replace the script set with one from the old save.
void BootstrapServer(ULONGLONG now)
{
	Api api{};
	VmBinding binding{};
	{
		std::lock_guard lock(g_stateMutex);
		auto& state = g_vms[0];
		// CLIENT's first native event proves campaign script initialization has
		// reached MapSpawn. Do not enter the server interpreter during save restore.
		if (!g_vms[1].receivedEvent || !state.wrapper || now < state.nextBootstrapLookup) return;
		api = g_apis[0];
		if (!api.getFunction || !api.callScriptInit) return;
		state.nextBootstrapLookup = now + 250;
		binding = state;
	}
	auto* current = CurrentRoot(binding.wrapper, 0);
	if (!current) return; // Restore is still in progress; retry without an engine call.
	bool refreshed{};
	{
		std::lock_guard lock(g_stateMutex);
		auto& state = g_vms[0];
		if (state.wrapper != binding.wrapper) return;
		if (state.vm != current) {
			state.vm = current;
			state.bootstrapAttempted = false;
			state.receivedEvent = false;
			refreshed = true;
		}
		if (state.bootstrapAttempted) return;
		binding = state;
	}
	if (refreshed) {
		tg::Reset(0, "server_vm_restored");
	}
	const int found = FindFunction(api, binding.vm, "TrueGear_ServerBootstrap");
	{
		std::lock_guard lock(g_stateMutex);
		auto& state = g_vms[0];
		if (state.wrapper != binding.wrapper || state.vm != binding.vm) return;
		state.bootstrapAttempted = true;
	}
	if (found == -1) return;
	// Never hold the VM lock while entering Squirrel: initialization sends events.
	if (found == 0) CallBootstrap(api, binding.wrapper);
}

template<int Context, int Operation> northstar::SqResult Receive(northstar::Sqvm* vm)
{
	try
	{
		Api api{};
		northstar::Sqvm* root{};
		northstar::ScriptVm* wrapper{};
		{
			std::lock_guard lock(g_stateMutex);
			api = g_apis[Context];
			wrapper = g_vms[Context].wrapper;
		}
		root = CurrentRoot(wrapper, Context);
		if (!api.module || !northstar::IsVmFromRoot(vm, root, SafeCopy)) return northstar::SqNull;
		if constexpr (Operation == 9) {
			if constexpr (Context == 0) return ReturnVrHandState(api, vm, 0);
			const unsigned char* arguments{};
			std::uint32_t maskType{}, eventType{};
			int mask{};
			if (!SafeCopy(&arguments, reinterpret_cast<const unsigned char*>(vm) + 0x48, sizeof(arguments)) || !arguments ||
				!SafeCopy(&maskType, arguments + 16, sizeof(maskType)) || maskType != 0x05000002 ||
				!SafeCopy(&mask, arguments + 24, sizeof(mask)) || mask < 1 || mask > 3 ||
				!SafeCopy(&eventType, arguments + 32, sizeof(eventType)) || eventType != 0x08000010)
				return ReturnVrHandState(api, vm, 0);
			std::string event;
			if (!CopyBoundedText(SafeGetString(api.getString, vm, 2), event)) return ReturnVrHandState(api, vm, 0);
			const auto source = event == "vr_item_grabbed" ? tg::CustomHapticSource::ItemGrab :
				event == "vr_item_released" ? tg::CustomHapticSource::ItemRelease : tg::CustomHapticSource::Unknown;
			if (source == tg::CustomHapticSource::Unknown) return ReturnVrHandState(api, vm, 0);
			std::int32_t originalResult{};
			if (!tg::TryMappedScriptPulse(g_vrModule, vm, source, originalResult)) return ReturnVrHandState(api, vm, 0);
			// A called original must never be retried by the script fallback.
			if (originalResult < 0) return originalResult;
			return ReturnVrHandState(api, vm, 1);
		}
		if constexpr (Operation == 4) return ReturnVrHandState(api, vm, ReadVrHandState());
		if constexpr (Operation == 5) {
			if constexpr (Context == 0) return ReturnVrHandState(api, vm, -1);
			ObserveVrSlots();
			return ReturnVrHandState(api, vm, PopVrSlotEvent());
		}
		if constexpr (Operation == 7) {
			if constexpr (Context == 0) return ReturnVrHandState(api, vm, -1);
			return ReturnVrHandState(api, vm, tg::PopVrHapticEvent());
		}
		if constexpr (Operation == 8) {
			if constexpr (Context == 0) return ReturnVrHapticAmplitude(api, vm, 0.0f);
			return ReturnVrHapticAmplitude(api, vm, tg::LastVrHapticAmplitude());
		}
		// The official SQVM layout stores the current argument stack at 0x48.
		// Check the first argument's type before using the engine's unchecked getter.
		const unsigned char* stack{};
		std::uint32_t type{};
		if (!SafeCopy(&stack, reinterpret_cast<const unsigned char*>(vm) + 0x48, sizeof(stack)) || !stack ||
			!SafeCopy(&type, stack + 16, sizeof(type)) || type != 0x08000010)
			return ScriptError(api, vm, "TrueGear requires one string argument.");
		std::string text;
		if (!CopyBoundedText(SafeGetString(api.getString, vm), text))
			return ScriptError(api, vm, "TrueGear text must contain 1 to 32768 bytes.");
		if constexpr (Operation == 1) {
			tg::Reset(Context, text.c_str());
			if constexpr (Context == 1) {
				ResetSlotObservation();
				tg::ResetVrHapticEvents();
			}
		}
		else if constexpr (Operation == 2) {
			if (!tg::SendPlay(Context, text.c_str()))
				return ScriptError(api, vm, "TrueGear event name must contain 1 to 256 ASCII letters, digits, or underscores.");
		}
		else if constexpr (Operation == 3) {
			if (!tg::SendPlayEffectByContent(Context, text.c_str()))
				return ScriptError(api, vm, "Unable to queue TrueGear effect JSON.");
		}
		else if constexpr (Operation == 6) {
			if constexpr (Context == 1) {
				if (!UpdateBodyPoses(text)) return ScriptError(api, vm, "TrueGear body poses require finite world vectors and valid pickup kinds.");
			}
		}
		else {
			if (!tg::Submit(Context, text.c_str()))
				return ScriptError(api, vm, "Unable to queue TrueGear event JSON.");
			{
				std::lock_guard lock(g_stateMutex);
				g_vms[Context].receivedEvent = true;
			}
		}
		return northstar::SqNull;
	}
	catch (...)
	{
		return northstar::SqNull;
	}
}

template<int Context, int Operation> northstar::FunctionRegistration& Registration()
{
	static northstar::FunctionRegistration registration = []
	{
		northstar::FunctionRegistration value{};
		constexpr const char* names[] = {"TrueGear_SubmitEvent", "TrueGear_Reset", "TrueGear_SendPlay", "TrueGear_SendPlayEffectByContent", "TrueGear_GetVRHandState", "TrueGear_GetVRSlotEvent", "TrueGear_SetVRBodyPoses", "TrueGear_GetVRHapticEvent", "TrueGear_GetVRHapticAmplitude", "TrueGear_TryPulseHandsMapped"};
		value.squirrelName = names[Operation];
		value.nativeName = value.squirrelName;
		value.help = Operation == 9 ? "Run one original item pulse with synchronous source identity; zero means the caller should use the original fallback." : Operation == 8 ? "Read the selected non-weapon amplitude of the last drained controller vibration: zero when unavailable." : Operation == 7 ? "Drain accepted non-weapon controller vibration observations: 0 left, 1 right, -1 none." : Operation == 6 ? "Observe world hand poses for body-slot location." : Operation == 5 ? "Drain verified body gun-slot actions captured each frame." : Operation == 4 ? "Read verified physical gun, support grip and grenade hand state." : Operation == 1 ? "Stop and clear TrueGear event state." :
			(Operation == 2 ? "Play a named event from TrueGear Player." :
			(Operation == 3 ? "Play effect content using TrueGear SDK JSON." : "Queue a structured TrueGear event."));
		constexpr bool getter = Operation == 4 || Operation == 5 || Operation == 7 || Operation == 8 || Operation == 9;
		value.returnTypeName = Operation == 8 ? "float" : getter ? "int" : "void";
		// Official Northstar eSQReturnType::Float is 0x1; Integer is 0x5.
		value.returnType = Operation == 8 ? 0x1 : getter ? 0x5 : 0x20;
		value.argumentTypes = Operation == 9 ? "int sideMask, string eventName" : getter ? "" : (Operation == 1 ? "string reason" : (Operation == 2 ? "string uuid" : "string json"));
		value.function = Receive<Context, Operation>;
		return value;
	}();
	return registration;
}

class Identity final : public northstar::PluginId
{
	const char* GetString(northstar::PluginString property) override
	{
		switch (property)
		{
		case northstar::PluginString::Name: return "TrueGear.Native";
		case northstar::PluginString::LogName: return "TRUEGEAR";
		case northstar::PluginString::DependencyName: return "TRUEGEAR_NATIVE";
		default: return nullptr;
		}
	}
	std::int64_t GetField(northstar::PluginField property) override
	{
		return property == northstar::PluginField::Context ? 0x2 : 0;
	}
};

class Callbacks final : public northstar::PluginCallbacks
{
	void Init(HMODULE module, const northstar::PluginNorthstarData* data, bool) override
	{
		try
		{
			g_northstar = module;
			if (data && data->pluginHandle) g_self = data->pluginHandle;
			if (const auto create = reinterpret_cast<northstar::CreateInterface>(GetProcAddress(module, "CreateInterface")))
				g_system = static_cast<northstar::System*>(create("NSSys001", nullptr));
			HMODULE pinned{};
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
				reinterpret_cast<LPCWSTR>(&Log), &pinned))
			{
				return;
			}
			tg::Initialize(g_self, Log);
		}
		catch (...) { }
	}
	void Finalize() override { FlushLogs(); }
	bool Unload() override
	{
		FlushLogs();
		return false;
	}
	void OnSqvmCreated(northstar::ScriptVm* wrapper) override
	{
		try
		{
			northstar::ScriptVm view{};
			if (!SafeCopy(&view, wrapper, sizeof(view)) || !view.sqvm || view.context < 0 || view.context > 1) return;
			const int context = view.context;
			if (!LoadApi(GetModuleHandleW(Builds[context].moduleName), context)) return;
			Api api{};
			{
				std::lock_guard lock(g_stateMutex);
				if (g_vms[context].wrapper == wrapper && g_vms[context].vm == view.sqvm) return;
				api = g_apis[context];
			}
			auto& submit = context == 0 ? Registration<0, false>() : Registration<1, false>();
			auto& reset = context == 0 ? Registration<0, true>() : Registration<1, true>();
			auto& play = context == 0 ? Registration<0, 2>() : Registration<1, 2>();
			auto& content = context == 0 ? Registration<0, 3>() : Registration<1, 3>();
			auto& hands = context == 0 ? Registration<0, 4>() : Registration<1, 4>();
			auto& slots = context == 0 ? Registration<0, 5>() : Registration<1, 5>();
			auto& poses = context == 0 ? Registration<0, 6>() : Registration<1, 6>();
			auto& haptics = context == 0 ? Registration<0, 7>() : Registration<1, 7>();
			auto& amplitude = context == 0 ? Registration<0, 8>() : Registration<1, 8>();
			auto& mappedPulse = context == 0 ? Registration<0, 9>() : Registration<1, 9>();
			if (!SafeRegister(api.registerFunction, wrapper, &submit) || !SafeRegister(api.registerFunction, wrapper, &reset) ||
				!SafeRegister(api.registerFunction, wrapper, &play) || !SafeRegister(api.registerFunction, wrapper, &content) ||
				!SafeRegister(api.registerFunction, wrapper, &hands) || !SafeRegister(api.registerFunction, wrapper, &slots) ||
				!SafeRegister(api.registerFunction, wrapper, &poses) || !SafeRegister(api.registerFunction, wrapper, &haptics) ||
				!SafeRegister(api.registerFunction, wrapper, &amplitude) || !SafeRegister(api.registerFunction, wrapper, &mappedPulse))
			{
				return;
			}
			{
				std::lock_guard lock(g_stateMutex);
				g_vms[context] = {wrapper, view.sqvm};
			}
			tg::BeginVm(context, wrapper);
			if (context == 1) {
				ResetSlotObservation();
				tg::ResetVrHapticEvents();
			}
		}
		catch (...) { }
	}
	void OnSqvmDestroying(northstar::ScriptVm* wrapper) override
	{
		try
		{
			for (int context = 0; context < 2; ++context)
			{
				bool matched{};
				{
					std::lock_guard lock(g_stateMutex);
					matched = g_vms[context].wrapper == wrapper;
					if (matched) g_vms[context] = {};
				}
				if (matched) {
					tg::EndVm(context, wrapper);
					if (context == 1) {
						ResetSlotObservation();
						tg::ResetVrHapticEvents();
					}
				}
			}
		}
		catch (...) { }
	}
	void OnLibraryLoaded(HMODULE, const char*) override {}
	void RunFrame() override {
		BootstrapServer(GetTickCount64());
		ObserveVrSlots();
		// XR may initialize after the plugin and VMs. Only pass the full-build
		// validated, currently loaded VR module to the independent output bridge.
		northstar::ScriptVm* clientVm{};
		{ std::lock_guard lock(g_stateMutex); clientVm = g_vms[1].wrapper; }
		if (clientVm && CurrentRoot(clientVm, 1) && g_vrModule && GetModuleHandleW(L"Titanfall2VR.dll") == g_vrModule) {
			HMODULE clientModule{};
			{ std::lock_guard lock(g_stateMutex); clientModule = g_apis[1].module; }
			if (clientModule && GetModuleHandleW(L"client.dll") == clientModule) {
				tg::InstallMappedRumbleObservers(g_vrModule, clientModule);
				tg::InstallCustomHapticBridge(g_vrModule, clientModule);
			}
			tg::InstallVrHapticBridge(g_vrModule);
		}
		FlushLogs();
	}
};

Identity g_identity;
Callbacks g_callbacks;
} // namespace

extern "C" __declspec(dllexport) void* CreateInterface(const char* name, int* status)
{
	void* result{};
	if (name && !std::strcmp(name, "PluginId001")) result = &g_identity;
	else if (name && !std::strcmp(name, "PluginCallbacks001")) result = &g_callbacks;
	if (status) *status = result ? 0 : 1;
	return result;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		g_self = instance;
		// The statically linked CRT requires thread attach/detach notifications.
	}
	return TRUE;
}
