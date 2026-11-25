#pragma once
#include "settings_protection.h"
#include <filesystem>
#include <optional>

namespace redesk::host::windows {
enum class StoreMode { Portable, Installed };
enum class SaveMode { Replace, CreateOnly };

// Portable means installation-free, not transferable between machines.
// All operations resolve the root from Windows Known Folders, never the EXE.
// Caller serializes writes; installed operations are SYSTEM/admin only.
std::filesystem::path settings_directory(StoreMode mode);
std::optional<HostSettings> load_settings(StoreMode mode);
void save_settings(StoreMode mode, const HostSettings& settings,
                   SaveMode save_mode = SaveMode::Replace);
}
