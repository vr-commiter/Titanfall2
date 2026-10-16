#pragma once

namespace tg {
// Both module identities are validated by the caller. Original output remains
// unchanged; these observers only publish an independently filtered copy.
bool InstallMappedRumbleObservers(void* validatedVrModule, void* validatedClientModule);
bool ReadMappedRumble(void* vrModule, float all, float nonWeapon, float& filtered) noexcept;
void ResetMappedRumble() noexcept;
}
