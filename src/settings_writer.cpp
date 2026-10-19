#include "settings_writer.h"
#include "file_transaction.h"
#include <windows.h>
#include <objbase.h>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace stage {
namespace {
bool OrdinalEqualsInsensitive(std::wstring_view left,std::wstring_view right) {
    if(left.size()>static_cast<size_t>(std::numeric_limits<int>::max())||
       right.size()>static_cast<size_t>(std::numeric_limits<int>::max()))return false;
    return CompareStringOrdinal(left.data(),static_cast<int>(left.size()),
        right.data(),static_cast<int>(right.size()),TRUE)==CSTR_EQUAL;
}

bool OrdinalStartsWithInsensitive(std::wstring_view value,std::wstring_view prefix) {
    return value.size()>=prefix.size()&&
        OrdinalEqualsInsensitive(value.substr(0,prefix.size()),prefix);
}

bool SameDirectoryObject(const std::filesystem::path& left,const std::filesystem::path& right) {
    const auto normalizedLeft=left.lexically_normal();
    const auto normalizedRight=right.lexically_normal();
    detail::DirectoryTreeLock leftLock,rightLock;DWORD leftError=ERROR_SUCCESS,rightError=ERROR_SUCCESS;
    if(!leftLock.Acquire(normalizedLeft,leftError)||!rightLock.Acquire(normalizedRight,rightError))return false;
    return detail::SameFileObject(leftLock.FinalHandle(),rightLock.FinalHandle());
}

bool PrivateThemeDirectoryAllowed(const std::filesystem::path& parent,const std::filesystem::path& directory) {
    return !parent.empty()&&SameDirectoryObject(parent,directory);
}

bool PrivateThemeHintAllowed(const std::filesystem::path& hint,const std::filesystem::path& directory) {
    const auto normalizedHint=hint.lexically_normal();
    const auto normalizedDirectory=directory.lexically_normal();
    if(!PrivateThemeDirectoryAllowed(normalizedHint.parent_path(),normalizedDirectory))return false;
    const auto name=normalizedHint.filename().wstring();
    return OrdinalEqualsInsensitive(name,L"current-custom.json")||
        ((OrdinalStartsWithInsensitive(name,L"recovered-custom-")||
          OrdinalStartsWithInsensitive(name,L"private-custom-"))&&
         OrdinalEqualsInsensitive(normalizedHint.extension().wstring(),L".json"));
}

bool SameThemeColor(const ThemeColor& left,const ThemeColor& right) {
    return left.r==right.r&&left.g==right.g&&left.b==right.b&&left.a==right.a;
}

std::filesystem::path ComparableThemePath(std::filesystem::path path,const std::filesystem::path& parent) {
    if(path.empty())return {};
    std::error_code error;
    if(!path.is_absolute())path=std::filesystem::absolute(parent/path,error);
    return error?std::filesystem::path{}:path.lexically_normal();
}

bool SameTheme(const ThemeDefinition& left,const ThemeDefinition& right,const std::filesystem::path& file) {
    if(left.name!=right.name||left.baseStyle!=right.baseStyle||left.response!=right.response||
       left.lyricSize!=right.lyricSize||left.reducedMotion!=right.reducedMotion||
       left.lyricDisplayMode!=right.lyricDisplayMode||left.backgroundShade!=right.backgroundShade||
       left.cameraMotion!=right.cameraMotion||left.backgroundKind!=right.backgroundKind||
       !SameThemeColor(left.background,right.background)||!SameThemeColor(left.panel,right.panel)||
       !SameThemeColor(left.ink,right.ink)||!SameThemeColor(left.muted,right.muted)||
       !SameThemeColor(left.accent,right.accent)||!SameThemeColor(left.glow,right.glow))return false;
    return left.backgroundKind==ThemeBackgroundKind::None||
        ComparableThemePath(left.backgroundPath,file.parent_path())==ComparableThemePath(right.backgroundPath,file.parent_path());
}

struct PrivateThemeCommitState {
    bool initialized=false;
    std::filesystem::path committed,staged;
};

void InitializePrivateThemeState(PrivateThemeCommitState& state,const AppSettings& settings) {
    if(state.initialized)return;
    state.initialized=true;
    if(settings.customThemePath.empty())return;
    const auto configured=std::filesystem::path(settings.customThemePath).lexically_normal();
    const auto directory=(UserDataDirectory()/L"themes").lexically_normal();
    if(PrivateThemeHintAllowed(configured,directory))state.committed=configured;
}
}

