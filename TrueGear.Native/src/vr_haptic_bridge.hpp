#pragma once

namespace tg {
// Caller verifies the complete original VR module identity before passing it.
bool InstallVrHapticBridge(void* validatedVrModule);
int PopVrHapticEvent();
// Selected non-weapon amplitude from the last Pop, invalidated by failure/reset.
float LastVrHapticAmplitude();
void ResetVrHapticEvents();
}
