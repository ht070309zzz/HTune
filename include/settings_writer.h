#pragma once
#include "settings.h"
#include "themes.h"
#include <cstdint>
#include <memory>
#include <optional>

namespace stage {
struct PrivateThemeSnapshotResult {
    std::filesystem::path file;
    std::wstring diagnostic;
    bool ok=false,reused=false;
};
// Application-private theme files are immutable once referenced by committed
// settings. A matching reusable snapshot is returned as-is; otherwise a new
// unique file is created without replacing any existing file.
bool IsApplicationPrivateThemeFile(const std::filesystem::path& file);
bool IsApplicationPrivateThemeDestination(const std::filesystem::path& file);
PrivateThemeSnapshotResult StageApplicationPrivateTheme(const ThemeDefinition& theme,
    const std::filesystem::path& reusableFile={});
struct SettingsSaveRequest {
    uint64_t revision=0;
    AppSettings settings;
    std::optional<ThemeDefinition> customTheme;
    bool themeRestorePending=false;
};
struct SettingsSaveResult {
    uint64_t revision=0;
    bool ok=false,settingsCommitted=false,themeCommitted=false,themePending=false;
    std::filesystem::path privateThemeFile;
    std::wstring customThemePath,diagnostic;
};
struct TrackPreferencesSaveRequest {
    uint64_t requestId=0,revision=0;
    TrackLyricPreferences preferences;
};
struct TrackPreferencesSaveResult {
    uint64_t requestId=0,revision=0;
    bool ok=false;
    std::wstring diagnostic;
};
// One fixed thread and one active request total. Each file has one replaceable
// latest pending snapshot and a result slot; when both wait, service alternates.
// No callbacks or references to the App/UI are retained.
class SettingsSaveWorker {
public:
    SettingsSaveWorker();
    ~SettingsSaveWorker();
    SettingsSaveWorker(const SettingsSaveWorker&)=delete;
    SettingsSaveWorker& operator=(const SettingsSaveWorker&)=delete;
    bool Submit(SettingsSaveRequest request);
    std::optional<SettingsSaveResult> Poll();
    bool SubmitPreferences(TrackPreferencesSaveRequest request);
    std::optional<TrackPreferencesSaveResult> PollPreferences();
    // Blocking only for explicit file actions and shutdown, never normal UI
    // auto-save. Ordinary file I/O is not guaranteed interruptible.
    void WaitIdle();
    void Stop(); // Reject new work, drain both accepted latest snapshots, join.
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