bool IsApplicationPrivateThemeFile(const std::filesystem::path& file) {
    if(file.empty())return false;
    return PrivateThemeHintAllowed(file.lexically_normal(),(UserDataDirectory()/L"themes").lexically_normal());
}

bool IsApplicationPrivateThemeDestination(const std::filesystem::path& file) {
    if(file.empty())return false;
    return PrivateThemeDirectoryAllowed(file.lexically_normal().parent_path(),
        (UserDataDirectory()/L"themes").lexically_normal());
}

PrivateThemeSnapshotResult StageApplicationPrivateTheme(const ThemeDefinition& theme,
    const std::filesystem::path& reusableFile) {
    PrivateThemeSnapshotResult result;
    try{
        const auto directory=(UserDataDirectory()/L"themes").lexically_normal();
        detail::DirectoryTreeLock directoryLock;DWORD directoryError=ERROR_SUCCESS;
        if(!directoryLock.Acquire(directory,directoryError,true)){
            result.diagnostic=L"私有主题目录不是稳定的本地普通目录（Windows "+
                std::to_wstring(directoryError)+L"）";return result;
        }
        if(!reusableFile.empty()){
            const auto reusable=reusableFile.lexically_normal();
            if(PrivateThemeHintAllowed(reusable,directory)){
                const auto loaded=LoadTheme(reusable);
                if(loaded.ok&&SameTheme(loaded.definition,theme,reusable)){
                    result.file=reusable;result.ok=true;result.reused=true;return result;
                }
            }
        }
        GUID guid{};wchar_t token[40]{};
        if(FAILED(CoCreateGuid(&guid))||!StringFromGUID2(guid,token,40)){
            result.diagnostic=L"无法生成新的私有主题文件名";return result;
        }
        const auto file=directory/(std::wstring(L"private-custom-")+token+L".json");
        const auto saved=SaveThemeNewFile(file,theme);
        if(!saved.ok){result.diagnostic=saved.diagnostic;return result;}
        result.file=file;result.ok=true;return result;
    }catch(...){result.diagnostic=L"自定义主题暂时无法写入新的私有文件";return result;}
}

namespace {
SettingsSaveResult SaveSnapshot(SettingsSaveRequest& request,PrivateThemeCommitState& themeState) {
    SettingsSaveResult result;result.revision=request.revision;
    result.customThemePath=request.settings.customThemePath;
    if(!request.settings.writable){result.diagnostic=L"原设置文件不作自动覆盖，本次调整仅在当前运行中生效";return result;}
    InitializePrivateThemeState(themeState,request.settings);
    std::wstring themeDiagnostic;
    std::filesystem::path preparedThemeFile;
    bool themePrepared=false;
    if(request.customTheme){
        try{
            const auto reusable=themeState.staged.empty()?themeState.committed:themeState.staged;
            auto prepared=StageApplicationPrivateTheme(*request.customTheme,reusable);
            if(!prepared.ok){result.themePending=true;themeDiagnostic=L"自定义主题尚未保存："+prepared.diagnostic;}
            else{
                preparedThemeFile=std::move(prepared.file);themePrepared=true;
                if(preparedThemeFile==themeState.committed)themeState.staged.clear();
                else themeState.staged=preparedThemeFile;
                request.settings.customThemePath=preparedThemeFile.wstring();
            }
        }catch(...){
            result.themePending=true;themeDiagnostic=L"自定义主题暂时无法保存，主题修改仍保留在运行中";
        }
        if(!themePrepared&&!themeState.committed.empty())request.settings.customThemePath=themeState.committed.wstring();
    }else if(!request.themeRestorePending)request.settings.customThemePath.clear();
    std::wstring settingsDiagnostic;
    result.settingsCommitted=SaveSettings(request.settings,settingsDiagnostic);
    if(result.settingsCommitted){
        if(request.customTheme&&themePrepared){
            themeState.committed=preparedThemeFile;themeState.staged.clear();
            result.privateThemeFile=themeState.committed;
            result.customThemePath=themeState.committed.wstring();result.themeCommitted=true;
        }else if(!request.customTheme&&!request.themeRestorePending){
            themeState.committed.clear();themeState.staged.clear();result.customThemePath.clear();
        }else if(!request.customTheme&&request.themeRestorePending){
            const auto configured=std::filesystem::path(request.settings.customThemePath).lexically_normal();
            themeState.committed=IsApplicationPrivateThemeFile(configured)?configured:std::filesystem::path{};
            themeState.staged.clear();
        }
    }
    if(!result.settingsCommitted&&settingsDiagnostic.empty())settingsDiagnostic=L"设置暂时无法保存，当前修改仍保留在运行中";
    if(!settingsDiagnostic.empty())result.diagnostic=std::move(settingsDiagnostic);
    if(!themeDiagnostic.empty()){
        if(!result.diagnostic.empty())result.diagnostic+=L" · ";
        result.diagnostic+=themeDiagnostic;
        if(result.settingsCommitted)result.diagnostic+=L"；其他设置已保存，将继续重试主题副本";
    }
    result.ok=result.settingsCommitted&&!result.themePending;
    return result;
}
}
struct SettingsSaveWorker::Impl {
    std::mutex mutex;
    std::condition_variable cv;
    std::optional<SettingsSaveRequest> pending;
    std::optional<SettingsSaveResult> completed;
    std::optional<TrackPreferencesSaveRequest> pendingPreferences;
    std::optional<TrackPreferencesSaveResult> completedPreferences;
    bool stopping=false,active=false,preferPreferences=false;
    PrivateThemeCommitState privateThemeState;
    std::thread thread;

