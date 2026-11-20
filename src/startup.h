#pragma once
#include "core.h"

namespace snooze {
std::wstring startup_command();
bool startup_registered(const Paths& paths);
enum class StartupState { Off, On, DisabledByWindows, Unknown };
StartupState startup_state(const Paths& paths);
bool startup_enabled(const Paths& paths);
void require_standard_user();
struct StartupLock {
    Handle mutex;
    StartupLock();
    ~StartupLock();
};
void set_startup(const Paths& paths, bool enabled);
struct StartupEntry {
    enum class Kind { Run, Shortcut, Task } kind;
    std::wstring name;
    std::wstring command;
    DWORD view = 0, type = 0;
    std::vector<BYTE> data;
    std::wstring executable;
};
std::vector<StartupEntry> find_zalo_startup();
void disable_zalo_startup(const Paths& paths, const std::vector<StartupEntry>& entries);
fs::path startup_folder();
void restore_zalo_startup(const Paths& paths, const fs::path& shortcut_directory);
void restore_startup_shortcut(const fs::path& target, const fs::path& backup);
bool startup_backup_exists(const Paths& paths);
void remove_legacy_shortcut(const Paths& paths, const fs::path& shortcut_directory);
void configure_startup(HWND owner, const Paths& paths, Config& config, bool enabled);
void apply_startup(const Paths& paths, Config& config, bool enabled,
                   const std::vector<StartupEntry>& handoff = {});
std::wstring task_restore_signature(std::wstring xml);
} // namespace snooze
