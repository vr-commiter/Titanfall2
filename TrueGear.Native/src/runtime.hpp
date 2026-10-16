#pragma once
#include <Windows.h>

namespace tg {
using LogCallback = void (*)(int, const char*);
void Initialize(HMODULE self, LogCallback log);
void Diagnostic(int level, const char* message) noexcept;
void BeginVm(int context, void* token);
void EndVm(int context, void* token);
bool Submit(int context, const char* json);
bool SendPlay(int context, const char* uuid);
bool SendPlayEffectByContent(int context, const char* json);
void Reset(int context, const char* reason);
}