    Impl(){thread=std::thread([this]{Run();});}
    ~Impl(){Stop();}
    void Run() {
        const auto initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        while(true){
            std::optional<SettingsSaveRequest> request;
            std::optional<TrackPreferencesSaveRequest> preferences;
            {
                std::unique_lock lock(mutex);cv.wait(lock,[this]{return stopping||pending.has_value()||pendingPreferences.has_value();});
                if(!pending&&!pendingPreferences)break;
                if(pendingPreferences&&(!pending||preferPreferences)){
                    preferences.swap(pendingPreferences);preferPreferences=false;
                }else{request.swap(pending);preferPreferences=true;}
                active=true;
            }
            if(preferences){
                TrackPreferencesSaveResult result;result.requestId=preferences->requestId;result.revision=preferences->revision;
                try{result.ok=SaveTrackLyricPreferences(preferences->preferences,result.diagnostic);}
                catch(...){result.diagnostic=L"歌词记忆保存任务暂时失败，当前修改仍保留在运行中";}
                if(!result.ok&&result.diagnostic.empty())result.diagnostic=L"歌词记忆暂时无法保存，当前修改仍保留在运行中";
                std::lock_guard lock(mutex);completedPreferences=std::move(result);active=false;
            }else{
                SettingsSaveResult result;
                try{result=SaveSnapshot(*request,privateThemeState);}
                catch(...){result.revision=request->revision;result.diagnostic=L"设置保存任务暂时失败，当前修改仍保留在运行中";}
                std::lock_guard lock(mutex);completed=std::move(result);active=false;
            }
            cv.notify_all();
        }
        if(SUCCEEDED(initialized))CoUninitialize();
    }
    void Stop() {
        {std::lock_guard lock(mutex);stopping=true;}
        cv.notify_all();if(thread.joinable())thread.join();
    }
};
SettingsSaveWorker::SettingsSaveWorker():impl_(std::make_unique<Impl>()){}
SettingsSaveWorker::~SettingsSaveWorker()=default;
bool SettingsSaveWorker::Submit(SettingsSaveRequest request) {
    std::optional<SettingsSaveRequest> retired;
    {
        std::lock_guard lock(impl_->mutex);if(impl_->stopping)return false;
        retired.swap(impl_->pending);impl_->pending=std::move(request);
    }
    impl_->cv.notify_all();return true;
}
std::optional<SettingsSaveResult> SettingsSaveWorker::Poll() {
    std::optional<SettingsSaveResult> result;
    {std::lock_guard lock(impl_->mutex);result.swap(impl_->completed);}
    return result;
}
bool SettingsSaveWorker::SubmitPreferences(TrackPreferencesSaveRequest request) {
    std::optional<TrackPreferencesSaveRequest> retired;
    {
        std::lock_guard lock(impl_->mutex);if(impl_->stopping)return false;
        retired.swap(impl_->pendingPreferences);impl_->pendingPreferences=std::move(request);
    }
    impl_->cv.notify_all();return true;
}
std::optional<TrackPreferencesSaveResult> SettingsSaveWorker::PollPreferences() {
    std::optional<TrackPreferencesSaveResult> result;
    {std::lock_guard lock(impl_->mutex);result.swap(impl_->completedPreferences);}
    return result;
}
void SettingsSaveWorker::WaitIdle() {
    std::unique_lock lock(impl_->mutex);
    impl_->cv.wait(lock,[this]{return !impl_->active&&!impl_->pending&&!impl_->pendingPreferences;});
}
void SettingsSaveWorker::Stop(){impl_->Stop();}
}
