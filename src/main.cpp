#include "playback.h"
#include "audio.h"
#include "lyrics.h"
#include "catalog_id.h"
#include "version.h"
#include "settings.h"
#include "optional_lyric_font.h"
#include "settings_writer.h"
#include "diagnostic_log.h"
#include "playback_log_sampling.h"
#include "renderer.h"
#include "stage_ui.h"
#include "stage_interface.h"
#include "settings_view.h"
#include "background.h"
#include "cover.h"
#include "themes.h"
#include "desktop_host.h"
#include "native_controls.h"
#include "resource.h"
#include "window_layout.h"
#include "window_chrome.h"
#include "manual_lyrics.h"
#include "local_file_limits.h"
#include "third_party/json.hpp"
#include <d3d11.h>
#include <dxgi1_3.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <imm.h>
#include <array>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <iomanip>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <stop_token>

using Microsoft::WRL::ComPtr;
using namespace stage;
using json = nlohmann::json;
namespace {
void ConfigureWindowChrome(HWND window) {
    // DWM keeps the native shadow and Windows 11 rounding while the client
    // covers the complete frame. These appearance hints are optional.
    const COLORREF border=DWMWA_COLOR_NONE;
    DwmSetWindowAttribute(window,DWMWA_BORDER_COLOR,&border,sizeof(border));
    const DWM_WINDOW_CORNER_PREFERENCE corners=DWMWCP_ROUND;
    DwmSetWindowAttribute(window,DWMWA_WINDOW_CORNER_PREFERENCE,&corners,sizeof(corners));
    const MARGINS shadow{1,1,1,1};DwmExtendFrameIntoClientArea(window,&shadow);
}
std::string Utf8(const std::wstring& s) {
    if(s.empty())return {}; int n=WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),nullptr,0,nullptr,nullptr);
    std::string r(n,'\0'); WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),r.data(),n,nullptr,nullptr); return r;
}
std::wstring Clock(int64_t ms) { std::wostringstream o; ms=std::max<int64_t>(0,ms)/1000; o<<ms/60<<L":"<<std::setw(2)<<std::setfill(L'0')<<ms%60; return o.str(); }
std::wstring DisplayText(std::wstring value){if(value.size()>512)value.resize(512);for(auto& c:value)if(c<32||c==127)c=L' ';return value;}
std::wstring PreferenceTitle(const PlaybackSnapshot& playback) {
    if(playback.provider==MusicProvider::Netease)return playback.title;
    // QQ metadata keys cannot inherit a NetEase correction for a different
    // recording with the same title. Verified catalog keys carry a provider prefix.
    return std::wstring(playback.provider==MusicProvider::SodaMusic?L"[SODA] ":playback.provider==MusicProvider::Kugou?L"[KG] ":L"[QQ] ")+playback.title+L" ["+playback.album+L"] "+std::to_wstring(playback.durationMs/1000);
}
std::wstring PlayerMatchKey(const PlaybackSnapshot& playback){return TrackMatchKey(PreferenceTitle(playback),playback.artist);}
std::wstring PlayerLyricKey(const PlaybackSnapshot& playback,const std::wstring& verified={}){
    return TrackLyricKey(verified,PreferenceTitle(playback),playback.artist);
}
int64_t LyricObservationBudgetMs(const PlaybackSnapshot& playback) {
    return playback.songIdVerified?500:1500;
}
bool LyricLookupObservationFresh(const PlaybackSnapshot& playback,int64_t now) {
    return playback.pid&&playback.processCreatedAtTicks&&playback.observedAtMs>0&&playback.observedAtMs<=now&&
        now-playback.observedAtMs<=LyricObservationBudgetMs(playback);
}
bool VerifiedLyricIdentityCurrent(const PlaybackSnapshot& playback,const std::wstring& lyricId) {
    const auto now=MonotonicMs();
    // Identity can be current without a usable play/pause clock. Reuse the
    // native observation's 500 ms freshness budget; never borrow old progress.
    return playback.songIdVerified&&!playback.metadataStale&&!playback.identityReacquiring&&
        LyricLookupObservationFresh(playback,now)&&
        SameCatalogId(lyricId,playback.songId);
}
bool HasSearchableSongText(const PlaybackSnapshot& playback) {
    // Text from an unverified fallback can still drive title search. Once a
    // native ID exists, only catalog text explicitly validated for that ID may
    // label it or drive an alternate-version search.
    return !playback.title.empty()&&(!playback.songIdVerified||playback.catalogMetadataReady);
}
bool HasLyricLookupIdentity(const PlaybackSnapshot& playback) {
    return !playback.metadataStale&&!playback.identityReacquiring&&LyricLookupObservationFresh(playback,MonotonicMs())&&
        ((playback.songIdVerified&&SameCatalogId(playback.songId,playback.songId))||HasSearchableSongText(playback));
}
bool LyricVersionConflict(const PlaybackSnapshot& playback,const std::wstring& id) {
    if(!SameCatalogId(id,id))return false;
    if(!CatalogMatchesProvider(id,playback.provider))return true;
    if(playback.songIdVerified)return !SameCatalogId(id,playback.songId);
    return SameSongId(playback.recordCandidateId,playback.recordCandidateId)&&!SameSongId(id,playback.recordCandidateId);
}
enum class LyricContentChange { None,AuxiliaryOnly,Original };
LyricContentChange CompareLyricContent(const LyricTimeline& a,const LyricTimeline& b) {
    if(a.songId!=b.songId||a.wordTimed!=b.wordTimed||a.lines.size()!=b.lines.size())return LyricContentChange::Original;
    bool auxiliaryChanged=false;
    for(size_t i=0;i<a.lines.size();++i){
        const auto& left=a.lines[i];const auto& right=b.lines[i];
        if(left.startMs!=right.startMs||left.durationMs!=right.durationMs||left.text!=right.text||left.words.size()!=right.words.size()||left.credit!=right.credit)
            return LyricContentChange::Original;
        auxiliaryChanged=auxiliaryChanged||left.translation!=right.translation||left.romanization!=right.romanization;
        for(size_t j=0;j<left.words.size();++j){
            const auto& x=left.words[j];const auto& y=right.words[j];
            if(x.startMs!=y.startMs||x.durationMs!=y.durationMs||x.text!=y.text)return LyricContentChange::Original;
        }
    }
    return auxiliaryChanged?LyricContentChange::AuxiliaryOnly:LyricContentChange::None;
}
size_t InitialLyricLine(const LyricTimeline& timeline) {
    // Opening provider credits are visible content, including static preview.
    for(size_t i=0;i<timeline.lines.size();++i)if(!timeline.lines[i].text.empty())return i;
    return 0;
}
std::string UtcNow() { SYSTEMTIME t{}; GetSystemTime(&t); char b[64]{}; sprintf_s(b,"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds); return b; }
std::filesystem::path DefaultLogPath() {
    auto root=UserDataDirectory()/L"logs";
    auto t=UtcNow(); for(char& c:t)if(c==':'||c=='.')c='-';return root/("capture-"+t+".jsonl");
}
struct Logger {
    int64_t started=MonotonicMs(),lastSampleAt=INT64_MIN;
    DiagnosticLogWriter writer;std::string lastSignature;std::mutex sampleMutex;
    PlaybackLogPoint lastLogPoint;
    explicit Logger(const std::filesystem::path& path) noexcept:writer(path,started){}
    void Write(json value,DiagnosticLogKind kind) noexcept {
        try{value["logSession"]=writer.SessionId();auto line=value.dump();line.push_back('\n');writer.Append(std::move(line),kind);}
        catch(...){writer.SerializationFailed(kind);}
    }
    void Sample(const PlaybackSnapshot& p,const AudioFeatures& a) noexcept {try{
        const auto now=MonotonicMs();
        const auto signature=Utf8(p.title)+"\x1f"+Utf8(p.artist)+"\x1f"+std::to_string(p.pid)+"\x1f"+std::to_string((int)p.state)+"\x1f"+std::to_string((int)p.reliability)+"\x1f"+std::to_string(a.pid)+"\x1f"+(a.active?"1":"0")+"\x1f"+(p.metadataStale?"1":"0")+"\x1f"+(p.catalogMetadataReady?"1":"0")+"\x1f"+(p.identityReacquiring?"1":"0")+"\x1f"+(a.connecting?"1":"0")+"\x1f"+(a.timingTrusted?"1":"0")+"\x1f"+Utf8(p.songId)+"\x1f"+(p.songIdVerified?"1":"0")+"\x1f"+(p.minimized?"1":"0")+"\x1f"+std::to_string(p.processCreatedAtTicks)+"\x1f"+std::to_string(a.processCreatedAtTicks)+"\x1f"+Utf8(p.recordCandidateId);
        const auto point=PlaybackLogPointAt(p,now);PlaybackLogPoint previous;
        bool discontinuity=false;const char* sampleReason="periodic";
        {std::lock_guard lock(sampleMutex);
            previous=lastLogPoint;discontinuity=PlaybackLogPositionDiscontinuous(previous,point);
            const bool changed=signature!=lastSignature;
            if(!changed&&!discontinuity&&now-lastSampleAt<1000)return;
            sampleReason=changed?"snapshot-change":discontinuity?"position-discontinuity-observed":"periodic";
            lastSignature=signature;lastSampleAt=now;lastLogPoint=point;
        }
        const json audioState={{"pid",a.pid},{"processCreatedAtTicks",std::to_string(a.processCreatedAtTicks)},{"active",a.active},{"connecting",a.connecting},{"timingTrusted",a.timingTrusted},
            {"rms",a.rms},{"peak",a.peak},{"sampledAtMs",a.sampledAtMs},{"observedAtMs",a.observedAtMs},{"diagnostic",Utf8(a.diagnostic)}};
        // Previous anchors are captured sampling attempts, not disk receipts.
        // Both raw points remain in this one record even if an earlier row was dropped.
        const json transition=discontinuity?json{{"previousPositionMs",previous.positionMs},{"previousSampledAtMs",previous.sampledAtMs},
            {"previousObservedAtMs",previous.observedAtMs},{"previousCapturedAtMs",previous.capturedAtMs},
            {"rawMovementMs",point.positionMs-previous.positionMs},{"sampleIntervalMs",point.sampledAtMs-previous.sampledAtMs},
            {"seekCommandConfirmed",false},{"displayConfirmed",nullptr},{"referenceClockCaptured",false}}:json(nullptr);
        Write({{"schema",2},{"appVersion",MUSIC_STAGE_VERSION},{"utc",UtcNow()},{"elapsedMs",now-started},{"monotonicMs",now},
            {"sampleReason",sampleReason},{"positionDiscontinuity",transition},{"pid",p.pid},{"processCreatedAtTicks",std::to_string(p.processCreatedAtTicks)},{"title",Utf8(p.title)},{"artist",Utf8(p.artist)},{"clientVersion",Utf8(p.clientVersion)},{"state",Utf8(PlayStateName(p.state))},{"stateSampledAtMs",p.stateSampledAtMs},{"metadataStale",p.metadataStale},{"catalogMetadataReady",p.catalogMetadataReady},{"identityReacquiring",p.identityReacquiring},{"nativeClockAdvanceAllowed",p.nativeClockAdvanceAllowed},{"reliability",Utf8(ReliabilityName(p.reliability))},{"source",Utf8(p.source)},{"provider",Utf8(MusicProviderName(p.provider))},{"mediaIdentityRevision",p.mediaIdentityRevision},{"mediaCoverAvailable",bool(p.mediaCover)},{"positionMs",p.positionMs},{"projectedPositionMs",PlaybackPositionAt(p,now)},{"durationMs",p.durationMs},{"sampledAtMs",p.sampledAtMs},{"observedAtMs",p.observedAtMs},{"sampleAgeMs",p.sampledAtMs>0?now-p.sampledAtMs:-1},{"minimized",p.minimized},{"songId",Utf8(p.songId)},{"songIdVerified",p.songIdVerified},{"recordCandidateId",Utf8(p.recordCandidateId)},{"diagnostic",Utf8(p.diagnostic)},{"audio",audioState}},DiagnosticLogKind::Sample);
    }catch(...){writer.SerializationFailed(DiagnosticLogKind::Sample);}}
    void Event(const std::string& name,const std::string& detail) noexcept {
        try{Write({{"event",name.substr(0,128)},{"utc",UtcNow()},{"elapsedMs",MonotonicMs()-started},{"detail",detail.substr(0,4096)}},DiagnosticLogKind::Event);}
        catch(...){writer.SerializationFailed(DiagnosticLogKind::Event);}
    }
    std::wstring Problem() const {
        const auto value=writer.Status();std::wstring message;
        if(value.state==DiagnosticLogState::Failed)message=L"日志文件不可用，诊断记录可能不完整";
        else if(value.state==DiagnosticLogState::LimitReached)message=L"日志已达容量上限，后续记录未保存";
        else if(value.droppedSamples||value.droppedEvents)message=L"日志记录不完整";
        if(!message.empty()&&(value.droppedSamples||value.droppedEvents))message+=L" · 缺采样 "+std::to_wstring(value.droppedSamples)+L" / 事件 "+std::to_wstring(value.droppedEvents);
        return message;
    }
    void Stop() noexcept {writer.Stop();}
};
void MaintainAudio(ProcessAudioCapture& capture, DWORD& targetPid, uint64_t& targetCreatedAt, int64_t& retryAt, int& failures,
                   const PlaybackSnapshot& p,const AudioFeatures& a,bool visualDemand,
                   const ProcessAudioCapture::Callback& callback) {
    const auto now=MonotonicMs();
    if(!visualDemand){
        const bool hadCapture=targetPid||targetCreatedAt||a.pid||a.active||a.connecting;
        if(hadCapture)capture.Stop();
        targetPid=0;targetCreatedAt=0;failures=0;retryAt=0;
        if(hadCapture&&callback){
            AudioFeatures suspended;suspended.observedAtMs=now;
            suspended.diagnostic=L"当前没有可见舞台，音频动效分析已暂停";
            callback(suspended);
        }
        return;
    }
    const DWORD desiredPid=p.processCreatedAtTicks?p.pid:0;
    const uint64_t desiredCreatedAt=desiredPid?p.processCreatedAtTicks:0;
    bool start=desiredPid!=targetPid||desiredCreatedAt!=targetCreatedAt;
    if(start){capture.Stop();targetPid=desiredPid;targetCreatedAt=desiredCreatedAt;failures=0;retryAt=now+1000;}
    else if(targetPid&&a.pid==targetPid&&a.processCreatedAtTicks==targetCreatedAt){
        const auto observed=a.observedAtMs>0?a.observedAtMs:a.sampledAtMs;
        const bool observationCurrent=observed>0&&observed<=now;
        const bool stalled=observationCurrent&&now-observed>(a.connecting?15000:10000);
        if(a.active&&!stalled){failures=0;retryAt=now+1000;}
        else if(observationCurrent&&now>=retryAt&&(!a.connecting||stalled)){
            capture.Stop();start=true;failures++;retryAt=now+(failures==1?1000:failures==2?2000:5000);
        }
    }
    if(start){
        if(targetPid)capture.Start(targetPid,callback,targetCreatedAt);
        else if(callback){AudioFeatures stopped;stopped.observedAtMs=now;stopped.diagnostic=p.pid?L"播放器进程实例尚未核验，音频响应已停止":L"未选择音乐播放器进程，音频响应已停止";callback(stopped);}
    }
}
enum class LyricOrigin { VerifiedClient, ConfirmedCandidate, AutomaticCandidate, LocalImport, ManualInput };
bool LyricRetryTargetAllowed(const PlaybackSnapshot& playback,const std::wstring& id,LyricOrigin origin) {
    if(!SameCatalogId(id,id))return false;
    switch(origin){
        case LyricOrigin::VerifiedClient:return playback.songIdVerified&&SameCatalogId(id,playback.songId);
        case LyricOrigin::AutomaticCandidate:return !LyricVersionConflict(playback,id);
        case LyricOrigin::ConfirmedCandidate:return true;
        default:return false; // Local imports and unbound manual previews never issue network refresh retries.
    }
}
enum class LyricCacheState { None,Pending,Stored,PreservedWords,Failed };
const wchar_t* LyricCacheStatus(LyricCacheState state) noexcept {
    switch(state){
        case LyricCacheState::Pending:return L"歌词缓存等待写入，退出前未必保存";
        case LyricCacheState::Stored:return L"歌词缓存已保存";
        case LyricCacheState::PreservedWords:return L"已保护原逐字缓存，本次逐句版本未保存";
        case LyricCacheState::Failed:return L"歌词缓存未保存，当前歌词仍可显示，旧文件保留";
        default:return L"";
    }
}
struct SuspendedLyricRefresh {
    DWORD pid=0;
    std::wstring playbackId,lyricId;
    LyricOrigin origin=LyricOrigin::AutomaticCandidate;
    uint64_t processCreatedAtTicks=0;
    bool wordTiming=false;
};
struct LocalImportBinding {
    DWORD pid=0;
    uint64_t processCreatedAtTicks=0;
    std::wstring playbackId;
    bool awaitingReconfirmation=false;
};
struct LyricRequestOwner {
    uint64_t generation=0;
    DWORD pid=0;
    uint64_t processCreatedAtTicks=0;
    std::wstring playbackSongId;
};
LyricRequestOwner CaptureLyricRequestOwner(const PlaybackSnapshot& playback,uint64_t generation) {
    LyricRequestOwner owner{generation,playback.pid,playback.processCreatedAtTicks,{}};
    if(playback.songIdVerified&&!playback.metadataStale&&!playback.identityReacquiring&&SameCatalogId(playback.songId,playback.songId))
        owner.playbackSongId=playback.songId;
    return owner;
}
enum class LyricCommitDisposition { Commit,Wait,Drop };
enum class LyricCommitReason {
    IndependentPreview,PostCompletionIdentity,UnverifiedMetadata,
    GenerationChanged,Exiting,Cancelled,ProcessChanged,IdentityReacquiring,
    MetadataUnavailable,IdentityUnverified,SongChanged,MissingObservation,
    FutureObservation,AwaitingNewObservation,ObservationExpired,
    NewRequest,VerifiedIdentityArrived,ManualSelection,FollowCurrent,ReplacedResult,PlaybackBoundaryChanged
};
struct LyricCommitDecision {
    LyricCommitDisposition disposition;
    LyricCommitReason reason;
};
enum class LyricCommitTraceStage { ResultArrival,PendingPoll,PendingRetired };
struct LyricCommitTrace {
    LyricCommitDecision decision{LyricCommitDisposition::Drop,LyricCommitReason::GenerationChanged};
    LyricCommitTraceStage stage=LyricCommitTraceStage::ResultArrival;
    uint64_t requestGeneration=0,currentGeneration=0,requestCreatedAt=0,currentCreatedAt=0;
    DWORD requestPid=0,currentPid=0;
    std::array<char,40> requestPlaybackId{},lyricId{},currentSongId{},resultSongId{};
    int64_t completedAtMs=0,observedAtMs=0,recordedAtMs=0;
    LyricOrigin origin=LyricOrigin::AutomaticCandidate;
    LyricLoadState resultState=LyricLoadState::Idle;
    size_t lineCount=0;
    bool verified=false,stale=false,reacquiring=false,fromCache=false,wordTimed=false;
};
std::array<char,40> LyricTraceSongId(std::wstring_view value) noexcept {
    std::array<char,40> output{};
    if(IsKugouCatalogId(value)){for(size_t i=0;i<value.size();++i)output[i]=static_cast<char>(CatalogLower(value[i]));return output;}
    const size_t prefix=IsSodaCatalogId(value)?5:IsQqCatalogId(value)?3:0;
    const auto digits=CatalogDigits(value);if(digits.empty())return output;
    for(size_t i=0;i<prefix;++i)output[i]=static_cast<char>(value[i]);
    for(size_t i=0;i<digits.size();++i)output[prefix+i]=static_cast<char>(digits[i]);
    return output;
}
const char* LyricCommitReasonName(LyricCommitReason reason) noexcept {
    switch(reason){
        case LyricCommitReason::IndependentPreview:return "independent-preview";
        case LyricCommitReason::PostCompletionIdentity:return "post-completion-identity";
        case LyricCommitReason::UnverifiedMetadata:return "unverified-metadata";
        case LyricCommitReason::GenerationChanged:return "generation-changed";
        case LyricCommitReason::Exiting:return "application-exiting";
        case LyricCommitReason::Cancelled:return "request-cancelled";
        case LyricCommitReason::ProcessChanged:return "process-instance-changed";
        case LyricCommitReason::IdentityReacquiring:return "identity-reacquiring";
        case LyricCommitReason::MetadataUnavailable:return "metadata-unavailable";
        case LyricCommitReason::IdentityUnverified:return "identity-unverified";
        case LyricCommitReason::SongChanged:return "song-changed";
        case LyricCommitReason::MissingObservation:return "observation-missing";
        case LyricCommitReason::FutureObservation:return "observation-from-future";
        case LyricCommitReason::AwaitingNewObservation:return "awaiting-post-completion-observation";
        case LyricCommitReason::ObservationExpired:return "observation-expired";
        case LyricCommitReason::NewRequest:return "new-request";
        case LyricCommitReason::VerifiedIdentityArrived:return "verified-identity-arrived";
        case LyricCommitReason::ManualSelection:return "manual-selection";
        case LyricCommitReason::FollowCurrent:return "follow-current";
        case LyricCommitReason::ReplacedResult:return "replaced-result";
        case LyricCommitReason::PlaybackBoundaryChanged:return "playback-boundary-changed";
    }
    return "unknown";
}
struct PendingLyricCommit {
    LyricTimeline result;
    LyricRequestOwner owner;
    std::wstring lyricId;
    LyricOrigin origin=LyricOrigin::AutomaticCandidate;
    std::optional<SongCandidate> selected;
    std::optional<std::filesystem::path> importPathToRemember;
    bool refreshInFlight=false;
    bool refreshRetryRequired=false;
    bool fallbackOnlineAfterImportFailure=false;
    int64_t completedAtMs=0;
    std::optional<LyricCommitReason> lastWaitReason;
    int64_t lastWaitLoggedAtMs=0;
};
enum class LyricApplyOutcome { Rejected,Deferred,Ready };
struct Shared {
    std::mutex mutex;std::stop_source generationStop;PlaybackSnapshot playback;AudioFeatures audio;std::shared_ptr<const LyricTimeline> lyrics=std::make_shared<LyricTimeline>();
    std::vector<SongCandidate> candidates;std::wstring networkStatus=L"等待网易云、QQ 音乐、酷狗或汽水音乐歌曲，识别后自动查找歌词";
    LyricCacheState lyricCacheState=LyricCacheState::None;
    std::wstring suspendedNetworkStatus;
    std::optional<SuspendedLyricRefresh> suspendedLyricRefresh;
    std::optional<LocalImportBinding> localImportBinding;
    std::optional<PendingLyricCommit> pendingLyricCommit;
    std::wstring matchedId,coverFile,coverSongId,coverStatus,coverError,retrySongId,verifiedTrackId;
    std::wstring automaticLyricTargetId; // Current job target, never a displayed or verified playback ID.
    LyricOrigin retryOrigin=LyricOrigin::AutomaticCandidate;
    LyricOrigin loadedOrigin=LyricOrigin::AutomaticCandidate;
    uint64_t generation=0,autoSearchGeneration=UINT64_MAX;
    int64_t songChangedAt=0,retryAfter=0;
    int retryFailures=0;
    bool busy=false,manualLyrics=false,busyAutomaticLookup=false,lyricRefreshPending=false;
    bool coverRevalidatePending=false;
    bool manualQueryActive=false,candidatesManual=false;
    uint64_t busyGeneration=UINT64_MAX;
    uint64_t manualLoadedGeneration=UINT64_MAX;
    std::wstring manualTitle,manualArtist;
    TrackLyricPreferences trackPreferences;
    uint64_t preferenceRevision=0,importAttemptGeneration=UINT64_MAX;
    uint64_t preferenceSaveRequest=0,preferenceQueuedRevision=0;
    std::wstring importAttemptKey,preferenceNotice;
    std::wstring preferenceSaveDiagnostic;
    int64_t preferenceSaveAt=0,localCoverRetryAt=0;
    bool preferencesDirty=false,preferenceSaveInFlight=false;
};
enum { ID_SEARCH=101,ID_FETCH,ID_IMPORT,ID_CANDIDATES,ID_OFFSET,ID_THEME,ID_STRENGTH,ID_FULLSCREEN,ID_PANEL,ID_IMAGE,ID_VIDEO,ID_COVER,ID_TAB,ID_TEXTSIZE,ID_MOTION,ID_THEMEIMPORT,ID_THEMEEXPORT,ID_ACCENT,ID_DESKTOP,ID_DIAGNOSTICS,ID_CLOCK,ID_WINDOW,ID_OPEN,ID_EXIT,ID_TRANSLATION,ID_ROMANIZATION,ID_ONLINELYRICS,ID_OFFSETRESET,ID_LYRICMODE,ID_AUTOHIDE,ID_SETTINGSPANE,ID_RESUME_LYRICS,ID_SHADE,ID_CAMERA,ID_BACKDROP_RESET,ID_MATCHLYRICS,ID_MANUALQUERY,ID_MANUALSEARCH,ID_SETTINGSREPAIR,ID_SETTINGSNOTE,ID_COVERSTATUS,ID_LYRICMEMORYREPAIR,ID_LYRICMEMORYNOTE,ID_THEMERESTORE,ID_THEMENOTE,ID_BACKGROUNDRETRY,ID_BACKGROUNDNOTE,ID_USAGEHELP=207,ID_CANDIDATEDETAILSLABEL=212,ID_CANDIDATEDETAILS,ID_QUALITY,ID_FRAMERATE,ID_PAUSEFULLSCREEN,ID_QUALITYLABEL,ID_FRAMERATELABEL };
constexpr wchar_t UsageHelpText[] =
    L"使用说明与快捷键\r\n\r\n"
    L"网易云、QQ 音乐、酷狗或汽水音乐负责播放，HTune自动跟随。网易云支持原生内部时钟；QQ 使用 Windows 媒体会话，并从 QQ 自有歌词库取得 QRC/LRC；酷狗使用媒体歌曲信息、原生进度和酷狗 KRC/LRC 歌词。酷狗原生进度适配 20.1.51.27967，其他版本需重新适配。汽水使用系统媒体锚点、本地目录严格匹配与汽水逐字/逐句歌词，目录缺失时自动重试；ID 候选不是直接播放 ID。当前来源见本页上方。\r\n\r\n"
    L"未取得可靠进度时，已加载歌词保持静态预览，逐字高亮暂停。\r\n\r\n"
    L"歌词阅读\r\n"
    L"舞台滚轮：浏览歌词\r\n"
    L"Ctrl+F6：把焦点移到歌词舞台\r\n"
    L"↑ / ↓：小步阅读，组内到边缘后换句\r\n"
    L"PageUp / PageDown：按歌词视口翻页\r\n"
    L"Home：返回当前句\r\n\r\n"
    L"十秒不继续浏览后结束临时浏览；有可靠时间源且歌词版本一致时恢复跟随。独立查找预览需先在歌词页选择“跟随当前歌曲”。\r\n\r\n"
    L"画面与设置\r\n"
    L"移动鼠标：显示底部控制台\r\n"
    L"Ctrl+,：打开 / 关闭 Backstage 设置\r\n"
    L"Tab / Shift+Tab：在控制台与设置之间移动焦点；Enter / 空格确认\r\n"
    L"Esc：关闭 Backstage，或退出全屏\r\n"
    L"F11：全屏 / 返回\r\n"
    L"双击舞台：切换全屏\r\n"
    L"Ctrl+L：切换歌词呈现\r\n\r\n"
    L"桌面模式可从系统托盘打开设置或恢复窗口。本地图片或视频读取失败时，可在画面页选择“重试当前背景素材”；原素材文件保持原样。";
constexpr UINT WM_DATA=WM_APP+1;
constexpr UINT WM_TRAY=WM_APP+2,WM_STAGE_LOST=WM_APP+3,WM_ACTIVATE_APP=WM_APP+4,WM_UI_ACTIONS=WM_APP+5;
constexpr UINT_PTR TimerTick=1,TimerModalFrame=2;
const UINT taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated");
LRESULT CALLBACK StageProc(HWND,UINT,WPARAM,LPARAM);
LRESULT CALLBACK SettingsPaneProc(HWND,UINT,WPARAM,LPARAM);
struct App {
    struct FramePermit {const SceneRenderer* owner=nullptr;HANDLE handle=nullptr;uint64_t generation=0;};
    struct ModalScope {
        App& owner;
        explicit ModalScope(App& value):owner(value){++owner.modalDepth;owner.NoteActivity();owner.UpdateModalFrameTimer();}
        ~ModalScope() noexcept {if(owner.modalDepth)--owner.modalDepth;owner.UpdateModalFrameTimer();}
        ModalScope(const ModalScope&)=delete;ModalScope& operator=(const ModalScope&)=delete;
    };
    std::atomic<HWND> window{nullptr};HWND list=nullptr,search=nullptr,fetch=nullptr,offsetEdit=nullptr,themeCombo=nullptr,strengthSlider=nullptr,status=nullptr;
    HFONT font=nullptr;Shared data;PlaybackMonitor monitor;ProcessAudioCapture capture;DWORD audioPid=0;uint64_t audioCreatedAt=0;int64_t audioRetryAt=0;int audioFailures=0;Logger log;
    // Fixed snapshots are protected by data.mutex; only the UI thread drains.
    std::array<LyricCommitTrace,16> lyricCommitTraces{};
    size_t lyricCommitTraceCount=0;
    uint64_t lyricCommitTraceDropped=0,lyricCommitTraceReportedDropped=0;
    struct JobRequest {
        uint64_t generation=0;
        bool retryOnException=false;
        std::wstring retryTargetId;
        LyricOrigin retryOrigin=LyricOrigin::AutomaticCandidate;
        std::function<void()> task;
    };
    struct CoverRequest {
        uint64_t generation=0,serial=0;
        std::wstring id;
        CoverIdOrigin origin=CoverIdOrigin::SelectedCandidate;
        std::stop_token generationToken;
        std::stop_source cancellation;
        bool automaticCandidate=false;
        bool forceRevalidate=false;
        std::filesystem::path protectedFile;
    };
    SceneRenderer renderer;BackgroundController background;AppSettings settings;SettingsSaveWorker settingsWriter;std::thread jobThread,coverThread,lyricCacheThread;std::mutex jobMutex;std::condition_variable jobCv,coverCv,lyricCacheCv;std::optional<JobRequest> job;bool stopping=false;
    using LyricCacheSnapshot=std::shared_ptr<const LyricTimeline>;
    // Fixed storage: at most four pending writes plus four recent snapshots.
    std::array<LyricCacheSnapshot,4> pendingLyricCaches{},recentLyricCaches{};
    size_t pendingLyricCacheCount=0,recentLyricCacheCount=0;
    std::shared_ptr<CoverRequest> pendingCover,activeCover;
    uint64_t coverSerial=0;
    NativeControls nativeControls;DesktopHost desktopHost;HWND desktopWindow=nullptr,tabs=nullptr,sizeSlider=nullptr;std::unique_ptr<SceneRenderer> desktopRenderer;
    unsigned desktopPresentationReports=0;int64_t desktopPresentationReportAt=0;
    ThemeTransition themeTransition;std::optional<ThemePalette> displayedPalette;
    std::shared_ptr<const ThemeDefinition> customTheme;std::vector<std::pair<HWND,int>> pageControls;
    std::wstring lyricFontNotice;
    bool themeRestorePending=false,preserveSavedThemeFile=false,themeRestoreStatePainted=false,themeRestoreShown=false;
    std::wstring themeRestoreDiagnostic,themeRestoreDiagnosticShown;
    std::filesystem::path privateThemeSaveFile,stagedRecoveryThemeFile;
    std::wstring uiNotice,lastUiNotice;int page=0,desktopRetries=0,mainRenderFailures=0,desktopRenderFailures=0;int64_t uiNoticeUntil=0,desktopRetryAt=0,desktopPollAt=0,mainRenderRetryAt=0,desktopRenderRetryAt=0,trayRetryAt=0;
    bool desiredDesktop=false,closingDesktop=false,desktopActive=false,trayAdded=false,showDiagnostics=false,mainRecoveryPending=false,desktopRecoveryPending=false;
    int64_t renderVisibilityPollAt=0;
    FramePermit framePermit;
    unsigned modalDepth=0;
    bool sizingLoop=false,menuLoop=false,frameDrawing=false,frameWaiting=false,mainResizePending=false,desktopResizePending=false;
    bool tickDeferred=false,closeDeferred=false,desktopCleanupDeferred=false,shutdownFinalizing=false,shutdownFinalized=false;
    bool windowBoundsPending=false;
    bool windowPlacementReady=false;
    unsigned windowBoundsFailures=0;
    int64_t windowBoundsRetryAt=0;
    HWND modalTimerWindow=nullptr;
    std::atomic<bool> exiting{false};
    HWND settingsPane=nullptr,lyricModeCombo=nullptr,lastPaneFocus=nullptr,qualityCombo=nullptr,frameRateCombo=nullptr;
    bool paneFocusRevealPending=false;
    std::array<int,4> paneScroll{};int paneVisibleHeight=1,paneContentHeight=1;float paneWheelCarry=0;
    bool quietFullscreen=false,cursorHidden=false;int64_t interactionAt=0;POINT lastPointer{};bool pointerKnown=false;
    // Foreground application state for presentation only (never playback or
    // identity evidence). A stage hidden behind a fullscreen or maximized
    // foreground window on its monitor draws nothing; another monitor's stage
    // keeps a lite cadence while a fullscreen application runs.
    struct ForegroundCover { HMONITOR monitor=nullptr; bool fullscreen=false,covers=false; bool operator==(const ForegroundCover&) const=default; };
    ForegroundCover foregroundCover;bool onBattery=false,foregroundRecheck=false;int64_t powerPollAt=0;
    int paintedTheme=-1;const ThemeDefinition* paintedCustomTheme=nullptr;
    HWND shadeSlider=nullptr,cameraSlider=nullptr;
    bool manualLyricBrowse=false;int64_t lyricBrowseUntil=0,settingsSaveAt=0;
    float previewScrollOffset=0;
    uint64_t lyricBrowseGeneration=UINT64_MAX;size_t lyricBrowseReadingRevision=0;int stageWheelCarry=0;
    bool associationMismatchShown=false;
    bool compactHintShown=false;
    HWND manualQueryEdit=nullptr;
    std::wstring preferredCandidateId;
    uint64_t preferredCandidateGeneration=UINT64_MAX;
    struct CandidateListView {
        uint64_t generation=UINT64_MAX;
        size_t revision=0;
        bool manual=false,updating=false,selectionEstablished=false;
        std::vector<SongCandidate> rows;
        int selected=-1; // UI-owned selection; the native list mirrors it
    } candidateList;
    // Stage UI (console, pills, Backstage) drawn by the renderer. While it is
    // available the native settings rail stays hidden; its saved visibility
    // is kept for older builds. Presentation state only.
    std::unique_ptr<ui::Context> stageUi;StageInterface stageInterface;InterfaceState interfaceState;
    std::vector<UiAction> pendingUiActions;
    bool uiActionsPosted=false,uiPointerCaptured=false,uiMouseTracked=false,uiImeEnabled=true;
    int64_t consoleActivityAt=0,uiAnimatingAt=0;POINT lastClientPointer{-1,-1};
    bool lyricReturnShown=false,lyricReturnEnabled=false,associationNeeded=false;
    std::wstring shownCoverStatus,shownStatusText,shownCandidateDetails;
    uint64_t shownBackgroundRevision=UINT64_MAX;
    std::wstring shownBackgroundNotice;
    bool backgroundNoticePainted=false,backgroundRetryShown=false;
    bool manualFollowShown=false;
    bool settingsWriteBlockedLogged=false,settingsStatePainted=false,settingsWritableShown=true;
    bool preferenceWriteBlockedLogged=false,preferenceStatePainted=false,preferenceWritableShown=true;
    bool settingsPendingShown=false,preferencePendingShown=false,settingsThemePendingShown=false;
    bool settingsSaveInFlight=false,settingsThemePending=false;uint64_t settingsSaveRevision=0;
    std::wstring settingsSaveDiagnostic,settingsSaveDiagnosticShown,preferenceSaveDiagnosticShown;
    uint64_t presentedLyricRevision=UINT64_MAX,presentedLyricReadingRevision=UINT64_MAX;size_t presentedLyricIndex=0;
    PresentedLyricPosition presentedLyricViewport;
    bool presentedLyricValid=false;
    bool presentedLyricDesktop=false;uint64_t presentedLyricRendererGeneration=0;
    std::wstring activePreferenceKey,activeMetadataKey;uint64_t activePreferenceGeneration=UINT64_MAX;
    bool settingOffsetText=false;
    enum class FullscreenTransition {Stable,Entering,Exiting,RecoveryPending};
    enum class FullscreenFailureState {Unchanged,Recovered,RecoveryPending};
    struct FullscreenRestoreState {
        bool valid=false,panelVisible=true;
        LONG_PTR style=0;
        WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
    } fullscreenRestore;
    FullscreenTransition fullscreenTransition=FullscreenTransition::Stable;
    int offset=0,theme=0;float strength=1;bool fullscreen=false,controlsReady=false,panelVisible=true;std::atomic<int> backgroundSelection{0};size_t previewIndex=0,shownLyricReadingRevision=0;std::atomic<size_t> revision{0},lyricRevision{0},lyricReadingRevision{0};
    explicit App(const std::filesystem::path& path):log(path) {
        std::wstring diagnostic;
        settings=LoadSettings(diagnostic);offset=settings.offsetMs;theme=settings.theme;strength=settings.response;
        panelVisible=settings.panelVisible;desiredDesktop=settings.presentationMode==1;backgroundSelection=settings.backgroundKind;
        // Images may still prepare while hidden, but a video must not start
        // continuous decoding before a visible window/desktop stage exists.
        background.SetPlaybackDemand(false);
        if(settings.backgroundKind==1)background.SetImage(settings.backgroundPath);
        else if(settings.backgroundKind==2)background.SetVideo(settings.backgroundPath);
        if(!diagnostic.empty())log.Event("settings-load",Utf8(diagnostic));
        data.trackPreferences=LoadTrackLyricPreferences(diagnostic);
        if(!diagnostic.empty()){log.Event("track-preferences-load",Utf8(diagnostic));uiNotice=diagnostic;}
        if(!settings.customThemePath.empty()){
            auto loaded=LoadTheme(settings.customThemePath);
            if(loaded.ok){customTheme=std::make_shared<ThemeDefinition>(std::move(loaded.definition));theme=customTheme->baseStyle;RememberPrivateThemeSaveFile();}
            else{themeRestorePending=true;preserveSavedThemeFile=true;themeRestoreDiagnostic=loaded.diagnostic;log.Event("theme-restore",Utf8(loaded.diagnostic));uiNotice=L"保存的主题暂时无法载入，已保留关联；可在主题页重试";}
        }
        try{
            jobThread=std::thread([this]{RunJobs();});
            coverThread=std::thread([this]{RunCoverJobs();});
            lyricCacheThread=std::thread([this]{RunLyricCacheJobs();});
        }catch(...){StopJobWorkers();throw;}
        interactionAt=MonotonicMs();
    }
    ~App(){
        const auto root=window.load();
        try{FinalizeShutdown(root&&IsWindow(root));}
        catch(...){try{FinalizeShutdown(false);}catch(...){}}
        if(const auto current=window.load();current&&IsWindow(current)){
            KillTimer(current,TimerTick);SetWindowLongPtrW(current,GWLP_USERDATA,0);DestroyWindow(current);window=nullptr;
        }
        if(font)DeleteObject(font);
    }
    // jobMutex is held; stop callbacks only forward a stop/event signal.
    bool CancelCoverRequestsLocked() {
        const bool current=pendingCover||(activeCover&&activeCover->serial==coverSerial);
        if(activeCover)activeCover->cancellation.request_stop();
        if(pendingCover)pendingCover->cancellation.request_stop();
        pendingCover.reset();++coverSerial;return current;
    }
    // Both state and queue locks are held at a lyric generation boundary.
    void CancelCoverGenerationLocked() {
        if(CancelCoverRequestsLocked()){
            data.coverStatus=data.coverFile.empty()?L"此前封面请求已取消，等待重新获取":L"已保留此前封面，旧获取请求已取消";
            data.coverError.clear();data.localCoverRetryAt=0;
        }
    }
    // data.mutex is held. Geometry follows every content change, while a
    // supplemental-text refresh does not invalidate the original reading grid.
    void MarkLyricContentChanged(bool originalChanged=true) noexcept {
        ++lyricRevision;if(originalChanged)++lyricReadingRevision;
    }
    void StopJobWorkers() {
        std::array<LyricCacheSnapshot,4> retiredPending{},retiredRecent{};
        {std::lock_guard lock(jobMutex);stopping=true;job.reset();CancelCoverRequestsLocked();
            // Pending accepted lyrics remain owned by the cache worker. Once
            // stopping is visible, producers cannot add more, so it can drain
            // the bounded queue before returning.
            recentLyricCaches.swap(retiredRecent);recentLyricCacheCount=0;}
        jobCv.notify_all();coverCv.notify_all();lyricCacheCv.notify_all();
        if(jobThread.joinable())jobThread.join();
        if(coverThread.joinable())coverThread.join();
        if(lyricCacheThread.joinable())lyricCacheThread.join();
        {std::lock_guard lock(jobMutex);pendingLyricCaches.swap(retiredPending);pendingLyricCacheCount=0;}
    }
    // data.mutex is held. Completion also checks ownership of busyGeneration.
    void ClearLyricBusyLocked() {
        data.busy=false;data.busyAutomaticLookup=false;data.lyricRefreshPending=false;
        data.busyGeneration=UINT64_MAX;data.automaticLyricTargetId.clear();
    }
    void PromoteAutomaticLyricIdentityLocked(const PlaybackSnapshot& playback) {
        if(data.loadedOrigin!=LyricOrigin::AutomaticCandidate||!VerifiedLyricIdentityCurrent(playback,data.matchedId))return;
        data.loadedOrigin=LyricOrigin::VerifiedClient;
        if(data.retryOrigin==LyricOrigin::AutomaticCandidate&&SameCatalogId(data.retrySongId,playback.songId))data.retryOrigin=LyricOrigin::VerifiedClient;
        constexpr std::wstring_view prefix=L"自动搜索候选（未证实播放 ID）";
        if(data.networkStatus.starts_with(prefix))data.networkStatus.replace(0,prefix.size(),L"播放器当前歌曲核验");
    }
    bool RefreshNativeCandidateDisplayLocked() {
        const auto& playback=data.playback;
        if(data.candidatesManual||ManualPreviewLocked()||!HasSearchableSongText(playback)||
            !VerifiedLyricIdentityCurrent(playback,playback.songId))return false;
        bool changed=false;
        for(auto& candidate:data.candidates){
            if(!SameCatalogId(candidate.id,playback.songId))continue;
            const auto& artist=playback.artist.empty()?candidate.artist:playback.artist;
            const auto& album=playback.album.empty()?candidate.album:playback.album;
            const auto duration=playback.durationMs>0?playback.durationMs:candidate.durationMs;
            if(candidate.title==playback.title&&candidate.artist==artist&&
                candidate.album==album&&candidate.durationMs==duration)continue;
            try{
                // Prepare all allocations before changing the visible row.
                // Keep its ID, source flag, order and selection model intact.
                auto updated=candidate;
                updated.title=playback.title;updated.artist=artist;updated.album=album;updated.durationMs=duration;
                candidate=std::move(updated);changed=true;
            }catch(...){/* Optional display enrichment can retry on the next UI tick. */}
        }
        if(changed)++revision;
        return changed;
    }
    LyricCacheSnapshot ReadLyricMemory(const std::wstring& id) {
        std::scoped_lock lock(data.mutex,jobMutex);if(stopping)return {};
        // A disk cache published before Fetch is also a usable in-memory
        // timeline. Keep that immutable reference available if another writer
        // evicts the disk copy while the online refresh is in flight. This
        // display shortcut excludes imported and independent manual views.
        const bool catalog=data.loadedOrigin==LyricOrigin::VerifiedClient||
            data.loadedOrigin==LyricOrigin::AutomaticCandidate||data.loadedOrigin==LyricOrigin::ConfirmedCandidate;
        const auto displayed=catalog&&data.lyrics&&data.lyrics->state==LyricLoadState::Ready&&
            !data.lyrics->lines.empty()&&SameCatalogId(data.matchedId,id)&&SameCatalogId(data.lyrics->songId,id)?
            data.lyrics:LyricCacheSnapshot{};
        if(displayed&&displayed->wordTimed)return displayed;
        LyricCacheSnapshot lineFallback;
        for(size_t i=recentLyricCacheCount;i>0;--i){
            const auto& snapshot=recentLyricCaches[i-1];
            if(!SameCatalogId(snapshot->songId,id))continue;
            if(snapshot->wordTimed)return snapshot;
            if(!lineFallback)lineFallback=snapshot;
        }
        return displayed?displayed:lineFallback;
    }
    LyricsClient MakeLyricsClient(std::stop_token token) {
        return LyricsClient(token,LyricCacheMode::Deferred,[this](const std::wstring& id){return ReadLyricMemory(id);});
    }
    // Both App locks are held. No allocation, serialization or I/O here.
    // Retired references are released by ApplyLyricResult after unlocking.
    void QueueLyricCacheLocked(const LyricCacheSnapshot& snapshot,std::array<LyricCacheSnapshot,2>& retired) noexcept {
        if(stopping)return;
        const auto append=[&](auto& slots,size_t& count,LyricCacheSnapshot& released){
            size_t remove=count;
            for(size_t i=0;i<count;++i){
                if(SameCatalogId(slots[i]->songId,snapshot->songId)&&slots[i]->wordTimed==snapshot->wordTimed){remove=i;break;}
            }
            if(remove==count&&count==slots.size()){
                remove=0;
                for(size_t i=0;i<count;++i)if(!SameCatalogId(slots[i]->songId,snapshot->songId)){remove=i;break;}
            }
            if(remove<count){
                released=std::move(slots[remove]);
                for(size_t i=remove+1;i<count;++i)slots[i-1]=std::move(slots[i]);
                --count;
            }
            slots[count++]=snapshot;
        };
        append(recentLyricCaches,recentLyricCacheCount,retired[0]);
        append(pendingLyricCaches,pendingLyricCacheCount,retired[1]);
        data.lyricCacheState=LyricCacheState::Pending;lyricCacheCv.notify_one();
    }
    void RunLyricCacheJobs() {
        while(true){
            LyricCacheSnapshot snapshot;
            {
                std::unique_lock lock(jobMutex);
                lyricCacheCv.wait(lock,[this]{return stopping||pendingLyricCacheCount>0;});
                if(stopping&&pendingLyricCacheCount==0)return;
                snapshot=std::move(pendingLyricCaches[0]);
                for(size_t i=1;i<pendingLyricCacheCount;++i)pendingLyricCaches[i-1]=std::move(pendingLyricCaches[i]);
                --pendingLyricCacheCount;
            }
            // Static accepted lyrics may finish archiving after a song change.
            // Only this exact immutable display object may receive the status.
            const auto saved=StoreLyricCache(*snapshot);bool visible=false;
            {
                std::scoped_lock lock(data.mutex,jobMutex);
                if(!exiting&&!stopping&&data.lyrics==snapshot){
                    data.lyricCacheState=saved==LyricCacheSaveResult::Stored?LyricCacheState::Stored:
                        saved==LyricCacheSaveResult::PreservedWords?LyricCacheState::PreservedWords:LyricCacheState::Failed;
                    visible=true;
                }
            }
            if(visible)if(const auto target=window.load())PostMessageW(target,WM_DATA,0,0);
            try{log.Event("lyric-cache-write",Utf8(snapshot->songId)+" result="+
                (saved==LyricCacheSaveResult::Stored?"stored":saved==LyricCacheSaveResult::PreservedWords?"preserved-words":"failed"));}catch(...){}
        }
    }
    bool ManualPreviewLocked() const {return data.manualQueryActive||data.loadedOrigin==LyricOrigin::ManualInput;}
    // data.mutex is held. A file/request must still describe the display's
    // selected entity; a catalog lookup never makes that entity now-playing.
    bool CoverIdentityAllowedLocked(const std::wstring& id,bool automaticCandidate) const {
        if(!SameSongId(id,id))return false;
        if(ManualPreviewLocked())return data.manualLoadedGeneration==data.generation&&SameSongId(id,data.matchedId);
        if(data.playback.metadataStale)return false;
        if(data.playback.songIdVerified)return SameSongId(id,data.playback.songId);
        if(automaticCandidate&&LyricVersionConflict(data.playback,id))return false;
        // A local import has no catalog ID. Its UI maintenance path may use
        // the existing explicitly confirmed cover association for this track.
        return SameSongId(id,data.matchedId)||data.loadedOrigin==LyricOrigin::LocalImport;
    }
    // Both queue/state locks are held. In particular, accepting the first
    // lyric result must not cancel an already running SAME-ID cover request.
    void CancelObsoleteCoverRequestsLocked() {
        const auto obsolete=[&](const std::shared_ptr<CoverRequest>& request){
            return request&&request->serial==coverSerial&&
                !CoverIdentityAllowedLocked(request->id,request->automaticCandidate);
        };
        if(obsolete(pendingCover)||obsolete(activeCover))CancelCoverGenerationLocked();
    }
    std::wstring CurrentPreferenceKeyLocked() const {
        if(data.manualQueryActive||data.playback.metadataStale)return {};
        // A previously verified ID is only an ID for the snapshot that is
        // currently carrying the same native verification.  After native
        // identity expires, title/SMTC fallback must use a metadata key and
        // cannot inherit another song's per-track choices through this field.
        const auto verified=data.playback.songIdVerified&&
            SameCatalogId(data.playback.songId,data.verifiedTrackId)?data.playback.songId:std::wstring{};
        return PlayerLyricKey(data.playback,verified);
    }
    int CurrentOffsetLocked() const {
        const auto key=CurrentPreferenceKeyLocked();
        if(activePreferenceGeneration==data.generation&&activePreferenceKey==key)return offset;
        const auto found=data.trackPreferences.entries.find(key);
        return found!=data.trackPreferences.entries.end()&&found->second.offsetMs?*found->second.offsetMs:settings.offsetMs;
    }
    bool LyricVersionMismatchLocked() const {
        if(ManualPreviewLocked())return false;
        if(data.lyrics->lines.empty())return false;
        if(data.loadedOrigin==LyricOrigin::LocalImport)
            return data.localImportBinding&&data.localImportBinding->awaitingReconfirmation;
        return LyricVersionConflict(data.playback,data.matchedId);
    }
    LyricAssociation LyricAssociationLocked() const {
        if(data.lyrics->lines.empty())return LyricAssociation::Unknown;
        if(data.loadedOrigin==LyricOrigin::ManualInput)return LyricAssociation::UserConfirmed;
        if(data.loadedOrigin==LyricOrigin::LocalImport)return LyricAssociation::LocalImport;
        if(VerifiedLyricIdentityCurrent(data.playback,data.matchedId))return LyricAssociation::VerifiedCurrent;
        if(data.loadedOrigin==LyricOrigin::VerifiedClient||SameCatalogId(data.matchedId,data.verifiedTrackId))return LyricAssociation::PreviouslyVerified;
        return data.loadedOrigin==LyricOrigin::ConfirmedCandidate?LyricAssociation::UserConfirmed:LyricAssociation::AutomaticCandidate;
    }
    void UpdateAssociationControl() {
        bool needed=false,mismatch=false;
        {std::lock_guard lock(data.mutex);const auto association=LyricAssociationLocked();mismatch=LyricVersionMismatchLocked();
            needed=!ManualPreviewLocked()&&!data.playback.metadataStale&&(mismatch||association==LyricAssociation::AutomaticCandidate||association==LyricAssociation::PreviouslyVerified||
                association==LyricAssociation::UserConfirmed||(!data.candidates.empty()&&data.lyrics->lines.empty()));}
        needed=needed&&!CompactStageControls();
        associationNeeded=needed;
        const auto button=Control(ID_MATCHLYRICS);if(!button)return;
        if(mismatch!=associationMismatchShown){associationMismatchShown=mismatch;SetWindowTextW(button,mismatch?L"修正歌词版本":L"核对歌词版本");}
        // With the stage UI this is a pill on the stage; the button stays hidden.
        if(stageUi)needed=false;
        if(!needed&&GetFocus()==button)SetFocus(window);
        const bool shown=(GetWindowLongPtrW(button,GWL_STYLE)&WS_VISIBLE)!=0;
        if(shown!=needed)ShowWindow(button,needed?SW_SHOW:SW_HIDE);
    }
    void OpenLyricMatching() {
        if(stageUi)OpenBackstage(2);
        else{page=0;TabCtrl_SetCurSel(tabs,0);SetPanel(true);NoteActivity();}
        bool busy=false;{std::lock_guard lock(data.mutex);busy=data.busy;}
        if(!busy)Search(true);else uiNotice=L"歌词仍在加载，完成后可重新搜索或选择版本";
        if(!stageUi){SetFocus(search);EnsurePaneFocusVisible(true);}
    }
    void OpenBackstage(int target=-1) {
        if(!stageUi)return;
        if(target>=0)interfaceState.page=std::clamp(target,0,3);
        const auto root=window.load();
        if(root&&(!IsWindowVisible(root)||IsIconic(root)))OpenWindow();
        if(!interfaceState.backstage){interfaceState.backstage=true;stageUi->ClearFocus();}
        NoteActivity();
    }
    void CloseBackstage() {
        if(!stageUi||!interfaceState.backstage)return;
        interfaceState.backstage=false;interfaceState.offsetEditing=false;
        stageUi->ClearFocus();NoteActivity();
    }
    void ToggleBackstage() {
        if(!stageUi){SetPanel(!panelVisible);Persist();return;}
        if(interfaceState.backstage)CloseBackstage();else OpenBackstage();
    }
    HWND Control(int id) const {
        if(const auto direct=GetDlgItem(window,id))return direct;
        return settingsPane?GetDlgItem(settingsPane,id):nullptr;
    }
    void NoteActivity() {
        interactionAt=MonotonicMs();
        if(quietFullscreen){quietFullscreen=false;if(!stageUi)ShowWindow(Control(ID_PANEL),SW_SHOW);}
        if(cursorHidden){cursorHidden=false;POINT p{};if(GetCursorPos(&p)&&WindowFromPoint(p)==window)SetCursor(LoadCursorW(nullptr,IDC_ARROW));}
    }
    void UpdateFullscreenChrome() {
        const bool eligible=fullscreen&&!panelVisible&&!interfaceState.backstage&&!manualLyricBrowse&&!NestedLoopActive()&&settings.fullscreenAutoHide&&window&&
            GetForegroundWindow()==window&&IsWindowVisible(window)&&!IsIconic(window)&&!GetCapture();
        POINT pointer{};const bool hasPointer=GetCursorPos(&pointer)!=FALSE;
        if(hasPointer){
            if(eligible&&(!pointerKnown||pointer.x!=lastPointer.x||pointer.y!=lastPointer.y))interactionAt=MonotonicMs();
            lastPointer=pointer;pointerKnown=true;
        }
        const bool quiet=eligible&&MonotonicMs()-interactionAt>=2800;
        if(quiet!=quietFullscreen){
            quietFullscreen=quiet;const auto toggle=Control(ID_PANEL);
            if(quiet&&GetFocus()==toggle)SetFocus(window);
            // The console hides with quiet fullscreen; the native toggle is retired.
            if(stageUi){if(quiet)stageUi->ClearFocus();}
            else ShowWindow(toggle,quiet?SW_HIDE:SW_SHOW);
        }
        const bool ownPointer=hasPointer&&WindowFromPoint(pointer)==window;
        if(quiet&&ownPointer){SetCursor(nullptr);cursorHidden=true;}
        else if(cursorHidden){if(ownPointer)SetCursor(LoadCursorW(nullptr,IDC_ARROW));cursorHidden=false;}
    }
    void ScrollPane(int delta) {
        const int currentPage=std::clamp(page,0,3);auto& scroll=paneScroll[currentPage];
        const int next=std::clamp(scroll+delta,0,std::max(0,paneContentHeight-paneVisibleHeight));
        if(next==scroll)return;scroll=next;Layout();
    }
    void PaneWheel(int wheelDelta) {
        NoteActivity();paneWheelCarry-=wheelDelta*36.f/WHEEL_DELTA;
        const int delta=(int)paneWheelCarry;paneWheelCarry-=delta;if(delta)ScrollPane(delta);
    }
    void PaneScrollCommand(int command) {
        NoteActivity();SCROLLINFO info{sizeof(info),SIF_TRACKPOS};GetScrollInfo(settingsPane,SB_VERT,&info);
        switch(command){
            case SB_LINEUP:ScrollPane(-32);break;case SB_LINEDOWN:ScrollPane(32);break;
            case SB_PAGEUP:ScrollPane(-std::max(32,paneVisibleHeight-32));break;
            case SB_PAGEDOWN:ScrollPane(std::max(32,paneVisibleHeight-32));break;
            case SB_TOP:ScrollPane(-paneContentHeight);break;case SB_BOTTOM:ScrollPane(paneContentHeight);break;
            case SB_THUMBTRACK:case SB_THUMBPOSITION:ScrollPane(info.nTrackPos-paneScroll[std::clamp(page,0,3)]);break;
        }
    }
    void EnsurePaneFocusVisible(bool force=false) {
        const auto focused=GetFocus();if(!settingsPane||!panelVisible||!focused||!IsChild(settingsPane,focused)){lastPaneFocus=nullptr;paneFocusRevealPending=false;return;}
        if(!force&&!paneFocusRevealPending&&focused==lastPaneFocus)return;
        // Native buttons/trackbars own the pointer until release. Moving the
        // pane between button-down and button-up can cancel a legitimate click.
        const auto pointerCapture=GetCapture();
        if(pointerCapture&&(pointerCapture==settingsPane||IsChild(settingsPane,pointerCapture))){paneFocusRevealPending=true;return;}
        lastPaneFocus=focused;paneFocusRevealPending=false;
        RECT child{},client{};if(!GetWindowRect(focused,&child)||!GetClientRect(settingsPane,&client))return;
        MapWindowPoints(nullptr,settingsPane,reinterpret_cast<POINT*>(&child),2);
        const float scale=GetDpiForWindow(window)/96.f;
        if(child.top<0)ScrollPane((int)std::floor(child.top/scale)-8);
        else if(child.bottom>client.bottom)ScrollPane((int)std::ceil((child.bottom-client.bottom)/scale)+8);
    }
    void ChangeLyricMode(int mode) {
        settings.lyricDisplayMode=std::clamp(mode,0,2);SendMessageW(lyricModeCombo,CB_SETCURSEL,settings.lyricDisplayMode,0);
        NoteActivity();Persist();
    }
    void MarkTrackPreferencesLocked() {
        data.preferenceRevision++;data.preferencesDirty=true;data.preferenceSaveAt=MonotonicMs()+750;
        if(!data.trackPreferences.writable)data.preferenceNotice=L"歌词记忆保存已暂停，本次调整仅在当前运行中生效；可在歌词页恢复保存";
    }
    TrackLyricPreference* PreferenceForEditLocked(const std::wstring& key) {
        if(key.empty())return nullptr;
        auto& entries=data.trackPreferences.entries;auto found=entries.find(key);
        if(found!=entries.end())return &found->second;
        if(entries.size()>=512){data.preferenceNotice=L"已记住512首歌，既有记录已保留，本首调整暂不保存";return nullptr;}
        return &entries.emplace(key,TrackLyricPreference{}).first->second;
    }
    void SyncOffsetText() {
        if(!offsetEdit)return;settingOffsetText=true;
        SetWindowTextW(offsetEdit,std::to_wstring(offset).c_str());settingOffsetText=false;
    }
    void RefreshTrackPreferences() {
        bool changed=false,hasTrack=false;
        {
            std::lock_guard lock(data.mutex);
            if(data.playback.metadataStale)return;
            const auto key=CurrentPreferenceKeyLocked();const auto meta=PlayerLyricKey(data.playback);
            bool inheritedOffset=false;
            hasTrack=!key.empty();
            // A metadata preference can predate this process. Once the native
            // song ID is verified, migrate that exact title/artist record even
            // when this generation started directly with the ID. Existing
            // song-specific fields win; only missing fields are inherited.
            if(key.starts_with(L"song:")&&meta.starts_with(L"meta:")&&key!=meta){
                auto provisional=data.trackPreferences.entries.find(meta);
                if(provisional!=data.trackPreferences.entries.end()){
                    const auto provisionalImport=provisional->second.importPath;
                    auto exact=data.trackPreferences.entries.find(key);
                    if(exact==data.trackPreferences.entries.end()){
                        // Keep the provisional record intact until the new key
                        // is inserted; an allocation failure must not erase the
                        // only copy held in memory.
                        auto preference=provisional->second;
                        const bool hasOffset=preference.offsetMs.has_value();
                        data.trackPreferences.entries.emplace(key,std::move(preference));
                        inheritedOffset=hasOffset;
                        data.trackPreferences.entries.erase(meta);
                    }else{
                        if(!exact->second.offsetMs&&provisional->second.offsetMs){
                            exact->second.offsetMs=provisional->second.offsetMs;inheritedOffset=true;
                        }
                        if(exact->second.importPath.empty()&&!provisional->second.importPath.empty())exact->second.importPath=std::move(provisional->second.importPath);
                        data.trackPreferences.entries.erase(provisional);
                    }
                    MarkTrackPreferencesLocked();
                    const auto resolved=data.trackPreferences.entries.find(key);
                    if(data.importAttemptGeneration==data.generation&&data.importAttemptKey==meta&&resolved!=data.trackPreferences.entries.end()&&
                       resolved->second.importPath==provisionalImport)data.importAttemptKey=key;
                }
            }
            // Native ID may already be active when its catalog text arrives.
            // Newly inherited offsets must reach the active value as well as
            // the map. Ordinary refreshes still retain session-only edits.
            if(key==activePreferenceKey&&data.generation==activePreferenceGeneration&&!inheritedOffset)return;
            const auto found=data.trackPreferences.entries.find(key);
            offset=found!=data.trackPreferences.entries.end()&&found->second.offsetMs?*found->second.offsetMs:settings.offsetMs;
            activePreferenceKey=key;activeMetadataKey=meta;activePreferenceGeneration=data.generation;changed=true;
        }
        if(changed){SyncOffsetText();NoteActivity();if(window){EnableWindow(offsetEdit,hasTrack);EnableWindow(Control(ID_OFFSETRESET),hasTrack);}}
    }
    void OffsetChanged() {
        if(settingOffsetText||!controlsReady)return;
        RefreshTrackPreferences();wchar_t text[32]{};const int count=GetWindowTextW(offsetEdit,text,32);
        if(count<=0||count>=31)return;
        int index=0,sign=1,value=0;if(text[0]==L'-'||text[0]==L'+'){sign=text[0]==L'-'?-1:1;index++;}
        if(index==count)return;
        for(;index<count;index++){if(text[index]<L'0'||text[index]>L'9')return;value=std::min(30001,value*10+(text[index]-L'0'));}
        ApplyOffsetValue(sign*value);
    }
    // The current song's offset, by value (native edit or stage UI). Only the
    // preference of the identity that is active right now is changed.
    void ApplyOffsetValue(int value) {
        RefreshTrackPreferences();
        const int newOffset=std::clamp(value,-30000,30000);
        std::lock_guard lock(data.mutex);if(activePreferenceGeneration!=data.generation||activePreferenceKey!=CurrentPreferenceKeyLocked())return;
        offset=newOffset;if(auto preference=PreferenceForEditLocked(activePreferenceKey)){
            if(preference->offsetMs!=newOffset){preference->offsetMs=newOffset;MarkTrackPreferencesLocked();}
        }
    }
    void CommitOrRestoreOffsetText() {
        // Valid edits already commit through EN_CHANGE. Merely leaving the
        // inherited display must not create a song-specific override.
        RefreshTrackPreferences();
        // Partial signs, empty text and non-numeric paste are intentionally
        // tolerated while editing, then silently restored to the committed
        // bounded value when the control loses focus.
        SyncOffsetText();
    }
    void ResetOffset() {
        RefreshTrackPreferences();
        {std::lock_guard lock(data.mutex);if(data.playback.metadataStale)return;offset=0;if(auto preference=PreferenceForEditLocked(CurrentPreferenceKeyLocked())){
            if(preference->offsetMs!=0){preference->offsetMs=0;MarkTrackPreferencesLocked();}
        }}SyncOffsetText();
    }
    void ApplyTrackPreferenceSaveResult(TrackPreferencesSaveResult result,bool notify) {
        bool currentContent=false,reportFailure=false,reportRecovery=false;std::wstring previousDiagnostic;
        {
            std::lock_guard lock(data.mutex);
            if(result.requestId!=data.preferenceSaveRequest)return;
            data.preferenceSaveInFlight=false;
            currentContent=result.revision==data.preferenceRevision;
            if(currentContent){
                previousDiagnostic=data.preferenceSaveDiagnostic;
                if(result.ok){
                    data.preferencesDirty=false;data.preferenceSaveAt=0;
                    reportRecovery=!previousDiagnostic.empty();data.preferenceSaveDiagnostic.clear();
                }else{
                    data.preferencesDirty=true;
                    const auto now=MonotonicMs(),retryAt=now+5000;
                    if(data.preferenceSaveAt<=now||data.preferenceSaveAt>retryAt)data.preferenceSaveAt=retryAt;
                    reportFailure=previousDiagnostic!=result.diagnostic;
                    data.preferenceSaveDiagnostic=result.diagnostic;
                }
            }else{
                // A successful OLD snapshot cannot acknowledge newer content
                // or clear its error/debounce. No saved values are copied back.
                data.preferencesDirty=true;
                if(!data.preferenceSaveAt)data.preferenceSaveAt=MonotonicMs()+750;
            }
        }
        if(!currentContent)log.Event("track-preferences-save-superseded","contentRevision="+std::to_string(result.revision)+(result.ok?" saved=true":" saved=false"));
        else if(reportFailure||(!notify&&!result.ok)){
            if(notify)uiNotice=result.diagnostic;
            log.Event(notify?"track-preferences-save":"track-preferences-save-at-exit",Utf8(result.diagnostic));
        }else if(reportRecovery){
            if(notify&&uiNotice==previousDiagnostic)uiNotice=L"歌词记忆保存已恢复";
            log.Event("track-preferences-save-recovered","Current lyric memory content committed");
        }
        if(notify&&!exiting)UpdateTrackPreferenceNotice();
    }
    void PollTrackPreferenceSave(bool notify=true) {
        if(auto result=settingsWriter.PollPreferences())ApplyTrackPreferenceSaveResult(std::move(*result),notify);
    }
    void FlushTrackPreferences(bool force=false) {
        PollTrackPreferenceSave(!exiting);
        TrackPreferencesSaveRequest request;bool protectedFile=false;std::wstring failure;
        try{
            std::lock_guard lock(data.mutex);if(!data.preferencesDirty)return;
            protectedFile=!data.trackPreferences.writable;
            if(protectedFile)data.preferenceSaveAt=0;
            else{
                if(data.preferenceSaveInFlight&&data.preferenceQueuedRevision==data.preferenceRevision)return;
                if(!force&&MonotonicMs()<data.preferenceSaveAt)return;
                request.requestId=++data.preferenceSaveRequest;request.revision=data.preferenceRevision;
                data.preferenceSaveInFlight=true;data.preferenceQueuedRevision=data.preferenceRevision;
                request.preferences=data.trackPreferences; // Bounded entry count; file I/O stays off this lock.
                data.preferenceSaveAt=0;
            }
        }catch(...){failure=L"无法准备歌词记忆保存任务，当前修改仍保留在运行中";}
        if(protectedFile){
            if(!preferenceWriteBlockedLogged){preferenceWriteBlockedLogged=true;log.Event("track-preferences-write-protected","Original lyric memory preserved; explicit recovery available");}
            return;
        }
        const auto requestId=request.requestId,contentRevision=request.revision;
        if(failure.empty()){
            try{if(settingsWriter.SubmitPreferences(std::move(request)))return;}
            catch(...){failure=L"无法提交歌词记忆保存任务，当前修改仍保留在运行中";}
            if(failure.empty())failure=L"保存线程已停止，歌词记忆修改仍保留在运行中";
        }
        ApplyTrackPreferenceSaveResult({requestId,contentRevision,false,std::move(failure)},!exiting);
    }
    // data.mutex is held. Imported-file association is a commit side effect:
    // a result waiting for post-completion identity proof must not write it.
    void RememberImportLocked(const std::filesystem::path& path,uint64_t gen) {
        if(gen!=data.generation||exiting)return;
        const auto key=CurrentPreferenceKeyLocked();
        if(auto preference=PreferenceForEditLocked(key)){
            if(preference->importPath!=path.wstring()){preference->importPath=path.wstring();MarkTrackPreferencesLocked();}
            data.importAttemptGeneration=gen;data.importAttemptKey=key;
        }else if(key.empty())data.preferenceNotice=L"本地歌词已载入；识别到歌曲后再次导入可记住关联";
    }
    void RestoreRememberedImport() {
        std::filesystem::path path;std::wstring key;uint64_t gen;std::stop_token token;LyricRequestOwner owner;
        {
            std::lock_guard lock(data.mutex);if(data.busy||data.manualQueryActive||!HasLyricLookupIdentity(data.playback)||
                (data.loadedOrigin==LyricOrigin::LocalImport&&data.localImportBinding&&
                    data.localImportBinding->awaitingReconfirmation))return;
            gen=data.generation;key=CurrentPreferenceKeyLocked();
            if(data.importAttemptGeneration==gen&&data.importAttemptKey==key)return;
            const auto found=data.trackPreferences.entries.find(key);
            if(found==data.trackPreferences.entries.end()||found->second.importPath.empty())return;
            path=found->second.importPath;token=data.generationStop.get_token();owner=CaptureLyricRequestOwner(data.playback,gen);
        }
        if(Queue(gen,false,L"正在恢复这首歌的本地歌词…",[this,path,gen,token,owner]{
            auto client=MakeLyricsClient(token);auto result=client.Import(path);
            ApplyLyricResult(std::move(result),owner,{},LyricOrigin::LocalImport,nullptr,false,nullptr,true);
        })){std::lock_guard lock(data.mutex);if(gen==data.generation){data.importAttemptGeneration=gen;data.importAttemptKey=key;}}
    }
    void RestoreOnlineLyrics() {
        bool independent=false;{std::lock_guard lock(data.mutex);independent=ManualPreviewLocked();}
        if(independent){FollowCurrentLyrics();return;}
        {
            std::lock_guard lock(data.mutex);if(data.busy||!HasLyricLookupIdentity(data.playback))return;
            // Also clear a provisional association retained beside an ID record.
            const auto metadataKey=HasSearchableSongText(data.playback)?PlayerLyricKey(data.playback):std::wstring{};
            for(const auto& key:{CurrentPreferenceKeyLocked(),metadataKey}){
                if(key.empty())continue;
                auto found=data.trackPreferences.entries.find(key);if(found==data.trackPreferences.entries.end())continue;
                if(!found->second.importPath.empty()){found->second.importPath.clear();MarkTrackPreferencesLocked();}
                if(!found->second.offsetMs)data.trackPreferences.entries.erase(found);
            }
            data.manualLyrics=false;data.retryAfter=0;data.retrySongId.clear();data.autoSearchGeneration=UINT64_MAX;
            data.importAttemptGeneration=data.generation;data.importAttemptKey=CurrentPreferenceKeyLocked();
        }
        Search();uiNotice=L"已取消这首歌的导入关联，在线歌词就绪前保留现有文字";
    }
    void MaintainCover() {
        uint64_t gen;std::wstring id;std::stop_token token;CoverIdOrigin origin=CoverIdOrigin::SelectedCandidate;
        bool independent=false,automaticCandidate=false,forceRevalidate=false;
        std::filesystem::path protectedFile;
        {
            std::lock_guard lock(data.mutex);
            const auto now=MonotonicMs();
            if(backgroundSelection!=0||now<data.localCoverRetryAt)return;
            independent=ManualPreviewLocked();
            if(!independent&&data.playback.provider!=MusicProvider::Netease){
                data.coverStatus=std::wstring(MusicProviderName(data.playback.provider))+(data.playback.metadataStale?L"：等待重新确认歌曲":
                    data.playback.mediaCover?L"当前歌曲封面":L"：等待提供封面");
                data.coverError.clear();return;
            }
            if(independent){
                if(data.manualLoadedGeneration!=data.generation)return;
                id=data.matchedId;
            }else{
                if(data.playback.metadataStale)return;
                if(data.playback.songIdVerified){id=data.playback.songId;origin=CoverIdOrigin::VerifiedPlaybackId;}
                else{
                    // Unverified candidates still need an accepted lyric
                    // association. The current native ID needs no lyrics.
                    if(data.busy||(data.lyrics->state!=LyricLoadState::Ready&&data.lyrics->state!=LyricLoadState::NoLyrics))return;
                    if(!data.manualLyrics&&data.autoSearchGeneration!=data.generation&&now>=data.retryAfter)return;
                    if(data.loadedOrigin==LyricOrigin::LocalImport)id=data.verifiedTrackId;
                    else{id=data.matchedId;automaticCandidate=data.loadedOrigin==LyricOrigin::AutomaticCandidate;}
                }
            }
            gen=data.generation;token=data.generationStop.get_token();
            if(id.empty()){
                if(independent)return;
                const auto saved=settings.confirmedCandidates.find(PlayerMatchKey(data.playback));
                if(saved!=settings.confirmedCandidates.end()){id=saved->second;origin=CoverIdOrigin::SelectedCandidate;}
            }
            if(!CoverIdentityAllowedLocked(id,automaticCandidate)||
                (!data.coverRevalidatePending&&!data.coverFile.empty()&&SameSongId(data.coverSongId,id)))return;
            forceRevalidate=data.coverRevalidatePending;
            if(forceRevalidate&&!data.coverFile.empty())protectedFile=data.coverFile;
        }
        QueueCover(id,gen,origin,token,automaticCandidate,forceRevalidate,std::move(protectedFile));
    }
    void ScheduleRetryLocked() {
        static constexpr int delays[]={3,8,15,30,60};
        const int slot=std::clamp(data.retryFailures,0,4);
        data.retryFailures=std::min(data.retryFailures+1,5);
        data.retryAfter=MonotonicMs()+delays[slot]*1000LL;
        data.autoSearchGeneration=UINT64_MAX;
        data.manualLyrics=false;
        data.networkStatus+=L" · "+std::to_wstring(delays[slot])+L" 秒后自动重试";
    }
    bool HasRetainedRefreshSourceLocked(const std::wstring& id) const {
        return data.lyrics&&data.lyrics->state==LyricLoadState::Ready&&!data.lyrics->lines.empty()&&
            SameCatalogId(id,data.matchedId)&&SameCatalogId(id,data.lyrics->songId)&&
            (data.lyrics->fromCache||data.lyrics->retryWordTiming);
    }
    void RunJobs() {
        while(true){
            JobRequest request;
            {
                std::unique_lock lock(jobMutex);
                jobCv.wait(lock,[this]{return stopping||job.has_value();});
                if(stopping)return;
                request=std::move(*job);job.reset();
            }
            try{request.task();}
            catch(const std::exception& e){
                {
                    std::lock_guard lock(data.mutex);
                    if(request.generation==data.generation&&!exiting){
                        data.networkStatus=L"歌词任务暂时失败，请重试";
                        if(data.lyrics->lines.empty()){
                            auto failed=std::make_shared<LyricTimeline>();failed->state=LyricLoadState::Failed;
                            data.lyrics=std::move(failed);data.lyricCacheState=LyricCacheState::None;MarkLyricContentChanged();
                        }
                        // The cache result can still be behind the identity
                        // barrier when its following online refresh throws.
                        // Preserve the cache and carry the retry through its
                        // eventual commit instead of leaving it "in flight".
                        auto retryTarget=request.retryTargetId;
                        if(!SameCatalogId(retryTarget,retryTarget)&&
                                data.busyGeneration==request.generation)
                            retryTarget=data.automaticLyricTargetId;
                        bool retryCarriedByPending=false;
                        if(data.pendingLyricCommit&&data.pendingLyricCommit->owner.generation==request.generation&&
                            SameCatalogId(retryTarget,data.pendingLyricCommit->lyricId)&&
                            SameCatalogId(retryTarget,data.pendingLyricCommit->result.songId)&&
                            data.pendingLyricCommit->result.state==LyricLoadState::Ready&&data.pendingLyricCommit->result.fromCache&&
                            data.pendingLyricCommit->refreshInFlight){
                            data.pendingLyricCommit->refreshInFlight=false;
                            data.pendingLyricCommit->refreshRetryRequired=true;
                            retryCarriedByPending=true;
                        }
                        if(request.retryOnException&&!retryCarriedByPending){
                            const bool targetIsId=SameCatalogId(retryTarget,retryTarget);
                            const bool targetAllowed=LyricRetryTargetAllowed(data.playback,retryTarget,request.retryOrigin);
                            if(targetAllowed){
                                data.retrySongId=retryTarget;
                                data.retryOrigin=request.retryOrigin;
                                if(HasRetainedRefreshSourceLocked(retryTarget))
                                    data.retryFailures=std::max(data.retryFailures,3);
                            }else if(targetIsId){
                                // New native/record evidence disproved the
                                // exact automatic target while it was in
                                // flight. Retry discovery promptly instead of
                                // inheriting that obsolete target's backoff.
                                data.retrySongId.clear();
                                data.retryOrigin=LyricOrigin::AutomaticCandidate;
                                data.retryFailures=0;
                            }
                            ScheduleRetryLocked();
                        }
                    }
                }
                log.Event("network-exception",e.what());
            }
            {
                // A job owns the busy flag until every return/exception has
                // unwound. Old-song exceptions cannot change new-song state.
                std::lock_guard lock(data.mutex);if(data.busyGeneration==request.generation)ClearLyricBusyLocked();
            }
            if(const auto target=window.load())PostMessageW(target,WM_DATA,0,0);
        }
    }
    bool Queue(uint64_t gen,bool retryOnException,std::wstring statusText,std::function<void()> task,
               bool allowUnidentified=false,bool automaticLookup=false,std::wstring retryTargetId={},
               LyricOrigin retryOrigin=LyricOrigin::AutomaticCandidate) {
        if(exiting)return false;
        {
            std::scoped_lock lock(data.mutex,jobMutex);
            if(stopping||data.busy||(!allowUnidentified&&!HasLyricLookupIdentity(data.playback))||gen!=data.generation)return false;
            // A verified ID may arrive between Search's snapshot and this
            // queue operation. Keep only an automatic request already aimed
            // at that exact ID; otherwise the next tick takes the direct path.
            if(automaticLookup&&data.playback.songIdVerified&&
                    (!SameCatalogId(retryTargetId,retryTargetId)||
                     !SameCatalogId(retryTargetId,data.playback.songId)))return false;
            // A newly accepted request supersedes any completed result that
            // was still waiting for its post-completion identity sample.
            DiscardPendingLyricLocked(LyricCommitReason::NewRequest);
            job=JobRequest{gen,retryOnException,std::move(retryTargetId),retryOrigin,std::move(task)};
            data.busy=true;data.busyAutomaticLookup=automaticLookup;
            // Publish retained-refresh ownership before the worker can begin.
            // A native identity loss in this short queue-to-task interval must
            // still suspend and later resume the exact cached/line-timed lyric
            // version instead of falling back to another playback ID.
            data.lyricRefreshPending=retryOnException&&
                LyricRetryTargetAllowed(data.playback,job->retryTargetId,job->retryOrigin)&&
                HasRetainedRefreshSourceLocked(job->retryTargetId);
            data.busyGeneration=gen;if(retryOnException)data.autoSearchGeneration=gen;
            data.automaticLyricTargetId=automaticLookup&&SameCatalogId(job->retryTargetId,job->retryTargetId)?
                job->retryTargetId:std::wstring{};
            data.networkStatus=std::move(statusText);
            if(data.lyrics->lines.empty()){
                auto loading=std::make_shared<LyricTimeline>();loading->state=LyricLoadState::Loading;
                data.lyrics=std::move(loading);data.lyricCacheState=LyricCacheState::None;MarkLyricContentChanged();
            }
        }
        jobCv.notify_one();return true;
    }
    void Begin() {
        monitor.Start([this](const PlaybackSnapshot& p){
            std::scoped_lock lock(data.mutex,jobMutex);
            if(exiting)return;
            if(data.manualQueryActive){
                data.playback=p;if(p.songIdVerified)data.verifiedTrackId=p.songId;
                if(data.loadedOrigin==LyricOrigin::ManualInput&&data.manualLoadedGeneration==data.generation&&
                    VerifiedLyricIdentityCurrent(p,data.matchedId)){
                    data.manualQueryActive=false;data.loadedOrigin=LyricOrigin::ConfirmedCandidate;data.manualLyrics=true;
                    data.manualTitle.clear();data.manualArtist.clear();data.suspendedNetworkStatus.clear();
                    data.suspendedLyricRefresh.reset();
                    data.networkStatus=L"手选歌词已核验为当前歌曲 · "+data.lyrics->diagnostic;
                    const bool ready=data.lyrics->state==LyricLoadState::Ready&&!data.lyrics->lines.empty();
                    if(ready)data.networkStatus+=L" · "+std::to_wstring(data.lyrics->lines.size())+L" 行";
                    if(ready&&(data.lyrics->fromCache||(data.lyrics->retryWordTiming&&!data.lyrics->wordTimed))){
                        data.retrySongId=data.matchedId;data.retryOrigin=LyricOrigin::ConfirmedCandidate;
                        data.retryFailures=std::max(data.retryFailures,3);ScheduleRetryLocked();
                    }
                    CancelObsoleteCoverRequestsLocked();
                }
                return;
            }
            if(p.identityReacquiring){
                if(!data.playback.identityReacquiring){
                    if(data.suspendedNetworkStatus.empty())data.suspendedNetworkStatus=data.networkStatus;
                    data.networkStatus=data.pendingLyricCommit?
                        L"歌词结果已就绪，等待当前歌曲重新确认":L"原生歌曲身份待重新确认，歌词保持静态并继续当前加载任务";
                }
                auto reacquiring=p;
                if(reacquiring.title.empty()){
                    reacquiring.title=data.playback.title;reacquiring.artist=data.playback.artist;reacquiring.album=data.playback.album;
                }
                data.playback=std::move(reacquiring);return;
            }
            if(p.metadataStale){
                if(!data.playback.metadataStale||data.playback.identityReacquiring){
                    // Preserve only a real, current lyric-refresh operation.
                    // The resume key is the PLAYBACK ID, not a lyric candidate.
                    data.suspendedLyricRefresh.reset();
                    const bool interruptedRefresh=data.busy&&data.busyGeneration==data.generation&&data.lyricRefreshPending;
                    // Reacquiring snapshots deliberately clear songIdVerified,
                    // but verifiedTrackId still names the last identity from
                    // this exact process epoch. Preserve it only inside the
                    // bounded refresh/local-import bindings below; hard-expiry
                    // cleanup removes it from every ordinary lookup/preference
                    // path.
                    const auto interruptedPlaybackId=data.playback.songIdVerified?data.playback.songId:
                        data.playback.identityReacquiring?data.verifiedTrackId:std::wstring{};
                    if(data.loadedOrigin==LyricOrigin::LocalImport){
                        auto importPlaybackId=interruptedPlaybackId;
                        if(!SameCatalogId(importPlaybackId,importPlaybackId)&&data.localImportBinding)
                            importPlaybackId=data.localImportBinding->playbackId;
                        // This binding is display-only while native identity is
                        // absent.  It cannot drive the timeline or become the
                        // next track's association until the exact process
                        // epoch and song ID are observed again.
                        data.localImportBinding=LocalImportBinding{data.playback.pid,
                            data.playback.processCreatedAtTicks,std::move(importPlaybackId),true};
                    }else data.localImportBinding.reset();
                    if(interruptedRefresh&&SameCatalogId(interruptedPlaybackId,interruptedPlaybackId)&&!data.matchedId.empty()&&
                            data.loadedOrigin!=LyricOrigin::LocalImport&&data.loadedOrigin!=LyricOrigin::ManualInput){
                        data.suspendedLyricRefresh=SuspendedLyricRefresh{data.playback.pid,interruptedPlaybackId,data.matchedId,data.loadedOrigin,
                            data.playback.processCreatedAtTicks,data.lyrics->retryWordTiming&&!data.lyrics->wordTimed};
                    }
                    data.generationStop.request_stop();data.generationStop=std::stop_source{};data.generation++;
                    DiscardPendingLyricLocked(LyricCommitReason::MetadataUnavailable,&p);
                    CancelCoverGenerationLocked();
                    job.reset();ClearLyricBusyLocked();
                    // Expiry is a hard identity boundary.  Keep the displayed
                    // lyric timeline as a static preview, but do not let its
                    // old verified ID or retry provenance become an input to
                    // the next unverified title/SMTC fallback.  A genuinely
                    // interrupted work/display can only be restored through an
                    // exact process-epoch + verified-playback-ID binding saved
                    // above.
                    data.verifiedTrackId.clear();
                    data.retrySongId.clear();data.retryOrigin=LyricOrigin::AutomaticCandidate;
                    data.retryAfter=0;data.retryFailures=0;
                    if(interruptedRefresh)data.suspendedNetworkStatus=L"此前歌词已保留，联网刷新已中断";
                    else if(data.suspendedNetworkStatus.empty())data.suspendedNetworkStatus=data.networkStatus;
                    revision++;
                    data.autoSearchGeneration=data.generation;data.networkStatus=L"等待当前歌曲信息确认，暂时保留此前歌词";
                }
                auto stale=p;
                // Display-only history. The stale snapshot's PID, verified ID
                // and clock remain unavailable; no target or motion is reused.
                if(stale.title.empty()){
                    stale.title=data.playback.title;stale.artist=data.playback.artist;stale.album=data.playback.album;
                }
                data.playback=std::move(stale);return;
            }
            const bool catalogLyrics=data.loadedOrigin==LyricOrigin::AutomaticCandidate||data.loadedOrigin==LyricOrigin::ConfirmedCandidate||
                data.loadedOrigin==LyricOrigin::VerifiedClient;
            const bool sameLoadedCatalog=catalogLyrics&&VerifiedLyricIdentityCurrent(p,data.matchedId);
            const bool sameAutomaticTarget=data.busy&&data.busyAutomaticLookup&&data.busyGeneration==data.generation&&
                VerifiedLyricIdentityCurrent(p,data.automaticLyricTargetId);
            const bool localImportAwaiting=data.loadedOrigin==LyricOrigin::LocalImport&&data.localImportBinding&&
                data.localImportBinding->awaitingReconfirmation;
            const bool localImportReconfirmed=localImportAwaiting&&p.songIdVerified&&
                p.pid==data.localImportBinding->pid&&p.processCreatedAtTicks&&
                p.processCreatedAtTicks==data.localImportBinding->processCreatedAtTicks&&
                SameCatalogId(p.songId,data.localImportBinding->playbackId);
            const bool sameVerifiedSong=p.songIdVerified&&(SameCatalogId(p.songId,data.verifiedTrackId)||sameLoadedCatalog||
                sameAutomaticTarget||localImportReconfirmed);
            const bool sameProcess=p.provider==data.playback.provider&&p.pid==data.playback.pid&&p.processCreatedAtTicks==data.playback.processCreatedAtTicks;
            const bool reconfirmedSong=data.playback.metadataStale&&sameProcess&&p.songIdVerified&&
                (SameCatalogId(p.songId,data.verifiedTrackId)||localImportReconfirmed);
            const bool reacquisitionLostVerification=data.playback.identityReacquiring&&!p.songIdVerified;
            // A bounded reacquisition keeps the generation only for the same
            // process epoch and verified ID. A different instance or ID
            // cancels old work even when the visible song happens to match.
            const bool identityChanged=!sameProcess||reacquisitionLostVerification||
                (p.provider!=MusicProvider::Netease&&p.mediaIdentityRevision!=data.playback.mediaIdentityRevision)||
                (!reconfirmedSong&&!sameVerifiedSong&&(p.title!=data.playback.title||p.artist!=data.playback.artist))||
                (p.songIdVerified&&!data.verifiedTrackId.empty()&&!SameCatalogId(p.songId,data.verifiedTrackId))||
                (localImportAwaiting&&p.songIdVerified&&!localImportReconfirmed);
            // AutomaticCandidate and VerifiedClient both bind matchedId to the
            // catalog song selected for automatic following. After a hard
            // identity loss, an exact different ID must retire that timeline
            // even when the new song has identical visible title/artist text.
            const bool identityResolved=p.songIdVerified&&!data.playback.songIdVerified&&
                (data.loadedOrigin==LyricOrigin::AutomaticCandidate||data.loadedOrigin==LyricOrigin::VerifiedClient)&&
                !data.matchedId.empty()&&!SameCatalogId(data.matchedId,p.songId);
            if(identityChanged||identityResolved){
                data.suspendedNetworkStatus.clear();
                data.suspendedLyricRefresh.reset();
                data.generationStop.request_stop();data.generationStop=std::stop_source{};data.generation++;
                DiscardPendingLyricLocked(sameProcess?LyricCommitReason::PlaybackBoundaryChanged:LyricCommitReason::ProcessChanged,&p);
                CancelCoverGenerationLocked();
                job.reset();ClearLyricBusyLocked();
                data.lyrics=std::make_shared<LyricTimeline>();data.lyricCacheState=LyricCacheState::None;data.candidates.clear();data.matchedId.clear();
                data.candidatesManual=false;data.manualTitle.clear();data.manualArtist.clear();
                data.coverFile.clear();data.coverSongId.clear();data.coverStatus.clear();data.coverError.clear();if(backgroundSelection==0)background.Clear();
                data.coverRevalidatePending=false;
                data.manualLyrics=false;data.songChangedAt=MonotonicMs();data.retryAfter=0;data.retryFailures=0;
                data.loadedOrigin=LyricOrigin::AutomaticCandidate;
                data.localImportBinding.reset();
                data.retrySongId.clear();data.autoSearchGeneration=UINT64_MAX;revision++;MarkLyricContentChanged();
                data.localCoverRetryAt=0;
                data.verifiedTrackId=p.songIdVerified?p.songId:std::wstring{};
                data.networkStatus=p.songIdVerified&&!HasSearchableSongText(p)?L"当前歌曲 ID 已确认，歌曲资料加载中，正在准备歌词…":
                    p.title.empty()?L"等待网易云、QQ 音乐、酷狗或汽水音乐歌曲":L"歌曲已识别，正在准备自动查找歌词…";
            }else if(p.songIdVerified&&!data.playback.songIdVerified&&data.busy&&data.busyAutomaticLookup&&
                    !sameAutomaticTarget){
                // A title search started before native ID verification. It
                // must not keep the known-ID lyric load waiting on its HTTP
                // requests. Compare the active FETCH target, not an older
                // displayed lyric ID; retain a current same-ID fetch.
                data.generationStop.request_stop();data.generationStop=std::stop_source{};++data.generation;
                DiscardPendingLyricLocked(LyricCommitReason::VerifiedIdentityArrived,&p);
                CancelCoverGenerationLocked();
                job.reset();ClearLyricBusyLocked();
                data.retrySongId.clear();data.retryAfter=0;data.autoSearchGeneration=UINT64_MAX;
                data.networkStatus=L"播放器已确认歌曲版本，正在加载对应歌词…";
            }else if(!data.manualLyrics&&!data.pendingLyricCommit&&(data.playback.metadataStale||
                    (p.songIdVerified&&!data.playback.songIdVerified&&data.lyrics->lines.empty()&&!sameLoadedCatalog&&!sameAutomaticTarget))){
                // Identity enrichment is not a new song. Retry the now-known
                // ID without erasing lyrics, cover, or the active job. A
                // same-ID NoLyrics result or fetch already owns that decision.
                data.autoSearchGeneration=UINT64_MAX;data.retryAfter=0;
            }
            if(!p.songIdVerified&&!data.manualLyrics&&p.recordCandidateId!=data.playback.recordCandidateId&&
                SameSongId(p.recordCandidateId,p.recordCandidateId)){
                data.autoSearchGeneration=UINT64_MAX;data.retryAfter=0;
                if(data.retryOrigin==LyricOrigin::AutomaticCandidate)data.retrySongId.clear();
            }
            if(data.playback.metadataStale){
                const auto& suspended=data.suspendedLyricRefresh;
                if(suspended&&p.pid==suspended->pid&&p.processCreatedAtTicks&&p.processCreatedAtTicks==suspended->processCreatedAtTicks&&p.songIdVerified&&SameCatalogId(p.songId,suspended->playbackId)){
                    data.retrySongId=suspended->lyricId;data.retryOrigin=suspended->origin;
                    data.manualLyrics=false;data.autoSearchGeneration=UINT64_MAX;data.retryAfter=0;
                    data.networkStatus=suspended->wordTiming?L"当前歌曲已重新核验，继续尝试逐字歌词…":L"当前歌曲已重新核验，继续刷新歌词…";
                }else if(!data.pendingLyricCommit&&!data.suspendedNetworkStatus.empty())data.networkStatus=std::move(data.suspendedNetworkStatus);
                data.suspendedNetworkStatus.clear();data.suspendedLyricRefresh.reset();
            }
            if(localImportReconfirmed&&data.localImportBinding)data.localImportBinding->awaitingReconfirmation=false;
            else if(data.loadedOrigin==LyricOrigin::LocalImport&&data.localImportBinding&&
                    !data.localImportBinding->awaitingReconfirmation&&data.localImportBinding->playbackId.empty()&&
                    p.songIdVerified&&p.pid==data.localImportBinding->pid&&
                    p.processCreatedAtTicks==data.localImportBinding->processCreatedAtTicks)
                data.localImportBinding->playbackId=p.songId;
            PromoteAutomaticLyricIdentityLocked(p);
            if(p.songIdVerified)data.verifiedTrackId=p.songId;
            data.playback=p;
            CancelObsoleteCoverRequestsLocked();
        },settings.nativeClockEnabled);
        log.Event("application-start",renderer.Software()?"D3D11 WARP":"D3D11 hardware");SetTimer(window,TimerTick,250,nullptr);
    }
    void UpdateStatusText(std::wstring text) {
        if(!status||text==shownStatusText)return;
        DWORD selectionStart=0,selectionEnd=0;
        SendMessageW(status,EM_GETSEL,reinterpret_cast<WPARAM>(&selectionStart),reinterpret_cast<LPARAM>(&selectionEnd));
        const auto firstLine=SendMessageW(status,EM_GETFIRSTVISIBLELINE,0,0);
        if(!SetWindowTextW(status,text.c_str()))return;
        shownStatusText=std::move(text);
        const auto length=static_cast<DWORD>(std::max(0,GetWindowTextLengthW(status)));
        SendMessageW(status,EM_SETSEL,std::min(selectionStart,length),std::min(selectionEnd,length));
        SendMessageW(status,EM_LINESCROLL,0,firstLine-SendMessageW(status,EM_GETFIRSTVISIBLELINE,0,0));
    }
    bool FullscreenUsesMonitorBounds() const noexcept {
        return fullscreenTransition==FullscreenTransition::Entering||
            (fullscreen&&fullscreenTransition!=FullscreenTransition::Exiting);
    }
    void RequestWindowBoundsFit() noexcept {
        windowBoundsPending=true;windowBoundsFailures=0;windowBoundsRetryAt=0;
    }
    void MaintainWindowBounds() {
        if(!windowBoundsPending||NestedLoopActive()||frameDrawing||frameWaiting)return;
        const auto now=MonotonicMs();if(now<windowBoundsRetryAt)return;
        const auto root=window.load();
        if(!root||!IsWindow(root)){windowBoundsPending=false;windowBoundsFailures=0;windowBoundsRetryAt=0;return;}
        if(IsIconic(root)){windowBoundsRetryAt=now+500;return;}
        const bool monitorBounds=FullscreenUsesMonitorBounds();
        if((!monitorBounds&&IsZoomed(root))||FitOwnWindow(root,monitorBounds)){
            windowBoundsPending=false;windowBoundsFailures=0;windowBoundsRetryAt=0;return;
        }
        if(++windowBoundsFailures>=4){
            windowBoundsPending=false;windowBoundsRetryAt=0;
            uiNotice=L"窗口范围暂时无法适配当前显示器；恢复或移动窗口后会再次尝试";
            log.Event("window-bounds-incomplete",Utf8(uiNotice));
            return;
        }
        windowBoundsRetryAt=now+250*windowBoundsFailures;
    }
    void ApplyVisualMediaDemand(const PlaybackSnapshot& playback,const AudioFeatures& audio) {
        const bool visualDemand=HasVisualDemand();
        background.SetPlaybackDemand(visualDemand);
        MaintainAudio(capture,audioPid,audioCreatedAt,audioRetryAt,audioFailures,playback,audio,visualDemand,
            [this](const AudioFeatures& features){std::lock_guard lock(data.mutex);if(!exiting)data.audio=features;});
    }
    void RefreshVisualMediaDemand() {
        if(exiting)return;
        PlaybackSnapshot playback;AudioFeatures audio;
        {std::lock_guard lock(data.mutex);playback=data.playback;audio=data.audio;}
        ApplyVisualMediaDemand(playback,audio);
    }
    void Tick() {
        MaintainWindowBounds();
        PollForegroundCover();PollPower();
        TryCommitPendingLyric();
        FlushLyricCommitTraces();
        PollSettingsSave();
        if(settingsSaveAt&&MonotonicMs()>=settingsSaveAt)Persist();
        RefreshTrackPreferences();RestoreRememberedImport();MaintainCover();FlushTrackPreferences();
        {std::lock_guard lock(data.mutex);if(!data.preferenceNotice.empty()){uiNotice=std::move(data.preferenceNotice);data.preferenceNotice.clear();}}
        PlaybackSnapshot p;AudioFeatures a;std::wstring lyricStatus;size_t lyricCount=0;{std::lock_guard lock(data.mutex);p=data.playback;a=data.audio;lyricStatus=data.networkStatus;
            if(data.lyricCacheState!=LyricCacheState::None)lyricStatus=std::wstring(LyricCacheStatus(data.lyricCacheState))+L" · "+lyricStatus;
            lyricCount=data.lyrics?data.lyrics->lines.size():0;}
        ApplyVisualMediaDemand(p,a);
        const auto now=MonotonicMs();if(uiNotice!=lastUiNotice){lastUiNotice=uiNotice;uiNoticeUntil=uiNotice.empty()?0:now+8000;}if(uiNoticeUntil&&now>=uiNoticeUntil){uiNotice.clear();lastUiNotice.clear();uiNoticeUntil=0;}
        log.Sample(p,a);std::wstring diagnostic=std::wstring(MusicProviderName(p.provider))+L" "+DisplayText(p.clientVersion)+L" · "+PlayStateName(p.state)+L" · "+ReliabilityName(p.reliability)+L"\r\n歌词 "+std::to_wstring(lyricCount)+L" 行 · "+DisplayText(lyricStatus)+L"\r\n音频 · "+DisplayText(a.diagnostic);if(!uiNotice.empty())diagnostic=L"提示 · "+DisplayText(uiNotice)+L"\r\n"+diagnostic;
        const auto logProblem=log.Problem();if(!logProblem.empty())diagnostic=L"日志 · "+DisplayText(logProblem)+L"\r\n"+diagnostic;
        UpdateStatusText(std::move(diagnostic));UpdateControls();
        if(!trayAdded&&now>=trayRetryAt){trayRetryAt=now+5000;AddTray();}if(desktopActive&&now>=desktopPollAt){desktopPollAt=now+1000;std::wstring info;if(!desktopHost.Poll(info))OnStageLost(info);else if(!info.empty())log.Event("desktop-recovered",Utf8(info));}
        if(desiredDesktop&&!desktopActive&&desktopRetryAt&&now>=desktopRetryAt){desktopRetryAt=0;if(!EnterDesktop(false)){if(++desktopRetries<3)desktopRetryAt=now+(1000LL<<desktopRetries);else FallbackToWindowAfterDesktopFailure(L"桌面宿主暂时不可用，已恢复窗口");}}
        // Six bounded snapshots per attachment. Counts mean DXGI submissions,
        // never that the shell has made the final composed desktop visible.
        if(desktopActive&&desktopRenderer&&desktopPresentationReports<6&&now>=desktopPresentationReportAt){
            desktopPresentationReportAt=now+1000;++desktopPresentationReports;
            log.Event("desktop-presentation",desktopRenderer->PresentationDiagnostic()+
                " drawable="+std::to_string(DesktopDrawable())+" suppressed="+std::to_string(HostSuppressed(true))+
                " recovery="+std::to_string(desktopRecoveryPending));
        }
        bool automatic=false;{std::lock_guard lock(data.mutex);const auto autoNow=MonotonicMs();automatic=!data.busy&&!data.pendingLyricCommit&&!data.manualQueryActive&&!data.manualLyrics&&
            HasLyricLookupIdentity(data.playback)&&data.autoSearchGeneration!=data.generation&&autoNow-data.songChangedAt>=700&&autoNow>=data.retryAfter;}if(automatic)Search();
    }
    const SongCandidate* SelectedCandidate() const {
        if(candidateList.updating||candidateList.generation==UINT64_MAX)return nullptr;
        const int index=candidateList.selected;
        if(index<0||static_cast<size_t>(index)>=candidateList.rows.size())return nullptr;
        return &candidateList.rows[static_cast<size_t>(index)];
    }
    // A native list selection change is folded into the UI-owned selection.
    void SyncCandidateSelectionFromList() {
        if(candidateList.updating||!list)return;
        const auto item=SendMessageW(list,LB_GETCURSEL,0,0);
        if(item==LB_ERR){candidateList.selected=-1;return;}
        const auto index=SendMessageW(list,LB_GETITEMDATA,static_cast<WPARAM>(item),0);
        candidateList.selected=index>=0&&static_cast<size_t>(index)<candidateList.rows.size()?static_cast<int>(index):-1;
    }
    // Stage UI selection of a row from the snapshot it was shown with.
    bool SelectCandidateIndex(int index,uint64_t generation) {
        if(candidateList.updating||generation!=candidateList.generation||index<0||static_cast<size_t>(index)>=candidateList.rows.size())return false;
        candidateList.selected=index;candidateList.selectionEstablished=true;
        if(list){
            const auto count=SendMessageW(list,LB_GETCOUNT,0,0);
            for(LRESULT item=0;item<count;++item)
                if(SendMessageW(list,LB_GETITEMDATA,static_cast<WPARAM>(item),0)==index){SendMessageW(list,LB_SETCURSEL,static_cast<WPARAM>(item),0);break;}
        }
        UpdateCandidateFetchEnabled();
        return true;
    }
    void ClearPreferredCandidate() {preferredCandidateId.clear();preferredCandidateGeneration=UINT64_MAX;}
    void UpdateCandidateFetchEnabled() {
        bool enabled=false;std::wstring details;CandidateFetchState(enabled,details);
        EnableWindow(fetch,enabled);
        if(details!=shownCandidateDetails&&SetWindowTextW(Control(ID_CANDIDATEDETAILS),details.c_str()))
            shownCandidateDetails=std::move(details);
    }
    void CandidateFetchState(bool& enabled,std::wstring& details) {
        const auto selected=SelectedCandidate();
        enabled=selected!=nullptr;bool currentContext=false;
        {std::lock_guard lock(data.mutex);const bool currentGeneration=!exiting&&candidateList.generation==data.generation;
            currentContext=currentGeneration&&candidateList.manual==data.candidatesManual;
            enabled=enabled&&currentGeneration&&
            (candidateList.manual||(!data.busy&&HasLyricLookupIdentity(data.playback)));}
        // Read the same UI-owned row as Fetch, never a newer worker array.
        // Stable text preserves native selection and internal scroll on Tick.
        details.clear();
        if(selected&&currentContext){
            const auto field=[](const std::wstring& value){
                auto text=DisplayText(value);if(value.size()>text.size())text+=L"…";
                return text.empty()?std::wstring(L"—"):text;
            };
            details=L"歌名："+field(selected->title)+L"\r\n歌手："+field(selected->artist)+
                L"\r\n专辑："+field(selected->album)+L"\r\n时长："+
                (selected->durationMs>0?Clock(selected->durationMs):L"—")+L"\r\n歌曲 ID："+field(selected->id);
            if(selected->clientRecordCandidate)details+=L"\r\n最近播放记录 · 需确认";
        }
    }
    void RebuildCandidateList(std::vector<SongCandidate> candidates,size_t currentRevision,uint64_t generation,bool manual) {
        const bool sameContext=candidateList.generation==generation&&candidateList.manual==manual;
        bool preserveSelection=sameContext&&candidateList.selectionEstablished;
        std::wstring selectedId;
        if(preferredCandidateGeneration==generation&&!preferredCandidateId.empty()){
            selectedId=preferredCandidateId;preserveSelection=true;
        }else if(preserveSelection){if(const auto selected=SelectedCandidate())selectedId=selected->id;}
        candidateList.updating=true;
        struct RebuildScope {bool& updating;~RebuildScope(){updating=false;}} scope{candidateList.updating};
        candidateList.generation=UINT64_MAX;
        EnableWindow(fetch,FALSE);
        SendMessageW(list,LB_RESETCONTENT,0,0);
        candidateList.rows=std::move(candidates);candidateList.manual=manual;candidateList.selected=-1;
        candidateList.selectionEstablished=sameContext&&candidateList.selectionEstablished;
        int selection=preserveSelection?-1:0;
        for(size_t index=0;index<candidateList.rows.size();++index){
            const auto& c=candidateList.rows[index];
            auto label=DisplayText(c.title)+L" — "+DisplayText(c.artist)+L"\n"+(c.clientRecordCandidate?L"最近播放记录 · 需确认 · ":L"")+DisplayText(c.album)+(c.durationMs>0?L" · "+Clock(c.durationMs):L"")+L" · ID "+c.id;
            const auto item=SendMessageW(list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
            if(item==LB_ERR||item==LB_ERRSPACE){log.Event("candidate-list-allocation","Native list rejected an item");break;}
            if(SendMessageW(list,LB_SETITEMDATA,static_cast<WPARAM>(item),static_cast<LPARAM>(index))==LB_ERR){
                // A native row with default item data must never alias row zero.
                if(SendMessageW(list,LB_DELETESTRING,static_cast<WPARAM>(item),0)==LB_ERR)SendMessageW(list,LB_RESETCONTENT,0,0);
                log.Event("candidate-list-binding","Native list rejected item binding");break;
            }
            if(preserveSelection&&SameCatalogId(c.id,selectedId))selection=static_cast<int>(item);
        }
        if(SendMessageW(list,LB_GETCOUNT,0,0)>0){
            candidateList.selectionEstablished=true;
            SendMessageW(list,LB_SETCURSEL,static_cast<WPARAM>(selection),0);
            if(selection>=0){
                const auto index=SendMessageW(list,LB_GETITEMDATA,static_cast<WPARAM>(selection),0);
                if(index>=0&&static_cast<size_t>(index)<candidateList.rows.size())candidateList.selected=static_cast<int>(index);
            }
        }
        candidateList.generation=generation;candidateList.revision=currentRevision;
        ClearPreferredCandidate();
    }
    void UpdateControls() {
        if(candidateList.updating)return;
        RefreshTrackPreferences();
        UpdateSettingsNotice();
        UpdateTrackPreferenceNotice();
        UpdateThemeRestoreNotice();
        UpdateBackgroundNotice();
        std::wstring coverStatus;{std::lock_guard lock(data.mutex);coverStatus=data.coverStatus;}
        if(coverStatus!=shownCoverStatus){shownCoverStatus=coverStatus;SetWindowTextW(Control(ID_COVERSTATUS),(L"歌曲封面 · "+DisplayText(coverStatus.empty()?L"等待歌曲信息":coverStatus)).c_str());}
        UpdateLyricBrowsing();
        UpdateAssociationControl();
        bool canSearch,canFollow,changed,independent,manual=false,candidateMetadataChanged=false;uint64_t currentGeneration=0;size_t currentRevision=0;std::vector<SongCandidate> candidates;
        {std::lock_guard lock(data.mutex);independent=ManualPreviewLocked();
            canFollow=(!data.busy||independent)&&HasLyricLookupIdentity(data.playback);
            canSearch=(!data.busy||independent)&&HasLyricLookupIdentity(data.playback)&&HasSearchableSongText(data.playback);
            candidateMetadataChanged=RefreshNativeCandidateDisplayLocked();
            currentRevision=revision.load();changed=candidateList.revision!=currentRevision||candidateList.generation!=data.generation||
                candidateList.manual!=data.candidatesManual||preferredCandidateGeneration==data.generation;
            if(changed){candidates=data.candidates;currentGeneration=data.generation;manual=data.candidatesManual;}}
        if(candidateMetadataChanged)log.Event("candidate-display-refresh","Current native candidate metadata refreshed");
        EnableWindow(search,canSearch);EnableWindow(Control(ID_ONLINELYRICS),canFollow||independent);
        if(manualFollowShown!=independent){manualFollowShown=independent;SetWindowTextW(Control(ID_ONLINELYRICS),independent?L"跟随当前歌曲":L"恢复在线歌词");}
        {std::lock_guard lock(data.mutex);
            EnableWindow(Control(ID_IMPORT),!data.busy&&!data.manualQueryActive&&HasLyricLookupIdentity(data.playback));
            const bool hasTrack=!CurrentPreferenceKeyLocked().empty();EnableWindow(offsetEdit,hasTrack);EnableWindow(Control(ID_OFFSETRESET),hasTrack);}
        std::shared_ptr<const LyricTimeline> previewTimeline;size_t currentLyricReadingRevision=0;bool browsingThisTimeline=false;
        {std::lock_guard lock(data.mutex);previewTimeline=data.lyrics;currentLyricReadingRevision=lyricReadingRevision.load();
            browsingThisTimeline=manualLyricBrowse&&lyricBrowseGeneration==data.generation&&lyricBrowseReadingRevision==currentLyricReadingRevision;}
        if(shownLyricReadingRevision!=currentLyricReadingRevision){
            shownLyricReadingRevision=currentLyricReadingRevision;
            if(!browsingThisTimeline){
                previewIndex=InitialLyricLine(*previewTimeline);
                previewScrollOffset=0;
            }
        }
        if(changed)RebuildCandidateList(std::move(candidates),currentRevision,currentGeneration,manual);
        UpdateCandidateFetchEnabled();
    }
    LyricCommitDecision LyricCommitDecisionLocked(const PendingLyricCommit& pending,int64_t now) const {
        if(pending.owner.generation!=data.generation)return {LyricCommitDisposition::Drop,LyricCommitReason::GenerationChanged};
        if(exiting)return {LyricCommitDisposition::Drop,LyricCommitReason::Exiting};
        if(pending.result.state==LyricLoadState::Cancelled)return {LyricCommitDisposition::Drop,LyricCommitReason::Cancelled};
        if(pending.origin==LyricOrigin::ManualInput)return {LyricCommitDisposition::Commit,LyricCommitReason::IndependentPreview};
        const auto& playback=data.playback;
        if(pending.owner.pid!=playback.pid||pending.owner.processCreatedAtTicks!=playback.processCreatedAtTicks)
            return {LyricCommitDisposition::Drop,LyricCommitReason::ProcessChanged};
        const auto barrierId=!pending.owner.playbackSongId.empty()?pending.owner.playbackSongId:
            playback.songIdVerified&&SameCatalogId(playback.songId,pending.lyricId)?playback.songId:std::wstring{};
        if(!barrierId.empty()){
            if(playback.identityReacquiring)return {LyricCommitDisposition::Wait,LyricCommitReason::IdentityReacquiring};
            if(playback.metadataStale)return {LyricCommitDisposition::Drop,LyricCommitReason::MetadataUnavailable};
            if(!playback.songIdVerified)return {LyricCommitDisposition::Drop,LyricCommitReason::IdentityUnverified};
            if(!SameCatalogId(playback.songId,barrierId))return {LyricCommitDisposition::Drop,LyricCommitReason::SongChanged};
        }else{
            if(playback.identityReacquiring)return {LyricCommitDisposition::Drop,LyricCommitReason::IdentityReacquiring};
            if(playback.metadataStale)return {LyricCommitDisposition::Drop,LyricCommitReason::MetadataUnavailable};
        }
        // Completion is a freshness barrier even for unverified caption/SMTC
        // metadata. That branch may describe a candidate, never a verified ID;
        // old text alone cannot authorize a late lyric result for current use.
        if(playback.observedAtMs<=0)return {LyricCommitDisposition::Wait,LyricCommitReason::MissingObservation};
        if(playback.observedAtMs>now)return {LyricCommitDisposition::Wait,LyricCommitReason::FutureObservation};
        if(playback.observedAtMs<=pending.completedAtMs)return {LyricCommitDisposition::Wait,LyricCommitReason::AwaitingNewObservation};
        if(now-playback.observedAtMs>LyricObservationBudgetMs(playback))return {LyricCommitDisposition::Wait,LyricCommitReason::ObservationExpired};
        return {LyricCommitDisposition::Commit,barrierId.empty()?LyricCommitReason::UnverifiedMetadata:LyricCommitReason::PostCompletionIdentity};
    }
    // data.mutex is held. Capture fixed fields only; serialization, allocation
    // and log-writer locking must not delay native callbacks under App locks.
    void TraceLyricCommitLocked(const PendingLyricCommit& pending,LyricCommitDecision decision,
        LyricCommitTraceStage stage,int64_t now,const PlaybackSnapshot* incoming=nullptr) noexcept {
        if(lyricCommitTraceCount==lyricCommitTraces.size()){
            if(lyricCommitTraceDropped<UINT64_MAX)++lyricCommitTraceDropped;
            return;
        }
        const auto& playback=incoming?*incoming:data.playback;
        auto& trace=lyricCommitTraces[lyricCommitTraceCount++];
        trace.decision=decision;trace.stage=stage;trace.recordedAtMs=now;
        trace.requestGeneration=pending.owner.generation;trace.currentGeneration=data.generation;
        trace.requestPid=pending.owner.pid;trace.requestCreatedAt=pending.owner.processCreatedAtTicks;
        trace.currentPid=playback.pid;trace.currentCreatedAt=playback.processCreatedAtTicks;
        trace.requestPlaybackId=LyricTraceSongId(pending.owner.playbackSongId);trace.lyricId=LyricTraceSongId(pending.lyricId);
        trace.currentSongId=LyricTraceSongId(playback.songId);trace.resultSongId=LyricTraceSongId(pending.result.songId);
        trace.completedAtMs=pending.completedAtMs;trace.observedAtMs=playback.observedAtMs;
        trace.origin=pending.origin;trace.resultState=pending.result.state;trace.lineCount=pending.result.lines.size();
        trace.verified=playback.songIdVerified;trace.stale=playback.metadataStale;trace.reacquiring=playback.identityReacquiring;
        trace.fromCache=pending.result.fromCache;trace.wordTimed=pending.result.wordTimed;
    }
    void FlushLyricCommitTraces(bool drainAll=false) noexcept {
        // UI thread only: at most eight events per Tick, sixteen at shutdown.
        // Keep the shutdown bound even if an earlier worker-stop step failed.
        size_t remaining=0,budget=drainAll?lyricCommitTraces.size():8;
        do{
            std::array<LyricCommitTrace,8> batch{};size_t count=0;uint64_t dropped=0;bool reportLoss=false;
            try{
                std::lock_guard lock(data.mutex);
                count=std::min({batch.size(),lyricCommitTraceCount,budget});budget-=count;
                for(size_t i=0;i<count;++i)batch[i]=lyricCommitTraces[i];
                for(size_t i=count;i<lyricCommitTraceCount;++i)lyricCommitTraces[i-count]=lyricCommitTraces[i];
                lyricCommitTraceCount-=count;remaining=lyricCommitTraceCount;
                dropped=lyricCommitTraceDropped;reportLoss=dropped!=lyricCommitTraceReportedDropped;
                lyricCommitTraceReportedDropped=dropped;
            }catch(...){log.writer.SerializationFailed(DiagnosticLogKind::Event);return;}
            if(reportLoss){try{
                log.Write({{"event","lyric-commit-trace-loss"},{"lyricGateSchema",1},{"appVersion",MUSIC_STAGE_VERSION},{"utc",UtcNow()},
                    {"elapsedMs",MonotonicMs()-log.started},{"droppedTraceEvents",dropped}},DiagnosticLogKind::Event);
            }catch(...){log.writer.SerializationFailed(DiagnosticLogKind::Event);}}
            for(size_t i=0;i<count;++i){try{
                const auto& trace=batch[i];const auto now=trace.recordedAtMs;
                const auto disposition=trace.decision.disposition==LyricCommitDisposition::Commit?"eligible":
                    trace.decision.disposition==LyricCommitDisposition::Wait?"wait":"drop";
                const auto stage=trace.stage==LyricCommitTraceStage::ResultArrival?"result-arrival":
                    trace.stage==LyricCommitTraceStage::PendingPoll?"pending-poll":"pending-retired";
                const auto origin=trace.origin==LyricOrigin::VerifiedClient?"native-id":
                    trace.origin==LyricOrigin::ConfirmedCandidate?"confirmed-candidate":
                    trace.origin==LyricOrigin::AutomaticCandidate?"automatic-candidate":
                    trace.origin==LyricOrigin::LocalImport?"local-import":"manual-input";
                log.Write({{"event","lyric-commit-gate"},{"lyricGateSchema",1},{"appVersion",MUSIC_STAGE_VERSION},
                    {"utc",UtcNow()},{"elapsedMs",now-log.started},{"monotonicMs",now},{"stage",stage},
                    {"serializedAtMonotonicMs",MonotonicMs()},{"utcIsSerializationTime",true},
                    {"decision",disposition},{"reason",LyricCommitReasonName(trace.decision.reason)},
                    {"requestGeneration",std::to_string(trace.requestGeneration)},{"currentGeneration",std::to_string(trace.currentGeneration)},
                    {"requestPid",trace.requestPid},{"requestProcessCreatedAtTicks",std::to_string(trace.requestCreatedAt)},
                    {"currentPid",trace.currentPid},{"currentProcessCreatedAtTicks",std::to_string(trace.currentCreatedAt)},
                    {"requestPlaybackId",trace.requestPlaybackId.data()},{"lyricId",trace.lyricId.data()},
                    {"currentSongId",trace.currentSongId.data()},{"resultSongId",trace.resultSongId.data()},{"songIdVerified",trace.verified},
                    {"metadataStale",trace.stale},{"identityReacquiring",trace.reacquiring},
                    {"completedAtMs",trace.completedAtMs},{"observedAtMs",trace.observedAtMs},
                    {"observationAgeMs",trace.observedAtMs>0&&trace.observedAtMs<=now?now-trace.observedAtMs:-1},
                    {"origin",origin},{"resultState",static_cast<int>(trace.resultState)},
                    {"fromCache",trace.fromCache},{"wordTimed",trace.wordTimed},{"lineCount",trace.lineCount},
                    {"waitUpdatesCoalesced",true},{"displayConfirmed",nullptr}},DiagnosticLogKind::Event);
            }catch(...){log.writer.SerializationFailed(DiagnosticLogKind::Event);}}
        }while(drainAll&&remaining&&budget);
    }
    void DiscardPendingLyricLocked(LyricCommitReason reason,const PlaybackSnapshot* incoming=nullptr) noexcept {
        if(!data.pendingLyricCommit)return;
        TraceLyricCommitLocked(*data.pendingLyricCommit,{LyricCommitDisposition::Drop,reason},
            LyricCommitTraceStage::PendingRetired,MonotonicMs(),incoming);
        data.pendingLyricCommit.reset();
    }
    static int PendingLyricRank(const PendingLyricCommit& pending) noexcept {
        if(pending.result.state==LyricLoadState::Ready&&!pending.result.lines.empty())
            return 30+(pending.result.fromCache?0:4)+(pending.result.wordTimed?2:0);
        if(pending.result.state==LyricLoadState::NoLyrics)return 20;
        if(pending.result.state==LyricLoadState::Failed)return 10;
        return 0;
    }
    static bool SamePendingLyricRequest(const PendingLyricCommit& left,const PendingLyricCommit& right) noexcept {
        const auto sameOptionalId=[](const std::wstring& a,const std::wstring& b){return (a.empty()&&b.empty())||SameCatalogId(a,b);};
        return left.owner.generation==right.owner.generation&&left.owner.pid==right.owner.pid&&
            left.owner.processCreatedAtTicks==right.owner.processCreatedAtTicks&&
            sameOptionalId(left.owner.playbackSongId,right.owner.playbackSongId)&&
            sameOptionalId(left.lyricId,right.lyricId)&&left.origin==right.origin;
    }
    void StorePendingLyricCommitLocked(PendingLyricCommit&& pending) {
        if(!data.pendingLyricCommit||!SamePendingLyricRequest(*data.pendingLyricCommit,pending)){
            DiscardPendingLyricLocked(LyricCommitReason::ReplacedResult);
            data.pendingLyricCommit=std::move(pending);return;
        }
        if(PendingLyricRank(pending)>=PendingLyricRank(*data.pendingLyricCommit)){
            data.pendingLyricCommit=std::move(pending);return;
        }
        // An unusable online refresh must not evict a ready cache result that has
        // not yet crossed the identity barrier. The refresh is no longer in
        // flight; retain an explicit retry side effect for the later commit.
        if(data.pendingLyricCommit->result.state==LyricLoadState::Ready&&data.pendingLyricCommit->result.fromCache){
            data.pendingLyricCommit->refreshInFlight=false;
            data.pendingLyricCommit->refreshRetryRequired=true;
        }
    }
    bool CommitLyricResultLocked(PendingLyricCommit&& pending,std::array<LyricCacheSnapshot,2>& retired) {
        auto result=std::move(pending.result);
        const auto gen=pending.owner.generation;
        const auto id=std::move(pending.lyricId);
        auto origin=pending.origin;
        const auto selected=pending.selected?&*pending.selected:nullptr;
        auto importPathToRemember=std::move(pending.importPathToRemember);
        const bool refreshInFlight=pending.refreshInFlight;
        const bool refreshRetryRequired=pending.refreshRetryRequired;
        const bool fallbackOnlineAfterImportFailure=pending.fallbackOnlineAfterImportFailure;
        if(gen!=data.generation||exiting||result.state==LyricLoadState::Cancelled)return false;
        data.lyricRefreshPending=refreshInFlight&&result.fromCache&&result.state==LyricLoadState::Ready;
        if(origin==LyricOrigin::LocalImport&&result.state==LyricLoadState::NoLyrics){
            result.state=LyricLoadState::Failed;result.diagnostic=L"文件中没有可显示的有效歌词";
        }
        if(origin==LyricOrigin::AutomaticCandidate&&LyricVersionConflict(data.playback,id)){
            // The actual ID can arrive while an unverified search is in
            // flight. Do not publish that older guess after enrichment.
            data.retrySongId=data.playback.songIdVerified?data.playback.songId:std::wstring{};
            data.retryOrigin=data.playback.songIdVerified?LyricOrigin::VerifiedClient:LyricOrigin::AutomaticCandidate;
            data.autoSearchGeneration=UINT64_MAX;data.retryAfter=0;data.manualLyrics=false;
            data.networkStatus=data.playback.songIdVerified?L"播放器已确认歌曲版本，正在切换到对应歌词…":
                L"最近记录提示其他版本，正在重新核对歌词候选…";
            return false;
        }
        if(origin==LyricOrigin::AutomaticCandidate&&VerifiedLyricIdentityCurrent(data.playback,id))origin=LyricOrigin::VerifiedClient;
        const bool manualRequest=origin==LyricOrigin::ManualInput;
        const bool accepted=(result.state==LyricLoadState::Ready&&!result.lines.empty())||result.state==LyricLoadState::NoLyrics;
        if(manualRequest&&accepted&&VerifiedLyricIdentityCurrent(data.playback,id)){
            origin=LyricOrigin::ConfirmedCandidate;data.manualQueryActive=false;
            data.verifiedTrackId=data.playback.songId;data.manualTitle.clear();data.manualArtist.clear();
        }
        const bool manualInput=origin==LyricOrigin::ManualInput; // Final association decides automatic recovery.
        const auto label=origin==LyricOrigin::VerifiedClient?L"播放器当前歌曲核验":
            origin==LyricOrigin::ConfirmedCandidate?L"你确认的候选版本":
            origin==LyricOrigin::LocalImport?L"本地导入":
            origin==LyricOrigin::ManualInput?L"手动歌词预览（未核验当前播放）":L"自动搜索候选（未证实播放 ID）";
        const bool ready=result.state==LyricLoadState::Ready&&!result.lines.empty();
        const bool saveCache=ready&&!result.fromCache&&result.cacheWriteSuggested&&origin!=LyricOrigin::LocalImport;
        result.cacheWriteSuggested=false;
        const bool requestedWordRetry=ready&&!result.wordTimed&&result.retryWordTiming;
        const bool wordRetry=requestedWordRetry&&!manualInput&&origin!=LyricOrigin::LocalImport;
        // Keep request advice dormant in independent manual previews. Existing
        // renderer guards suppress promises there; a later same-ID verification
        // can resume recovery without cloning this immutable timeline.
        result.retryWordTiming=requestedWordRetry&&origin!=LyricOrigin::LocalImport;
        data.networkStatus=std::wstring(label)+L" · "+result.diagnostic;
        if(ready||result.state==LyricLoadState::NoLyrics){
            const bool cached=ready&&result.fromCache;
            const bool needsRefresh=cached||wordRetry;
            if(manualRequest&&!SameSongId(data.coverSongId,id)){
                data.coverFile.clear();data.coverSongId.clear();data.coverStatus.clear();data.coverError.clear();if(backgroundSelection==0)background.Clear();
                data.coverRevalidatePending=false;
            }
            data.matchedId=id;data.retrySongId.clear();data.retryAfter=0;
            data.loadedOrigin=origin;
            if(origin==LyricOrigin::LocalImport){
                auto playbackId=pending.owner.playbackSongId;
                if(!SameCatalogId(playbackId,playbackId)&&data.playback.songIdVerified&&
                        data.playback.pid==pending.owner.pid&&
                        data.playback.processCreatedAtTicks==pending.owner.processCreatedAtTicks)
                    playbackId=data.playback.songId;
                data.localImportBinding=LocalImportBinding{pending.owner.pid,
                    pending.owner.processCreatedAtTicks,std::move(playbackId),false};
            }else data.localImportBinding.reset();
            if(origin==LyricOrigin::ManualInput){
                data.manualLoadedGeneration=gen;
                data.manualTitle=selected&&!selected->title.empty()?selected->title:L"歌曲 ID "+id;
                data.manualArtist=selected?selected->artist:std::wstring{};
            }else{data.manualTitle.clear();data.manualArtist.clear();}
            if(!needsRefresh)data.retryFailures=0;
            data.manualLyrics=origin==LyricOrigin::LocalImport||origin==LyricOrigin::ConfirmedCandidate||origin==LyricOrigin::ManualInput;
            data.autoSearchGeneration=gen;
            CancelObsoleteCoverRequestsLocked();
            if(ready)data.networkStatus+=L" · "+std::to_wstring(result.lines.size())+L" 行";
            const auto contentChange=CompareLyricContent(*data.lyrics,result);
            const bool stateChanged=data.lyrics->state!=result.state;
            data.lyrics=std::make_shared<LyricTimeline>(std::move(result));
            if(contentChange!=LyricContentChange::None||stateChanged)
                MarkLyricContentChanged(stateChanged||contentChange==LyricContentChange::Original);
            data.lyricCacheState=LyricCacheState::None;
            if(saveCache)QueueLyricCacheLocked(data.lyrics,retired);
            if(ready&&origin==LyricOrigin::LocalImport&&importPathToRemember)
                RememberImportLocked(*importPathToRemember,gen);
            if(needsRefresh&&!manualInput&&origin!=LyricOrigin::LocalImport&&(!refreshInFlight||refreshRetryRequired)){
                data.retrySongId=id;data.retryOrigin=origin;
                data.retryFailures=std::max(data.retryFailures,3);ScheduleRetryLocked();
            }else if(cached&&manualInput)data.networkStatus+=L" · 已使用本机缓存，可重新查找更新";
            else if(requestedWordRetry&&manualInput)data.networkStatus+=L" · 可重新查找更新逐字歌词";
        }else{
            if(data.lyrics->lines.empty()){data.lyrics=std::make_shared<LyricTimeline>(std::move(result));data.lyricCacheState=LyricCacheState::None;MarkLyricContentChanged();}
            else data.networkStatus+=L" · 保留当前已加载歌词";
            if(origin!=LyricOrigin::LocalImport&&!manualInput){
                data.retrySongId=id;data.retryOrigin=origin;
                if(HasRetainedRefreshSourceLocked(id))
                    data.retryFailures=std::max(data.retryFailures,3);
                ScheduleRetryLocked();
            }
        }
        if(!ready&&origin==LyricOrigin::LocalImport&&fallbackOnlineAfterImportFailure){
            data.preferenceNotice=L"记住的本地歌词暂时无法读取，导入记录已保留，继续查找在线歌词";
            data.autoSearchGeneration=UINT64_MAX;data.retryAfter=0;
        }
        return ready;
    }
    LyricApplyOutcome ApplyLyricResult(LyricTimeline result,LyricRequestOwner owner,const std::wstring& id,LyricOrigin origin,
                                      const SongCandidate* selected=nullptr,bool refreshInFlight=false,
                                      const std::filesystem::path* importPathToRemember=nullptr,
                                      bool fallbackOnlineAfterImportFailure=false) {
        PendingLyricCommit pending{std::move(result),std::move(owner),id,origin,
            selected?std::optional<SongCandidate>(*selected):std::nullopt,
            importPathToRemember?std::optional<std::filesystem::path>(*importPathToRemember):std::nullopt,
            refreshInFlight,false,fallbackOnlineAfterImportFailure,MonotonicMs(),std::nullopt,0};
        std::array<LyricCacheSnapshot,2> retired;
        std::scoped_lock lock(data.mutex,jobMutex);
        const auto now=MonotonicMs();const auto decision=LyricCommitDecisionLocked(pending,now);
        TraceLyricCommitLocked(pending,decision,LyricCommitTraceStage::ResultArrival,now);
        if(decision.disposition==LyricCommitDisposition::Wait){pending.lastWaitReason=decision.reason;pending.lastWaitLoggedAtMs=now;}
        switch(decision.disposition){
            case LyricCommitDisposition::Commit:{
                // A cache result may already be waiting when the following
                // online result crosses the barrier inside this call. Merge
                // them before committing so the older pending cache cannot
                // overwrite a newer online result on the next Tick.
                if(data.pendingLyricCommit){
                    if(SamePendingLyricRequest(*data.pendingLyricCommit,pending)){
                        StorePendingLyricCommitLocked(std::move(pending));
                        pending=std::move(*data.pendingLyricCommit);
                        data.pendingLyricCommit.reset();
                    }else DiscardPendingLyricLocked(LyricCommitReason::ReplacedResult);
                }
                return CommitLyricResultLocked(std::move(pending),retired)?LyricApplyOutcome::Ready:LyricApplyOutcome::Rejected;
            }
            case LyricCommitDisposition::Wait:
                StorePendingLyricCommitLocked(std::move(pending));
                data.networkStatus=data.playback.identityReacquiring?
                    L"歌词请求已完成，等待当前歌曲重新确认":L"歌词请求已完成，等待当前歌曲的新信息";
                return LyricApplyOutcome::Deferred;
            case LyricCommitDisposition::Drop:
            default:return LyricApplyOutcome::Rejected;
        }
    }
    void TryCommitPendingLyric() {
        std::array<LyricCacheSnapshot,2> retired;
        std::optional<PendingLyricCommit> released;
        std::scoped_lock lock(data.mutex,jobMutex);
        if(!data.pendingLyricCommit)return;
        const auto now=MonotonicMs();const auto decision=LyricCommitDecisionLocked(*data.pendingLyricCommit,now);
        if(decision.disposition==LyricCommitDisposition::Wait){
            auto& pending=*data.pendingLyricCommit;
            if((!pending.lastWaitReason||*pending.lastWaitReason!=decision.reason)&&
                (!pending.lastWaitLoggedAtMs||(now>=pending.lastWaitLoggedAtMs&&now-pending.lastWaitLoggedAtMs>=1000))){
                TraceLyricCommitLocked(pending,decision,LyricCommitTraceStage::PendingPoll,now);
                pending.lastWaitReason=decision.reason;pending.lastWaitLoggedAtMs=now;
            }
            return;
        }
        TraceLyricCommitLocked(*data.pendingLyricCommit,decision,LyricCommitTraceStage::PendingPoll,now);
        released=std::move(data.pendingLyricCommit);data.pendingLyricCommit.reset();
        if(decision.disposition==LyricCommitDisposition::Commit)
            CommitLyricResultLocked(std::move(*released),retired);
    }
    struct ManualContext {uint64_t generation=0;std::stop_token token;PlaybackSnapshot playback;};
    std::optional<ManualContext> PrepareManualContext(bool keepCandidates=false,std::optional<uint64_t> expectedGeneration=std::nullopt) {
        std::scoped_lock lock(data.mutex,jobMutex);if(stopping||exiting||(expectedGeneration&&*expectedGeneration!=data.generation))return std::nullopt;
        if(!ManualPreviewLocked()){
            data.manualTitle=data.lyrics->lines.empty()?L"手动查找歌词":!HasSearchableSongText(data.playback)?L"此前加载的歌词":data.playback.title;
            data.manualArtist=data.lyrics->lines.empty()?std::wstring{}:data.playback.artist;
        }
        // Explicit user input supersedes a pending or running request. Its
        // generation owns busy; an old worker cannot release a newer request.
        data.generationStop.request_stop();data.generationStop=std::stop_source{};++data.generation;CancelCoverGenerationLocked();job.reset();
        DiscardPendingLyricLocked(LyricCommitReason::ManualSelection);
        ClearLyricBusyLocked();
        data.manualQueryActive=true;data.candidatesManual=true;
        if(!keepCandidates)data.candidates.clear();
        revision++;
        data.retrySongId.clear();data.retryAfter=0;data.retryFailures=0;data.suspendedNetworkStatus.clear();
        data.suspendedLyricRefresh.reset();
        data.autoSearchGeneration=data.generation;
        return ManualContext{data.generation,data.generationStop.get_token(),data.playback};
    }
    void QueueManualCandidate(SongCandidate candidate,const ManualContext& context,bool replaceList) {
        const auto gen=context.generation;const auto token=context.token;
        const auto owner=CaptureLyricRequestOwner(context.playback,gen);
        {std::lock_guard lock(data.mutex);if(gen!=data.generation||token.stop_requested())return;
            if(replaceList){data.candidates={candidate};revision++;}else EnsureCandidateVisibleLocked(candidate);}
        preferredCandidateId=candidate.id;preferredCandidateGeneration=gen;
        Queue(gen,false,L"正在获取指定歌词…",[this,candidate=std::move(candidate),gen,token,owner]{
            if(exiting||token.stop_requested())return;
            auto client=MakeLyricsClient(token);auto result=client.Fetch(candidate.id);const auto count=result.lines.size();
            const bool accepted=result.state==LyricLoadState::NoLyrics||(result.state==LyricLoadState::Ready&&!result.lines.empty());
            ApplyLyricResult(std::move(result),owner,candidate.id,LyricOrigin::ManualInput,&candidate);
            if(const auto target=window.load())PostMessageW(target,WM_DATA,0,0);
            log.Event("manual-lyric-fetch",Utf8(candidate.id)+" lines="+std::to_string(count));
            if(accepted)QueueCover(candidate.id,gen,CoverIdOrigin::SelectedCandidate,token);
        },true);
        UpdateControls();
    }
    void ManualFind() {
        const int length=GetWindowTextLengthW(manualQueryEdit);
        if(length>512){uiNotice=L"输入最多512个字符，请缩短歌名或歌曲链接";return;}
        std::wstring input(static_cast<size_t>(std::max(0,length))+1,L'\0');
        const auto copied=GetWindowTextW(manualQueryEdit,input.data(),static_cast<int>(input.size()));input.resize(static_cast<size_t>(std::max(0,copied)));
        ManualFindText(std::move(input));
    }
    void ManualFindText(std::wstring input) {
        if(input.size()>512){uiNotice=L"输入最多512个字符，请缩短歌名或歌曲链接";return;}
        const auto request=ParseManualLyricRequest(input);
        if(request.kind==ManualLyricKind::Invalid){uiNotice=request.diagnostic;return;}
        const auto context=PrepareManualContext();if(!context)return;
        ClearPreferredCandidate();
        if(request.kind==ManualLyricKind::SongId){
            SongCandidate candidate;candidate.id=request.songId;candidate.title=L"歌曲 ID "+candidate.id;
            const auto& p=context->playback;
            if(p.songIdVerified&&!p.metadataStale&&SameCatalogId(candidate.id,p.songId)){
                if(HasSearchableSongText(p)){candidate.title=p.title;candidate.artist=p.artist;candidate.album=p.album;}
                candidate.durationMs=p.durationMs;
            }
            QueueManualCandidate(std::move(candidate),*context,true);return;
        }
        const auto gen=context->generation;const auto token=context->token;
        Queue(gen,false,L"正在查找歌词版本…",[this,query=request.query,provider=context->playback.provider,gen,token]{
            if(exiting||token.stop_requested())return;
            auto client=MakeLyricsClient(token);std::wstring error;auto candidates=client.Search(query,error,provider);
            if(token.stop_requested())return;
            {std::lock_guard lock(data.mutex);if(gen!=data.generation)return;
                data.candidates=std::move(candidates);data.candidatesManual=true;revision++;
                const bool failed=!error.empty()&&error!=L"没有搜索候选";
                data.networkStatus=failed?L"歌词查找 · "+DisplayText(error)+L" · 可重新查找":data.candidates.empty()?L"没有找到候选，可换一个歌名或歌曲 ID":L"请核对歌名、歌手和专辑，再采用所选歌词";
                if(data.lyrics->lines.empty()){
                    auto empty=std::make_shared<LyricTimeline>();empty->state=failed?LyricLoadState::Failed:data.candidates.empty()?LyricLoadState::NoLyrics:LyricLoadState::AwaitingChoice;
                    data.lyrics=std::move(empty);data.lyricCacheState=LyricCacheState::None;MarkLyricContentChanged();
                }
            }
            // The input and any pasted URL are never written to history/logs.
            log.Event("manual-lyric-search",error.empty()?"Candidates returned; explicit choice required":"Search returned no usable candidates");
        },true);
        UpdateControls();
    }
    void FollowCurrentLyrics(bool forceCandidates=false) {
        {
            std::scoped_lock lock(data.mutex,jobMutex);if(stopping||exiting)return;
            if(ManualPreviewLocked()){
                data.generationStop.request_stop();data.generationStop=std::stop_source{};++data.generation;CancelCoverGenerationLocked();job.reset();
                DiscardPendingLyricLocked(LyricCommitReason::FollowCurrent);
                ClearLyricBusyLocked();data.manualQueryActive=false;
                // Retain the previous text as an explicitly unbound preview
                // until the current song's request has produced a new result.
                data.loadedOrigin=LyricOrigin::ManualInput;data.localImportBinding.reset();data.manualLyrics=false;
                data.candidates.clear();data.candidatesManual=false;revision++;
                data.verifiedTrackId=data.playback.songIdVerified&&!data.playback.metadataStale&&!data.playback.identityReacquiring?data.playback.songId:std::wstring{};
                data.retrySongId.clear();data.retryAfter=0;data.retryFailures=0;data.autoSearchGeneration=UINT64_MAX;
                data.songChangedAt=MonotonicMs()-700;data.suspendedNetworkStatus.clear();data.suspendedLyricRefresh.reset();
                data.networkStatus=data.playback.songIdVerified&&!data.playback.identityReacquiring?
                    L"正在按当前歌曲 ID 加载歌词，歌曲资料可稍后补全":
                    data.playback.title.empty()||data.playback.metadataStale?L"等待当前歌曲信息 · 此前文字保持静态预览":L"正在跟随当前歌曲，歌词就绪前保留此前文字";
            }
        }
        ClearPreferredCandidate();Search(forceCandidates);UpdateControls();
    }
    void Search(bool forceCandidates=false) {
        PlaybackSnapshot track;uint64_t gen;std::stop_token token;LyricRequestOwner owner;
        std::wstring pendingId;LyricOrigin pendingOrigin=LyricOrigin::AutomaticCandidate;
        {
            std::lock_guard lock(data.mutex);if(data.busy||data.manualQueryActive||!HasLyricLookupIdentity(data.playback)||
                (forceCandidates&&!HasSearchableSongText(data.playback)))return;
            track=data.playback;gen=data.generation;token=data.generationStop.get_token();owner=CaptureLyricRequestOwner(track,gen);
            if(!forceCandidates){pendingId=data.retrySongId;pendingOrigin=data.retryOrigin;}
        }
        if(!LyricRetryTargetAllowed(track,pendingId,pendingOrigin)){
            pendingId.clear();pendingOrigin=LyricOrigin::AutomaticCandidate;
        }
        std::wstring savedId;
        if(HasSearchableSongText(track)){
            const auto saved=settings.confirmedCandidates.find(PlayerMatchKey(track));
            if(saved!=settings.confirmedCandidates.end()&&CatalogMatchesProvider(saved->second,track.provider))savedId=saved->second;
        }
        // A valid retry identifies the exact lyric version being refreshed.
        // In particular, a user-confirmed alternate version must not be
        // silently replaced by the playback ID merely because its cached or
        // line-timed result is due for an online refresh. Verified-client and
        // automatic retries were constrained to the current identity above.
        const std::wstring knownId=forceCandidates?std::wstring{}:!pendingId.empty()?pendingId:
            track.songIdVerified?track.songId:std::move(savedId);
        if(!knownId.empty()){
            const auto origin=track.songIdVerified&&SameCatalogId(track.songId,knownId)?LyricOrigin::VerifiedClient:
                !pendingId.empty()?pendingOrigin:LyricOrigin::ConfirmedCandidate;
            const bool catalogTextReady=HasSearchableSongText(track);
            const auto displayTitle=catalogTextReady?track.title:L"歌曲 ID "+knownId;
            Queue(gen,true,L"正在加载「"+displayTitle+L"」的歌词…",[this,knownId,track,gen,origin,token,owner]{
                if(exiting||token.stop_requested())return;
                bool publishCache=false;
                {std::lock_guard lock(data.mutex);if(gen!=data.generation)return;
                    publishCache=data.lyrics->state!=LyricLoadState::Ready||data.lyrics->lines.empty()||!SameCatalogId(data.matchedId,knownId);
                    data.lyricRefreshPending=!publishCache&&(data.lyrics->fromCache||data.lyrics->retryWordTiming);
                    const bool catalogTextReady=HasSearchableSongText(track);
                    data.candidates={{knownId,catalogTextReady?track.title:L"歌曲 ID "+knownId,
                        catalogTextReady?track.artist:std::wstring{},catalogTextReady?track.album:std::wstring{},track.durationMs}};
                    data.candidatesManual=false;revision++;}
                auto client=MakeLyricsClient(token);
                if(publishCache){
                    if(auto cached=client.Cached(knownId)){
                        const auto count=cached->lines.size();
                        if(ApplyLyricResult(std::move(*cached),owner,knownId,origin,nullptr,true)==LyricApplyOutcome::Ready){
                            if(const auto target=window.load())PostMessageW(target,WM_DATA,0,0);
                            log.Event("cached-lyric-ready",Utf8(knownId)+" lines="+std::to_string(count));
                        }
                    }
                }
                if(exiting||token.stop_requested())return;
                auto lyric=client.Fetch(knownId);const auto count=lyric.lines.size();
                ApplyLyricResult(std::move(lyric),owner,knownId,origin);
                if(const auto target=window.load())PostMessageW(target,WM_DATA,0,0);
                log.Event("record-lyric-fetch",Utf8(knownId)+" lines="+std::to_string(count));
                QueueCover(knownId,gen,origin==LyricOrigin::VerifiedClient?CoverIdOrigin::VerifiedPlaybackId:CoverIdOrigin::SelectedCandidate,token,origin==LyricOrigin::AutomaticCandidate);
            },false,origin==LyricOrigin::AutomaticCandidate,knownId,origin);return;
        }
        Queue(gen,true,L"正在查找「"+track.title+L"」的歌词…",[this,track,gen,token,owner]{
            if(exiting||token.stop_requested())return;
            auto client=MakeLyricsClient(token);std::wstring error;auto result=client.Search(track,error);
            if(token.stop_requested())return;
            bool recordOnlyAdded=false;
            if(SameSongId(track.recordCandidateId,track.recordCandidateId)&&
                std::none_of(result.begin(),result.end(),[&](const auto& item){return SameSongId(item.id,track.recordCandidateId);})){
                result.push_back({track.recordCandidateId,track.title,track.artist,track.album,track.durationMs,true});
                recordOnlyAdded=true;
            }
            auto match=error.empty()?MatchExactUniqueCandidate(track,result):std::optional<size_t>{};
            // The record can disprove uniqueness or offer a manual choice. It
            // cannot establish an automatic match without an online result.
            if(recordOnlyAdded&&match&&result[*match].clientRecordCandidate)match.reset();
            const std::wstring id=match?result[*match].id:std::wstring{};
            {
                std::lock_guard lock(data.mutex);if(gen!=data.generation)return;
                data.candidates=std::move(result);data.candidatesManual=false;revision++;
                if(match&&data.busy&&data.busyGeneration==gen)data.automaticLyricTargetId=id;
                data.networkStatus=!error.empty()?L"歌词搜索 · "+error+(recordOnlyAdded?L" · 最近记录仅供手动确认":L""):match?L"歌名与歌手唯一匹配，正在加载歌词…":
                    data.candidates.empty()?L"没有找到歌词候选，可导入本地歌词":L"歌词版本尚待确认，请核对候选后获取歌词";
                if(!match){
                    const bool emptyCatalog=(track.provider==MusicProvider::Kugou&&data.candidates.empty()&&error==L"没有搜索候选")||
                        (track.provider==MusicProvider::SodaMusic&&(error.empty()||error==L"没有搜索候选"));
                    const bool retry=emptyCatalog||(!error.empty()&&error!=L"没有搜索候选");
                    if(data.lyrics->lines.empty()){
                        auto empty=std::make_shared<LyricTimeline>();empty->state=retry?LyricLoadState::Failed:
                            data.candidates.empty()?LyricLoadState::NoLyrics:LyricLoadState::AwaitingChoice;
                        data.lyrics=std::move(empty);data.lyricCacheState=LyricCacheState::None;MarkLyricContentChanged();
                    }
                    if(retry){
                        // An empty player catalog response is not proof that the
                        // song has no lyrics. Recover without rapid polling.
                        if(emptyCatalog)data.retryFailures=std::max(data.retryFailures,3);
                        ScheduleRetryLocked();
                    }
                    else{data.retryAfter=0;data.retryFailures=0;data.retrySongId.clear();}
                }
            }
            if(const auto target=window.load())PostMessageW(target,WM_DATA,0,0);
            log.Event("search-completed",Utf8(error));
            if(match&&!exiting&&!token.stop_requested()){
                auto lyric=client.Fetch(id);const auto count=lyric.lines.size();
                ApplyLyricResult(std::move(lyric),owner,id,LyricOrigin::AutomaticCandidate);
                if(const auto target=window.load())PostMessageW(target,WM_DATA,0,0);
                log.Event("automatic-lyric-fetch",Utf8(id)+" lines="+std::to_string(count));
                QueueCover(id,gen,CoverIdOrigin::SelectedCandidate,token,true);
            }
        },false,!forceCandidates);
    }
    void EnsureCandidateVisibleLocked(const SongCandidate& candidate) {
        if(std::none_of(data.candidates.begin(),data.candidates.end(),[&](const auto& entry){return SameCatalogId(entry.id,candidate.id);})){
            data.candidates.insert(data.candidates.begin(),candidate);revision++;
        }
    }
    bool RememberConfirmedCandidate(const std::wstring& key,const std::wstring& id) {
        if(key.empty()||key.size()>MaxConfirmedCandidateKeyChars||key.find(L'\0')!=std::wstring::npos||Utf8(key).empty()||!SameCatalogId(id,id)){
            uiNotice=L"当前歌曲信息无法安全写入歌词版本记忆；本次选择仍继续使用";
            log.Event("confirmed-candidate-invalid","Mapping was not persisted because its key or song ID was invalid");
            return false;
        }
        auto found=settings.confirmedCandidates.find(key);
        if(found!=settings.confirmedCandidates.end()){
            if(SameCatalogId(found->second,id))return true;
            found->second=id;Persist();return true;
        }
        if(settings.confirmedCandidates.size()>=MaxConfirmedCandidates){
            uiNotice=L"已记住512首歌的歌词版本；本次选择继续使用，但不会随机覆盖既有记忆";
            log.Event("confirmed-candidate-limit","New mapping kept for this playback only; existing mappings preserved");
            return false;
        }
        settings.confirmedCandidates.emplace(key,id);Persist();return true;
    }
    void Fetch() {
        const auto selected=SelectedCandidate();if(!selected)return;
        SongCandidate candidate=*selected;
        const bool manual=candidateList.manual;const auto displayedGeneration=candidateList.generation;
        std::wstring id,key;uint64_t gen;std::stop_token token;LyricRequestOwner owner;
        {
            std::lock_guard lock(data.mutex);
            if(exiting||displayedGeneration!=data.generation||(!manual&&(data.busy||!HasLyricLookupIdentity(data.playback))))return;
            id=candidate.id;gen=data.generation;token=data.generationStop.get_token();
            key=HasSearchableSongText(data.playback)?PlayerMatchKey(data.playback):std::wstring{};
            owner=CaptureLyricRequestOwner(data.playback,gen);
        }
        if(manual){const auto context=PrepareManualContext(true,displayedGeneration);if(context)QueueManualCandidate(std::move(candidate),*context,false);return;}
        if(Queue(gen,true,L"正在获取所选版本的歌词…",[this,id,gen,token,owner]{
            if(exiting||token.stop_requested())return;
            auto client=MakeLyricsClient(token);auto result=client.Fetch(id);const auto count=result.lines.size();
            ApplyLyricResult(std::move(result),owner,id,LyricOrigin::ConfirmedCandidate);
            if(const auto target=window.load())PostMessageW(target,WM_DATA,0,0);
            log.Event("lyric-fetch",Utf8(id)+" lines="+std::to_string(count));
            QueueCover(id,gen,CoverIdOrigin::SelectedCandidate,token);
        },false,false,id,LyricOrigin::ConfirmedCandidate)){
            {std::lock_guard lock(data.mutex);if(gen==data.generation){
                EnsureCandidateVisibleLocked(candidate);
                const auto metadataKey=HasSearchableSongText(data.playback)?PlayerLyricKey(data.playback):std::wstring{};
                for(const auto& preferenceKey:{CurrentPreferenceKeyLocked(),metadataKey}){
                    if(preferenceKey.empty())continue;
                    auto found=data.trackPreferences.entries.find(preferenceKey);if(found==data.trackPreferences.entries.end())continue;
                    if(!found->second.importPath.empty()){found->second.importPath.clear();MarkTrackPreferencesLocked();}
                    if(!found->second.offsetMs)data.trackPreferences.entries.erase(found);
                }
                data.importAttemptGeneration=gen;data.importAttemptKey=CurrentPreferenceKeyLocked();
            }}
            if(!key.empty())RememberConfirmedCandidate(key,id);
            preferredCandidateId=id;preferredCandidateGeneration=gen;UpdateControls();
        }
    }
    void Import() {
        uint64_t gen;std::stop_token token;LyricRequestOwner owner;
        {std::lock_guard lock(data.mutex);if(data.busy||data.manualQueryActive||!HasLyricLookupIdentity(data.playback))return;
            gen=data.generation;token=data.generationStop.get_token();owner=CaptureLyricRequestOwner(data.playback,gen);}
        wchar_t file[MaxLocalFilePathChars+1]{};OPENFILENAMEW ofn{sizeof(ofn)};ofn.hwndOwner=window;
        ofn.lpstrFilter=L"歌词文件 (*.lrc;*.yrc)\0*.lrc;*.yrc\0所有文件\0*.*\0";
        ofn.lpstrFile=file;ofn.nMaxFile=static_cast<DWORD>(std::size(file));ofn.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
        ModalScope modal(*this);if(!GetOpenFileNameW(&ofn)||exiting)return;
        // The file dialog is modal: bind to the song that opened it. If that
        // song changed while selecting a file, Queue rejects the stale request.
        const auto path=std::filesystem::path(file);
        if(!Queue(gen,false,L"正在读取本地歌词…",[this,path,gen,token,owner]{
            auto client=MakeLyricsClient(token);auto result=client.Import(path);
            const auto outcome=ApplyLyricResult(std::move(result),owner,{},LyricOrigin::LocalImport,nullptr,false,&path);
            log.Event("lyric-import",outcome==LyricApplyOutcome::Ready?"Local lyric file committed; path omitted":
                outcome==LyricApplyOutcome::Deferred?"Local lyric file parsed; identity commit pending; path omitted":
                "Import rejected or cancelled; current lyrics preserved");
        }))uiNotice=L"歌曲或加载状态已改变，请重新选择歌词文件";
    }
    void UpdateLyricBrowsing() {
        bool canReturn=false;const auto now=MonotonicMs();
        {std::lock_guard lock(data.mutex);
            if(manualLyricBrowse&&(now>=lyricBrowseUntil||lyricBrowseGeneration!=data.generation||lyricBrowseReadingRevision!=lyricReadingRevision.load())){
                manualLyricBrowse=false;lyricBrowseUntil=0;stageWheelCarry=0;previewScrollOffset=0;
            }
            canReturn=manualLyricBrowse&&!ManualPreviewLocked()&&!LyricVersionMismatchLocked()&&CurrentTimelineSource(data.playback,now)!=TimelineSource::Unavailable;
        }
        lyricReturnShown=manualLyricBrowse&&!CompactStageControls();lyricReturnEnabled=canReturn;
        const auto button=Control(ID_RESUME_LYRICS);if(!button)return;
        // With the stage UI this is a pill on the stage; the button stays hidden.
        const bool needed=lyricReturnShown&&!stageUi;
        if(!needed&&GetFocus()==button)SetFocus(window);
        if(IsWindowEnabled(button)!=(canReturn?TRUE:FALSE))EnableWindow(button,canReturn);
        const bool shown=(GetWindowLongPtrW(button,GWL_STYLE)&WS_VISIBLE)!=0;
        if(shown!=needed)ShowWindow(button,needed?SW_SHOW:SW_HIDE);
    }
    void ResumeLyricFollowing() {
        {std::lock_guard lock(data.mutex);if(ManualPreviewLocked()){uiNotice=L"当前为手动歌词预览，可在歌词设置中选择“跟随当前歌曲”";return;}}
        {std::lock_guard lock(data.mutex);if(LyricVersionMismatchLocked()){uiNotice=L"歌词版本不同，请先核对版本";return;}}
        {std::lock_guard lock(data.mutex);const auto now=MonotonicMs();if(!data.lyrics->lines.empty()&&CurrentTimelineSource(data.playback,now)!=TimelineSource::Unavailable)
            previewIndex=LyricLineAt(data.lyrics->lines,WithLyricOffset(PlaybackPositionAt(data.playback,now),CurrentOffsetLocked()));}
        manualLyricBrowse=false;lyricBrowseUntil=0;stageWheelCarry=0;previewScrollOffset=0;NoteActivity();UpdateLyricBrowsing();
    }
    enum class LyricScrollStep { Small,Page };
    void ScrollLyrics(int steps,LyricScrollStep amount=LyricScrollStep::Small) {
        if(!steps)return;
        {std::lock_guard lock(data.mutex);const auto now=MonotonicMs();const auto& lines=data.lyrics->lines;if(lines.empty())return;
            const auto currentRevision=lyricRevision.load();
            const auto currentReadingRevision=lyricReadingRevision.load();
            const bool ownGeometry=presentedLyricValid&&presentedLyricRevision==currentRevision&&!presentedLyricDesktop&&
                presentedLyricRendererGeneration==renderer.FrameGeneration();
            if(!manualLyricBrowse||now>=lyricBrowseUntil||lyricBrowseGeneration!=data.generation||lyricBrowseReadingRevision!=currentReadingRevision){
                // Input begins from the sentence the user actually saw. The
                // App preview index can be old even though static presentation
                // correctly retained a much later successfully drawn sentence.
                previewScrollOffset=0;
                if(presentedLyricValid&&presentedLyricReadingRevision==currentReadingRevision){
                    previewIndex=std::min(presentedLyricIndex,lines.size()-1);
                    previewScrollOffset=ownGeometry?presentedLyricViewport.scrollOffset:0.f;
                }
                else if(!ManualPreviewLocked()&&!LyricVersionMismatchLocked()&&CurrentTimelineSource(data.playback,now)!=TimelineSource::Unavailable)
                    previewIndex=LyricLineAt(lines,WithLyricOffset(PlaybackPositionAt(data.playback,now),CurrentOffsetLocked()));
                else previewIndex=InitialLyricLine(*data.lyrics);
            }
            const int direction=steps>0?1:-1;
            int64_t remaining=steps>0?static_cast<int64_t>(steps):-static_cast<int64_t>(steps);
            const bool hasGeometry=ownGeometry&&presentedLyricIndex==previewIndex;
            if(hasGeometry&&presentedLyricViewport.maxScrollOffset>0&&std::isfinite(presentedLyricViewport.maxScrollOffset)){
                const float maximum=presentedLyricViewport.maxScrollOffset;
                const float smallStep=std::isfinite(presentedLyricViewport.scrollStep)?std::max(1.f,presentedLyricViewport.scrollStep):1.f;
                const float requestedStep=amount==LyricScrollStep::Page?presentedLyricViewport.pageScrollStep:smallStep;
                const float step=std::isfinite(requestedStep)?std::max(smallStep,requestedStep):smallStep;
                previewScrollOffset=std::isfinite(previewScrollOffset)?std::clamp(previewScrollOffset,0.f,maximum):0.f;
                const float distance=direction>0?maximum-previewScrollOffset:previewScrollOffset;
                if(distance>.5f){
                    const float needed=std::ceil(distance/step);
                    const int64_t consumed=needed>=static_cast<float>(remaining)?remaining:static_cast<int64_t>(needed);
                    previewScrollOffset=static_cast<float>(consumed)>=needed?(direction>0?maximum:0.f):
                        std::clamp(previewScrollOffset+direction*static_cast<float>(consumed)*step,0.f,maximum);
                    remaining-=consumed;
                }
            }
            if(remaining){
                const auto next=static_cast<size_t>(std::clamp<int64_t>(static_cast<int64_t>(previewIndex)+direction*remaining,0,static_cast<int64_t>(lines.size()-1)));
                if(next!=previewIndex){previewIndex=next;previewScrollOffset=direction>0?0.f:std::numeric_limits<float>::max();}
            }
            manualLyricBrowse=true;lyricBrowseUntil=now+10000;lyricBrowseGeneration=data.generation;lyricBrowseReadingRevision=currentReadingRevision;
        }
        NoteActivity();UpdateLyricBrowsing();
    }
    bool HandleReadonlyTextTab(const MSG& message) {
        if(message.message!=WM_KEYDOWN||message.wParam!=VK_TAB||
            (GetKeyState(VK_CONTROL)&0x8000)||(GetKeyState(VK_MENU)&0x8000))return false;
        const auto root=window.load(),focus=GetFocus();
        if(!root||!focus||message.hwnd!=focus||
            (focus!=status&&focus!=Control(ID_USAGEHELP)&&focus!=Control(ID_CANDIDATEDETAILS))||
            !(GetWindowLongPtrW(focus,GWL_STYLE)&ES_READONLY))return false;
        // These are reading surfaces, not Tab-entry editors. Preserve the
        // native order and visibility rules, including the scrolling pane.
        const auto next=GetNextDlgTabItem(root,focus,(GetKeyState(VK_SHIFT)&0x8000)!=0);
        if(!next||next==focus)return false;
        SetFocus(next);
        if(GetFocus()!=next)return false;
        SendMessageW(root,WM_CHANGEUISTATE,MAKEWPARAM(UIS_CLEAR,UISF_HIDEFOCUS),0);
        EnsurePaneFocusVisible(true);
        return true;
    }
    bool HandleLyricReadingKey(const MSG& message) {
        const auto key=static_cast<UINT>(message.wParam);
        if(key!=VK_F6&&key!=VK_HOME&&key!=VK_UP&&key!=VK_DOWN&&key!=VK_PRIOR&&key!=VK_NEXT)return false;
        const auto root=window.load();if(!MainDrawable())return false;
        const auto focus=GetFocus();
        if(message.hwnd!=focus)return false;
        const bool control=(GetKeyState(VK_CONTROL)&0x8000)!=0,shift=(GetKeyState(VK_SHIFT)&0x8000)!=0,
            alt=(GetKeyState(VK_MENU)&0x8000)!=0;
        if(key==VK_F6&&control&&!shift&&!alt&&focus&&(focus==root||IsChild(root,focus))){
            if(!(message.lParam&(LPARAM(1)<<30))){NoteActivity();SetFocus(root);}
            return true;
        }
        if(focus!=root&&(!focus||focus!=Control(ID_RESUME_LYRICS)))return false;
        if(key==VK_HOME){ResumeLyricFollowing();return true;}
        if(control||shift||alt)return false;
        switch(key){
            case VK_UP:ScrollLyrics(-1);return true;
            case VK_DOWN:ScrollLyrics(1);return true;
            case VK_PRIOR:ScrollLyrics(-1,LyricScrollStep::Page);return true;
            case VK_NEXT:ScrollLyrics(1,LyricScrollStep::Page);return true;
            default:return false;
        }
    }
    void StageWheel(int wheelDelta) {
        stageWheelCarry-=wheelDelta;const int steps=stageWheelCarry/WHEEL_DELTA;stageWheelCarry%=WHEEL_DELTA;if(steps)ScrollLyrics(steps);
    }
    void UpdateBackdropLabels() {
        const auto shade=L"背景淡化 · "+std::to_wstring((int)std::lround(settings.backgroundShade*100))+L"%";
        const auto camera=L"镜头幅度 · "+std::to_wstring((int)std::lround(settings.cameraMotion*100))+L"%";
        SetWindowTextW(Control(209),shade.c_str());SetWindowTextW(Control(210),camera.c_str());
    }
    void ResetBackdropAdjustments() {
        settings.backgroundShade=0;settings.cameraMotion=1;SyncThemeControls();Persist();
    }
    // data.mutex and jobMutex are held. This is also checked after Fetch,
    // independently of the cancellation signal or same-song request timing.
    bool CurrentCoverRequestLocked(const CoverRequest& request) const {
        return !stopping&&!exiting&&request.generation==data.generation&&request.serial==coverSerial&&
            !request.generationToken.stop_requested()&&!request.cancellation.stop_requested()&&
            CoverIdentityAllowedLocked(request.id,request.automaticCandidate);
    }
    bool QueueCover(const std::wstring& id,uint64_t gen,CoverIdOrigin origin,std::stop_token token={},
        bool automaticCandidate=false,bool forceRevalidate=false,std::filesystem::path protectedFile={}) noexcept {
        try{
            if(exiting||token.stop_requested()||!SameSongId(id,id))return false;
            auto request=std::make_shared<CoverRequest>();
            request->id=id;request->generation=gen;request->origin=origin;
            request->generationToken=token;request->automaticCandidate=automaticCandidate;
            request->forceRevalidate=forceRevalidate;request->protectedFile=std::move(protectedFile);
            {
                std::scoped_lock lock(data.mutex,jobMutex);
                if(stopping||exiting||gen!=data.generation||token.stop_requested()||
                    !CoverIdentityAllowedLocked(id,automaticCandidate))return false;
                if(data.playback.provider!=MusicProvider::Netease&&!ManualPreviewLocked())return false;
                if(request->protectedFile.empty()&&!data.coverFile.empty())request->protectedFile=data.coverFile;
                if(!data.coverRevalidatePending&&!data.coverFile.empty()&&SameSongId(data.coverSongId,id))return false;
                const auto matches=[&](const std::shared_ptr<CoverRequest>& previous){
                    // Enriching the same entity from candidate to native ID
                    // changes its assurance, not the image to download. A new
                    // explicit refresh must still supersede a cache-only job.
                    return previous&&CurrentCoverRequestLocked(*previous)&&SameSongId(previous->id,id)&&
                        (!forceRevalidate||previous->forceRevalidate);
                };
                if(matches(pendingCover)||matches(activeCover))return false;
                CancelCoverRequestsLocked();request->serial=coverSerial;pendingCover=std::move(request);
                data.coverStatus=activeCover?L"封面已排队，等待旧封面任务收尾…":L"正在获取封面…";
                data.coverError.clear();data.localCoverRetryAt=MonotonicMs()+30000;
            }
            coverCv.notify_one();return true;
        }catch(...){
            // Queue allocation is a cover failure, never a lyric exception.
            try{
                std::scoped_lock lock(data.mutex,jobMutex);
                if(!exiting&&!stopping&&gen==data.generation&&!token.stop_requested()&&CoverIdentityAllowedLocked(id,automaticCandidate)){
                    CancelCoverRequestsLocked();data.coverStatus=L"封面任务暂时无法排队";data.coverError=data.coverStatus;
                    data.localCoverRetryAt=MonotonicMs()+30000;
                }
            }catch(...){}
            return false;
        }
    }
    void CoverRequestFailed(const std::shared_ptr<CoverRequest>& request,const char* detail) noexcept {
        try{
            std::scoped_lock lock(data.mutex,jobMutex);
            if(CurrentCoverRequestLocked(*request)){
                data.coverStatus=L"封面暂时未能取得";data.coverError=data.coverStatus;data.localCoverRetryAt=MonotonicMs()+30000;
            }
        }catch(...){}
        try{log.Event("cover-error",detail);}catch(...){}
    }
    void FetchCover(const std::shared_ptr<CoverRequest>& request) noexcept {
        try{
            // This callback only forwards cancellation. It never takes App
            // locks, closes a decoder or touches the window.
            std::stop_callback cancelled(request->generationToken,[source=request->cancellation]() mutable {source.request_stop();});
            {
                std::scoped_lock lock(data.mutex,jobMutex);if(!CurrentCoverRequestLocked(*request))return;
                data.coverStatus=L"正在获取封面…";data.coverError.clear();
            }
            CoverClient client(request->cancellation.get_token());
            const auto result=client.Fetch(request->id,request->origin,
                {request->forceRevalidate,request->protectedFile});
            const auto path=result.file.wstring();
            std::scoped_lock lock(data.mutex,jobMutex);if(!CurrentCoverRequestLocked(*request))return;
            data.coverStatus=result.diagnostic;data.coverError=path.empty()?result.diagnostic:std::wstring{};
            // A user can request an online refresh while a cache-only request
            // is finishing. That older completion must not consume the newer
            // intent even if the identity was temporarily too stale to queue
            // the force job at click time.
            const bool newerRefresh=!request->forceRevalidate&&data.coverRevalidatePending;
            const bool refreshStillPending=result.refreshPending||newerRefresh||
                (request->forceRevalidate&&path.empty());
            data.coverRevalidatePending=refreshStillPending;
            data.localCoverRetryAt=(path.empty()||result.refreshPending||
                (request->forceRevalidate&&path.empty()))?MonotonicMs()+30000:0;
            if(!path.empty()){
                const bool changed=path!=data.coverFile||!SameSongId(data.coverSongId,request->id);
                data.coverFile=path;data.coverSongId=request->id;
                if(changed&&backgroundSelection==0)background.SetImage(data.coverFile);
            }
        }catch(const std::exception& error){CoverRequestFailed(request,error.what());}
        catch(...){CoverRequestFailed(request,"Cover request failed with a non-standard exception");}
    }
    void RunCoverJobs() {
        for(;;){
            std::shared_ptr<CoverRequest> request;
            {
                std::unique_lock lock(jobMutex);coverCv.wait(lock,[this]{return stopping||pendingCover;});
                if(stopping)return;
                request=std::exchange(pendingCover,{});activeCover=request;
            }
            FetchCover(request);
            {std::lock_guard lock(jobMutex);if(activeCover==request)activeCover.reset();}
            if(const auto target=window.load())PostMessageW(target,WM_DATA,0,0);
        }
    }
    void ChooseBackground(bool video) {wchar_t file[MaxLocalFilePathChars+1]{};OPENFILENAMEW ofn{sizeof(ofn)};ofn.hwndOwner=window;ofn.lpstrFile=file;ofn.nMaxFile=static_cast<DWORD>(std::size(file));ofn.lpstrFilter=video?L"视频 (*.mp4;*.mov;*.mkv;*.wmv)\0*.mp4;*.mov;*.mkv;*.wmv\0所有文件\0*.*\0":L"图片 (*.jpg;*.jpeg;*.png;*.bmp;*.webp)\0*.jpg;*.jpeg;*.png;*.bmp;*.webp\0所有文件\0*.*\0";ofn.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;ModalScope modal(*this);if(!GetOpenFileNameW(&ofn)||exiting)return;{std::lock_guard lock(data.mutex);backgroundSelection=video?2:1;if(video)background.SetVideo(file);else background.SetImage(file);}settings.backgroundKind=video?2:1;settings.backgroundPath=file;Persist();}
    void UseCover(bool revalidate=false) {
        {
            std::scoped_lock lock(data.mutex,jobMutex);
            const bool switchingSource=backgroundSelection!=0;
            backgroundSelection=0;data.localCoverRetryAt=0;
            if(revalidate){
                // Validation/download stays on the cover worker. An existing
                // cover frame survives retries; switching from private media
                // still stops that source through the normal image command.
                data.coverRevalidatePending=true;
                const bool inFlight=(pendingCover&&CurrentCoverRequestLocked(*pendingCover))||
                    (activeCover&&CurrentCoverRequestLocked(*activeCover));
                data.coverStatus=inFlight?L"封面重试已在处理中":L"已请求重试，等待可用的歌曲身份";
                data.coverError.clear();
                if(switchingSource){
                    if(data.coverFile.empty())background.Clear();
                    else background.SetImage(data.coverFile);
                }
            }else if(data.coverFile.empty())background.Clear();
            else background.SetImage(data.coverFile);
        }
        if(revalidate){MaintainCover();log.Event("cover-retry","Asynchronous cover validation/recovery requested; original files preserved");}
        settings.backgroundKind=0;settings.backgroundPath.clear();Persist();
    }
    BackgroundSnapshot DisplayedBackground(const PlaybackSnapshot& playback,bool independent) {
        if(backgroundSelection!=0||playback.provider==MusicProvider::Netease||independent)return background.GetSnapshot();
        // Share one effective source between the renderer and settings. The
        // current QQ artwork is independent of the lyric search candidate.
        BackgroundSnapshot result;result.kind=BackgroundKind::Image;
        result.sourceRevision=(uint64_t{1}<<63)|(static_cast<uint64_t>(playback.provider)<<60)|(playback.mediaIdentityRevision&((uint64_t{1}<<60)-1));
        if(!playback.metadataStale)result.frame=playback.mediaCover;
        result.loading=!result.frame&&!playback.metadataStale;
        result.fileName=std::wstring(MusicProviderName(playback.provider))+L"当前封面";return result;
    }
    void UpdateBackgroundNotice() {
        const auto retry=Control(ID_BACKGROUNDRETRY),note=Control(ID_BACKGROUNDNOTE);if(!retry||!note)return;
        PlaybackSnapshot playback;bool independent;
        {std::lock_guard lock(data.mutex);playback=data.playback;independent=ManualPreviewLocked();}
        const auto current=DisplayedBackground(playback,independent);
        bool canRetry=false;std::wstring message;BackgroundNoteState(current,canRetry,message);
        if(backgroundNoticePainted&&shownBackgroundRevision==current.sourceRevision&&shownBackgroundNotice==message&&backgroundRetryShown==canRetry)return;
        backgroundNoticePainted=true;shownBackgroundRevision=current.sourceRevision;shownBackgroundNotice=message;backgroundRetryShown=canRetry;
        if(!canRetry&&GetFocus()==retry)SetFocus(window);
        EnableWindow(retry,canRetry);SetWindowTextW(note,message.c_str());
    }
    void BackgroundNoteState(const BackgroundSnapshot& current,bool& canRetry,std::wstring& message) const {
        const bool failed=FAILED(current.errorCode);
        canRetry=current.kind!=BackgroundKind::None&&!current.filePath.empty()&&!current.loading&&failed;
        auto name=DisplayText(current.fileName);if(name.size()>40)name=name.substr(0,39)+L"…";
        message.clear();
        if(current.loading){
            message=failed?L"正在重试加载背景":L"正在加载背景";
            if(current.frame)message+=L"，暂时保留上次画面";
        }else if(failed){
            auto reason=DisplayText(current.error);if(reason.empty())reason=L"素材暂时不可读或格式不支持";
            if(reason.size()>64)reason=reason.substr(0,63)+L"…";
            message=backgroundSelection==0?
                L"歌曲封面显示失败。\r\n可点击“使用 / 重试歌曲封面”重新获取。\r\n"+reason:
                L"背景加载失败，素材引用已保留。\r\n可重试当前素材或重新选择文件。\r\n"+reason;
        }else if(current.kind==BackgroundKind::Image&&current.frame)message=L"图片背景已就绪";
        else if(current.kind==BackgroundKind::Video&&current.frame)message=current.playing?L"视频背景播放中":L"视频背景已停止";
        else message=backgroundSelection==0?L"当前使用主题背景，等待歌曲封面":L"当前没有可显示的背景素材";
        if(!name.empty())message+=L"\r\n"+name;
    }
    void RetryBackground() {
        if(background.Retry(shownBackgroundRevision)){
            uiNotice=L"正在重新加载当前背景素材";
            log.Event("background-retry","Retry requested for the last displayed failed source revision");
        }else uiNotice=L"背景状态已变化或正在加载，请按当前状态操作";
        UpdateBackgroundNotice();
    }
    ThemeDefinition CurrentTheme() const {
        auto value=customTheme?*customTheme:BuiltInTheme(theme);
        value.lyricFont=settings.lyricFont;value.lyricFontPath=settings.lyricFontPath;
        value.response=strength;value.lyricSize=settings.lyricSize;value.reducedMotion=settings.reducedMotion;value.lyricDisplayMode=settings.lyricDisplayMode;
        value.motionVariant=settings.motionVariant;value.motionFx=settings.motionFx;value.motionCamera=settings.motionCamera;value.motionLines=settings.motionLines;value.motionBackground=settings.motionBackground;value.motionFilter=settings.motionFilter;
        value.backgroundShade=settings.backgroundShade;value.cameraMotion=settings.cameraMotion;
        value.backgroundKind=backgroundSelection==1?ThemeBackgroundKind::Image:backgroundSelection==2?ThemeBackgroundKind::Video:ThemeBackgroundKind::None;
        value.backgroundPath=value.backgroundKind==ThemeBackgroundKind::None?std::filesystem::path{}:std::filesystem::path(settings.backgroundPath);
        return value;
    }
    ThemeDefinition DisplayedTheme() const {
        auto value=CurrentTheme();if(displayedPalette)ApplyPalette(value,*displayedPalette);return value;
    }
    void CaptureSettingsValues() {
        // Legacy offsetMs is a fallback for older settings, not the last song's
        // adjustment. Per-track edits are saved separately after a short debounce.
        settings.theme=theme;settings.response=strength;
        if(!fullscreen&&!stageUi)settings.panelVisible=panelVisible;
    }
    void RememberPrivateThemeSaveFile() {
        // Only a path already committed by settings can become the in-memory
        // private theme path. Imported external themes remain read-only sources.
        privateThemeSaveFile.clear();
        const auto file=std::filesystem::path(settings.customThemePath).lexically_normal();
        if(IsApplicationPrivateThemeFile(file))privateThemeSaveFile=file;
    }
    bool PrepareThemeForRecovery(AppSettings& candidate,std::wstring& diagnostic) {
        diagnostic.clear();candidate=settings;
        if(customTheme){
            const auto reusable=stagedRecoveryThemeFile.empty()?privateThemeSaveFile:stagedRecoveryThemeFile;
            auto prepared=StageApplicationPrivateTheme(CurrentTheme(),reusable);
            if(!prepared.ok){diagnostic=L"自定义主题尚未保存："+prepared.diagnostic;return false;}
            stagedRecoveryThemeFile=std::move(prepared.file);
            candidate.customThemePath=stagedRecoveryThemeFile.wstring();
        }else if(!themeRestorePending)candidate.customThemePath.clear();
        return true;
    }
    void UpdateThemeRestoreNotice() {
        const auto restore=Control(ID_THEMERESTORE),note=Control(ID_THEMENOTE);if(!restore||!note)return;
        if(themeRestoreStatePainted&&themeRestoreShown==themeRestorePending&&themeRestoreDiagnosticShown==themeRestoreDiagnostic)return;
        themeRestoreStatePainted=true;themeRestoreShown=themeRestorePending;themeRestoreDiagnosticShown=themeRestoreDiagnostic;
        if(!themeRestorePending&&GetFocus()==restore)SetFocus(window);
        EnableWindow(restore,themeRestorePending);
        SetWindowTextW(note,ThemeRestoreNoteText().c_str());
    }
    std::wstring ThemeRestoreNoteText() const {
        auto reason=DisplayText(themeRestoreDiagnostic);if(reason.size()>80)reason=reason.substr(0,79)+L"…";
        return themeRestorePending?L"保存的主题暂时无法载入，现使用内置风格。原文件和关联已保留，可重试或选择其他主题。\r\n"+reason:std::wstring(L"当前主题可用。载入本地主题时保留源文件，自动保存使用本应用的主题副本。");
    }
    void RetrySavedTheme() {
        if(!themeRestorePending||settings.customThemePath.empty())return;
        auto loaded=LoadTheme(settings.customThemePath);
        if(!loaded.ok){themeRestoreDiagnostic=loaded.diagnostic;uiNotice=L"主题仍暂时无法载入，原关联已保留";log.Event("theme-restore-retry-failed",Utf8(loaded.diagnostic));UpdateThemeRestoreNotice();SyncThemeControls();return;}
        customTheme=std::make_shared<ThemeDefinition>(std::move(loaded.definition));theme=customTheme->baseStyle;
        themeRestorePending=false;preserveSavedThemeFile=false;themeRestoreDiagnostic.clear();privateThemeSaveFile.clear();RememberPrivateThemeSaveFile();
        // Main settings remain authoritative for the user's current material,
        // size and response, just as they are during an ordinary startup.
        uiNotice=L"已恢复保存的主题配色与排版，当前舞台参数保持";log.Event("theme-restore-retry","Saved theme restored; current stage preferences retained");
        SyncThemeControls();Persist();UpdateThemeRestoreNotice();
    }
    void SelectTheme(int choice) {
        if(choice>=0&&choice<BuiltInStyleCount){theme=NormalizeBuiltInStyle(choice);settings.motionVariant=-1;customTheme.reset();themeRestorePending=false;themeRestoreDiagnostic.clear();}
        else if(choice==CustomThemeChoice&&!customTheme){
            if(themeRestorePending){RetrySavedTheme();return;}
            auto value=CurrentTheme();value.name=L"我的音乐舞台";customTheme=std::make_shared<ThemeDefinition>(std::move(value));
        }else if(choice!=CustomThemeChoice)return;
        InvalidateRect(window,nullptr,FALSE);Persist();UpdateThemeRestoreNotice();
    }
    void UpdateSettingsNotice() {
        const auto repair=Control(ID_SETTINGSREPAIR),note=Control(ID_SETTINGSNOTE);if(!repair||!note)return;
        const bool pending=settingsSaveAt>0||settingsSaveInFlight;
        if(settingsStatePainted&&settingsWritableShown==settings.writable&&settingsPendingShown==pending&&
           settingsThemePendingShown==settingsThemePending&&settingsSaveDiagnosticShown==settingsSaveDiagnostic)return;
        settingsStatePainted=true;settingsWritableShown=settings.writable;settingsPendingShown=pending;
        settingsThemePendingShown=settingsThemePending;settingsSaveDiagnosticShown=settingsSaveDiagnostic;
        const auto state=SettingsNoteState();
        if(!state.actionable&&GetFocus()==repair)SetFocus(window);
        EnableWindow(repair,state.actionable);
        SetWindowTextW(repair,state.button.c_str());
        SetWindowTextW(note,state.note.c_str());
    }
    struct ActionNote {bool actionable=false;std::wstring button,note;};
    ActionNote SettingsNoteState() const {
        const bool pending=settingsSaveAt>0||settingsSaveInFlight;
        ActionNote state;state.actionable=!settings.writable||!settingsSaveDiagnostic.empty();
        state.button=settings.writable?L"重试设置保存":L"恢复设置保存";
        std::wstring message;
        if(!settings.writable){
            message=L"自动保存已暂停。\r\n本次调整仅在当前运行中生效。\r\n恢复时会保留原设置备份，再保存当前设置。";
            auto reason=DisplayText(settingsSaveDiagnostic);if(reason.size()>80)reason=reason.substr(0,79)+L"…";
            if(!reason.empty())message+=L"\r\n"+reason;
        }
        else if(!settingsSaveDiagnostic.empty()){
            auto reason=DisplayText(settingsSaveDiagnostic);if(reason.size()>80)reason=reason.substr(0,79)+L"…";
            message=settingsThemePending?
                L"常规设置已保存，自定义主题副本仍待保存。\r\n稍后会自动重试，也可点击重试。\r\n"+reason:
                L"设置尚未保存，稍后会自动重试，也可点击重试。\r\n当前修改仍保留在运行中。\r\n"+reason;
        }else if(pending)message=L"设置调整待保存。\r\n自动保存完成前，当前修改仍保留在运行中。";
        else message=L"设置随调整自动保存在本机。\r\n原文件异常时，可在此恢复设置保存。";
        state.note=std::move(message);
        return state;
    }
    void ReportSettingsSaveFailure(std::wstring diagnostic) {
        if(diagnostic.empty())diagnostic=L"设置暂时无法保存，当前修改仍保留在运行中";
        if(settingsSaveDiagnostic!=diagnostic){uiNotice=diagnostic;log.Event("settings-save",Utf8(diagnostic));}
        settingsSaveDiagnostic=std::move(diagnostic);
        const auto retryAt=MonotonicMs()+5000;if(!settingsSaveAt||settingsSaveAt>retryAt)settingsSaveAt=retryAt;
        if(!exiting)UpdateSettingsNotice();
    }
    void PollSettingsSave(bool notify=true) {
        auto result=settingsWriter.Poll();
        if(!result||result->revision!=settingsSaveRevision)return;
        settingsSaveInFlight=false;settingsThemePending=result->settingsCommitted&&result->themePending;
        // Never copy saved settings back over newer, unsent slider values.
        // Only the current request's persistence metadata belongs to the UI.
        if(result->themeCommitted&&customTheme){
            privateThemeSaveFile=std::move(result->privateThemeFile);
            settings.customThemePath=std::move(result->customThemePath);
            stagedRecoveryThemeFile.clear();themeRestorePending=false;preserveSavedThemeFile=false;themeRestoreDiagnostic.clear();
        }else if(result->ok&&!customTheme&&!themeRestorePending){
            settings.customThemePath.clear();privateThemeSaveFile.clear();stagedRecoveryThemeFile.clear();preserveSavedThemeFile=false;
        }
        if(!result->ok){
            if(notify)ReportSettingsSaveFailure(std::move(result->diagnostic));
            else{settingsSaveDiagnostic=std::move(result->diagnostic);log.Event("settings-save-at-exit",Utf8(settingsSaveDiagnostic));}
            return;
        }
        if(!settingsSaveDiagnostic.empty()){
            if(uiNotice==settingsSaveDiagnostic)uiNotice=L"设置保存已恢复";
            log.Event("settings-save-recovered","Accepted theme and settings snapshot committed; newer edits may remain pending");
        }
        settingsSaveDiagnostic.clear();settingsThemePending=false;
        if(notify){UpdateSettingsNotice();UpdateThemeRestoreNotice();}
    }
    void Persist(bool captureValues=true) {
        settingsSaveAt=0;if(captureValues)CaptureSettingsValues();
        if(!settings.writable){
            uiNotice=L"原设置文件不作自动覆盖，本次调整暂不保存；可在状态页恢复设置保存";
            if(!settingsWriteBlockedLogged){settingsWriteBlockedLogged=true;log.Event("settings-write-protected",Utf8(uiNotice));}
            if(!exiting)UpdateSettingsNotice();return;
        }
        try{
            SettingsSaveRequest request;request.revision=++settingsSaveRevision;request.settings=settings;
            if(customTheme)request.customTheme=CurrentTheme();
            request.themeRestorePending=themeRestorePending;
            if(!settingsWriter.Submit(std::move(request))){settingsSaveInFlight=false;ReportSettingsSaveFailure(L"设置保存线程已停止，当前修改仍保留在运行中");return;}
            settingsSaveInFlight=true;
        }catch(...){settingsSaveInFlight=false;ReportSettingsSaveFailure(L"无法准备设置保存任务，当前修改仍保留在运行中");return;}
        if(!exiting)UpdateSettingsNotice();
    }
    void RecoverSettingsSaving() {
        if(settings.writable){
            if(settingsSaveDiagnostic.empty()){uiNotice=L"设置当前没有保存错误";return;}
            Persist();uiNotice=settingsSaveInFlight?L"已安排重试设置保存，请以保存状态为准":settingsSaveDiagnostic;return;
        }
        ModalScope modal(*this);
        if(MessageBoxW(window,L"将保留原设置文件的备份，再保存当前界面的设置。\r\n\r\n旧版本或无法读取的配置不会自动转换。此操作不修改你的主题源文件、背景素材或歌曲。\r\n\r\n现在恢复设置保存？",L"恢复设置保存",MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)!=IDYES||exiting)return;
        settingsWriter.WaitIdle();PollSettingsSave();
        settingsSaveAt=0;CaptureSettingsValues();RememberWindowPlacement();
        std::wstring diagnostic;
        AppSettings candidate;
        if(!PrepareThemeForRecovery(candidate,diagnostic)){ReportSettingsSaveFailure(std::move(diagnostic));return;}
        std::filesystem::path backup;
        const bool recovered=RecoverSettings(candidate,backup,diagnostic);uiNotice=diagnostic;
        log.Event(recovered?"settings-recovered":"settings-recovery-failed",Utf8(diagnostic));
        if(recovered){
            settings=std::move(candidate);settingsWriteBlockedLogged=false;settingsSaveDiagnostic.clear();
            if(customTheme){
                privateThemeSaveFile=std::move(stagedRecoveryThemeFile);
                themeRestorePending=false;preserveSavedThemeFile=false;themeRestoreDiagnostic.clear();
            }else if(!themeRestorePending){
                privateThemeSaveFile.clear();stagedRecoveryThemeFile.clear();preserveSavedThemeFile=false;
            }else RememberPrivateThemeSaveFile();
        }
        else settingsSaveDiagnostic=diagnostic;
        UpdateSettingsNotice();
    }
    void UpdateTrackPreferenceNotice() {
        const auto repair=Control(ID_LYRICMEMORYREPAIR),note=Control(ID_LYRICMEMORYNOTE);if(!repair||!note)return;
        bool writable=true,pending=false;std::wstring diagnostic;
        {std::lock_guard lock(data.mutex);writable=data.trackPreferences.writable;pending=data.preferencesDirty||data.preferenceSaveInFlight;diagnostic=data.preferenceSaveDiagnostic;}
        if(preferenceStatePainted&&preferenceWritableShown==writable&&preferencePendingShown==pending&&preferenceSaveDiagnosticShown==diagnostic)return;
        preferenceStatePainted=true;preferenceWritableShown=writable;preferencePendingShown=pending;preferenceSaveDiagnosticShown=diagnostic;
        const auto state=TrackPreferenceNoteState(writable,pending,diagnostic);
        if(!state.actionable&&GetFocus()==repair)SetFocus(window);
        EnableWindow(repair,state.actionable);SetWindowTextW(repair,state.button.c_str());
        SetWindowTextW(note,state.note.c_str());
    }
    ActionNote TrackPreferenceNoteState(bool writable,bool pending,const std::wstring& diagnostic) const {
        ActionNote state;state.actionable=!writable||!diagnostic.empty();
        state.button=writable?L"重试歌词记忆保存":L"恢复歌词记忆保存";
        std::wstring message;
        if(!writable){
            message=L"歌词记忆自动保存已暂停。\r\n本次偏移和导入关联仅在当前运行中生效。\r\n恢复时会备份原文件，再保存当前记忆。";
            auto reason=DisplayText(diagnostic);if(reason.size()>80)reason=reason.substr(0,79)+L"…";
            if(!reason.empty())message+=L"\r\n"+reason;
        }
        else if(!diagnostic.empty()){
            auto reason=DisplayText(diagnostic);if(reason.size()>80)reason=reason.substr(0,79)+L"…";
            message=L"歌词记忆尚未保存，稍后会自动重试，也可点击重试。\r\n当前偏移和导入关联仍保留在运行中。\r\n"+reason;
        }else if(pending)message=L"本次偏移或导入关联待保存。\r\n自动保存完成前，修改仍保留在运行中。";
        else message=L"偏移和导入关联随歌曲保存在本机。\r\n记忆文件异常时，可在此恢复保存。";
        state.note=std::move(message);
        return state;
    }
    void RecoverTrackPreferenceSaving() {
        bool writable=true,failed=false;
        {std::lock_guard lock(data.mutex);writable=data.trackPreferences.writable;failed=!data.preferenceSaveDiagnostic.empty();}
        if(writable){
            if(!failed){uiNotice=L"歌词记忆当前没有保存错误";return;}
            FlushTrackPreferences(true);UpdateTrackPreferenceNotice();
            bool saved=false,queued=false;std::wstring diagnostic;
            {std::lock_guard lock(data.mutex);saved=!data.preferencesDirty&&data.preferenceSaveDiagnostic.empty();queued=data.preferenceSaveInFlight;diagnostic=data.preferenceSaveDiagnostic;}
            if(saved)uiNotice=L"当前歌词记忆已保存";
            else if(queued)uiNotice=L"已安排重试歌词记忆保存，请以保存状态为准";
            else if(!diagnostic.empty())uiNotice=std::move(diagnostic);
            else uiNotice=L"歌词记忆仍有新修改待保存";
            return;
        }
        ModalScope modal(*this);
        if(MessageBoxW(window,L"将备份原歌词记忆文件，再保存当前运行中的偏移和导入关联。\r\n\r\n无法读取的旧内容只保留在备份中，不会自动合并。此操作不修改歌词源文件、歌曲或主设置。\r\n\r\n现在恢复歌词记忆保存？",L"恢复歌词记忆保存",MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)!=IDYES||exiting)return;
        settingsWriter.WaitIdle();PollSettingsSave();PollTrackPreferenceSave();
        TrackLyricPreferences snapshot;uint64_t serial=0;
        {std::lock_guard lock(data.mutex);if(data.trackPreferences.writable)return;snapshot=data.trackPreferences;serial=data.preferenceRevision;}
        std::filesystem::path backup;std::wstring diagnostic;
        const bool recovered=RecoverTrackLyricPreferences(snapshot,backup,diagnostic);
        if(recovered){
            std::lock_guard lock(data.mutex);data.trackPreferences.writable=true;
            ++data.preferenceSaveRequest;data.preferenceSaveInFlight=false;
            // File I/O ran outside the state lock. Never replace newer memory
            // with the snapshot that was just committed.
            if(serial==data.preferenceRevision){data.preferencesDirty=false;data.preferenceSaveAt=0;}
            else{data.preferencesDirty=true;data.preferenceSaveAt=MonotonicMs()+750;}
            preferenceWriteBlockedLogged=false;data.preferenceNotice.clear();data.preferenceSaveDiagnostic.clear();
        }
        else{std::lock_guard lock(data.mutex);data.preferenceSaveDiagnostic=diagnostic;}
        uiNotice=diagnostic;log.Event(recovered?"track-preferences-recovered":"track-preferences-recovery-failed",Utf8(diagnostic));
        UpdateTrackPreferenceNotice();
    }
    bool CompactStageControls() const {
        RECT client{};if(!window||!GetClientRect(window,&client))return false;
        const auto scale=WindowDpi(window)/96.f;const float width=client.right/scale;
        return CompactStageLayout(width-(panelVisible?SettingsRailWidth(width):0),client.bottom/scale);
    }
    void RestoreWindowBounds() {
        if(!window)return;
        if(settings.hasWindowPlacement){
            WINDOWPLACEMENT saved{sizeof(saved)};saved.showCmd=SW_HIDE;
            saved.rcNormalPosition={settings.windowX,settings.windowY,settings.windowX+settings.windowWidth,settings.windowY+settings.windowHeight};
            if(!SetWindowPlacement(window,&saved))log.Event("window-placement-restore","Native placement was unavailable; fitting the current window instead");
        }
        const auto dpi=WindowDpi(window);
        SIZE preferred{MulDiv(1280,dpi,96),MulDiv(900,dpi,96)};
        if(settings.hasWindowPlacement){
            preferred={settings.windowWidth,settings.windowHeight};
            if(settings.windowDpi){preferred.cx=MulDiv(preferred.cx,dpi,settings.windowDpi);preferred.cy=MulDiv(preferred.cy,dpi,settings.windowDpi);}
        }
        FitOwnWindow(window,false,preferred,!settings.hasWindowPlacement);
        // CreateWindowEx can emit WM_SIZE before this startup restoration.
        // Only later size notifications are allowed to replace the loaded
        // placement snapshot.
        windowPlacementReady=true;
    }
    void ShowInitialWindow(int requestedShow) {
        const auto root=window.load();if(!root||!IsWindow(root))return;
        const bool savedMaximized=settings.windowMaximized;
        const bool requestedMinimized=requestedShow==SW_SHOWMINIMIZED||requestedShow==SW_SHOWMINNOACTIVE||requestedShow==SW_MINIMIZE;
        const int actualShow=savedMaximized&&!requestedMinimized?SW_SHOWMAXIMIZED:requestedShow;
        ShowWindow(root,actualShow);
        // Windows may ignore the first ShowWindow argument when the launcher
        // supplied STARTF_USESHOWWINDOW. The saved application placement still
        // owns normal-versus-maximized state unless startup explicitly asked
        // for a minimized window.
        if(savedMaximized&&!requestedMinimized&&!IsZoomed(root))ShowWindow(root,SW_SHOWMAXIMIZED);
        if(requestedMinimized&&savedMaximized){
            WINDOWPLACEMENT placement{sizeof(placement)};
            if(GetWindowPlacement(root,&placement)){
                placement.flags|=WPF_RESTORETOMAXIMIZED;placement.showCmd=actualShow;
                if(!SetWindowPlacement(root,&placement))log.Event("window-placement-minimized","Unable to preserve restore-to-maximized state");
            }else log.Event("window-placement-minimized","Unable to read minimized startup placement");
        }
        UpdateWindow(root);
        if(controlsReady)RefreshVisualMediaDemand();
    }
    bool CaptureWindowPlacement() {
        const auto root=window.load();if(!root||!IsWindow(root)||fullscreen||(desktopActive&&!IsWindowVisible(root)))return false;
        WINDOWPLACEMENT current{sizeof(current)};if(!GetWindowPlacement(root,&current))return false;
        const RECT r=current.rcNormalPosition;const int w=r.right-r.left,h=r.bottom-r.top;if(w<320||h<240||w>16384||h>16384)return false;
        settings.hasWindowPlacement=true;settings.windowX=r.left;settings.windowY=r.top;settings.windowWidth=w;settings.windowHeight=h;settings.windowDpi=(int)WindowDpi(root);
        settings.windowMaximized=current.showCmd==SW_SHOWMAXIMIZED||(current.showCmd==SW_SHOWMINIMIZED&&(current.flags&WPF_RESTORETOMAXIMIZED));return true;
    }
    void RememberWindowPlacement() {if(CaptureWindowPlacement())Persist();}
    void ChooseTheme(bool exporting) {
        wchar_t file[MaxLocalFilePathChars+1]{};OPENFILENAMEW ofn{sizeof(ofn)};ofn.hwndOwner=window;ofn.lpstrFile=file;ofn.nMaxFile=static_cast<DWORD>(std::size(file));ofn.lpstrFilter=L"HTune 主题 (*.json)\0*.json\0";ofn.lpstrDefExt=L"json";ofn.Flags=OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR|(exporting?OFN_OVERWRITEPROMPT:OFN_FILEMUSTEXIST);
        ModalScope modal(*this);if(!(exporting?GetSaveFileNameW(&ofn):GetOpenFileNameW(&ofn))||exiting)return;
        // Finish older automatic writes before the explicit file action. App
        // private snapshots remain owned by the settings transaction and are
        // never valid export destinations.
        if(exporting){settingsWriter.WaitIdle();PollSettingsSave();}
        ThemeResult result;
        if(exporting&&IsApplicationPrivateThemeDestination(file))
            result.diagnostic=L"应用私有主题快照由设置事务管理，请选择其他目录导出";
        else result=exporting?SaveTheme(file,CurrentTheme()):LoadTheme(file);
        uiNotice=result.diagnostic;
        if(!result.ok){MessageBoxW(window,result.diagnostic.c_str(),L"主题未能载入或保存",MB_OK|MB_ICONINFORMATION);return;}
        if(!exporting){customTheme=std::make_shared<ThemeDefinition>(std::move(result.definition));themeRestorePending=false;themeRestoreDiagnostic.clear();theme=customTheme->baseStyle;strength=customTheme->response;settings.lyricSize=customTheme->lyricSize;settings.lyricFont=customTheme->lyricFont;settings.lyricFontPath=customTheme->lyricFontPath.wstring();settings.reducedMotion=customTheme->reducedMotion;settings.lyricDisplayMode=customTheme->lyricDisplayMode;
            settings.motionVariant=customTheme->motionVariant;settings.motionFx=customTheme->motionFx;settings.motionCamera=customTheme->motionCamera;settings.motionLines=customTheme->motionLines;settings.motionBackground=customTheme->motionBackground;settings.motionFilter=customTheme->motionFilter;
            settings.backgroundShade=customTheme->backgroundShade;settings.cameraMotion=customTheme->cameraMotion;
            if(customTheme->backgroundKind==ThemeBackgroundKind::None)UseCover();
            else{{std::lock_guard lock(data.mutex);backgroundSelection=customTheme->backgroundKind==ThemeBackgroundKind::Video?2:1;if(backgroundSelection==2)background.SetVideo(customTheme->backgroundPath);else background.SetImage(customTheme->backgroundPath);}settings.backgroundKind=backgroundSelection;settings.backgroundPath=customTheme->backgroundPath.wstring();}
            SyncThemeControls();Persist();}
        log.Event(exporting?"theme-export":"theme-import",exporting?"User-selected local theme destination":"User-selected local theme; original file preserved");
    }
    void ChooseLyricFont(bool importFile) {
        std::wstring family,path;
        if(importFile){
            wchar_t selected[MaxLocalFilePathChars+1]{};OPENFILENAMEW ofn{sizeof(ofn)};
            ofn.hwndOwner=window;ofn.lpstrFile=selected;ofn.nMaxFile=static_cast<DWORD>(std::size(selected));
            ofn.lpstrFilter=L"字体 (*.ttf;*.otf)\0*.ttf;*.otf\0";ofn.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
            ModalScope modal(*this);if(!GetOpenFileNameW(&ofn)||exiting)return;path=selected;
        }else{
            LOGFONTW lf{};lf.lfHeight=-24;lf.lfCharSet=DEFAULT_CHARSET;
            const auto current=settings.lyricFont.empty()||settings.lyricFont.front()==L'@'?std::wstring(L"Microsoft YaHei UI"):settings.lyricFont;
            wcsncpy_s(lf.lfFaceName,current.c_str(),_TRUNCATE);
            CHOOSEFONTW dialog{sizeof(dialog)};dialog.hwndOwner=window;dialog.lpLogFont=&lf;
            dialog.Flags=CF_SCREENFONTS|CF_INITTOLOGFONTSTRUCT|CF_FORCEFONTEXIST|CF_NOSCRIPTSEL;
            ModalScope modal(*this);if(!ChooseFontW(&dialog)||exiting)return;family=lf.lfFaceName;
        }
        Microsoft::WRL::ComPtr<IDWriteFactory> factory;
        if(FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(factory.GetAddressOf()))))return;
        OptionalLyricFont check;check.Select(factory.Get(),family,path);
        if(!check.Active()){
            lyricFontNotice=L"未能读取该字体。请选择有效的本地 TTF / OTF 文件（不超过 64 MiB）或已安装字体，原字体保持不变。";return;
        }
        settings.lyricFont=check.Family();settings.lyricFontPath=path;lyricFontNotice.clear();Persist();
    }
    void ChooseAccent() {
        auto value=CurrentTheme();static COLORREF colors[16]{};CHOOSECOLORW dialog{sizeof(dialog)};dialog.hwndOwner=window;dialog.lpCustColors=colors;dialog.rgbResult=RGB((int)(value.accent.r*255),(int)(value.accent.g*255),(int)(value.accent.b*255));dialog.Flags=CC_FULLOPEN|CC_RGBINIT;
        ModalScope modal(*this);if(!ChooseColorW(&dialog)||exiting)return;value.name=L"我的音乐舞台";value.accent={GetRValue(dialog.rgbResult)/255.f,GetGValue(dialog.rgbResult)/255.f,GetBValue(dialog.rgbResult)/255.f,1};customTheme=std::make_shared<ThemeDefinition>(std::move(value));themeRestorePending=false;themeRestoreDiagnostic.clear();SyncThemeControls();Persist();
    }
    void SyncThemeControls() {
        SendMessageW(shadeSlider,TBM_SETPOS,TRUE,(int)std::lround(settings.backgroundShade*100));
        SendMessageW(cameraSlider,TBM_SETPOS,TRUE,(int)std::lround(settings.cameraMotion*100));UpdateBackdropLabels();
        SendMessageW(lyricModeCombo,CB_SETCURSEL,settings.lyricDisplayMode,0);
        SendMessageW(Control(ID_AUTOHIDE),BM_SETCHECK,settings.fullscreenAutoHide?BST_CHECKED:BST_UNCHECKED,0);
        SendMessageW(Control(ID_TRANSLATION),BM_SETCHECK,settings.showTranslation?BST_CHECKED:BST_UNCHECKED,0);
        SendMessageW(Control(ID_ROMANIZATION),BM_SETCHECK,settings.showRomanization?BST_CHECKED:BST_UNCHECKED,0);
        SendMessageW(qualityCombo,CB_SETCURSEL,std::clamp(settings.qualityMode,0,3),0);
        SendMessageW(frameRateCombo,CB_SETCURSEL,std::clamp(settings.frameRateMode,0,2),0);
        SendMessageW(Control(ID_PAUSEFULLSCREEN),BM_SETCHECK,settings.pauseForFullscreenApps?BST_CHECKED:BST_UNCHECKED,0);
        SendMessageW(themeCombo,CB_SETCURSEL,ThemeChoiceIndex(customTheme||themeRestorePending?CustomThemeChoice:theme),0);SendMessageW(strengthSlider,TBM_SETPOS,TRUE,(int)(strength*100));SendMessageW(sizeSlider,TBM_SETPOS,TRUE,(int)settings.lyricSize);SendMessageW(Control(ID_MOTION),BM_SETCHECK,settings.reducedMotion?BST_CHECKED:BST_UNCHECKED,0);SendMessageW(Control(ID_CLOCK),BM_SETCHECK,settings.nativeClockEnabled?BST_CHECKED:BST_UNCHECKED,0);
    }
    void SetPanel(bool visible) {
        if(!visible&&(GetFocus()==status||(settingsPane&&IsChild(settingsPane,GetFocus()))))SetFocus(window);
        NoteActivity();lastPaneFocus=nullptr;paneFocusRevealPending=false;panelVisible=visible;for(const auto& [child,controlPage]:pageControls)ShowWindow(child,visible&&(controlPage<0||controlPage==page)?SW_SHOW:SW_HIDE);
        ShowWindow(settingsPane,visible?SW_SHOW:SW_HIDE);ShowWindow(tabs,visible?SW_SHOW:SW_HIDE);Layout();
    }
    bool SetOwnWindowStyle(LONG_PTR style) {
        const auto root=window.load();if(!root||!IsWindow(root)){SetLastError(ERROR_INVALID_WINDOW_HANDLE);return false;}
        SetLastError(ERROR_SUCCESS);const auto previous=SetWindowLongPtrW(root,GWL_STYLE,style);const auto error=GetLastError();
        if(!previous&&error!=ERROR_SUCCESS){SetLastError(error);return false;}
        SetLastError(ERROR_SUCCESS);const auto applied=GetWindowLongPtrW(root,GWL_STYLE);const auto readError=GetLastError();
        if((!applied&&readError!=ERROR_SUCCESS)||applied!=style){SetLastError(readError==ERROR_SUCCESS?ERROR_INVALID_STATE:readError);return false;}
        return true;
    }
    bool RefreshOwnWindowFrame(HWND root) {
        return SetWindowPos(root,nullptr,0,0,0,0,
            SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED)!=FALSE;
    }
    bool RestoreFullscreenSnapshot(HWND root,const FullscreenRestoreState& saved,DWORD& restoreError,bool& exact) {
        restoreError=ERROR_SUCCESS;exact=false;
        if(!SetOwnWindowStyle(saved.style)){restoreError=GetLastError();return false;}
        const bool placementRestored=SetWindowPlacement(root,&saved.placement)!=FALSE;
        if(!placementRestored)restoreError=GetLastError();
        const bool frameRestored=RefreshOwnWindowFrame(root);
        if(!frameRestored&&restoreError==ERROR_SUCCESS)restoreError=GetLastError();
        exact=placementRestored&&frameRestored;if(exact)return true;
        const auto normal=saved.placement.rcNormalPosition;
        const SIZE preferred{std::clamp<LONG>(normal.right-normal.left,1,16384),
            std::clamp<LONG>(normal.bottom-normal.top,1,16384)};
        ShowWindow(root,SW_RESTORE);
        const bool fallbackFrame=RefreshOwnWindowFrame(root);
        if(!fallbackFrame&&restoreError==ERROR_SUCCESS)restoreError=GetLastError();
        const bool fitted=fallbackFrame&&FitOwnWindow(root,false,preferred,false);
        if(!fitted&&restoreError==ERROR_SUCCESS)restoreError=GetLastError();
        return fitted;
    }
    void PreserveFullscreenRecovery(const FullscreenRestoreState& saved) {
        fullscreenRestore=saved;fullscreenRestore.valid=true;fullscreen=true;
        fullscreenTransition=FullscreenTransition::RecoveryPending;SetPanel(false);
    }
    void ReportFullscreenFailure(const wchar_t* action,DWORD error,FullscreenFailureState state) {
        const wchar_t* suffix=L"，未更改当前窗口";const char* event="fullscreen-transition-rejected";
        if(state==FullscreenFailureState::Recovered){suffix=L"，已恢复可用窗口";event="fullscreen-transition-recovered";}
        else if(state==FullscreenFailureState::RecoveryPending){suffix=L"，已保留恢复信息，可按 Esc 重试恢复";event="fullscreen-transition-incomplete";}
        uiNotice=std::wstring(action)+L"（Windows "+std::to_wstring(error==ERROR_SUCCESS?ERROR_INVALID_STATE:error)+L"）"+suffix;
        log.Event(event,Utf8(uiNotice));
    }
    bool EnterFullscreen() {
        const auto root=window.load();if(!root||!IsWindow(root)){ReportFullscreenFailure(L"主窗口不可用，无法进入全屏",ERROR_INVALID_WINDOW_HANDLE,FullscreenFailureState::Unchanged);return false;}
        FullscreenRestoreState saved;saved.panelVisible=panelVisible;
        SetLastError(ERROR_SUCCESS);saved.style=GetWindowLongPtrW(root,GWL_STYLE);auto error=GetLastError();
        if((!saved.style&&error!=ERROR_SUCCESS)||!GetWindowPlacement(root,&saved.placement)){
            if(error==ERROR_SUCCESS)error=GetLastError();ReportFullscreenFailure(L"无法保存进入全屏前的窗口状态",error,FullscreenFailureState::Unchanged);return false;
        }
        const auto fullscreenMonitor=WindowMonitor(MonitorFromWindow(root,MONITOR_DEFAULTTONEAREST));
        if(!fullscreenMonitor||fullscreenMonitor->rcMonitor.right<=fullscreenMonitor->rcMonitor.left||fullscreenMonitor->rcMonitor.bottom<=fullscreenMonitor->rcMonitor.top){
            ReportFullscreenFailure(L"无法读取全屏显示器范围",GetLastError(),FullscreenFailureState::Unchanged);return false;
        }
        saved.valid=true;fullscreenRestore=saved;fullscreenTransition=FullscreenTransition::Entering;
        const auto fullscreenStyle=saved.style&~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW);
        if(!SetOwnWindowStyle(fullscreenStyle)){
            error=GetLastError();fullscreenTransition=FullscreenTransition::Exiting;
            DWORD restoreError=ERROR_SUCCESS;bool exact=false;const bool recovered=RestoreFullscreenSnapshot(root,saved,restoreError,exact);
            if(recovered){fullscreenRestore={};fullscreen=false;fullscreenTransition=FullscreenTransition::Stable;}
            else PreserveFullscreenRecovery(saved);
            const auto reportError=!recovered&&restoreError!=ERROR_SUCCESS?restoreError:error;
            ReportFullscreenFailure(L"无法切换全屏窗口样式",reportError,recovered?FullscreenFailureState::Recovered:FullscreenFailureState::RecoveryPending);return false;
        }
        const auto& bounds=fullscreenMonitor->rcMonitor;
        if(!SetWindowPos(root,HWND_TOP,bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top,
                         SWP_NOACTIVATE|SWP_FRAMECHANGED)){
            error=GetLastError();fullscreenTransition=FullscreenTransition::Exiting;
            DWORD restoreError=ERROR_SUCCESS;bool exact=false;const bool recovered=RestoreFullscreenSnapshot(root,saved,restoreError,exact);
            if(recovered){fullscreenRestore={};fullscreen=false;fullscreenTransition=FullscreenTransition::Stable;}
            else PreserveFullscreenRecovery(saved);
            const auto reportError=!recovered&&restoreError!=ERROR_SUCCESS?restoreError:error;
            ReportFullscreenFailure(L"无法铺满目标显示器",reportError,recovered?FullscreenFailureState::Recovered:FullscreenFailureState::RecoveryPending);return false;
        }
        fullscreen=true;fullscreenTransition=FullscreenTransition::Stable;SetPanel(false);return true;
    }
    bool ExitFullscreen() {
        const auto root=window.load();
        if(!root||!IsWindow(root)||!fullscreenRestore.valid){
            ReportFullscreenFailure(L"缺少可恢复的全屏窗口状态",ERROR_INVALID_STATE,FullscreenFailureState::Unchanged);return false;
        }
        const auto saved=fullscreenRestore;
        fullscreenTransition=FullscreenTransition::Exiting;
        DWORD error=ERROR_SUCCESS;bool exact=false;const bool recovered=RestoreFullscreenSnapshot(root,saved,error,exact);
        if(!recovered){fullscreen=true;fullscreenTransition=FullscreenTransition::RecoveryPending;
            ReportFullscreenFailure(L"无法恢复普通窗口",error,FullscreenFailureState::RecoveryPending);return false;}
        fullscreen=false;fullscreenTransition=FullscreenTransition::Stable;fullscreenRestore={};SetPanel(saved.panelVisible);RequestWindowBoundsFit();
        if(!exact)ReportFullscreenFailure(L"原窗口位置未能完整恢复",error,FullscreenFailureState::Recovered);
        return true;
    }
    bool ToggleFullscreen(bool userSelected=true) {
        NoteActivity();
        if(fullscreenTransition==FullscreenTransition::Entering||fullscreenTransition==FullscreenTransition::Exiting){
            ReportFullscreenFailure(L"窗口模式正在切换",ERROR_BUSY,FullscreenFailureState::Unchanged);return false;
        }
        if(userSelected){desiredDesktop=false;desktopRetryAt=0;settings.presentationMode=0;}
        if(desktopActive){desiredDesktop=false;CleanupDesktop();OpenWindow();}
        // Capture the normal/maximized placement after leaving desktop mode
        // and before a possible borderless transition. Persisting this same
        // snapshot keeps a later direct exit from losing the window state.
        if(userSelected){CaptureWindowPlacement();Persist();}
        return (fullscreen||fullscreenRestore.valid||fullscreenTransition==FullscreenTransition::RecoveryPending)?ExitFullscreen():EnterFullscreen();
    }
    bool AddTray() {
        NOTIFYICONDATAW icon{sizeof(icon)};icon.hWnd=window;icon.uID=1;icon.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP;icon.uCallbackMessage=WM_TRAY;icon.hIcon=LoadIconW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDI_MUSIC_STAGE));if(!icon.hIcon)icon.hIcon=LoadIconW(nullptr,IDI_APPLICATION);wcscpy_s(icon.szTip,L"HTune · 音乐视觉伴侣");
        trayAdded=Shell_NotifyIconW(NIM_ADD,&icon)!=FALSE;if(trayAdded){icon.uVersion=NOTIFYICON_VERSION_4;Shell_NotifyIconW(NIM_SETVERSION,&icon);}return trayAdded;
    }
    void RemoveTray(){if(trayAdded&&window){NOTIFYICONDATAW icon{sizeof(icon)};icon.hWnd=window;icon.uID=1;Shell_NotifyIconW(NIM_DELETE,&icon);}trayAdded=false;}
    void OpenWindow(){if(!window)return;const int show=IsIconic(window)?SW_RESTORE:
            !IsWindowVisible(window)&&settings.windowMaximized?SW_SHOWMAXIMIZED:SW_SHOW;
        ShowWindow(window,show);RequestWindowBoundsFit();SetForegroundWindow(window);
        if(controlsReady)RefreshVisualMediaDemand();}
    void TrayMenu() {
        ModalScope modal(*this);
        HMENU menu=CreatePopupMenu();AppendMenuW(menu,MF_STRING,ID_OPEN,L"打开设置");AppendMenuW(menu,MF_STRING,ID_WINDOW,L"窗口模式");AppendMenuW(menu,MF_STRING,ID_DESKTOP,L"桌面模式");AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING,ID_EXIT,L"退出 HTune");POINT point{};GetCursorPos(&point);SetForegroundWindow(window);const int selected=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,0,window,nullptr);DestroyMenu(menu);PostMessageW(window,WM_NULL,0,0);if(selected)PostMessageW(window,WM_COMMAND,selected,0);
    }
    void CleanupDesktop() {
        if(frameDrawing){desktopCleanupDeferred=true;return;}
        desktopCleanupDeferred=false;if(framePermit.owner==desktopRenderer.get())framePermit={};
        desktopResizePending=false;desktopRecoveryPending=false;desktopRenderFailures=0;desktopRenderRetryAt=0;
        closingDesktop=true;desktopActive=false;desktopRenderer.reset();std::wstring diagnostic;const bool restored=desktopHost.Detach(&diagnostic);if(!diagnostic.empty())log.Event(restored?"desktop-detach":"desktop-detach-incomplete",Utf8(diagnostic));
        auto old=desktopWindow;if(old&&IsWindow(old)){ShowWindow(old,SW_HIDE);DestroyWindow(old);}desktopWindow=nullptr;desktopHost.Detach();closingDesktop=false;
    }
    void FallbackToWindowAfterDesktopFailure(std::wstring notice) {
        // Transient host recovery keeps desiredDesktop and its saved preference.
        // Only the terminal fallback reaches here and commits window mode.
        desiredDesktop=false;desktopRetryAt=0;desktopRetries=0;CleanupDesktop();
        settings.presentationMode=0;uiNotice=std::move(notice);OpenWindow();Persist();
    }
    void FinalizeShutdown(bool mayReadWindow) noexcept {
        if(shutdownFinalized||shutdownFinalizing)return;
        shutdownFinalizing=true;
        bool complete=true;
        const auto attempt=[&](auto&& action) noexcept {try{action();}catch(...){complete=false;}};
        if(mayReadWindow&&controlsReady){attempt([&]{CaptureSettingsValues();});attempt([&]{CaptureWindowPlacement();});}
        exiting=true;UpdateModalFrameTimer();framePermit={};
        attempt([&]{std::lock_guard lock(data.mutex);data.generationStop.request_stop();});
        // Submit both in-memory snapshots before any potentially blocking
        // worker join. The WM_DESTROY fallback skips all HWND reads.
        if(controlsReady)attempt([&]{Persist(false);});
        attempt([&]{FlushTrackPreferences(true);});
        desiredDesktop=false;
        attempt([&]{CleanupDesktop();});attempt([&]{RemoveTray();});attempt([&]{background.Stop();});
        attempt([&]{monitor.Stop();});attempt([&]{capture.Stop();});
        // Workers can still finish an offset/import association while they
        // unwind, so submit one more preference snapshot after they stop.
        attempt([&]{StopJobWorkers();});attempt([&]{FlushTrackPreferences(true);});
        attempt([&]{settingsWriter.Stop();});attempt([&]{PollSettingsSave(false);});attempt([&]{PollTrackPreferenceSave(false);});
        FlushLyricCommitTraces(true);
        if(complete){
            log.Event("application-stop-completed","Final persistence handoff finished; background jobs and callbacks stopped");
            log.Stop();shutdownFinalized=true;
        }else log.Event("application-stop-incomplete","A shutdown step failed; WM_DESTROY or the destructor will retry idempotent cleanup");
        shutdownFinalizing=false;
    }
    bool EnterDesktop(bool userSelected=true) {
        if(desktopActive){if(userSelected)ShowWindow(window,SW_HIDE);return true;}
        desktopPresentationReports=0;desktopPresentationReportAt=0;
        if(userSelected){desiredDesktop=true;desktopRetries=0;desktopRetryAt=0;}
        if(fullscreen&&!ToggleFullscreen(false)){if(userSelected)desiredDesktop=false;return false;}if(!trayAdded&&!AddTray()){if(userSelected)desiredDesktop=false;OpenWindow();uiNotice=L"系统托盘暂时不可用，请使用窗口模式";return false;}
        if(userSelected)CaptureWindowPlacement();
        std::wstring diagnostic;auto parentDpi=desktopHost.Prepare(diagnostic);if(!parentDpi){uiNotice=diagnostic;log.Event("desktop-unavailable",Utf8(diagnostic));if(userSelected)desiredDesktop=false;return false;}
        HMONITOR monitorTarget=MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST);MONITORINFO mi{sizeof(mi)};if(!GetMonitorInfoW(monitorTarget,&mi)){uiNotice=L"无法读取显示器，请使用窗口模式";if(userSelected)desiredDesktop=false;return false;}
        auto previousDpi=SetThreadDpiAwarenessContext(parentDpi);if(!previousDpi){uiNotice=L"无法匹配桌面DPI，请使用窗口模式";if(userSelected)desiredDesktop=false;return false;}
        desktopWindow=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"MusicStageDesktopStage",L"HTune 桌面舞台",WS_POPUP,mi.rcMonitor.left,mi.rcMonitor.top,mi.rcMonitor.right-mi.rcMonitor.left,mi.rcMonitor.bottom-mi.rcMonitor.top,nullptr,nullptr,GetModuleHandleW(nullptr),this);SetThreadDpiAwarenessContext(previousDpi);
        try{if(!desktopWindow)throw std::runtime_error("Desktop stage window creation failed");if(!desktopHost.Attach(desktopWindow,monitorTarget,diagnostic)){uiNotice=diagnostic;throw std::runtime_error("Desktop host attachment failed");}desktopRenderer=std::make_unique<SceneRenderer>();desktopRenderer->Create(desktopWindow,true);desktopResizePending=false;desktopRecoveryPending=false;desktopRenderFailures=0;desktopRenderRetryAt=0;desktopActive=true;desktopPollAt=MonotonicMs()+1000;uiNotice=L"桌面模式已开启，可从系统托盘打开设置";log.Event("desktop-attached",Utf8(diagnostic));ShowWindow(window,SW_HIDE);if(controlsReady)RefreshVisualMediaDemand();if(userSelected){settings.presentationMode=1;Persist();}return true;}
        catch(const std::exception& e){log.Event("desktop-create-error",e.what());CleanupDesktop();if(userSelected){desiredDesktop=false;OpenWindow();}return false;}
    }
    void OnStageLost(const std::wstring& diagnostic=L"桌面宿主已改变，正在恢复") {
        if(closingDesktop||exiting)return;log.Event("desktop-stage-lost",Utf8(diagnostic));CleanupDesktop();uiNotice=diagnostic;if(desiredDesktop){desktopRetries=0;desktopRetryAt=MonotonicMs()+1000;if(controlsReady)RefreshVisualMediaDemand();}else OpenWindow();
    }
    void WindowMode(){desiredDesktop=false;desktopRetryAt=0;CleanupDesktop();OpenWindow();if(fullscreen&&!ToggleFullscreen(false))return;CaptureWindowPlacement();settings.presentationMode=0;Persist();uiNotice.clear();}
    bool MainDrawable() const {
        const auto root=window.load();return root&&IsWindow(root)&&IsWindowVisible(root)&&!IsIconic(root);
    }
    bool DesktopDrawable() const {
        return desktopActive&&desktopWindow&&IsWindow(desktopWindow)&&IsWindowVisible(desktopWindow)&&desktopRenderer;
    }
    bool HasDrawableSurface() const {return MainDrawable()||DesktopDrawable();}
    bool HasVisualDemand() const {
        return (MainDrawable()&&!HostSuppressed(false)&&!mainRecoveryPending&&!renderer.Occluded())||
            (DesktopDrawable()&&!HostSuppressed(true)&&!desktopRecoveryPending&&!desktopRenderer->Occluded());
    }
    bool HasPresentableStage() const {
        return (MainDrawable()&&!HostSuppressed(false))||(DesktopDrawable()&&!HostSuppressed(true));
    }
    static bool ShellSurface(HWND candidate) {
        wchar_t name[64]{};
        if(!GetClassNameW(candidate,name,static_cast<int>(std::size(name))))return false;
        return !wcscmp(name,L"Progman")||!wcscmp(name,L"WorkerW")||!wcscmp(name,L"Shell_TrayWnd")||!wcscmp(name,L"Shell_SecondaryTrayWnd");
    }
    // One foreground sample per Tick. Our own windows, shell surfaces and
    // cloaked, minimized, click-through or see-through windows never cover.
    static ForegroundCover SampleForegroundCover() {
        ForegroundCover result;
        const HWND foreground=GetForegroundWindow();DWORD owner=0;
        if(!foreground||!GetWindowThreadProcessId(foreground,&owner)||!owner||owner==GetCurrentProcessId())return result;
        if(!IsWindowVisible(foreground)||IsIconic(foreground)||ShellSurface(foreground))return result;
        BOOL cloaked=FALSE;
        if(SUCCEEDED(DwmGetWindowAttribute(foreground,DWMWA_CLOAKED,&cloaked,sizeof(cloaked)))&&cloaked)return result;
        const auto exStyle=GetWindowLongPtrW(foreground,GWL_EXSTYLE);
        if(exStyle&WS_EX_TRANSPARENT)return result;
        if(exStyle&WS_EX_LAYERED){
            COLORREF key=0;BYTE alpha=255;DWORD flags=0;
            if(!GetLayeredWindowAttributes(foreground,&key,&alpha,&flags)||(flags&LWA_COLORKEY)||((flags&LWA_ALPHA)&&alpha<250))return result;
        }
        const auto monitor=MonitorFromWindow(foreground,MONITOR_DEFAULTTONULL);
        MONITORINFO info{sizeof(info)};RECT bounds{};
        if(!monitor||!GetMonitorInfoW(monitor,&info))return result;
        if(FAILED(DwmGetWindowAttribute(foreground,DWMWA_EXTENDED_FRAME_BOUNDS,&bounds,sizeof(bounds)))&&!GetWindowRect(foreground,&bounds))return result;
        result.monitor=monitor;
        result.fullscreen=bounds.left<=info.rcMonitor.left&&bounds.top<=info.rcMonitor.top&&
            bounds.right>=info.rcMonitor.right&&bounds.bottom>=info.rcMonitor.bottom;
        QUERY_USER_NOTIFICATION_STATE state{};
        if(!result.fullscreen&&SUCCEEDED(SHQueryUserNotificationState(&state))&&
            (state==QUNS_RUNNING_D3D_FULL_SCREEN||state==QUNS_PRESENTATION_MODE))result.fullscreen=true;
        result.covers=result.fullscreen||IsZoomed(foreground);
        return result;
    }
    HMONITOR HostMonitor(bool desktop) const {
        const HWND host=desktop?desktopWindow:window.load();
        return host&&IsWindow(host)?MonitorFromWindow(host,MONITOR_DEFAULTTONEAREST):nullptr;
    }
    // Hidden behind a fullscreen or maximized foreground window on its own
    // monitor: the stage draws nothing and audio/video analysis pauses.
    bool HostSuppressed(bool desktop) const {
        return settings.pauseForFullscreenApps&&foregroundCover.covers&&foregroundCover.monitor&&
            foregroundCover.monitor==HostMonitor(desktop);
    }
    // Still visible on another monitor while a fullscreen application runs.
    bool HostLite(bool desktop) const {
        return settings.pauseForFullscreenApps&&foregroundCover.fullscreen&&!HostSuppressed(desktop);
    }
    void PollForegroundCover() {
        const auto sampled=SampleForegroundCover();
        if(sampled==foregroundCover)return;
        const bool before=HostSuppressed(false)||HostSuppressed(true);
        foregroundCover=sampled;
        const bool after=HostSuppressed(false)||HostSuppressed(true);
        if(before!=after)log.Event(after?"stage-suspended":"stage-resumed",
            foregroundCover.fullscreen?"Foreground fullscreen application":foregroundCover.covers?"Foreground maximized window":"Foreground no longer covers a stage");
        RefreshVisualMediaDemand();
    }
    void PollPower() {
        const auto now=MonotonicMs();if(powerPollAt&&now<powerPollAt)return;powerPollAt=now+5000;
        SYSTEM_POWER_STATUS power{};onBattery=GetSystemPowerStatus(&power)&&power.ACLineStatus==0;
    }
    bool NestedLoopActive() const {return modalDepth||sizingLoop||menuLoop;}
    void UpdateModalFrameTimer() noexcept {
        const auto root=window.load();
        const bool active=NestedLoopActive()&&controlsReady&&!exiting&&root&&IsWindow(root);
        if(active&&modalTimerWindow!=root){
            if(modalTimerWindow&&IsWindow(modalTimerWindow))KillTimer(modalTimerWindow,TimerModalFrame);
            modalTimerWindow=SetTimer(root,TimerModalFrame,16,nullptr)?root:nullptr;
            if(!modalTimerWindow){try{log.Event("modal-frame-timer-unavailable","Win32 error="+std::to_string(GetLastError()));}catch(...){}}
        }else if(!active&&modalTimerWindow){
            if(IsWindow(modalTimerWindow))KillTimer(modalTimerWindow,TimerModalFrame);modalTimerWindow=nullptr;
        }
    }
    FramePermit FrameWaitTarget() const {
        const SceneRenderer* target=nullptr;
        if(MainDrawable()&&!HostSuppressed(false)&&!mainRecoveryPending&&!renderer.Occluded())target=&renderer;
        else if(DesktopDrawable()&&!HostSuppressed(true)&&!desktopRecoveryPending&&!desktopRenderer->Occluded())target=desktopRenderer.get();
        return target&&target->FrameEvent()?FramePermit{target,target->FrameEvent(),target->FrameGeneration()}:FramePermit{};
    }
    bool HasFramePermit(const FramePermit& expected) const {
        return expected.handle&&framePermit.owner==expected.owner&&framePermit.handle==expected.handle&&framePermit.generation==expected.generation;
    }
    void GrantFramePermit(const FramePermit& permit) {
        // Visibility can change while Windows dispatches sent messages from a
        // wait. Keep the acquired slot for its live chain until it is used.
        const auto matches=[&](const SceneRenderer& target){return permit.owner==&target&&permit.handle==target.FrameEvent()&&permit.generation==target.FrameGeneration();};
        if(permit.handle&&(matches(renderer)||(desktopRenderer&&matches(*desktopRenderer))))framePermit=permit;
    }
    bool TakeFramePermit(SceneRenderer& target) {
        const auto handle=target.FrameEvent();if(!handle)return true;
        const FramePermit expected{&target,handle,target.FrameGeneration()};
        if(HasFramePermit(expected)){framePermit={};return true;}
        if(framePermit.owner==&target)framePermit={};
        const auto ready=WaitForSingleObject(handle,0);
        if(ready==WAIT_FAILED)FrameWaitFailed(handle,GetLastError());
        return ready==WAIT_OBJECT_0;
    }
    DWORD FrameWaitTimeout(HANDLE handle) const {
        if(handle)return 1000;
        if(DesktopDrawable()&&!HostSuppressed(true)&&!desktopRecoveryPending&&!desktopRenderer->Occluded())return desktopRenderer->CompositionPaced()?0:10;
        if((MainDrawable()&&mainRecoveryPending)||(DesktopDrawable()&&desktopRecoveryPending))return 100;
        return 250;
    }
    void FrameWaitFailed(HANDLE handle,DWORD error) {
        if(!handle)return;
        if(framePermit.handle==handle)framePermit={};
        if(handle==renderer.FrameEvent()){mainRecoveryPending=true;mainRenderRetryAt=0;}
        else if(desktopRenderer&&handle==desktopRenderer->FrameEvent()){desktopRecoveryPending=true;desktopRenderRetryAt=0;}
        log.Event("frame-wait-failed","Win32 error="+std::to_string(error));
    }
    void PollSceneVisibility() {
        const auto now=MonotonicMs();if(now<renderVisibilityPollAt)return;renderVisibilityPollAt=now+250;
        auto refresh=[&](SceneRenderer& target,bool desktop,bool& pending,int& failures,int64_t& retryAt){
            if(pending||!target.Occluded())return;
            try{target.RefreshVisibility();}
            catch(const std::exception& e){++failures;pending=true;retryAt=now+std::min(5000,failures*500);
                log.Event(desktop?"desktop-visibility-recovery":"window-visibility-recovery",e.what());}
        };
        if(MainDrawable())refresh(renderer,false,mainRecoveryPending,mainRenderFailures,mainRenderRetryAt);
        if(DesktopDrawable())refresh(*desktopRenderer,true,desktopRecoveryPending,desktopRenderFailures,desktopRenderRetryAt);
    }
    void RequestSceneResize(bool desktop) {
        if(desktop)desktopResizePending=true;else mainResizePending=true;
    }
    void ApplySceneResizes() {
        auto resize=[&](SceneRenderer& target,bool desktop,bool& requested,bool& recovery,int& failures,int64_t& retryAt){
            if(!requested||recovery)return;requested=false;
            try{target.Resize();}
            catch(const std::exception& e){++failures;recovery=true;retryAt=MonotonicMs()+std::min(5000,failures*500);
                if(framePermit.owner==&target)framePermit={};log.Event(desktop?"desktop-resize-recovery":"window-resize-recovery",e.what());}
        };
        if(MainDrawable())resize(renderer,false,mainResizePending,mainRecoveryPending,mainRenderFailures,mainRenderRetryAt);
        if(DesktopDrawable())resize(*desktopRenderer,true,desktopResizePending,desktopRecoveryPending,desktopRenderFailures,desktopRenderRetryAt);
    }
    void PumpFrames(bool nested=false) {
        // Activation re-samples the foreground at once instead of on the next Tick.
        if(foregroundRecheck&&!frameDrawing&&!frameWaiting&&!exiting&&controlsReady){foregroundRecheck=false;PollForegroundCover();}
        if(frameDrawing||frameWaiting||exiting||!controlsReady||!HasDrawableSurface())return;
        const bool visualDemandBefore=HasVisualDemand();
        {
            struct DrawingScope {bool& active;explicit DrawingScope(bool& value):active(value){active=true;}~DrawingScope(){active=false;}} drawingScope(frameDrawing);
            [&]{
                if(!HasPresentableStage())return; // Covered stages draw nothing; deferred work below still runs.
                PollSceneVisibility();ApplySceneResizes();const auto target=FrameWaitTarget();
                if(target.handle&&!HasFramePermit(target)){
                    const auto ready=WaitForSingleObject(target.handle,0);
                    if(ready==WAIT_OBJECT_0)GrantFramePermit(target);
                    else if(ready==WAIT_FAILED)FrameWaitFailed(target.handle,GetLastError());
                    else return;
                }
                DrawFrames(!nested);
            }();
        }
        if(HasVisualDemand()!=visualDemandBefore)RefreshVisualMediaDemand();
        if(desktopCleanupDeferred)CleanupDesktop();
        if(closeDeferred){closeDeferred=false;if(window)PostMessageW(window,WM_CLOSE,0,0);}
        if(tickDeferred&&!exiting){tickDeferred=false;Tick();}
    }
    void DrawFrames(bool allowBlockingPace=true) {
        RefreshTrackPreferences();UpdateLyricBrowsing();UpdateAssociationControl();EnsurePaneFocusVisible();UpdateFullscreenChrome();
        if(paintedTheme!=theme||paintedCustomTheme!=customTheme.get()){
            paintedTheme=theme;paintedCustomTheme=customTheme.get();RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN);
        }
        SceneInput scene;int sceneOffset=offset;bool browseFrame=false;
        {std::lock_guard lock(data.mutex);scene.playback=data.playback;scene.audio=data.audio;scene.lyrics=data.lyrics;scene.lyricStatus=data.networkStatus;scene.coverError=data.coverError;scene.lyricRevision=lyricRevision.load();scene.lyricReadingRevision=lyricReadingRevision.load();
            sceneOffset=CurrentOffsetLocked();
            browseFrame=manualLyricBrowse&&lyricBrowseGeneration==data.generation&&lyricBrowseReadingRevision==scene.lyricReadingRevision;
            scene.lyricAssociation=LyricAssociationLocked();scene.lyricVersionMismatch=LyricVersionMismatchLocked();
            scene.lyricRefreshPending=data.lyricRefreshPending&&data.busy&&data.busyGeneration==data.generation;
            scene.manualLyricPreview=ManualPreviewLocked();scene.lyricPreviewTitle=data.manualTitle;scene.lyricPreviewArtist=data.manualArtist;
        }
        scene.background=DisplayedBackground(scene.playback,scene.manualLyricPreview);
        // Take the frame clock after every producer snapshot. A callback may
        // publish while these copies are made; the frame must never predate
        // the playback, audio, or background state it is about to consume.
        const auto now=MonotonicMs();const auto palette=themeTransition.Update(PaletteOf(CurrentTheme()),now,settings.reducedMotion);
        if(!displayedPalette||!SamePalette(*displayedPalette,palette)){
            displayedPalette=palette;RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN);
        }
        nativeControls.SetTheme(DisplayedTheme());nativeControls.Advance(now);scene.palette=palette;
        scene.view={};scene.view.lyricFont=settings.lyricFont;scene.view.lyricFontPath=settings.lyricFontPath;scene.view.theme=theme;scene.view.offsetMs=sceneOffset;scene.view.response=strength;scene.view.lyricSize=settings.lyricSize;
        scene.view.previewIndex=previewIndex;scene.view.panelVisible=panelVisible;scene.view.fullscreen=fullscreen;
        scene.view.reducedMotion=settings.reducedMotion;scene.view.diagnostics=showDiagnostics;scene.view.customTheme=customTheme;scene.view.coverBackground=backgroundSelection==0;
        scene.view.showTranslation=settings.showTranslation;scene.view.showRomanization=settings.showRomanization;
        scene.view.lyricDisplayMode=settings.lyricDisplayMode;scene.view.quietFullscreen=quietFullscreen;
        scene.view.motionVariant=settings.motionVariant;scene.view.motionFx=settings.motionFx;scene.view.motionCamera=settings.motionCamera;scene.view.motionLines=settings.motionLines;scene.view.motionBackground=settings.motionBackground;scene.view.motionFilter=settings.motionFilter;
        scene.view.manualLyricBrowse=browseFrame;scene.view.backgroundShade=settings.backgroundShade;scene.view.cameraMotion=settings.cameraMotion;
        scene.view.previewScrollOffset=browseFrame?previewScrollOffset:0.f;
        scene.frameAtMs=now;
        scene.view.qualityMode=settings.qualityMode;scene.view.frameRateMode=settings.frameRateMode;scene.view.onBattery=onBattery;scene.view.interactionAtMs=std::max(interactionAt,uiAnimatingAt);
        const auto stageUiList=MainDrawable()&&!HostSuppressed(false)?BuildStageUi(now,palette):std::shared_ptr<const ui::DrawList>{};
        if(scene.lyrics&&!scene.lyrics->lines.empty()&&!browseFrame){
            const auto& lines=scene.lyrics->lines;
            if(presentedLyricValid&&presentedLyricReadingRevision==scene.lyricReadingRevision){
                scene.view.previewIndex=std::min(presentedLyricIndex,lines.size()-1);
                scene.view.previewScrollOffset=presentedLyricViewport.scrollOffset;
            }
            else if(shownLyricReadingRevision!=scene.lyricReadingRevision)scene.view.previewIndex=InitialLyricLine(*scene.lyrics);
            // This is only a static row fallback. The renderer still owns its
            // timeline freshness checks and never advances an invalid clock.
        }
        std::optional<PresentedLyricPosition> frameLyricIndex;bool framePresented=false,frameLyricDesktop=false;
        uint64_t frameLyricRendererGeneration=0;
        auto draw=[&](SceneRenderer& target,HWND surface,bool desktop,bool pace,bool& pending,int& failures,int64_t& retryAt)->bool{
            if(now<retryAt)return false;
            try{
                if(pending){target.Create(surface,desktop,failures>1);pending=false;retryAt=0;
                    if(desktop)desktopResizePending=false;else mainResizePending=false;}
                if(target.Occluded())return false;
                // Permission is consumed before any API that can reenter the
                // UI. Renderer generation also rejects reused HANDLE values.
                if(!TakeFramePermit(target))return false;
                scene.view.desktop=desktop;scene.view.panelVisible=desktop?false:panelVisible;scene.view.lite=HostLite(desktop);
                scene.view.backstage=!desktop&&stageUi&&interfaceState.backstage;scene.ui=desktop?nullptr:stageUiList;
                scene.view.consoleShown=!desktop&&stageUi&&stageInterface.ConsoleShown();
                const float pointerScale=WindowDpi(window)/96.f;
                scene.view.pointerActive=!desktop&&uiMouseTracked&&lastClientPointer.x>=0&&lastClientPointer.y>=0;
                scene.view.pointerX=lastClientPointer.x/pointerScale;scene.view.pointerY=lastClientPointer.y/pointerScale;
                std::optional<PresentedLyricPosition> selectedLine;
                const bool presented=target.Draw(scene,pace,&selectedLine);
                if(presented&&!framePresented){framePresented=true;frameLyricIndex=selectedLine;
                    frameLyricDesktop=desktop;frameLyricRendererGeneration=target.FrameGeneration();}
                failures=0;retryAt=0;return presented;
            }
            catch(const std::exception& e){++failures;log.Event(desktop?"desktop-scene-recovery":"window-scene-recovery",std::string(e.what())+" removedReason="+std::to_string((unsigned long)target.DeviceRemovedReason()));pending=true;retryAt=now+std::min(5000,failures*500);return false;}
        };
        const bool mainPresented=MainDrawable()&&!HostSuppressed(false)&&draw(renderer,window,false,true,mainRecoveryPending,mainRenderFailures,mainRenderRetryAt);
        if(DesktopDrawable()&&!HostSuppressed(true)){draw(*desktopRenderer,desktopWindow,true,allowBlockingPace&&!mainPresented,desktopRecoveryPending,desktopRenderFailures,desktopRenderRetryAt);if(desktopRenderFailures>=3)FallbackToWindowAfterDesktopFailure(L"显示设备暂时不可用，已恢复窗口画面");}
        if(framePresented){
            presentedLyricRevision=scene.lyricRevision;presentedLyricReadingRevision=scene.lyricReadingRevision;presentedLyricValid=frameLyricIndex.has_value();
            presentedLyricDesktop=frameLyricDesktop;presentedLyricRendererGeneration=frameLyricRendererGeneration;
            if(frameLyricIndex){presentedLyricIndex=frameLyricIndex->lineIndex;presentedLyricViewport=*frameLyricIndex;}
        }
    }
    // ---- Stage UI: view model, frame build, actions, input -------------
    static float PaletteLuma(const ThemeColor& c) {return .2126f*c.r+.7152f*c.g+.0722f*c.b;}
    static ui::Color UiColor(const ThemeColor& c) {return {c.r,c.g,c.b,1.f};}
    // Immutable snapshot for the stage UI. The light part (console, pills)
    // is built every frame; Backstage pages add the rest while visible.
    SettingsView BuildSettingsView(bool full) {
        SettingsView v;
        PlaybackSnapshot p;AudioFeatures a;bool manualPreview=false;std::wstring manualTitle,manualArtist,coverStatus;
        bool independent=false,busy=false,lookup=false,searchable=false,manualQuery=false,hasTrack=false,writable=true,pending=false;
        std::wstring preferenceDiagnostic;
        {
            std::lock_guard lock(data.mutex);
            p=data.playback;a=data.audio;manualPreview=ManualPreviewLocked();manualTitle=data.manualTitle;manualArtist=data.manualArtist;
            if(full){
                independent=manualPreview;busy=data.busy;lookup=HasLyricLookupIdentity(data.playback);searchable=HasSearchableSongText(data.playback);
                manualQuery=data.manualQueryActive;hasTrack=!CurrentPreferenceKeyLocked().empty();coverStatus=data.coverStatus;
                writable=data.trackPreferences.writable;pending=data.preferencesDirty||data.preferenceSaveInFlight;preferenceDiagnostic=data.preferenceSaveDiagnostic;
            }
        }
        // Title lines exactly as the stage names them.
        const bool verifiedIdOnly=!p.metadataStale&&p.songIdVerified&&p.title.empty()&&SameCatalogId(p.songId,p.songId);
        v.title=DisplayText(manualPreview?(manualTitle.empty()?L"手动查找歌词":manualTitle):p.identityReacquiring?L"正在重新确认歌曲":
            p.metadataStale?L"正在识别歌曲":verifiedIdOnly?L"歌曲 ID "+p.songId:p.title.empty()?L"打开网易云、QQ 音乐、酷狗或汽水音乐，播放一首歌":p.title);
        v.artist=DisplayText(manualPreview?(manualArtist.empty()?L"手动歌词预览":manualArtist):p.metadataStale?(p.title.empty()?L"等待歌曲信息":L"此前歌曲："+p.title):
            verifiedIdOnly?L"歌曲资料加载中":p.artist.empty()?L"等待歌曲信息":p.artist);
        const auto now=MonotonicMs();const auto audioAt=a.timingTrusted?a.sampledAtMs:a.observedAtMs;
        const bool currentAudio=a.active&&!a.connecting&&p.pid&&a.pid==p.pid&&audioAt>0&&now>=audioAt&&now-audioAt<AudioResponseFreshnessMs;
        v.audioLevel=currentAudio&&std::isfinite(a.peak)?std::clamp(a.peak,0.f,1.f):0.f; // presentation only
        v.fullscreen=fullscreen;v.desktop=desktopActive;v.compactLayout=CompactStageControls();
        v.response=strength;v.showTranslation=settings.showTranslation;v.lyricDisplayMode=settings.lyricDisplayMode;
        v.themeStyle=std::clamp(customTheme?customTheme->baseStyle:theme,0,BuiltInStyleCount-1);v.customTheme=customTheme!=nullptr;
        v.resumeLyrics=lyricReturnShown;v.resumeEnabled=lyricReturnEnabled;v.matchLyrics=associationNeeded;v.lyricMismatch=associationMismatchShown;
        v.notice=DisplayText(uiNotice);
        if(!full)return v;
        v.backgroundKind=std::clamp(backgroundSelection.load(),0,2);
        v.coverStatus=DisplayText(coverStatus.empty()?L"等待歌曲信息":coverStatus);
        {const auto current=DisplayedBackground(p,manualPreview);BackgroundNoteState(current,v.backgroundRetry,v.backgroundNotice);}
        v.motionVariant=settings.motionVariant;v.motionFx=settings.motionFx;v.motionCamera=settings.motionCamera;v.motionLines=settings.motionLines;v.motionBackground=settings.motionBackground;v.motionFilter=settings.motionFilter;
        v.lyricFont=settings.lyricFont;v.lyricFontPath=settings.lyricFontPath;v.lyricFontNotice=lyricFontNotice;
        v.lyricSize=settings.lyricSize;v.backgroundShade=settings.backgroundShade;v.cameraMotion=settings.cameraMotion;v.reducedMotion=settings.reducedMotion;
        v.qualityMode=settings.qualityMode;v.frameRateMode=settings.frameRateMode;v.pauseForFullscreenApps=settings.pauseForFullscreenApps;
        v.themeRestorePending=themeRestorePending;v.customThemeName=customTheme?DisplayText(customTheme->name):std::wstring{};
        v.themeNotice=themeRestorePending?ThemeRestoreNoteText():std::wstring{};
        v.independentPreview=independent;
        v.canFollow=(!busy||independent)&&lookup;v.canSearch=v.canFollow&&searchable;
        v.canImport=!busy&&!manualQuery&&lookup;v.hasTrack=hasTrack;
        if(!candidateList.updating&&candidateList.generation!=UINT64_MAX){
            v.candidateGeneration=candidateList.generation;v.selectedCandidate=candidateList.selected;
            v.candidates.reserve(candidateList.rows.size());
            for(const auto& c:candidateList.rows)
                v.candidates.push_back({c.id,DisplayText(c.title),DisplayText(c.artist),DisplayText(c.album),c.durationMs,c.clientRecordCandidate});
        }
        CandidateFetchState(v.canFetch,v.candidateDetails);
        v.offsetMs=offset;v.showRomanization=settings.showRomanization;
        {const auto note=TrackPreferenceNoteState(writable,pending,preferenceDiagnostic);
            v.lyricMemoryActionable=note.actionable;v.lyricMemoryButton=note.button;v.lyricMemoryNote=note.note;}
        v.diagnostics=shownStatusText;v.showDiagnostics=showDiagnostics;v.nativeClock=settings.nativeClockEnabled;v.fullscreenAutoHide=settings.fullscreenAutoHide;
        {const auto note=SettingsNoteState();v.settingsActionable=note.actionable;v.settingsButton=note.button;v.settingsNote=note.note;}
        v.usageHelp=UsageHelpText;
        return v;
    }
    // One stage UI frame over the main window: input since the last frame is
    // consumed here; resulting actions run later from the message loop.
    std::shared_ptr<const ui::DrawList> BuildStageUi(int64_t now,const ThemePalette& palette) {
        const auto root=window.load();RECT client{};
        if(!stageUi||!root||!GetClientRect(root,&client)||client.right<1||client.bottom<1)return {};
        const float scale=WindowDpi(root)/96.f;
        ui::FrameInfo info;info.width=client.right/scale;info.height=client.bottom/scale;info.nowMs=now;
        info.reducedMotion=settings.reducedMotion;info.window=root;
        InterfaceFrame frame;frame.width=info.width;frame.height=info.height;frame.nowMs=now;frame.reducedMotion=settings.reducedMotion;
        frame.windowChrome=HasWindowChrome(root);frame.maximized=IsZoomed(root)!=FALSE;frame.windowActive=GetForegroundWindow()==root;
        frame.style=customTheme?customTheme->baseStyle:theme;
        frame.pointerX=lastClientPointer.x/scale;frame.pointerY=lastClientPointer.y/scale;
        frame.pointerActive=uiMouseTracked&&lastClientPointer.x>=0&&lastClientPointer.y>=0;
        frame.lightStage=PaletteLuma(palette.background)>.45f;
        frame.ink=UiColor(palette.ink);frame.muted=UiColor(palette.muted);frame.accent=UiColor(palette.accent);
        POINT cursor{};bool pointerOver=false;
        if(GetCursorPos(&cursor)&&WindowFromPoint(cursor)==root&&ScreenToClient(root,&cursor))pointerOver=stageUi->HitTest(cursor.x/scale,cursor.y/scale);
        frame.console=!quietFullscreen&&(now-consoleActivityAt<2600||pointerOver||stageUi->WantsKeyboard()||stageUi->PointerCaptured());
        const auto view=BuildSettingsView(interfaceState.backstage||stageInterface.BackstageVisible());
        std::vector<UiAction> actions;
        stageUi->Begin(info);stageInterface.Build(*stageUi,view,interfaceState,frame,actions);stageUi->End();
        if(stageUi->Animating())uiAnimatingAt=now;
        QueueUiActions(actions);UpdateImeAssociation();
        if(stageUi->Output().Empty())return {};
        return std::make_shared<ui::DrawList>(stageUi->Output());
    }
    void QueueUiActions(std::vector<UiAction>& actions) {
        if(actions.empty())return;
        for(auto& action:actions)pendingUiActions.push_back(std::move(action));
        if(pendingUiActions.size()>64)pendingUiActions.erase(pendingUiActions.begin(),pendingUiActions.end()-64);
        const auto root=window.load();
        if(!uiActionsPosted&&root)uiActionsPosted=PostMessageW(root,WM_UI_ACTIONS,0,0)!=FALSE;
    }
    // Runs from the message loop like WM_COMMAND, so file and color dialogs
    // open from a normal dispatch, never from inside a frame.
    void RunUiActions() {
        uiActionsPosted=false;
        auto actions=std::move(pendingUiActions);pendingUiActions.clear();
        for(const auto& action:actions){if(exiting)return;NoteActivity();ExecuteUiAction(action);}
    }
    void ExecuteUiAction(const UiAction& a) {
        const auto clamp=[](float value,float low,float high,float fallback){return std::isfinite(value)?std::clamp(value,low,high):fallback;};
        switch(a.command){
            case UiCommand::MinimizeWindow:if(HasWindowChrome(window))PostMessageW(window,WM_SYSCOMMAND,SC_MINIMIZE,0);break;
            case UiCommand::MaximizeWindow:if(HasWindowChrome(window))PostMessageW(window,WM_SYSCOMMAND,IsZoomed(window)?SC_RESTORE:SC_MAXIMIZE,0);break;
            case UiCommand::CloseWindow:if(HasWindowChrome(window))PostMessageW(window,WM_CLOSE,0,0);break;
            case UiCommand::OpenBackstage:OpenBackstage();break;
            case UiCommand::CloseBackstage:CloseBackstage();break;
            case UiCommand::ToggleFullscreen:ToggleFullscreen();break;
            case UiCommand::EnterDesktop:CloseBackstage();EnterDesktop();break;
            case UiCommand::WindowMode:WindowMode();break;
            case UiCommand::UseCover:UseCover(true);break;
            case UiCommand::ChooseImage:ChooseBackground(false);break;
            case UiCommand::ChooseVideo:ChooseBackground(true);break;
            case UiCommand::RetryBackground:RetryBackground();break;
            case UiCommand::SetResponse:strength=clamp(a.number,0.f,2.f,strength);settingsSaveAt=MonotonicMs()+500;SyncThemeControls();break;
            case UiCommand::SetLyricFont:{const wchar_t* keys[]={L"",L"@wenkai",L"@huangyou",L"@kuaile"};settings.lyricFont=keys[std::clamp(a.value,0,3)];settings.lyricFontPath.clear();lyricFontNotice.clear();Persist();break;}
            case UiCommand::ChooseSystemLyricFont:ChooseLyricFont(false);break;
            case UiCommand::ImportLyricFont:ChooseLyricFont(true);break;
            case UiCommand::SetLyricSize:settings.lyricSize=std::round(clamp(a.number,26.f,80.f,settings.lyricSize));settingsSaveAt=MonotonicMs()+500;SyncThemeControls();break;
            case UiCommand::SetBackgroundShade:settings.backgroundShade=clamp(a.number,0.f,1.f,settings.backgroundShade);settingsSaveAt=MonotonicMs()+500;SyncThemeControls();break;
            case UiCommand::SetCameraMotion:settings.cameraMotion=clamp(a.number,0.f,2.f,settings.cameraMotion);settingsSaveAt=MonotonicMs()+500;SyncThemeControls();break;
            case UiCommand::ResetBackdrop:ResetBackdropAdjustments();break;
            case UiCommand::SetReducedMotion:settings.reducedMotion=a.value!=0;SyncThemeControls();Persist();break;
            case UiCommand::SetQuality:settings.qualityMode=std::clamp(a.value,0,3);SyncThemeControls();Persist();break;
            case UiCommand::SetFrameRate:settings.frameRateMode=std::clamp(a.value,0,2);SyncThemeControls();Persist();break;
            case UiCommand::SetPauseForFullscreen:settings.pauseForFullscreenApps=a.value!=0;SyncThemeControls();Persist();RefreshVisualMediaDemand();break;
            case UiCommand::SetMotionVariant:settings.motionVariant=std::clamp(a.value,-1,255);settingsSaveAt=MonotonicMs()+300;break;
            case UiCommand::SetMotionFx:settings.motionFx=std::clamp(a.value,0,5);settingsSaveAt=MonotonicMs()+300;break;
            case UiCommand::SetMotionCamera:settings.motionCamera=std::clamp(a.value,0,2);settingsSaveAt=MonotonicMs()+300;break;
            case UiCommand::SetMotionLines:settings.motionLines=std::clamp(a.value,1,10);settingsSaveAt=MonotonicMs()+300;break;
            case UiCommand::SetMotionFilter:settings.motionFilter=std::clamp(a.value,0,4);settingsSaveAt=MonotonicMs()+300;break;
            case UiCommand::SetMotionBackground:settings.motionBackground=std::clamp(a.value,0,4);settingsSaveAt=MonotonicMs()+300;break;
            case UiCommand::SelectStyle:SelectTheme(std::clamp(a.value,0,CustomThemeChoice));SyncThemeControls();break;
            case UiCommand::ImportTheme:ChooseTheme(false);break;
            case UiCommand::ExportTheme:ChooseTheme(true);break;
            case UiCommand::ChooseAccent:ChooseAccent();break;
            case UiCommand::RetryTheme:RetrySavedTheme();break;
            case UiCommand::FindLyrics:ManualFindText(a.text);break;
            case UiCommand::FollowCurrent:FollowCurrentLyrics(true);break;
            case UiCommand::SelectCandidate:SelectCandidateIndex(a.value,a.generation);break;
            case UiCommand::FetchCandidate:if(SelectCandidateIndex(a.value,a.generation))Fetch();break;
            case UiCommand::ImportLyrics:Import();break;
            case UiCommand::RestoreOnline:RestoreOnlineLyrics();break;
            case UiCommand::SetOffset:ApplyOffsetValue(a.value);SyncOffsetText();break;
            case UiCommand::ResetOffset:ResetOffset();break;
            case UiCommand::SetTranslation:settings.showTranslation=a.value!=0;SyncThemeControls();Persist();break;
            case UiCommand::SetRomanization:settings.showRomanization=a.value!=0;SyncThemeControls();Persist();break;
            case UiCommand::SetLyricMode:ChangeLyricMode(a.value);break;
            case UiCommand::RepairLyricMemory:RecoverTrackPreferenceSaving();break;
            case UiCommand::ResumeLyrics:ResumeLyricFollowing();break;
            case UiCommand::MatchLyrics:OpenLyricMatching();break;
            case UiCommand::SetDiagnostics:showDiagnostics=a.value!=0;SendMessageW(Control(ID_DIAGNOSTICS),BM_SETCHECK,showDiagnostics?BST_CHECKED:BST_UNCHECKED,0);break;
            case UiCommand::SetNativeClock:
                settings.nativeClockEnabled=a.value!=0;monitor.SetClientClockEnabled(settings.nativeClockEnabled);SyncThemeControls();
                uiNotice=settings.nativeClockEnabled?L"原生只读内部时钟已开启；只有严格校验通过才会驱动歌词":L"原生只读内部时钟已关闭；仍保留 Windows 媒体会话与界面观测";Persist();break;
            case UiCommand::SetFullscreenAutoHide:settings.fullscreenAutoHide=a.value!=0;SyncThemeControls();UpdateFullscreenChrome();Persist();break;
            case UiCommand::RepairSettings:RecoverSettingsSaving();break;
            case UiCommand::None:break;
        }
    }
    // Only a focused stage text field takes the IME; the stage itself never
    // starts a composition while browsing lyrics.
    void UpdateImeAssociation() {
        const auto root=window.load();if(!root||!stageUi)return;
        const bool editing=stageUi->EditingText();
        if(editing==uiImeEnabled)return;
        uiImeEnabled=editing;ImmAssociateContextEx(root,nullptr,editing?IACE_DEFAULT:0);
    }
    void PlaceImeWindow() {
        ui::Rect caret{};const auto root=window.load();
        if(!stageUi||!root||!stageUi->CaretRect(caret))return;
        HIMC context=ImmGetContext(root);if(!context)return;
        const float s=WindowDpi(root)/96.f;
        COMPOSITIONFORM composition{};composition.dwStyle=CFS_POINT;
        composition.ptCurrentPos={static_cast<LONG>(std::lround(caret.left*s)),static_cast<LONG>(std::lround(caret.top*s))};
        ImmSetCompositionWindow(context,&composition);
        CANDIDATEFORM candidate{};candidate.dwIndex=0;candidate.dwStyle=CFS_CANDIDATEPOS;
        candidate.ptCurrentPos={static_cast<LONG>(std::lround(caret.left*s)),static_cast<LONG>(std::lround(caret.bottom*s))+4};
        ImmSetCandidateWindow(context,&candidate);
        ImmReleaseContext(root,context);
    }
    // Keyboard for the stage UI while the main window itself has focus.
    // F11, Ctrl+, and Ctrl+L stay global, also while a field is edited.
    bool HandleStageUiMessage(MSG& message) {
        const auto root=window.load();
        if(!stageUi||!root||message.hwnd!=root||GetFocus()!=root)return false;
        if(message.message==WM_CHAR){
            if(!stageUi->EditingText())return false;
            stageUi->Char(static_cast<wchar_t>(message.wParam));return true;
        }
        if(message.message!=WM_KEYDOWN)return false;
        const UINT key=static_cast<UINT>(message.wParam);
        const bool ctrl=(GetKeyState(VK_CONTROL)&0x8000)!=0,shift=(GetKeyState(VK_SHIFT)&0x8000)!=0;
        if(key==VK_PROCESSKEY){
            if(!stageUi->EditingText())return false;
            TranslateMessage(&message);DispatchMessageW(&message);return true;
        }
        if(key==VK_F11||(ctrl&&(key==VK_OEM_COMMA||key=='L')))return false;
        const auto now=MonotonicMs();
        if(stageUi->KeyDown(key,ctrl,shift)){
            if(stageUi->EditingText())TranslateMessage(&message); // characters follow as WM_CHAR
            NoteActivity();consoleActivityAt=now;return true;
        }
        if(interfaceState.backstage&&key==VK_ESCAPE){CloseBackstage();return true;}
        if(key==VK_TAB&&!ctrl&&(interfaceState.backstage||stageInterface.Visible())){
            stageUi->FocusFirst();NoteActivity();consoleActivityAt=now;return true;
        }
        return false;
    }
    float ClientScale() const {const auto root=window.load();return root?WindowDpi(root)/96.f:1.f;}
    void RefreshFont() {
        auto replacement=CreateFontW(-MulDiv(14,GetDpiForWindow(window),96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");if(!replacement)return;
        EnumChildWindows(window,[](HWND child,LPARAM value)->BOOL{SendMessageW(child,WM_SETFONT,value,TRUE);return TRUE;},reinterpret_cast<LPARAM>(replacement));if(font)DeleteObject(font);font=replacement;SendMessageW(list,LB_SETITEMHEIGHT,0,MulDiv(48,GetDpiForWindow(window),96));
    }
    void Layout() {
        RECT rc{};GetClientRect(window,&rc);const float s=WindowDpi(window)/96.f;const int rail=(int)std::lround(SettingsRailWidth(rc.right/s)*s);
        const int x=std::max(0,(int)rc.right-rail+(int)(24*s)),w=std::max(1,rail-(int)(48*s));
        const int paneTop=(int)(106*s),paneHeight=std::max((int)(140*s),(int)rc.bottom-paneTop-(int)(124*s));
        constexpr UINT panePositionFlags=SWP_NOZORDER|SWP_NOCOPYBITS;
        SetWindowPos(settingsPane,nullptr,x,paneTop,w,paneHeight,panePositionFlags);
        const int currentPage=std::clamp(page,0,3);paneVisibleHeight=std::max(1,(int)std::floor(paneHeight/s));paneContentHeight=1;
        struct PanePosition {HWND control;int top,height;};
        std::vector<PanePosition> positions;positions.reserve(pageControls.size());
        auto pos=[&](HWND h,int y,int height,int visibleHeight=0){
            if(!h)return;positions.push_back({h,y,height});
            const auto entry=std::find_if(pageControls.begin(),pageControls.end(),[h](const auto& item){return item.first==h;});
            if(entry!=pageControls.end()&&entry->second==currentPage)
                paneContentHeight=std::max(paneContentHeight,y-106+(visibleHeight?visibleHeight:height)+12);
        };
        pos(Control(211),112,38);pos(manualQueryEdit,154,32);pos(Control(ID_MANUALSEARCH),196,36);pos(search,242,36);
        constexpr int lyricActionsShift=136,lyricOffsetTop=640+lyricActionsShift;
        pos(list,288,180);pos(Control(ID_CANDIDATEDETAILSLABEL),482,24);pos(Control(ID_CANDIDATEDETAILS),510,96);
        pos(fetch,482+lyricActionsShift,36);pos(Control(ID_IMPORT),526+lyricActionsShift,36);pos(Control(ID_ONLINELYRICS),568+lyricActionsShift,32);
        pos(Control(201),612+lyricActionsShift,24);pos(offsetEdit,lyricOffsetTop,32);pos(Control(ID_TRANSLATION),682+lyricActionsShift,28);pos(Control(ID_ROMANIZATION),716+lyricActionsShift,28);
        pos(Control(ID_OFFSETRESET),lyricOffsetTop,32);
        const bool compact=CompactStageControls();if(compact!=compactHintShown){compactHintShown=compact;
            SetWindowTextW(Control(208),compact?L"小窗口采用紧凑呈现，放大后恢复偏好":L"歌词呈现方式（Ctrl+L 切换）");}
        pos(Control(208),750+lyricActionsShift,40);pos(lyricModeCombo,802+lyricActionsShift,160,36);
        pos(Control(ID_LYRICMEMORYREPAIR),850+lyricActionsShift,36);pos(Control(ID_LYRICMEMORYNOTE),898+lyricActionsShift,208);
        pos(Control(ID_FULLSCREEN),112,36);pos(Control(ID_DESKTOP),158,36);pos(Control(ID_WINDOW),204,36);pos(Control(ID_IMAGE),258,36);pos(Control(ID_VIDEO),304,36);pos(Control(ID_COVER),350,36);pos(Control(203),400,24);pos(strengthSlider,429,35);pos(Control(205),478,24);pos(sizeSlider,507,35);pos(Control(ID_MOTION),555,30);
        pos(Control(209),606,24);pos(shadeSlider,634,35);pos(Control(210),688,24);pos(cameraSlider,716,35);pos(Control(ID_BACKDROP_RESET),776,36);
        pos(Control(ID_QUALITYLABEL),826,24);pos(qualityCombo,854,160,36);pos(Control(ID_FRAMERATELABEL),902,24);pos(frameRateCombo,930,160,36);
        pos(Control(ID_PAUSEFULLSCREEN),978,30);
        pos(Control(ID_COVERSTATUS),1024,100);
        pos(Control(ID_BACKGROUNDRETRY),1136,36);pos(Control(ID_BACKGROUNDNOTE),1188,240);
        pos(Control(202),112,24);pos(themeCombo,142,200,36);pos(Control(ID_THEMEIMPORT),196,36);pos(Control(ID_THEMEEXPORT),242,36);pos(Control(ID_ACCENT),288,36);pos(Control(206),344,160);pos(Control(ID_DIAGNOSTICS),112,34);pos(Control(ID_CLOCK),150,34);
        // The native edit owns text scrolling. Keep its whole viewport small
        // enough for the existing focus-reveal path to expose it in short panes.
        const int helpHeight=std::clamp(paneVisibleHeight-16,120,370),afterHelp=192+helpHeight+14;
        pos(Control(ID_USAGEHELP),192,helpHeight);pos(Control(ID_AUTOHIDE),afterHelp,30);
        pos(Control(ID_SETTINGSREPAIR),afterHelp+46,36);pos(Control(ID_SETTINGSNOTE),afterHelp+94,208);
        pos(Control(ID_THEMERESTORE),526,36);pos(Control(ID_THEMENOTE),578,208);
        auto& scroll=paneScroll[currentPage];scroll=std::clamp(scroll,0,std::max(0,paneContentHeight-paneVisibleHeight));
        SCROLLINFO info{sizeof(info),SIF_RANGE|SIF_PAGE|SIF_POS|SIF_DISABLENOSCROLL};info.nMin=0;info.nMax=paneContentHeight-1;info.nPage=(UINT)paneVisibleHeight;info.nPos=scroll;SetScrollInfo(settingsPane,SB_VERT,&info,TRUE);
        RECT paneClient{};GetClientRect(settingsPane,&paneClient);const int contentWidth=std::max(1,(int)paneClient.right-(int)(8*s));
        for(const auto& item:positions)SetWindowPos(item.control,nullptr,0,(int)((item.top-106-scroll)*s),contentWidth,(int)(item.height*s),panePositionFlags);
        SetWindowPos(offsetEdit,nullptr,0,(int)((lyricOffsetTop-106-scroll)*s),std::max(1,contentWidth-(int)(90*s)),(int)(32*s),panePositionFlags);
        SetWindowPos(Control(ID_OFFSETRESET),nullptr,contentWidth-(int)(80*s),(int)((lyricOffsetTop-106-scroll)*s),(int)(80*s),(int)(32*s),panePositionFlags);
        SetWindowPos(status,nullptr,x,std::max<LONG>((LONG)(100*s),rc.bottom-(LONG)(116*s)),w,(int)(100*s),SWP_NOZORDER);SetWindowPos(tabs,nullptr,x,(int)(66*s),w,(int)(31*s),SWP_NOZORDER);SetWindowPos(Control(ID_PANEL),nullptr,rc.right-(int)(130*s),(int)(18*s),(int)(106*s),(int)(30*s),SWP_NOZORDER);
        const int stageRight=(int)rc.right-(panelVisible?rail:0);SetWindowPos(Control(ID_RESUME_LYRICS),nullptr,std::max(0,stageRight-(int)(236*s)),(int)(66*s),(int)(200*s),(int)(32*s),SWP_NOZORDER);
        SetWindowPos(Control(ID_MATCHLYRICS),nullptr,(int)(36*s),(int)(66*s),(int)(160*s),(int)(32*s),SWP_NOZORDER);
        // Children returning from clipped positions must repaint their whole
        // content; invalidating only this WS_CLIPCHILDREN parent misses them.
        // Defer painting to normal dispatch, after every final child position.
        if(settingsPane)RedrawWindow(settingsPane,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_ERASE);
    }
    bool DrawCandidate(const DRAWITEMSTRUCT& draw) {
        if(draw.itemID==UINT_MAX||!draw.hDC)return false;
        const auto length=SendMessageW(list,LB_GETTEXTLEN,draw.itemID,0);if(length<0||length>8192)return false;
        std::array<wchar_t,8193> label{};const auto copied=SendMessageW(list,LB_GETTEXT,draw.itemID,reinterpret_cast<LPARAM>(label.data()));
        if(copied==LB_ERR||copied<0||copied>length)return false;
        const auto colors=DisplayedTheme();const auto rgb=[](ThemeColor c){return RGB((BYTE)(c.r*255),(BYTE)(c.g*255),(BYTE)(c.b*255));};
        const bool selected=(draw.itemState&ODS_SELECTED)!=0;
        const auto saved=SaveDC(draw.hDC);if(!saved)return false;
        auto fill=CreateSolidBrush(rgb(selected?colors.accent:colors.panel));
        if(!fill){RestoreDC(draw.hDC,saved);return false;}
        FillRect(draw.hDC,&draw.rcItem,fill);DeleteObject(fill);
        const auto selectedInk=colors.accent.r*.2126f+colors.accent.g*.7152f+colors.accent.b*.0722f>.5f?RGB(25,25,25):RGB(255,255,255);
        SetBkMode(draw.hDC,TRANSPARENT);SetTextColor(draw.hDC,selected?selectedInk:rgb(colors.ink));SelectObject(draw.hDC,font);
        const std::wstring_view text(label.data(),static_cast<size_t>(copied));const auto split=text.find(L'\n');
        RECT area=draw.rcItem;area.left+=8;area.right-=8;area.top+=4;area.bottom=area.top+MulDiv(21,GetDpiForWindow(window),96);
        const auto titleLength=split==std::wstring_view::npos?text.size():split;DrawTextW(draw.hDC,text.data(),static_cast<int>(titleLength),&area,DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
        if(split!=std::wstring::npos){area.top=area.bottom;area.bottom=draw.rcItem.bottom;SetTextColor(draw.hDC,selected?selectedInk:rgb(colors.muted));
            DrawTextW(draw.hDC,text.data()+split+1,static_cast<int>(text.size()-split-1),&area,DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);}
        if(draw.itemState&ODS_FOCUS)DrawFocusRect(draw.hDC,&draw.rcItem);RestoreDC(draw.hDC,saved);return true;
    }
    void Controls() {
        settingsPane=CreateWindowExW(WS_EX_CONTROLPARENT,L"MusicStageSettingsPane",L"舞台设置",WS_CHILD|WS_VSCROLL|WS_CLIPCHILDREN,0,0,292,400,window,reinterpret_cast<HMENU>((INT_PTR)ID_SETTINGSPANE),GetModuleHandleW(nullptr),this);
        if(!settingsPane)throw std::runtime_error("Native settings pane creation failed");
        auto make=[&](const wchar_t* cls,const wchar_t* text,int id,int controlPage,DWORD style=0)->HWND{auto h=CreateWindowExW(0,cls,text,WS_CHILD|WS_VISIBLE|style,0,0,200,30,controlPage<0?window.load():settingsPane,reinterpret_cast<HMENU>((INT_PTR)id),GetModuleHandleW(nullptr),nullptr);if(!h)throw std::runtime_error("Native control creation failed");pageControls.emplace_back(h,controlPage);return h;};
        auto button=[&](const wchar_t* text,int id,int controlPage){auto h=make(L"BUTTON",text,id,controlPage,BS_PUSHBUTTON|WS_TABSTOP);nativeControls.AttachButton(h);return h;};
        auto checkbox=[&](const wchar_t* text,int id,int controlPage){
            const auto h=make(L"BUTTON",text,id,controlPage,BS_AUTOCHECKBOX|WS_TABSTOP);
            // Keep the native toggle/keyboard/accessibility behavior while
            // allowing the parent's palette to color the label and face.
            const auto result=SetWindowTheme(h,L"",L"");
            if(FAILED(result))log.Event("checkbox-theme","Native checkbox palette setup failed: "+std::to_string(static_cast<unsigned long>(result)));
            return h;
        };
        auto panelButton=button(L"设置（Ctrl+,）",ID_PANEL,-1);pageControls.pop_back();(void)panelButton;
        auto returnButton=button(L"返回当前句 · Home",ID_RESUME_LYRICS,-1);pageControls.pop_back();ShowWindow(returnButton,SW_HIDE);
        auto matchButton=button(L"核对歌词版本",ID_MATCHLYRICS,-1);pageControls.pop_back();ShowWindow(matchButton,SW_HIDE);
        tabs=CreateWindowExW(0,WC_TABCONTROLW,L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|TCS_FIXEDWIDTH,0,0,292,31,window,reinterpret_cast<HMENU>(ID_TAB),GetModuleHandleW(nullptr),nullptr);if(!tabs)throw std::runtime_error("Native tab creation failed");for(auto name:{L"歌词",L"画面",L"主题",L"状态"}){TCITEMW item{};item.mask=TCIF_TEXT;item.pszText=const_cast<LPWSTR>(name);if(TabCtrl_InsertItem(tabs,TabCtrl_GetItemCount(tabs),&item)<0)throw std::runtime_error("Native tab item creation failed");}if(!nativeControls.AttachTabs(tabs))log.Event("tabs-theme","Native theme attachment failed; standard tabs retained");
        make(L"STATIC",L"歌名、网易云 ID/链接、qq-ID、kg-哈希或 soda-ID",211,0);
        manualQueryEdit=make(L"EDIT",L"",ID_MANUALQUERY,0,WS_BORDER|ES_AUTOHSCROLL|WS_TABSTOP);
        SendMessageW(manualQueryEdit,EM_SETLIMITTEXT,2048,0);
        SendMessageW(manualQueryEdit,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(L"输入后按 Enter 查找歌词"));
        button(L"查找 / 预览歌词",ID_MANUALSEARCH,0);
        search=button(L"按当前歌曲重新匹配",ID_SEARCH,0);list=make(L"LISTBOX",L"",ID_CANDIDATES,0,LBS_NOTIFY|LBS_OWNERDRAWFIXED|LBS_HASSTRINGS|WS_BORDER|WS_VSCROLL|WS_TABSTOP);
        make(L"STATIC",L"所选版本资料",ID_CANDIDATEDETAILSLABEL,0,SS_NOPREFIX);
        make(L"EDIT",L"",ID_CANDIDATEDETAILS,0,ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_VSCROLL|WS_TABSTOP);
        fetch=button(L"采用所选版本的歌词",ID_FETCH,0);button(L"导入 LRC / YRC 歌词",ID_IMPORT,0);button(L"恢复在线歌词",ID_ONLINELYRICS,0);make(L"STATIC",L"本首歌词偏移（毫秒）",201,0);offsetEdit=make(L"EDIT",std::to_wstring(offset).c_str(),ID_OFFSET,0,WS_BORDER|ES_AUTOHSCROLL|WS_TABSTOP);SendMessageW(offsetEdit,EM_SETLIMITTEXT,6,0);button(L"重置",ID_OFFSETRESET,0);
        checkbox(L"显示歌词翻译",ID_TRANSLATION,0);
        checkbox(L"显示罗马音 / 音译",ID_ROMANIZATION,0);
        make(L"STATIC",L"歌词呈现方式（Ctrl+L 切换）",208,0);
        lyricModeCombo=make(L"COMBOBOX",L"",ID_LYRICMODE,0,CBS_DROPDOWNLIST|WS_TABSTOP);
        if(!nativeControls.AttachCombo(lyricModeCombo))log.Event("combo-theme","Lyric mode combo kept standard painting");
        for(auto name:{L"流动歌词",L"聚焦单句",L"双行预读"})SendMessageW(lyricModeCombo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name));
        button(L"恢复歌词记忆保存",ID_LYRICMEMORYREPAIR,0);make(L"STATIC",L"",ID_LYRICMEMORYNOTE,0);
        button(L"全屏舞台（F11 / Esc）",ID_FULLSCREEN,1);button(L"融入桌面",ID_DESKTOP,1);button(L"恢复窗口模式",ID_WINDOW,1);button(L"选择图片背景",ID_IMAGE,1);button(L"选择视频背景",ID_VIDEO,1);button(L"使用 / 重试歌曲封面",ID_COVER,1);make(L"STATIC",L"音频响应强度",203,1);strengthSlider=make(TRACKBAR_CLASSW,L"",ID_STRENGTH,1,TBS_AUTOTICKS|WS_TABSTOP);SendMessageW(strengthSlider,TBM_SETRANGE,TRUE,MAKELPARAM(0,200));make(L"STATIC",L"歌词字号",205,1);sizeSlider=make(TRACKBAR_CLASSW,L"",ID_TEXTSIZE,1,TBS_AUTOTICKS|WS_TABSTOP);SendMessageW(sizeSlider,TBM_SETRANGE,TRUE,MAKELPARAM(26,80));checkbox(L"减少镜头与粒子运动",ID_MOTION,1);make(L"STATIC",L"歌曲封面 · 等待歌曲信息",ID_COVERSTATUS,1);
        make(L"STATIC",L"背景淡化",209,1);shadeSlider=make(TRACKBAR_CLASSW,L"",ID_SHADE,1,TBS_AUTOTICKS|WS_TABSTOP);SendMessageW(shadeSlider,TBM_SETRANGE,TRUE,MAKELPARAM(0,100));
        make(L"STATIC",L"镜头幅度",210,1);cameraSlider=make(TRACKBAR_CLASSW,L"",ID_CAMERA,1,TBS_AUTOTICKS|WS_TABSTOP);SendMessageW(cameraSlider,TBM_SETRANGE,TRUE,MAKELPARAM(0,200));button(L"恢复背景调节默认值",ID_BACKDROP_RESET,1);
        make(L"STATIC",L"画质（自动会按显卡负载调整）",ID_QUALITYLABEL,1);
        qualityCombo=make(L"COMBOBOX",L"",ID_QUALITY,1,CBS_DROPDOWNLIST|WS_TABSTOP);
        if(!nativeControls.AttachCombo(qualityCombo))log.Event("combo-theme","Quality combo kept standard painting");
        for(auto name:{L"自动",L"高",L"中",L"低"})SendMessageW(qualityCombo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name));
        make(L"STATIC",L"帧率",ID_FRAMERATELABEL,1);
        frameRateCombo=make(L"COMBOBOX",L"",ID_FRAMERATE,1,CBS_DROPDOWNLIST|WS_TABSTOP);
        if(!nativeControls.AttachCombo(frameRateCombo))log.Event("combo-theme","Frame rate combo kept standard painting");
        for(auto name:{L"自动（最高约 60 帧）",L"跟随屏幕刷新率",L"省电（约 30 帧）"})SendMessageW(frameRateCombo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name));
        checkbox(L"被全屏游戏或最大化窗口挡住时暂停舞台",ID_PAUSEFULLSCREEN,1);
        button(L"重试当前背景素材",ID_BACKGROUNDRETRY,1);make(L"STATIC",L"",ID_BACKGROUNDNOTE,1,SS_NOPREFIX);
        make(L"STATIC",L"主题风格",202,2);themeCombo=make(L"COMBOBOX",L"",ID_THEME,2,CBS_DROPDOWNLIST|WS_TABSTOP);
        if(!nativeControls.AttachCombo(themeCombo))log.Event("combo-theme","Theme combo kept standard painting");
        for(int i:AvailableBuiltInStyles){const auto definition=BuiltInTheme(i);SendMessageW(themeCombo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(definition.name.c_str()));}SendMessageW(themeCombo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"我的自定义主题"));button(L"载入本地主题",ID_THEMEIMPORT,2);button(L"导出当前主题",ID_THEMEEXPORT,2);button(L"选择强调色",ID_ACCENT,2);make(L"STATIC",L"主题保存在你的电脑。\r\n\r\n可自定义配色、字号、动效和背景；导入时保留原始主题文件。\r\n\r\n背景素材使用本地引用，移动主题时请一起保留素材。",206,2);button(L"重试已保存主题",ID_THEMERESTORE,2);make(L"STATIC",L"",ID_THEMENOTE,2);
        checkbox(L"显示接入诊断",ID_DIAGNOSTICS,3);checkbox(L"允许原生只读内部时钟",ID_CLOCK,3);
        make(L"EDIT",UsageHelpText,ID_USAGEHELP,3,ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_VSCROLL|WS_TABSTOP);
        checkbox(L"全屏空闲时隐藏辅助信息和指针",ID_AUTOHIDE,3);button(L"恢复设置保存",ID_SETTINGSREPAIR,3);make(L"STATIC",L"",ID_SETTINGSNOTE,3);status=make(L"EDIT",L"读取播放器、歌词与音频状态中…",204,-1,ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_VSCROLL|WS_TABSTOP);
        try{stageUi=std::make_unique<ui::Context>();}
        catch(const std::exception& e){stageUi.reset();log.Event("stage-ui-unavailable",e.what());}
        if(stageUi){
            // Backstage replaces the native settings rail; its toggle retires.
            panelVisible=false;ShowWindow(Control(ID_PANEL),SW_HIDE);
        }
        RefreshFont();SyncThemeControls();controlsReady=true;SetPanel(panelVisible);UpdateControls();AddTray();
    }

};
LRESULT CALLBACK SettingsPaneProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    auto app=reinterpret_cast<App*>(GetWindowLongPtrW(h,GWLP_USERDATA));
    if(msg==WM_NCCREATE){app=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app));}
    if(!app)return DefWindowProcW(h,msg,wp,lp);
    switch(msg){
        case WM_ERASEBKGND:return 1;
        case WM_PAINT:{PAINTSTRUCT paint{};const auto dc=BeginPaint(h,&paint);const auto color=app->DisplayedTheme().panel;
            const auto fill=CreateSolidBrush(RGB((BYTE)(color.r*255),(BYTE)(color.g*255),(BYTE)(color.b*255)));
            if(fill){FillRect(dc,&paint.rcPaint,fill);DeleteObject(fill);}EndPaint(h,&paint);return 0;}
        case WM_VSCROLL:try{app->PaneScrollCommand(LOWORD(wp));}catch(...){app->log.Event("settings-scroll","Unable to update native panel layout");}return 0;
        case WM_MOUSEWHEEL:try{app->PaneWheel(GET_WHEEL_DELTA_WPARAM(wp));}catch(...){app->log.Event("settings-scroll","Unable to update native panel layout");}return 0;
        case WM_COMMAND:case WM_NOTIFY:case WM_DRAWITEM:case WM_MEASUREITEM:case WM_HSCROLL:
        case WM_CTLCOLORSTATIC:case WM_CTLCOLOREDIT:case WM_CTLCOLORLISTBOX:case WM_CTLCOLORBTN:
            return app->window?SendMessageW(app->window,msg,wp,lp):0;
        case WM_NCDESTROY:SetWindowLongPtrW(h,GWLP_USERDATA,0);if(app->settingsPane==h)app->settingsPane=nullptr;return DefWindowProcW(h,msg,wp,lp);
    }
    return DefWindowProcW(h,msg,wp,lp);
}
LRESULT CALLBACK StageProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    auto app=reinterpret_cast<App*>(GetWindowLongPtrW(h,GWLP_USERDATA));
    if(msg==WM_NCCREATE){app=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);app->desktopWindow=h;SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app));}
    if(!app)return DefWindowProcW(h,msg,wp,lp);
    switch(msg){
        case WM_NCHITTEST:return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:return MA_NOACTIVATE;
        case WM_ERASEBKGND:return 1;
        case WM_PAINT:{PAINTSTRUCT paint{};BeginPaint(h,&paint);EndPaint(h,&paint);return 0;}
        case WM_SIZE:app->RequestSceneResize(true);return 0;
        case WM_DPICHANGED:case WM_DPICHANGED_AFTERPARENT:app->desktopPollAt=0;app->RequestSceneResize(true);return 0;
        case WM_DISPLAYCHANGE:app->desktopPollAt=0;return 0;
        case WM_DESTROY:if(!app->closingDesktop&&!app->exiting)PostMessageW(app->window,WM_STAGE_LOST,reinterpret_cast<WPARAM>(h),0);return 0;
        case WM_NCDESTROY:SetWindowLongPtrW(h,GWLP_USERDATA,0);if(app->desktopWindow==h)app->desktopWindow=nullptr;return DefWindowProcW(h,msg,wp,lp);
    }
    return DefWindowProcW(h,msg,wp,lp);
}
LRESULT CALLBACK WindowProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    auto app=reinterpret_cast<App*>(GetWindowLongPtrW(h,GWLP_USERDATA));if(msg==WM_NCCREATE){app=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);app->window=h;SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app));}
    if(!app)return DefWindowProcW(h,msg,wp,lp);
    if(msg==taskbarCreated){app->trayAdded=false;if(!app->AddTray())app->OpenWindow();app->desktopPollAt=0;return 0;}
    try{
        if(app->frameDrawing||app->frameWaiting){
            if(msg==WM_CLOSE){if(app->frameDrawing)app->closeDeferred=true;else PostMessageW(h,msg,wp,lp);return 0;}
            // WM_COMMAND carries a control HWND, not a borrowed notification
            // structure. Commands and keys can safely return to the queue.
            if(msg==WM_COMMAND&&LOWORD(wp)==ID_OFFSET&&app->settingOffsetText)return 0;
            if(msg==WM_COMMAND||msg==WM_KEYDOWN||msg==WM_TRAY||msg==WM_ACTIVATE_APP||msg==WM_STAGE_LOST||msg==WM_UI_ACTIONS||
               msg==WM_LBUTTONDBLCLK||msg==WM_SYSCOMMAND||msg==WM_NCLBUTTONDOWN||msg==WM_NCLBUTTONDBLCLK||
               msg==WM_NCRBUTTONDOWN||msg==WM_NCRBUTTONUP||msg==WM_CONTEXTMENU){PostMessageW(h,msg,wp,lp);return 0;}
        }
        switch(msg){
        case WM_NCCALCSIZE:{
            auto rect=wp?&reinterpret_cast<NCCALCSIZE_PARAMS*>(lp)->rgrc[0]:reinterpret_cast<RECT*>(lp);
            if(HasWindowChrome(h)&&IsZoomed(h)){
                if(const auto monitor=WindowMonitor(MonitorFromRect(rect,MONITOR_DEFAULTTONEAREST)))*rect=monitor->rcWork;
            }
            return 0;}
        case WM_NCHITTEST:{
            if(!HasWindowChrome(h)||app->uiPointerCaptured)return HTCLIENT;
            RECT client{};POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
            if(!GetClientRect(h,&client)||!ScreenToClient(h,&point))return DefWindowProcW(h,msg,wp,lp);
            return WindowChromeHitTest(point,{client.right,client.bottom},WindowDpi(h),IsZoomed(h)!=FALSE);}
        case WM_DWMCOMPOSITIONCHANGED:case WM_THEMECHANGED:ConfigureWindowChrome(h);break;
        case WM_NCACTIVATE:return DefWindowProcW(h,msg,wp,-1);
        case WM_CREATE:app->renderer.Create(h);app->Controls();app->Begin();ConfigureWindowChrome(h);
            SetWindowPos(h,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);return 0;
        case WM_SIZE:if(wp!=SIZE_MINIMIZED){app->RequestWindowBoundsFit();app->RequestSceneResize(false);app->Layout();app->EnsurePaneFocusVisible(true);if(app->sizingLoop)app->PumpFrames(true);
            else if(app->windowPlacementReady&&app->fullscreenTransition==App::FullscreenTransition::Stable&&!app->fullscreen&&app->CaptureWindowPlacement())app->settingsSaveAt=MonotonicMs()+500;}
            if(app->controlsReady)app->RefreshVisualMediaDemand();return 0;
        case WM_MOVING:case WM_SIZING:if(app->sizingLoop)app->PumpFrames(true);return DefWindowProcW(h,msg,wp,lp);
        case WM_ENTERSIZEMOVE:app->sizingLoop=true;app->NoteActivity();app->UpdateModalFrameTimer();return 0;
        case WM_EXITSIZEMOVE:app->sizingLoop=false;app->UpdateModalFrameTimer();app->RequestWindowBoundsFit();app->RememberWindowPlacement();return 0;
        case WM_ENTERMENULOOP:app->menuLoop=true;app->NoteActivity();app->UpdateModalFrameTimer();return 0;
        case WM_EXITMENULOOP:app->menuLoop=false;app->UpdateModalFrameTimer();return 0;
        case WM_GETMINMAXINFO:{auto info=reinterpret_cast<MINMAXINFO*>(lp);const auto dpi=WindowDpi(h);
            const auto monitor=WindowMonitor(MonitorFromWindow(h,MONITOR_DEFAULTTONEAREST));
            if(monitor){const auto minimum=MinimumWindowSize(dpi,monitor->rcWork);info->ptMinTrackSize={minimum.cx,minimum.cy};
                if(HasWindowChrome(h)){
                    info->ptMaxPosition={monitor->rcWork.left-monitor->rcMonitor.left,monitor->rcWork.top-monitor->rcMonitor.top};
                    info->ptMaxSize={monitor->rcWork.right-monitor->rcWork.left,monitor->rcWork.bottom-monitor->rcWork.top};
                    info->ptMaxTrackSize.x=std::max(info->ptMaxTrackSize.x,info->ptMaxSize.x);
                    info->ptMaxTrackSize.y=std::max(info->ptMaxTrackSize.y,info->ptMaxSize.y);
                }}
            else info->ptMinTrackSize={MulDiv(680,dpi,96),MulDiv(440,dpi,96)};return 0;}
        case WM_DPICHANGED:{auto desired=*reinterpret_cast<RECT*>(lp);const auto monitor=WindowMonitor(MonitorFromRect(&desired,MONITOR_DEFAULTTONEAREST));
            if(monitor){if(app->FullscreenUsesMonitorBounds())desired=monitor->rcMonitor;else if(!IsZoomed(h))desired=FitWindowToWorkArea(desired,monitor->rcWork,LOWORD(wp));}
            if(!monitor||!SetWindowPos(h,nullptr,desired.left,desired.top,desired.right-desired.left,desired.bottom-desired.top,SWP_NOZORDER|SWP_NOACTIVATE))app->RequestWindowBoundsFit();
            // A DPI transition does not guarantee a WM_SIZE when the suggested
            // pixel dimensions are unchanged. Resize later through the guarded
            // frame path so the D2D target DPI and scene scale cannot stay stale.
            app->RequestSceneResize(false);app->RefreshFont();app->Layout();return 0;}
        case WM_ERASEBKGND:return 1;
        case WM_PAINT:{PAINTSTRUCT ps{};BeginPaint(h,&ps);EndPaint(h,&ps);return 0;}
        case WM_TIMER:
            if(wp==TimerTick){if(app->frameDrawing||app->frameWaiting)app->tickDeferred=true;else{app->tickDeferred=false;app->Tick();}}
            else if(wp==TimerModalFrame&&app->NestedLoopActive()&&!app->frameWaiting)app->PumpFrames(true);
            return 0;
        case WM_DATA:app->UpdateControls();return 0;
        case WM_UI_ACTIONS:app->RunUiActions();return 0;
        case WM_MOUSEWHEEL:{app->NoteActivity();POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};const auto over=WindowFromPoint(point);
            if(app->stageUi&&over==h){
                POINT client=point;ScreenToClient(h,&client);const float s=app->ClientScale();
                if(app->stageUi->HitTest(client.x/s,client.y/s)){app->stageUi->Wheel(client.x/s,client.y/s,GET_WHEEL_DELTA_WPARAM(wp)/static_cast<float>(WHEEL_DELTA));return 0;}
            }
            if(app->panelVisible&&(over==app->settingsPane||IsChild(app->settingsPane,over)))app->PaneWheel(GET_WHEEL_DELTA_WPARAM(wp));
            else app->StageWheel(GET_WHEEL_DELTA_WPARAM(wp));return 0;}
        // The render loop observes actual pointer displacement. Windows can
        // synthesize WM_MOUSEMOVE after showing/hiding a control under a still
        // pointer; that must not repeatedly wake idle fullscreen chrome.
        case WM_MOUSEMOVE:
            if(app->stageUi){
                const POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
                if(p.x!=app->lastClientPointer.x||p.y!=app->lastClientPointer.y){
                    const float s=app->ClientScale();app->lastClientPointer=p;
                    app->stageUi->MouseMove(p.x/s,p.y/s);app->consoleActivityAt=MonotonicMs();
                }
                if(!app->uiMouseTracked){TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,h,0};app->uiMouseTracked=TrackMouseEvent(&track)!=FALSE;}
            }
            return 0;
        case WM_MOUSELEAVE:app->uiMouseTracked=false;app->lastClientPointer={-1,-1};if(app->stageUi&&!app->uiPointerCaptured)app->stageUi->MouseLeave();return 0;
        case WM_LBUTTONDOWN:case WM_LBUTTONDBLCLK:{
            app->NoteActivity();SetFocus(h);
            if(app->stageUi){
                const float s=app->ClientScale(),x=GET_X_LPARAM(lp)/s,y=GET_Y_LPARAM(lp)/s;
                if(app->stageUi->HitTest(x,y)){
                    app->stageUi->MouseDown(x,y,msg==WM_LBUTTONDBLCLK);SetCapture(h);app->uiPointerCaptured=true;app->consoleActivityAt=MonotonicMs();return 0;
                }
                app->stageUi->ClearFocus();
            }
            if(msg==WM_LBUTTONDBLCLK)app->ToggleFullscreen();
            return 0;}
        case WM_LBUTTONUP:
            if(app->uiPointerCaptured){
                app->uiPointerCaptured=false;const float s=app->ClientScale();
                app->stageUi->MouseUp(GET_X_LPARAM(lp)/s,GET_Y_LPARAM(lp)/s);ReleaseCapture();
            }
            return 0;
        case WM_CAPTURECHANGED:
            // Losing capture mid-press ends drags without a click.
            if(app->uiPointerCaptured&&reinterpret_cast<HWND>(lp)!=h){app->uiPointerCaptured=false;if(app->stageUi)app->stageUi->MouseUp(-1e6f,-1e6f);}
            return 0;
        case WM_CHAR:if(app->stageUi&&app->stageUi->EditingText()){app->stageUi->Char(static_cast<wchar_t>(wp));return 0;}break;
        case WM_IME_STARTCOMPOSITION:case WM_IME_COMPOSITION:app->PlaceImeWindow();break;
        case WM_ACTIVATE:app->renderVisibilityPollAt=0;app->foregroundRecheck=true;app->NoteActivity();return 0;
        case WM_SETCURSOR:if(LOWORD(lp)==HTCLIENT&&reinterpret_cast<HWND>(wp)==h&&app->quietFullscreen&&GetForegroundWindow()==h){SetCursor(nullptr);app->cursorHidden=true;return TRUE;}break;
        case WM_COMMAND:app->NoteActivity();switch(LOWORD(wp)){
            case ID_SEARCH:app->FollowCurrentLyrics(true);break;
            case ID_MANUALSEARCH:app->ManualFind();break;
            case ID_FETCH:app->Fetch();break;
            case ID_CANDIDATES:if(HIWORD(wp)==LBN_DBLCLK){app->SyncCandidateSelectionFromList();app->Fetch();}
                else if(HIWORD(wp)==LBN_SELCHANGE){app->SyncCandidateSelectionFromList();app->UpdateCandidateFetchEnabled();}break;
            case ID_IMPORT:app->Import();break;
            case ID_RESUME_LYRICS:app->ResumeLyricFollowing();break;
            case ID_MATCHLYRICS:app->OpenLyricMatching();break;
            case ID_BACKDROP_RESET:app->ResetBackdropAdjustments();break;
            case ID_FULLSCREEN:app->ToggleFullscreen();break;
            case ID_PANEL:app->ToggleBackstage();break;
            case ID_IMAGE:app->ChooseBackground(false);break;
            case ID_VIDEO:app->ChooseBackground(true);break;
            case ID_COVER:app->UseCover(true);break;
            case ID_BACKGROUNDRETRY:app->RetryBackground();break;
            case ID_DESKTOP:app->EnterDesktop();break;
            case ID_WINDOW:app->WindowMode();break;
            case ID_OPEN:app->OpenWindow();app->OpenBackstage();break;
            case ID_EXIT:app->log.Event("close-request","Tray exit");app->FinalizeShutdown(true);DestroyWindow(h);break;
            case ID_THEMEIMPORT:app->ChooseTheme(false);break;
            case ID_THEMEEXPORT:app->ChooseTheme(true);break;
            case ID_ACCENT:app->ChooseAccent();break;
            case ID_MOTION:app->settings.reducedMotion=SendMessageW(app->Control(ID_MOTION),BM_GETCHECK,0,0)==BST_CHECKED;app->Persist();break;
            case ID_TRANSLATION:app->settings.showTranslation=SendMessageW(app->Control(ID_TRANSLATION),BM_GETCHECK,0,0)==BST_CHECKED;app->Persist();break;
            case ID_ROMANIZATION:app->settings.showRomanization=SendMessageW(app->Control(ID_ROMANIZATION),BM_GETCHECK,0,0)==BST_CHECKED;app->Persist();break;
            case ID_LYRICMODE:if(HIWORD(wp)==CBN_SELCHANGE)app->ChangeLyricMode((int)SendMessageW(app->lyricModeCombo,CB_GETCURSEL,0,0));break;
            case ID_AUTOHIDE:app->settings.fullscreenAutoHide=SendMessageW(app->Control(ID_AUTOHIDE),BM_GETCHECK,0,0)==BST_CHECKED;app->UpdateFullscreenChrome();app->Persist();break;
            case ID_DIAGNOSTICS:app->showDiagnostics=SendMessageW(app->Control(ID_DIAGNOSTICS),BM_GETCHECK,0,0)==BST_CHECKED;break;
            case ID_SETTINGSREPAIR:app->RecoverSettingsSaving();break;
            case ID_LYRICMEMORYREPAIR:app->RecoverTrackPreferenceSaving();break;
            case ID_THEMERESTORE:app->RetrySavedTheme();break;
            case ID_CLOCK:app->settings.nativeClockEnabled=SendMessageW(app->Control(ID_CLOCK),BM_GETCHECK,0,0)==BST_CHECKED;app->monitor.SetClientClockEnabled(app->settings.nativeClockEnabled);app->uiNotice=app->settings.nativeClockEnabled?L"原生只读内部时钟已开启；只有严格校验通过才会驱动歌词":L"原生只读内部时钟已关闭；仍保留 Windows 媒体会话与界面观测";app->Persist();break;
            case ID_THEME:if(HIWORD(wp)==CBN_SELCHANGE)app->SelectTheme(ThemeIdAtChoice((int)SendMessageW(app->themeCombo,CB_GETCURSEL,0,0)));break;
            case ID_OFFSET:if(HIWORD(wp)==EN_CHANGE)app->OffsetChanged();else if(HIWORD(wp)==EN_KILLFOCUS)app->CommitOrRestoreOffsetText();break;
            case ID_OFFSETRESET:app->ResetOffset();break;
            case ID_ONLINELYRICS:app->RestoreOnlineLyrics();break;
            case ID_QUALITY:if(HIWORD(wp)==CBN_SELCHANGE){app->settings.qualityMode=std::clamp((int)SendMessageW(app->qualityCombo,CB_GETCURSEL,0,0),0,3);app->Persist();}break;
            case ID_FRAMERATE:if(HIWORD(wp)==CBN_SELCHANGE){app->settings.frameRateMode=std::clamp((int)SendMessageW(app->frameRateCombo,CB_GETCURSEL,0,0),0,2);app->Persist();}break;
            case ID_PAUSEFULLSCREEN:app->settings.pauseForFullscreenApps=SendMessageW(app->Control(ID_PAUSEFULLSCREEN),BM_GETCHECK,0,0)==BST_CHECKED;app->Persist();app->RefreshVisualMediaDemand();break;
        }return 0;
        case WM_NOTIFY:if(reinterpret_cast<NMHDR*>(lp)->hwndFrom==app->tabs&&reinterpret_cast<NMHDR*>(lp)->code==TCN_SELCHANGE){app->page=TabCtrl_GetCurSel(app->tabs);app->SetPanel(app->panelVisible);}return 0;
        case WM_MEASUREITEM:{auto item=reinterpret_cast<MEASUREITEMSTRUCT*>(lp);if(item->CtlID==ID_CANDIDATES){item->itemHeight=MulDiv(48,GetDpiForWindow(h),96);return TRUE;}break;}
        case WM_DRAWITEM:{auto item=reinterpret_cast<DRAWITEMSTRUCT*>(lp);if(app->nativeControls.DrawItem(*item,app->font))return TRUE;if(item->CtlID==ID_CANDIDATES&&app->DrawCandidate(*item))return TRUE;break;}
        case WM_HSCROLL:{const auto slider=reinterpret_cast<HWND>(lp);bool changed=true;app->NoteActivity();
            if(slider==app->strengthSlider)app->strength=(float)SendMessageW(slider,TBM_GETPOS,0,0)/100;
            else if(slider==app->sizeSlider)app->settings.lyricSize=(float)SendMessageW(slider,TBM_GETPOS,0,0);
            else if(slider==app->shadeSlider)app->settings.backgroundShade=(float)SendMessageW(slider,TBM_GETPOS,0,0)/100;
            else if(slider==app->cameraSlider)app->settings.cameraMotion=(float)SendMessageW(slider,TBM_GETPOS,0,0)/100;
            else changed=false;
            if(changed){app->UpdateBackdropLabels();app->settingsSaveAt=MonotonicMs()+500;}return 0;}
        case WM_KEYDOWN:if((wp==VK_F11||(wp==VK_ESCAPE&&app->fullscreen))&&!(lp&(LPARAM(1)<<30)))app->ToggleFullscreen();return 0;
        case WM_CTLCOLORSTATIC:case WM_CTLCOLOREDIT:case WM_CTLCOLORLISTBOX:case WM_CTLCOLORBTN:{auto dc=reinterpret_cast<HDC>(wp);auto colors=app->DisplayedTheme();auto rgb=[](ThemeColor c){return RGB((BYTE)(c.r*255),(BYTE)(c.g*255),(BYTE)(c.b*255));};SetTextColor(dc,rgb(colors.ink));SetBkColor(dc,rgb(colors.panel));SetDCBrushColor(dc,rgb(colors.panel));return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));}
        case WM_TRAY:if(LOWORD(lp)==WM_LBUTTONUP||LOWORD(lp)==NIN_SELECT||LOWORD(lp)==NIN_KEYSELECT)app->OpenWindow();else if(LOWORD(lp)==WM_CONTEXTMENU||LOWORD(lp)==WM_RBUTTONUP)app->TrayMenu();return 0;
        case WM_STAGE_LOST:if(!app->desktopWindow||app->desktopWindow==reinterpret_cast<HWND>(wp))app->OnStageLost();return 0;
        case WM_ACTIVATE_APP:app->OpenWindow();return 0;
        case WM_DISPLAYCHANGE:app->RequestWindowBoundsFit();app->renderVisibilityPollAt=0;app->desktopPollAt=0;return 0;
        case WM_SETTINGCHANGE:if(wp==SPI_SETWORKAREA)app->RequestWindowBoundsFit();return DefWindowProcW(h,msg,wp,lp);
        case WM_QUERYENDSESSION:return TRUE;
        case WM_ENDSESSION:
            if(!wp)return 0;
            app->log.Event("close-request","Windows session ending");app->FinalizeShutdown(true);if(IsWindow(h))DestroyWindow(h);return 0;
        case WM_CLOSE:if(app->desktopActive&&app->trayAdded){app->RememberWindowPlacement();ShowWindow(h,SW_HIDE);return 0;}app->log.Event("close-request","Window close");app->FinalizeShutdown(true);DestroyWindow(h);return 0;
        case WM_DESTROY:KillTimer(h,TimerTick);PostQuitMessage(0);app->FinalizeShutdown(false);return 0;
        case WM_NCDESTROY:SetWindowLongPtrW(h,GWLP_USERDATA,0);app->window=nullptr;return DefWindowProcW(h,msg,wp,lp);
    }}catch(const std::exception& e){app->log.Event("window-error",e.what());if(msg==WM_CREATE)return -1;
        // The regular error-exit path still has a valid HWND here. Capture its
        // final controls and placement before WM_DESTROY switches to no-HWND cleanup.
        app->FinalizeShutdown(true);MessageBoxW(h,L"运行错误，已尝试记录诊断。请保留日志及当前错误情况。",L"HTune 音乐视觉伴侣",MB_ICONERROR);DestroyWindow(h);return 0;}return DefWindowProcW(h,msg,wp,lp);
}
int Probe(int seconds,const std::filesystem::path& path,bool withAudio) {
    Shared data;Logger log(path);PlaybackMonitor monitor;ProcessAudioCapture capture;DWORD pid=0;uint64_t createdAt=0;int64_t retryAt=0;int failures=0;monitor.Start([&](const PlaybackSnapshot& p){std::lock_guard lock(data.mutex);data.playback=p;});
    auto started=MonotonicMs();log.Event("probe-start",withAudio?"Native playback + process audio probe":"Native playback probe");
    while(MonotonicMs()-started<(int64_t)seconds*1000){PlaybackSnapshot p;AudioFeatures a;{std::lock_guard lock(data.mutex);p=data.playback;a=data.audio;}
        if(withAudio)MaintainAudio(capture,pid,createdAt,retryAt,failures,p,a,true,
            [&](const AudioFeatures& f){std::lock_guard lock(data.mutex);data.audio=f;});
        log.Sample(p,a);std::this_thread::sleep_for(std::chrono::milliseconds(250));}
    monitor.Stop();capture.Stop();log.Event("probe-end","Completed requested duration; log is observations, not acceptance");log.Stop();
    const auto recorded=log.writer.Status();return recorded.state==DiagnosticLogState::Failed||recorded.state==DiagnosticLogState::LimitReached||recorded.droppedSamples||recorded.droppedEvents?2:0;
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show) {
    const HRESULT coResult=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);if(FAILED(coResult)){MessageBoxW(nullptr,L"Windows COM 初始化失败，HTune 无法启动。",L"HTune 音乐视觉伴侣",MB_ICONERROR);return 1;}
    // The background worker owns Media Foundation startup/shutdown only when
    // video is requested. Its failure must not reject the lyric/image stage.
    INITCOMMONCONTROLSEX ic{sizeof(ic),ICC_BAR_CLASSES|ICC_TAB_CLASSES};if(!InitCommonControlsEx(&ic)){CoUninitialize();MessageBoxW(nullptr,L"Windows 原生控件初始化失败，HTune 无法启动。",L"HTune 音乐视觉伴侣",MB_ICONERROR);return 1;}
    int result=0;try{
        int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);bool probe=false,audio=true;int seconds=20;auto logPath=DefaultLogPath();
        for(int i=1;i<argc;i++){std::wstring arg=argv[i];if(arg==L"--probe")probe=true;else if(arg==L"--no-audio")audio=false;else if(arg==L"--seconds"&&i+1<argc)seconds=std::clamp(_wtoi(argv[++i]),1,3600);else if(arg==L"--log"&&i+1<argc)logPath=argv[++i];}LocalFree(argv);
        if(probe)result=Probe(seconds,logPath,audio);else{
            SetLastError(ERROR_SUCCESS);HANDLE instanceMutex=CreateMutexW(nullptr,FALSE,L"Local\\MusicStage.NativePrototype.0.1");const DWORD mutexStatus=GetLastError();if(!instanceMutex)throw std::runtime_error("Could not create single-instance mutex");if(mutexStatus==ERROR_ALREADY_EXISTS){if(auto existing=FindWindowW(L"MusicStageNativePrototypeWindow",nullptr))PostMessageW(existing,WM_ACTIVATE_APP,0,0);CloseHandle(instanceMutex);CoUninitialize();return 0;}
            {App app(logPath);WNDCLASSEXW wc{sizeof(wc)};wc.hInstance=instance;wc.style=CS_DBLCLKS;wc.lpfnWndProc=WindowProc;wc.lpszClassName=L"MusicStageNativePrototypeWindow";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(IDI_MUSIC_STAGE));wc.hIconSm=static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(IDI_MUSIC_STAGE),IMAGE_ICON,GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),LR_DEFAULTCOLOR|LR_SHARED));if(!RegisterClassExW(&wc))throw std::runtime_error("Could not register application window class");auto stageClass=wc;stageClass.lpfnWndProc=StageProc;stageClass.lpszClassName=L"MusicStageDesktopStage";stageClass.hIcon=nullptr;stageClass.hIconSm=nullptr;if(!RegisterClassExW(&stageClass))throw std::runtime_error("Could not register desktop stage class");
            auto paneClass=wc;paneClass.style=0;paneClass.lpfnWndProc=SettingsPaneProc;paneClass.lpszClassName=L"MusicStageSettingsPane";paneClass.hIcon=nullptr;paneClass.hIconSm=nullptr;if(!RegisterClassExW(&paneClass))throw std::runtime_error("Could not register settings pane class");
            int x=CW_USEDEFAULT,y=CW_USEDEFAULT,w=1280,hgt=900;
            auto h=CreateWindowExW(0,wc.lpszClassName,L"HTune · 音乐视觉伴侣 " MUSIC_STAGE_VERSION_W,WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,x,y,w,hgt,nullptr,nullptr,instance,&app);if(!h)throw std::runtime_error("Could not create application window");app.RestoreWindowBounds();if(app.desiredDesktop){ShowWindow(h,SW_HIDE);if(!app.EnterDesktop(false)){app.desktopRetries=0;app.desktopRetryAt=MonotonicMs()+1000;app.ShowInitialWindow(show);}}else app.ShowInitialWindow(show);
            bool running=true;while(running){
                const auto target=app.FrameWaitTarget();const bool held=app.HasFramePermit(target);
                const auto wait=held?nullptr:target.handle;const auto timeout=held?0:app.FrameWaitTimeout(wait);
                DWORD wake=WAIT_FAILED;
                {
                    struct WaitingScope {bool& active;explicit WaitingScope(bool& value):active(value){active=true;}~WaitingScope(){active=false;}} waitingScope(app.frameWaiting);
                    wake=MsgWaitForMultipleObjectsEx(wait?1:0,wait?&wait:nullptr,timeout,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
                }
                // Reserve permission before dispatch can enter a native modal
                // loop. Its timer and this loop consume the same reservation.
                if(wait&&wake==WAIT_OBJECT_0)app.GrantFramePermit(target);
                if(wake==WAIT_FAILED)app.FrameWaitFailed(wait,GetLastError());
                const auto messagesStarted=MonotonicMs();unsigned messages=0;
                MSG msg{};while(messages<256&&MonotonicMs()-messagesStarted<8&&PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){messages++;if(msg.message==WM_QUIT){running=false;break;}
                    if((msg.message>=WM_KEYFIRST&&msg.message<=WM_KEYLAST)||(msg.message>=WM_MOUSEFIRST&&msg.message<=WM_MOUSELAST&&msg.message!=WM_MOUSEMOVE))app.NoteActivity();
                    if(app.HandleStageUiMessage(msg)){}
                    else if(msg.message==WM_KEYDOWN&&(msg.wParam==VK_F11||(msg.wParam==VK_ESCAPE&&app.fullscreen))){if(!(msg.lParam&(LPARAM(1)<<30)))app.ToggleFullscreen();}
                    else if(msg.message==WM_KEYDOWN&&msg.wParam==VK_OEM_COMMA&&(GetKeyState(VK_CONTROL)&0x8000)){if(!(msg.lParam&(LPARAM(1)<<30)))app.ToggleBackstage();}
                    else if(msg.message==WM_KEYDOWN&&msg.wParam=='L'&&(GetKeyState(VK_CONTROL)&0x8000)){if(!(msg.lParam&(LPARAM(1)<<30)))app.ChangeLyricMode((app.settings.lyricDisplayMode+1)%3);}
                    else if(msg.message==WM_KEYDOWN&&msg.wParam==VK_RETURN&&GetFocus()==app.manualQueryEdit){if(!(msg.lParam&(LPARAM(1)<<30)))app.ManualFind();}
                    else if(msg.message==WM_KEYDOWN&&(app.HandleReadonlyTextTab(msg)||app.HandleLyricReadingKey(msg))){}
                    else if(!IsDialogMessageW(h,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
                if(!running)break;
                app.PumpFrames();
            }}if(instanceMutex)CloseHandle(instanceMutex);
        }
    }catch(const std::exception&){MessageBoxW(nullptr,L"暂时无法打开 HTune。请确认数据目录可写，并稍后重试。",L"HTune 音乐视觉伴侣",MB_ICONERROR);result=1;}
    CoUninitialize();return result;
}
