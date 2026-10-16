#pragma once

#include <Windows.h>
#include <cstddef>
#include <cstdint>

// Minimal ABI declarations derived from the official NorthstarLauncher headers.
// See vendor/NorthstarPluginSDK/SOURCES.md and its retained MIT license.
// These interfaces have no virtual destructor: adding one changes the ABI.
namespace northstar
{
enum class PluginString : int { Name = 0, LogName = 1, DependencyName = 2 };
enum class PluginField : int { Context = 0, Color = 1 };
enum class LogLevel : int { Info = 0, Warning = 1, Error = 2 };

struct PluginNorthstarData
{
	HMODULE pluginHandle;
	std::uint64_t size;
};

struct Sqvm;
// SQVM coroutines have distinct addresses but share this state with their root.
// Layout source: official vscript/languages/squirrel_re/squirrel/sqvm.h.
struct SqvmPrefix
{
	std::byte reserved0[0x48];
	void* argumentStack;
	void* sharedState;
};
static_assert(offsetof(SqvmPrefix, argumentStack) == 0x48);
static_assert(offsetof(SqvmPrefix, sharedState) == 0x50);

using MemoryReader = bool (*)(void* destination, const void* source, std::size_t bytes);
inline bool IsVmFromRoot(const Sqvm* candidate, const Sqvm* root, MemoryReader read)
{
	if (!candidate || !root || !read) return false;
	void* candidateState{};
	void* rootState{};
	constexpr auto offset = offsetof(SqvmPrefix, sharedState);
	return read(&candidateState, reinterpret_cast<const std::byte*>(candidate) + offset, sizeof(candidateState)) &&
		read(&rootState, reinterpret_cast<const std::byte*>(root) + offset, sizeof(rootState)) &&
		rootState && rootState == candidateState;
}

struct ScriptVm
{
	std::byte reserved0[8];
	Sqvm* sqvm;
	std::byte reserved10[0x2c];
	std::int32_t context;
};
static_assert(offsetof(ScriptVm, sqvm) == 0x8);
static_assert(offsetof(ScriptVm, context) == 0x3c);

class PluginId
{
public:
	virtual const char* GetString(PluginString property) = 0;
	virtual std::int64_t GetField(PluginField property) = 0;
};

class PluginCallbacks
{
public:
	virtual void Init(HMODULE northstarModule, const PluginNorthstarData* data, bool reloaded) = 0;
	virtual void Finalize() = 0;
	virtual bool Unload() = 0;
	virtual void OnSqvmCreated(ScriptVm* vm) = 0;
	virtual void OnSqvmDestroying(ScriptVm* vm) = 0;
	virtual void OnLibraryLoaded(HMODULE module, const char* name) = 0;
	virtual void RunFrame() = 0;
};

class System
{
public:
	virtual void Log(std::int64_t unused, LogLevel level, char* message) = 0;
	virtual void Unload(std::int64_t unused) = 0;
	virtual void Reload(std::int64_t unused) = 0;
};

using CreateInterface = void* (*)(const char* name, int* status);
using SqResult = long;
using SqFunction = SqResult (*)(Sqvm*);
constexpr SqResult SqNull = 0;
constexpr SqResult SqError = -1;

struct FunctionRegistration
{
	const char* squirrelName{};
	const char* nativeName{};
	const char* help{};
	const char* returnTypeName{};
	const char* argumentTypes{};
	std::uint32_t unknown1{};
	std::uint32_t devLevel{};
	const char* shortName{};
	std::uint32_t unknown2{};
	std::uint32_t returnType{0x20};
	std::uint32_t* externalBuffer{};
	std::uint64_t externalBufferSize{};
	std::uint64_t unknown3{};
	std::uint64_t unknown4{};
	SqFunction function{};
};
static_assert(sizeof(FunctionRegistration) == 0x68);
static_assert(offsetof(FunctionRegistration, function) == 0x60);

using RegisterFunction = std::int64_t (*)(ScriptVm*, FunctionRegistration*, char);
using GetString = const char* (*)(Sqvm*, long);
using RaiseError = long (*)(Sqvm*, const char*);
using PushInteger = void (*)(Sqvm*, long);
using PushFloat = void (*)(Sqvm*, float);
struct ScriptObject
{
	std::uint32_t type{};
	std::uint32_t structNumber{};
	std::uint64_t value{};
};
static_assert(sizeof(ScriptObject) == 16);
using GetFunction = int (*)(Sqvm*, const char*, ScriptObject*, const char*);
using CallScriptInit = bool (*)(ScriptVm*, const char*);
} // namespace northstar
