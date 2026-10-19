#include "netease_playback.h"
#include "client_clock.h"
#include "native_http.h"
#include "song_metadata_cache.h"
#include <tlhelp32.h>
#include <winver.h>
#include <shlobj.h>
#include <winsqlite/winsqlite3.h>
#include "third_party/json.hpp"
#include <objbase.h>
#include <ole2.h>
#include <uiautomation.h>
#include <wrl/client.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cmath>
#include <cwchar>
#include <cstring>
#include <cwctype>
#include <deque>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace stage {
namespace {
using Microsoft::WRL::ComPtr;
using namespace winrt::Windows::Media::Control;
using namespace std::chrono_literals;

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return std::towlower(c); });
    return value;
}
std::wstring Trim(std::wstring value) {
    const auto begin = value.find_first_not_of(L" \t\r\n");
    if (begin == std::wstring::npos) return {};
    return value.substr(begin, value.find_last_not_of(L" \t\r\n") - begin + 1);
}
std::wstring ArtistCreditKey(const std::wstring& value) {
    std::wstring key;size_t start=0;bool first=true;
    while(start<=value.size()){
        const auto end=value.find(L'/',start);if(!first)key+=L'/';first=false;
        key+=Trim(value.substr(start,end==std::wstring::npos?end:end-start));
        if(end==std::wstring::npos)break;start=end+1;
    }
    return key;
}
bool SameArtistCredit(const std::wstring& a,const std::wstring& b){return ArtistCreditKey(a)==ArtistCreditKey(b);}
bool ArtistSourcesCompatible(const std::wstring& a,const std::wstring& b){
    const auto x=ArtistCreditKey(a),y=ArtistCreditKey(b);if(x.empty()||y.empty())return false;
    return x==y||x.starts_with(y+L"/")||y.starts_with(x+L"/");
}
bool RecordArtistMatches(const std::wstring& source,const std::wstring& full,const std::wstring& primary){
    return SameArtistCredit(source,full)||(!primary.empty()&&SameArtistCredit(source,primary));
}
bool IsNeteaseName(const std::wstring& value) {
    const auto lower = Lower(value);
    return lower.find(L"cloudmusic") != std::wstring::npos || lower.find(L"netease") != std::wstring::npos;
}
bool IsKnownMusicMainClass(const std::wstring& value) {
    const auto lower = Lower(value);
    return lower.find(L"orpheusbrowserhost") != std::wstring::npos || lower.find(L"cloudmusic") != std::wstring::npos ||
        lower.find(L"netease") != std::wstring::npos || lower == L"chrome_widgetwin_1" ||
        lower.find(L"cefbrowserwindow") != std::wstring::npos;
}
void AddDiagnostic(std::wstring& text, const std::wstring& addition) {
    if (addition.empty()) return;
    if (!text.empty()) text += L"；";
    text += addition;
}
std::wstring WindowTitle(HWND hwnd) {
    // GetWindowText on another process reads its caption and does not SendMessage
    // into its UI thread. Do not use WM_GETTEXT on arbitrary foreign controls.
    wchar_t text[1024]{};
    const auto length = GetWindowTextW(hwnd, text, static_cast<int>(std::size(text)));
    return length > 0 ? std::wstring(text, length) : std::wstring{};
}
class ScopedProcessHandle {
    HANDLE value_=nullptr;
public:
    explicit ScopedProcessHandle(HANDLE value=nullptr) noexcept:value_(value){}
    ScopedProcessHandle(const ScopedProcessHandle&)=delete;
    ScopedProcessHandle& operator=(const ScopedProcessHandle&)=delete;
    ~ScopedProcessHandle(){if(value_)CloseHandle(value_);}
    explicit operator bool() const noexcept{return value_!=nullptr;}
    HANDLE Get() const noexcept{return value_;}
    HANDLE Release() noexcept{return std::exchange(value_,nullptr);}
};
uint64_t ProcessCreationTime(HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
    return (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}
std::wstring ProcessImagePath(HANDLE process) {
    wchar_t path[32768]{};
    DWORD length = static_cast<DWORD>(std::size(path));
    std::wstring result;
    if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT && QueryFullProcessImageNameW(process, 0, path, &length))
        result.assign(path, length);
    return result;
}
std::wstring ExecutablePath(DWORD pid, uint64_t* createdAt=nullptr) {
    if (createdAt) *createdAt=0;
    ScopedProcessHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,FALSE,pid));
    if (!process) return {};
    auto result=ProcessImagePath(process.Get());
    if (createdAt) { *createdAt=ProcessCreationTime(process.Get()); if (!*createdAt) result.clear(); }
    return result;
}
std::wstring FileVersion(const std::wstring& path) {
    DWORD ignored = 0;
    const auto length = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!length || length > 1024 * 1024) return {};
    std::vector<unsigned char> data(length);
    if (!GetFileVersionInfoW(path.c_str(), 0, length, data.data())) return {};
    struct Translation { WORD language, codePage; };
    Translation* translations = nullptr;
    UINT translationBytes = 0;
    std::vector<Translation> queryLanguages;
    if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &translationBytes)) {
        for (UINT i = 0; i < std::min<UINT>(translationBytes / sizeof(Translation), 16); ++i)
            queryLanguages.push_back(translations[i]);
    }
    // Some installers omit Translation while preserving common string blocks.
    queryLanguages.push_back({0x0804, 0x04b0});
    queryLanguages.push_back({0x0409, 0x04b0});
    queryLanguages.push_back({0x0409, 0x04e4});
    for (const auto* field : {L"FileVersion", L"ProductVersion"}) for (const auto& translation : queryLanguages) {
        wchar_t query[96]{};
        swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\%ls", static_cast<unsigned>(translation.language), static_cast<unsigned>(translation.codePage), field);
        wchar_t* text = nullptr;
        UINT textLength = 0;
        if (VerQueryValueW(data.data(), query, reinterpret_cast<void**>(&text), &textLength) && text && textLength > 1 && textLength <= 256) {
            auto value = Trim(std::wstring(text, textLength - (text[textLength - 1] == L'\0' ? 1 : 0)));
            if (!value.empty()) return value;
        }
    }
    // The fixed resource revision can differ from the version shown to users;
    // it is a fallback only, never preferred over FileVersion/ProductVersion.
    VS_FIXEDFILEINFO* info = nullptr;
    UINT infoSize = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &infoSize) ||
        infoSize < sizeof(*info) || info->dwSignature != 0xfeef04bd) return {};
    return std::to_wstring(HIWORD(info->dwFileVersionMS)) + L"." + std::to_wstring(LOWORD(info->dwFileVersionMS)) +
        L"." + std::to_wstring(HIWORD(info->dwFileVersionLS)) + L"." + std::to_wstring(LOWORD(info->dwFileVersionLS));
}
struct ClientProcess {
    DWORD pid = 0, parentPid = 0;
    std::wstring name, path, version;
    HWND hwnd = nullptr;
    std::wstring caption;
    bool visible = false, minimized = false;
    std::wstring windowClass;
    int windowScore = -1;
    bool windowAmbiguous = false;
    uint64_t createdAtTicks = 0;
};
static_assert(std::is_nothrow_swappable_v<ClientProcess>);
class BoundClientProcess {
    HANDLE process_ = nullptr;
    ClientProcess client_;
public:
    BoundClientProcess() = default;
    BoundClientProcess(const BoundClientProcess&) = delete;
    BoundClientProcess& operator=(const BoundClientProcess&) = delete;
    ~BoundClientProcess() { Reset(); }
    void Reset() { if (process_) CloseHandle(process_); process_=nullptr; client_={}; }
    bool Running() const noexcept { return process_&&WaitForSingleObject(process_,0)==WAIT_TIMEOUT; }
    void Swap(BoundClientProcess& other) noexcept {
        std::swap(process_,other.process_);std::swap(client_,other.client_);
    }
    bool Select(const ClientProcess& client) {
        if (!client.createdAtTicks || !client.pid || client.path.empty()) return false;
        // Finish every potentially throwing copy before changing the retained
        // handle/identity pair.  A failed diagnostic allocation must never
        // leave a new process handle carrying the previous process's proof.
        ClientProcess nextClient = client;
        if (process_ && client_.pid==client.pid && client_.createdAtTicks==client.createdAtTicks &&
            Lower(client_.path)==Lower(client.path) && WaitForSingleObject(process_,0)==WAIT_TIMEOUT) {
            std::swap(client_,nextClient); return true;
        }
        ScopedProcessHandle pending(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,FALSE,client.pid));
        if (!pending) return false;
        const auto next=pending.Get();
        const bool matches=ProcessCreationTime(next)==client.createdAtTicks && Lower(ProcessImagePath(next))==Lower(client.path) &&
            WaitForSingleObject(next,0)==WAIT_TIMEOUT;
        if (!matches)return false;
        HANDLE previous=process_;
        process_=pending.Release();std::swap(client_,nextClient);
        if(previous)CloseHandle(previous);
        return true;
    }
    std::optional<ClientProcess> ReuseRunning() {
        if (!process_ || WaitForSingleObject(process_,0)!=WAIT_TIMEOUT) { Reset(); return std::nullopt; }
        auto client=client_;
        // The retained kernel instance is current; its old window is not.
        client.hwnd=nullptr; client.caption.clear(); client.windowClass.clear();
        client.visible=false; client.minimized=false; client.windowAmbiguous=false; client.windowScore=-1;
        return client;
    }
    std::optional<ClientProcess> ReuseListed(const std::vector<ClientProcess>& clients) {
        if (!process_ || WaitForSingleObject(process_,0)!=WAIT_TIMEOUT) { Reset(); return std::nullopt; }
        const auto found=std::find_if(clients.begin(),clients.end(),[&](const auto& client) {
            return client.pid==client_.pid&&client.createdAtTicks==client_.createdAtTicks&&
                Lower(client.path)==Lower(client_.path);
        });
        if(found==clients.end()||ProcessCreationTime(process_)!=found->createdAtTicks||
           Lower(ProcessImagePath(process_))!=Lower(found->path)){
            Reset();return std::nullopt;
        }
        // The complete scan saw the exact previously verified process instance.
        // Refresh only its current window fields; never choose a new ambiguous root.
        ClientProcess refreshed=*found;
        std::swap(client_,refreshed);
        return client_;
    }
};
struct WindowSearch { std::vector<ClientProcess>* processes; };
bool IsAuxiliaryWindowText(const std::wstring& text) {
    const auto lower = Lower(text);
    return lower.find(L"systemhint") != std::wstring::npos || lower.find(L"messagewindow") != std::wstring::npos ||
        lower.find(L"tooltip") != std::wstring::npos || lower.find(L"notifyicon") != std::wstring::npos ||
        lower.find(L"desktop lyric") != std::wstring::npos || lower.find(L"desktoplyrics") != std::wstring::npos ||
        lower.find(L"桌面歌词") != std::wstring::npos;
}
BOOL CALLBACK FindClientWindows(HWND hwnd, LPARAM param) {
    auto& items = *reinterpret_cast<WindowSearch*>(param)->processes;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    auto it = std::find_if(items.begin(), items.end(), [=](const auto& p) { return p.pid == pid; });
    if (it == items.end() || GetWindow(hwnd, GW_OWNER) || (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) return TRUE;
    const auto title = WindowTitle(hwnd);
    wchar_t classText[256]{};
    GetClassNameW(hwnd, classText, static_cast<int>(std::size(classText)));
    const std::wstring windowClass(classText);
    if (title.empty() || IsAuxiliaryWindowText(title) || IsAuxiliaryWindowText(windowClass)) return TRUE;
    RECT rect{};
    if (!GetWindowRect(hwnd, &rect)) return TRUE;
    if (IsIconic(hwnd)) {
        WINDOWPLACEMENT placement{sizeof(placement)};
        if (GetWindowPlacement(hwnd, &placement)) rect = placement.rcNormalPosition;
    }
    if (rect.right - rect.left < 320 || rect.bottom - rect.top < 220) return TRUE;
    const bool knownMainClass = IsKnownMusicMainClass(windowClass);
    const bool visible = IsWindowVisible(hwnd) != FALSE;
    const bool brandedCaption = title.find(L"网易云") != std::wstring::npos || IsNeteaseName(title);
    // Unknown helper classes and generic utility captions are insufficient to
    // identify a music main window. A hidden hint never replaces the real app.
    if (!knownMainClass && !brandedCaption) return TRUE;
    int score = knownMainClass ? 100 : 40;
    if (visible) score += 20;
    if (brandedCaption) score += 10;
    if (rect.right - rect.left >= 640 && rect.bottom - rect.top >= 400) score += 10;
    if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_APPWINDOW) score += 5;
    if (!it->hwnd || score > it->windowScore) {
        it->hwnd = hwnd;
        it->caption = title;
        it->visible = visible;
        it->minimized = IsIconic(hwnd) != FALSE;
        it->windowClass = windowClass;
        it->windowScore = score;
        it->windowAmbiguous = false;
    } else if (score == it->windowScore && hwnd != it->hwnd) {
        it->windowAmbiguous = true;
    }
    return TRUE;
}
std::vector<ClientProcess> DiscoverClients(bool* complete=nullptr, std::wstring* diagnostic=nullptr) {
    if (complete) *complete=false;
    if (diagnostic) diagnostic->clear();
    std::vector<ClientProcess> items;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        if (diagnostic) *diagnostic=L"进程快照失败，不能据空列表判断网易云已退出，Windows="+std::to_wstring(GetLastError());
        return items;
    }
    struct CandidateEntry {DWORD pid=0,parentPid=0;uint64_t createdAtTicks=0;std::wstring name;bool readable=false;};
    std::vector<CandidateEntry> candidates;
    PROCESSENTRY32W entry{sizeof(entry)};
    if (Process32FirstW(snapshot, &entry)) do {
        const auto name = Lower(entry.szExeFile);
        // The music client is cloudmusic.exe. Helpers/webviews are never chosen
        // as the audio target merely because they contain "netease" in a path.
        if (name != L"cloudmusic.exe" && name != L"neteasecloudmusic.exe") continue;
        uint64_t createdAt=0;
        auto path = ExecutablePath(entry.th32ProcessID,&createdAt);
        candidates.push_back({entry.th32ProcessID,entry.th32ParentProcessID,createdAt,name,!path.empty()});
        if (path.empty()) continue;
        // Version resources are optional metadata, fetched for the selected
        // instance by a separate worker after discovery has been published.
        items.push_back({entry.th32ProcessID, entry.th32ParentProcessID, name, path, {}});
        items.back().createdAtTicks=createdAt;
    } while (Process32NextW(snapshot, &entry));
    const auto traversalError=GetLastError();
    CloseHandle(snapshot);
    size_t unreadableChildren=0;bool unreadableRoot=false;
    for(const auto& candidate:candidates){
        if(candidate.readable)continue;
        const bool sameNameParent=std::any_of(candidates.begin(),candidates.end(),[&](const auto& parent){
            return parent.pid!=candidate.pid&&parent.pid==candidate.parentPid&&parent.name==candidate.name&&
                parent.createdAtTicks>0&&candidate.createdAtTicks>0&&parent.createdAtTicks<=candidate.createdAtTicks;
        });
        if(sameNameParent)++unreadableChildren;else unreadableRoot=true;
    }
    const bool traversed=traversalError==ERROR_NO_MORE_FILES;
    const bool finished=traversed&&!unreadableRoot;
    if (complete) *complete=finished;
    if(diagnostic){
        if(!traversed)*diagnostic=L"进程遍历未完整结束，不使用部分候选列表，Windows="+std::to_wstring(traversalError);
        else if(unreadableRoot)*diagnostic=L"有可能成为主进程的网易云实例暂不可核验，不从部分候选中猜选";
        else if(unreadableChildren)*diagnostic=L"有 "+std::to_wstring(unreadableChildren)+L" 个网易云子进程暂不可读；主进程候选仍可独立核验";
    }
    WindowSearch search{&items};
    EnumWindows(FindClientWindows, reinterpret_cast<LPARAM>(&search));
    for (auto& item : items) if (item.windowAmbiguous) {
        item.hwnd = nullptr;
        item.caption.clear();
        item.windowClass.clear();
        item.visible = false;
        item.minimized = false;
    }
    return items;
}
std::optional<ClientProcess> PickClient(const std::vector<ClientProcess>& items) {
    std::vector<ClientProcess> roots;
    for (const auto& process : items) {
        const bool sameNameParent = std::any_of(items.begin(), items.end(), [&](const auto& p) {
            return p.pid != process.pid && p.pid == process.parentPid && p.name == process.name &&
                p.createdAtTicks > 0 && process.createdAtTicks > 0 && p.createdAtTicks <= process.createdAtTicks;
        });
        if (!sameNameParent) roots.push_back(process);
    }
    if (roots.size() == 1) return roots.front();
    const auto visibleCount = std::count_if(roots.begin(), roots.end(), [](const auto& p) { return p.hwnd && p.visible; });
    if (visibleCount == 1) return *std::find_if(roots.begin(), roots.end(), [](const auto& p) { return p.hwnd && p.visible; });
    return std::nullopt;
}
bool IsRootClient(const ClientProcess& process,const std::vector<ClientProcess>& items) {
    return std::none_of(items.begin(),items.end(),[&](const auto& parent){
        return parent.pid!=process.pid&&parent.pid==process.parentPid&&parent.name==process.name&&
            parent.createdAtTicks>0&&process.createdAtTicks>0&&parent.createdAtTicks<=process.createdAtTicks;
    });
}
size_t RootClientCount(const std::vector<ClientProcess>& items) {
    return static_cast<size_t>(std::count_if(items.begin(),items.end(),[&](const auto& process){
        return IsRootClient(process,items);
    }));
}
using ClientInstanceKey=std::pair<DWORD,uint64_t>;
bool SameClientInstance(const ClientProcess& first,const ClientProcess& second) {
    return first.pid==second.pid&&first.createdAtTicks==second.createdAtTicks&&Lower(first.path)==Lower(second.path);
}
struct NativeClockClientChoice {
    std::optional<ClientProcess> selected;
    std::wstring diagnostic;
};
class NativeClockTargetProbe {
    struct Slot {
        ClientInstanceKey instance;
        std::unique_ptr<ClientClockReader> reader=std::make_unique<ClientClockReader>();
        std::wstring songId;
        int64_t positionMs=0,durationMs=0,sampledAtMs=0;
        bool stable=false;
    };
    struct RootEvidence {
        ClientInstanceKey instance;
        std::wstring songId;
        int64_t durationMs=0;
    };
    struct PendingOwner {
        ClientInstanceKey instance;
        std::vector<RootEvidence> roots;
        int64_t deadlineMs=0;
    };
    std::vector<Slot> slots_;
    int64_t topologyObservedAtMs_=0;
    std::optional<ClientInstanceKey> movingWinner_;
    unsigned movingWinnerSamples_=0;
    std::optional<ClientInstanceKey> provenWinner_;
    std::vector<RootEvidence> provenRoots_;
    std::optional<PendingOwner> pendingOwner_;

    static bool CompatibleDuration(int64_t left,int64_t right) {
        if(left<=0||right<=0)return false;
        return std::abs(left-right)<=std::max<int64_t>(1000,std::max(left,right)/200);
    }
    const Slot* FindSlot(const ClientInstanceKey& instance) const {
        const auto found=std::find_if(slots_.begin(),slots_.end(),[&](const Slot& slot){return slot.instance==instance;});
        return found==slots_.end()?nullptr:&*found;
    }
    bool CaptureEvidence(std::vector<RootEvidence>& evidence) const {
        evidence.clear();
        try{evidence.reserve(slots_.size());}catch(...){return false;}
        for(const auto& slot:slots_){
            if(!slot.stable||!SameSongId(slot.songId,slot.songId)||slot.durationMs<=0){evidence.clear();return false;}
            try{evidence.push_back(RootEvidence{slot.instance,slot.songId,slot.durationMs});}
            catch(...){evidence.clear();return false;}
        }
        std::sort(evidence.begin(),evidence.end(),[](const RootEvidence& left,const RootEvidence& right){
            return left.instance<right.instance;
        });
        return !evidence.empty();
    }
    bool EvidenceMatches(const std::vector<RootEvidence>& evidence,bool requireStable) const {
        if(evidence.empty()||evidence.size()!=slots_.size())return false;
        return std::all_of(evidence.begin(),evidence.end(),[&](const RootEvidence& expected){
            const auto slot=FindSlot(expected.instance);
            return slot&&(!requireStable||slot->stable)&&SameSongId(slot->songId,expected.songId)&&
                CompatibleDuration(slot->durationMs,expected.durationMs);
        });
    }
    bool EvidenceContradicted(const std::vector<RootEvidence>& evidence) const {
        return std::any_of(evidence.begin(),evidence.end(),[&](const RootEvidence& expected){
            const auto slot=FindSlot(expected.instance);
            if(!slot||!SameSongId(slot->songId,slot->songId))return false;
            return !SameSongId(slot->songId,expected.songId)||
                (slot->durationMs>0&&!CompatibleDuration(slot->durationMs,expected.durationMs));
        });
    }
    void ForgetProven() {
        provenWinner_.reset();provenRoots_.clear();
    }
    void ResetOwnershipEvidence() {
        ForgetProven();pendingOwner_.reset();
    }
    bool RememberProven(const ClientInstanceKey& instance) {
        std::vector<RootEvidence> evidence;
        if(!CaptureEvidence(evidence)||std::none_of(evidence.begin(),evidence.end(),[&](const RootEvidence& root){
            return root.instance==instance;
        }))return false;
        provenWinner_=instance;provenRoots_=std::move(evidence);return true;
    }
    bool BeginPending(int64_t now) {
        if(!provenWinner_||provenRoots_.empty())return false;
        try{
            PendingOwner pending{*provenWinner_,provenRoots_,now+2000};
            pendingOwner_=std::move(pending);
        }catch(...){return false;}
        ForgetProven();return true;
    }
public:
    void Reset() {
        slots_.clear();topologyObservedAtMs_=0;movingWinner_.reset();movingWinnerSamples_=0;ResetOwnershipEvidence();
    }
    int64_t TopologyObservedAtMs() const noexcept { return topologyObservedAtMs_; }
    NativeClockClientChoice Observe(const std::vector<ClientProcess>& items) {
        NativeClockClientChoice result;std::vector<ClientInstanceKey> roots;
        for(const auto& process:items)if(IsRootClient(process,items))roots.emplace_back(process.pid,process.createdAtTicks);
        std::sort(roots.begin(),roots.end());
        std::vector<ClientInstanceKey> previous;
        previous.reserve(slots_.size());for(const auto& slot:slots_)previous.push_back(slot.instance);
        std::sort(previous.begin(),previous.end());
        if(previous!=roots){
            // Evidence collected under another process topology is not
            // comparable: every surviving and new root starts at sample one.
            slots_.clear();
            topologyObservedAtMs_=MonotonicMs();
            movingWinner_.reset();movingWinnerSamples_=0;ResetOwnershipEvidence();
        }
        std::vector<ClientProcess> moving,stableCandidates;
        size_t candidates=0,stable=0;
        for(const auto& process:items){
            if(!IsRootClient(process,items))continue;
            const ClientInstanceKey key{process.pid,process.createdAtTicks};
            auto found=std::find_if(slots_.begin(),slots_.end(),[&](const Slot& slot){return slot.instance==key;});
            if(found==slots_.end()){slots_.push_back(Slot{key});found=std::prev(slots_.end());}
            ClientClockObservation observation;
            try{observation=found->reader->Observe(process.pid,PlayState::Unknown,process.createdAtTicks);}
            catch(...){found->reader->Reset();}
            found->stable=false;
            bool advances=false;
            if(observation.candidate){
                ++candidates;
                if(observation.stable){found->stable=true;++stable;stableCandidates.push_back(process);}
                if(observation.stable&&SameSongId(found->songId,observation.songId)&&found->sampledAtMs>0&&
                        observation.sampledAtMs>found->sampledAtMs&&observation.durationMs>0&&found->durationMs>0){
                    const auto elapsed=observation.sampledAtMs-found->sampledAtMs;
                    const auto advance=observation.positionMs-found->positionMs;
                    const auto durationAllowance=std::max<int64_t>(1000,std::max(observation.durationMs,found->durationMs)/200);
                    advances=elapsed<=1000&&std::abs(observation.durationMs-found->durationMs)<=durationAllowance&&
                        advance>25&&advance>=elapsed/2&&std::abs(advance-elapsed)<=500;
                }
                found->songId=observation.songId;found->positionMs=observation.positionMs;
                found->durationMs=observation.durationMs;found->sampledAtMs=observation.sampledAtMs;
            }else{
                found->songId.clear();found->positionMs=0;found->durationMs=0;found->sampledAtMs=0;
            }
            if(advances)moving.push_back(process);
        }
        const bool allRootsComparable=!roots.empty()&&stable==roots.size();
        const auto now=MonotonicMs();
        bool pendingWaiting=false;
        if(pendingOwner_){
            const auto competingMovement=std::any_of(moving.begin(),moving.end(),[&](const ClientProcess& process){
                return process.pid!=pendingOwner_->instance.first||process.createdAtTicks!=pendingOwner_->instance.second;
            });
            const bool signatureContradicted=EvidenceContradicted(pendingOwner_->roots);
            if(now>pendingOwner_->deadlineMs||competingMovement||signatureContradicted){
                pendingOwner_.reset();
                result.diagnostic=competingMovement?
                    L"其他主进程在复核期间提供前进证据，撤销待恢复的原生所有权":
                    signatureContradicted?L"至少一个主进程的歌曲ID或时长已经变化，撤销历史原生所有权":
                    L"原生所有权的短暂恢复窗口已超时，重新等待连续前进证据";
            }else if(allRootsComparable){
                const auto owner=std::find_if(stableCandidates.begin(),stableCandidates.end(),[&](const ClientProcess& process){
                    return process.pid==pendingOwner_->instance.first&&process.createdAtTicks==pendingOwner_->instance.second;
                });
                const bool ownerOnlyMovement=moving.empty()||(moving.size()==1&&owner!=stableCandidates.end()&&
                    moving.front().pid==owner->pid&&moving.front().createdAtTicks==owner->createdAtTicks);
                if(owner!=stableCandidates.end()&&ownerOnlyMovement&&EvidenceMatches(pendingOwner_->roots,true)){
                    const auto instance=pendingOwner_->instance;
                    pendingOwner_.reset();
                    if(RememberProven(instance)){
                        movingWinner_=instance;movingWinnerSamples_=2;result.selected=*owner;
                        result.diagnostic=L"全部主进程恢复稳定，既有实例的歌曲ID与时长未变，已恢复原生所有权";
                    }
                }else{
                    pendingOwner_.reset();
                    result.diagnostic=L"主进程恢复采样后未能复核既有实例，撤销历史原生所有权";
                }
            }else{
                pendingWaiting=true;
                result.diagnostic=L"主进程原生采样短暂不完整，暂停发布并限时复核既有所有权";
            }
        }
        if(!result.selected&&!pendingWaiting&&allRootsComparable&&moving.size()==1){
            const ClientInstanceKey winner{moving.front().pid,moving.front().createdAtTicks};
            if(provenWinner_&&*provenWinner_!=winner)ForgetProven();
            if(provenWinner_&&*provenWinner_==winner){
                movingWinner_=winner;movingWinnerSamples_=std::max(movingWinnerSamples_,2u);
            }else if(movingWinner_&&*movingWinner_==winner)++movingWinnerSamples_;
            else{movingWinner_=winner;movingWinnerSamples_=1;}
            if(provenWinner_&&*provenWinner_==winner&&RememberProven(winner)){
                result.selected=moving.front();
                result.diagnostic=L"多主进程均可比较，已证明实例仍是唯一前进中的原生时钟";
            }else if(movingWinnerSamples_>=2&&RememberProven(winner)){
                result.selected=moving.front();
                result.diagnostic=L"多主进程均可比较，只有一个实例连续通过歌曲ID、时长和实时进度前进核验";
            }else result.diagnostic=L"所有主进程均已有可比较样本；唯一前进实例仍需下一次样本复核";
        }else if(!result.selected&&!pendingWaiting){
            movingWinner_.reset();movingWinnerSamples_=0;
            if(!allRootsComparable){
                if(provenWinner_){
                    const auto competingMovement=std::any_of(moving.begin(),moving.end(),[&](const ClientProcess& process){
                        return process.pid!=provenWinner_->first||process.createdAtTicks!=provenWinner_->second;
                    });
                    const bool signatureContradicted=EvidenceContradicted(provenRoots_);
                    if(competingMovement||signatureContradicted||!BeginPending(now)){
                        ForgetProven();
                        result.diagnostic=competingMovement?
                            L"其他主进程已提供前进证据，撤销历史原生所有权":
                            L"至少一个主进程的歌曲ID或时长不再一致，撤销历史原生所有权";
                    }else{
                        pendingWaiting=true;
                        result.diagnostic=L"至少一个主进程暂时缺少稳定样本，暂停发布并限时复核既有原生所有权";
                    }
                }else result.diagnostic=L"至少一个主进程缺少稳定可比较的原生样本，不以无证据推断其未播放";
            }else if(moving.size()>1){
                ResetOwnershipEvidence();
                result.diagnostic=L"多个主进程同时通过原生进度前进核验，撤销历史所有权，不任意选择";
            }
            else if(candidates||stable)result.diagnostic=L"原生候选尚未形成唯一的实时进度前进证据";
            else result.diagnostic=L"多主进程中尚未取得可核验的原生歌曲时钟";
        }
        if(!result.selected&&!pendingWaiting&&allRootsComparable&&moving.empty()&&provenWinner_){
            const auto found=std::find_if(stableCandidates.begin(),stableCandidates.end(),[&](const ClientProcess& process){
                return process.pid==provenWinner_->first&&process.createdAtTicks==provenWinner_->second;
            });
            const auto instance=*provenWinner_;
            if(found!=stableCandidates.end()&&EvidenceMatches(provenRoots_,true)&&RememberProven(instance)){
                result.selected=*found;
                result.diagnostic+=L"；沿用该实例此前的连续前进所有权证据";
            }else ForgetProven();
        }
        return result;
    }
};
void SplitCaption(const std::wstring& caption, std::wstring& title, std::wstring& artist) {
    // Callers may reuse identity storage across songs. Every parse owns both
    // outputs, including empty, brand-only and title-only captions.
    title.clear();
    artist.clear();
    auto value = Trim(caption);
    bool hadBrandSuffix = false;
    for (const auto* suffix : {L" - 网易云音乐", L" | 网易云音乐", L" – 网易云音乐"}) {
        const std::wstring text(suffix);
        if (value.ends_with(text)) { value.resize(value.size() - text.size()); hadBrandSuffix = true; }
    }
    const auto lower = Lower(value);
    if (value.empty() || value == L"网易云音乐" || value == L"网易云" || lower == L"netease cloud music" ||
        lower == L"cloudmusic" || lower == L"cloud music" || lower == L"cloudmusic.exe" ||
        lower == L"neteasecloudmusic" || IsAuxiliaryWindowText(value)) return;
    const auto split = value.rfind(L" - ");
    if (split != std::wstring::npos && split > 0 && split + 3 < value.size()) {
        title = Trim(value.substr(0, split));
        artist = Trim(value.substr(split + 3));
    } else if (hadBrandSuffix) title = value;
}
std::optional<int64_t> ParseClock(const std::wstring& input) {
    auto value = Trim(input);
    if (value.size() < 4 || value.size() > 8 || value.back() == L':') return std::nullopt;
    std::vector<int64_t> parts;
    size_t start = 0;
    while (start < value.size()) {
        auto end = value.find(L':', start);
        if (end == std::wstring::npos) end = value.size();
        const auto count = end - start;
        if (count == 0 || count > 3 || (start > 0 && count != 2)) return std::nullopt;
        int64_t part = 0;
        for (size_t i = start; i < end; ++i) {
            if (value[i] < L'0' || value[i] > L'9') return std::nullopt;
            part = part * 10 + value[i] - L'0';
        }
        if (start > 0 && part > 59) return std::nullopt;
        parts.push_back(part);
        start = end + 1;
    }
    if (parts.size() != 2 && parts.size() != 3) return std::nullopt;
    const int64_t seconds = parts.size() == 2 ? parts[0] * 60 + parts[1] : parts[0] * 3600 + parts[1] * 60 + parts[2];
    if (seconds > 24 * 3600) return std::nullopt;
    return seconds * 1000;
}
struct ClockPair { int64_t position = 0, duration = 0; };
std::optional<ClockPair> ParsePair(const std::wstring& text) {
    const auto slash = text.find_first_of(L"/|");
    if (slash == std::wstring::npos || text.find_first_of(L"/|", slash + 1) != std::wstring::npos) return std::nullopt;
    auto a = ParseClock(text.substr(0, slash));
    auto b = ParseClock(text.substr(slash + 1));
    if (!a || !b || *b <= 0 || *a > *b) return std::nullopt;
    return ClockPair{*a, *b};
}
struct UiObservation {
    DWORD pid = 0;
    HWND hwnd = nullptr;
    std::wstring nativeCaption;
    int64_t sampledAtMs = 0;
    int64_t stateSampledAtMs = 0;
    bool hasClock = false;
    ClockPair clock;
    PlayState state = PlayState::Unknown;
    std::wstring diagnostic;
};
struct SongRecord {
    std::wstring id, title, artist, primaryArtist, album, diagnostic;
    int64_t durationMs = 0, playtimeUtcMs = 0;
    bool metadataValidated = false, nativeCorroborated = false, fromMetadataCache = false;
};
struct NativeDetailsRequest {
    ClientClockObservation identity;
    DWORD pid = 0;
    uint64_t epoch = 0, revision = 0;
    std::stop_token generationStop;
    std::stop_source httpStop;
    // The immutable request is shared with the single HTTPS worker. These
    // completion fields are read/written only under SharedState::mutex.
    bool started = false, complete = false;
    SongRecord result;
};
bool NativeDurationMatches(int64_t metadataDuration, int64_t nativeDuration) {
    return SongMetadataDurationMatches(metadataDuration,nativeDuration);
}
struct ClientVersionRequest {
    const DWORD pid;
    const uint64_t processCreatedAtTicks;
    const std::wstring path;
    // Completion fields are protected by SharedState::mutex. The worker owns
    // immutable identity fields while reading outside that lock.
    std::wstring version;
    bool started=false,complete=false;
    int64_t retryAtMs=0;
    explicit ClientVersionRequest(const ClientProcess& client):pid(client.pid),
        processCreatedAtTicks(client.createdAtTicks),path(client.path) {}
};
bool SameClientVersionPath(const std::wstring& first,const std::wstring& second) noexcept {
    return !first.empty()&&!second.empty()&&
        CompareStringOrdinal(first.c_str(),-1,second.c_str(),-1,TRUE)==CSTR_EQUAL;
}
bool ClientVersionMatches(const ClientVersionRequest& request,const ClientProcess& client) noexcept {
    return request.pid!=0&&request.processCreatedAtTicks!=0&&request.pid==client.pid&&
        request.processCreatedAtTicks==client.createdAtTicks&&SameClientVersionPath(request.path,client.path);
}
struct ClientDiscoverySnapshot {
    std::vector<ClientProcess> clients;
    std::optional<ClientProcess> selected;
    std::wstring diagnostic;
    bool ready=false,complete=false,retained=false,nativeOwnershipProven=false;
    uint64_t revision=0,metadataNativeRevision=0;
    // Conservative age of the topology proof: completion must not refresh a
    // scan whose early process/ownership evidence is already too old.
    int64_t scanStartedAtMs=0,observedAtMs=0;
};
enum class NativeIdentityAvailability { None, Current, Reacquiring, Expired };
struct NativeCacheOwner {
    DWORD pid = 0;
    uint64_t processCreatedAtTicks = 0, epoch = 0, identityRevision = 0;
    std::wstring songId;
};
struct NativeCacheWrite {
    SongMetadata metadata;
    NativeCacheOwner owner;
};
struct NativeCacheDiagnostic {
    NativeCacheOwner owner;
    std::wstring text;
};
struct SharedState {
    std::atomic<bool> stop = false;
    std::atomic<bool> metadataDirty = true;
    std::atomic<uint64_t> metadataRevision = 0;
    // Unlike metadataRevision, this counter advances only for a real SMTC
    // MediaPropertiesChanged event. Internal invalidations (discovery/native
    // expiry) therefore cannot make an unchanged system-session cache look
    // like newly observed song metadata.
    // Each SMTC attachment owns a generation. Event revisions are meaningful
    // only inside that generation, so a callback queued by a detached session
    // can never make a new session's cached metadata look freshly observed.
    std::atomic<uint64_t> smtcSessionGeneration = 0;
    std::atomic<uint64_t> smtcMediaEventRevision = 0;
    std::atomic<bool> wake = false;
    std::atomic<bool> clientClockEnabled = true;
    std::atomic<uint64_t> clientClockModeRevision = 1;
    std::recursive_mutex callbackMutex;
    NeteasePlaybackMonitor::Callback callback;
    std::mutex mutex;
    std::condition_variable cv;
    std::condition_variable discoveryCv;
    std::condition_variable clientVersionCv;
    ClientDiscoverySnapshot discovery;
    std::shared_ptr<ClientVersionRequest> clientVersionRequest;
    uint64_t clientVersionRevision=0;
    bool clientVersionWorkerAvailable=false;
    DWORD requestedPid = 0;
    HWND requestedWindow = nullptr;
    // Exact native caption captured by the monitor. UIA returns that same raw
    // caption; comparing it with a reconstructed "title\nartist" identity
    // rejects valid observations because NetEase normally uses "title - artist".
    std::wstring requestedCaption;
    uint64_t uiGeneration = 0;
    UiObservation ui;
    std::condition_variable nativeCv;
    uint64_t nativeEpoch=0,nativeObservationEpoch=0;
    DWORD nativeObservationPid=0;
    ClientClockObservation nativeObservation;
    ClientProcess requestedClient;
    SongRecord nativeRecord;
    DWORD nativeRecordPid = 0;
    uint64_t nativeRecordEpoch = 0, nativeIdentityRevision = 0;
    std::wstring nativeIdentityId, nativeLookupDiagnostic;
    NativeIdentityAvailability nativeIdentityAvailability=NativeIdentityAvailability::None;
    uint64_t nativeAvailabilityRevision=1,nativeAvailabilityDeliveredRevision=0;
    uint64_t nativeExpiredSmtcSessionGeneration=0,nativeExpiredAfterSmtcEventRevision=0;
    // Last parseable caption whose discovery sample completed no later than a
    // stable native sample for this exact process/identity. A later caption
    // can therefore be compared without reading mutable window state at loss.
    DWORD nativeStableCaptionPid=0;
    uint64_t nativeStableCaptionCreatedAt=0;
    int64_t nativeIdentityConfirmedAtMs=0;
    std::wstring nativeStableCaptionTitle,nativeStableCaptionArtist;
    int64_t nativeLossStartedAtMs=0;
    std::stop_source nativeLookupStop;
    std::shared_ptr<NativeDetailsRequest> nativeDetailsRequest;
    std::deque<NativeCacheWrite> nativeCacheWrites;
    NativeCacheDiagnostic nativeCacheDiagnostic;
    std::condition_variable nativeCacheCv;
    std::condition_variable detailsCv;
    PlaybackSnapshot nativeSeed;
    DWORD stateHintPid=0;
    PlayState stateHint=PlayState::Unknown;
    int64_t stateHintSampledAtMs=0;
    bool nativeSeedReady=false,nativePreferred=true,nativeSeedIdentity=false;
    int64_t lastNativeDeliveredAtMs=0;
    std::wstring lastNativeDeliveredId;
};
// Optional metadata must never turn a complete topology into a failed scan.
// Both helpers are called with SharedState::mutex held and do no file I/O.
void ApplyClientVersionLocked(const SharedState& state,std::vector<ClientProcess>& clients) noexcept {
    const auto& request=state.clientVersionRequest;
    if(!request||!request->complete||request->version.empty())return;
    for(auto& client:clients)if(ClientVersionMatches(*request,client)){
        try{client.version=request->version;}catch(...){}
    }
}
void RequestClientVersionLocked(SharedState& state) noexcept {
    const auto& client=state.requestedClient;
    if(state.stop.load()||!state.clientVersionWorkerAvailable||!client.pid||!client.createdAtTicks||client.path.empty()){
        state.clientVersionRequest.reset();return;
    }
    const auto& request=state.clientVersionRequest;
    if(request&&ClientVersionMatches(*request,client)&&
        (!request->complete||!request->version.empty()||MonotonicMs()<request->retryAtMs))return;
    try{
        auto next=std::make_shared<ClientVersionRequest>(client);
        state.clientVersionRequest=std::move(next);
        state.clientVersionCv.notify_one();
    }catch(...){/* optional version allocation cannot invalidate the binding */}
}
void ClientVersionWorker(const std::shared_ptr<SharedState>& state) {
    while(!state->stop.load()){
        std::shared_ptr<ClientVersionRequest> request;
        {
            std::unique_lock lock(state->mutex);
            state->clientVersionCv.wait(lock,[&]{return state->stop.load()||
                (state->clientVersionRequest&&!state->clientVersionRequest->started);});
            if(state->stop.load())return;
            request=state->clientVersionRequest;request->started=true;
        }
        std::wstring version;
        try{
            ScopedProcessHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,request->pid));
            if(process&&ProcessCreationTime(process.Get())==request->processCreatedAtTicks&&
                SameClientVersionPath(ProcessImagePath(process.Get()),request->path)){
                version=FileVersion(request->path);
                if(ProcessCreationTime(process.Get())!=request->processCreatedAtTicks||
                    !SameClientVersionPath(ProcessImagePath(process.Get()),request->path))version.clear();
            }
        }catch(...){version.clear();}
        {
            std::lock_guard lock(state->mutex);
            if(state->stop.load())return;
            if(state->clientVersionRequest!=request||!ClientVersionMatches(*request,state->requestedClient))continue;
            request->version=std::move(version);request->complete=true;
            request->retryAtMs=MonotonicMs()+5000;
            ++state->clientVersionRevision;
        }
        state->discoveryCv.notify_all();
    }
}
// Caller holds mutex; cancellation callbacks only signal the HTTP request.
void CancelNativeDetailsLocked(SharedState& state) {
    if (state.nativeDetailsRequest) state.nativeDetailsRequest->httpStop.request_stop();
    state.nativeDetailsRequest.reset();
}
bool NativeDetailsMatch(const NativeDetailsRequest& request, DWORD pid, uint64_t epoch, uint64_t revision) {
    return request.pid == pid && request.epoch == epoch && request.revision == revision && !request.generationStop.stop_requested();
}
bool SameNativeCacheOwner(const NativeCacheOwner& left,const NativeCacheOwner& right) {
    return left.pid==right.pid&&left.processCreatedAtTicks==right.processCreatedAtTicks&&left.epoch==right.epoch&&
        left.identityRevision==right.identityRevision&&SameSongId(left.songId,right.songId);
}
NativeCacheOwner CurrentNativeCacheOwnerLocked(const SharedState& state) {
    return NativeCacheOwner{state.requestedPid,state.requestedClient.createdAtTicks,state.nativeEpoch,
        state.nativeIdentityRevision,state.nativeIdentityId};
}
bool NativeCacheOwnerCurrentLocked(const SharedState& state,const NativeCacheOwner& owner) {
    return !state.stop.load()&&state.clientClockEnabled.load()&&state.nativePreferred&&
        state.nativeIdentityAvailability==NativeIdentityAvailability::Current&&
        owner.pid!=0&&owner.processCreatedAtTicks!=0&&owner.pid==state.requestedPid&&
        owner.processCreatedAtTicks==state.requestedClient.createdAtTicks&&owner.epoch==state.nativeEpoch&&
        owner.identityRevision==state.nativeIdentityRevision&&SameSongId(owner.songId,state.nativeIdentityId);
}
void ClearNativeCacheDiagnosticLocked(SharedState& state) {
    state.nativeCacheDiagnostic={};
}
// Called only after current native identity/sample confirmation, with mutex
// held. Optional persistence must not turn a successful identity into failure.
void QueueNativeCacheWriteLocked(SharedState& state) noexcept {
    try{
        const auto& record=state.nativeRecord;
        if(record.fromMetadataCache||!record.metadataValidated||!record.nativeCorroborated)return;
        NativeCacheWrite request;
        request.owner=CurrentNativeCacheOwnerLocked(state);
        if(!NativeCacheOwnerCurrentLocked(state,request.owner)||!SameSongId(record.id,request.owner.songId))return;
        request.metadata=SongMetadata{record.id,record.title,record.artist,record.album,record.durationMs};
        auto& queue=state.nativeCacheWrites;
        std::erase_if(queue,[&](const auto& item){return SameNativeCacheOwner(item.owner,request.owner);});
        queue.push_back(std::move(request));if(queue.size()>16)queue.pop_front();
    }catch(...){
        try{
            auto owner=CurrentNativeCacheOwnerLocked(state);
            if(NativeCacheOwnerCurrentLocked(state,owner))
                state.nativeCacheDiagnostic=NativeCacheDiagnostic{std::move(owner),L"歌曲资料缓存暂未排入保存，实时跟随继续"};
        }catch(...){}
    }
}
// Windows SDK declares the public SQLite API (its upstream header disclaims
// copyright). The implementation is loaded only from Windows System32;
// no SQLite/C#/Node runtime is installed or copied into this application.
struct SystemSqlite {
    HMODULE module = nullptr;
    decltype(&sqlite3_open_v2) open = nullptr;
    decltype(&sqlite3_close_v2) close = nullptr;
    decltype(&sqlite3_prepare_v2) prepare = nullptr;
    decltype(&sqlite3_bind_text) bindText = nullptr;
    decltype(&sqlite3_step) step = nullptr;
    decltype(&sqlite3_finalize) finalize = nullptr;
    decltype(&sqlite3_column_int64) integer = nullptr;
    decltype(&sqlite3_column_text) text = nullptr;
    decltype(&sqlite3_column_bytes) bytes = nullptr;
    decltype(&sqlite3_busy_timeout) busyTimeout = nullptr;
    decltype(&sqlite3_db_readonly) readOnly = nullptr;
    decltype(&sqlite3_progress_handler) progressHandler = nullptr;
    template <typename Function> bool Resolve(Function& target, const char* name) {
        const auto address = GetProcAddress(module, name);
        static_assert(sizeof(target) == sizeof(address));
        std::memcpy(&target, &address, sizeof(target));
        return target != nullptr;
    }
    SystemSqlite() {
        module = LoadLibraryExW(L"winsqlite3.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) return;
        const bool loaded = Resolve(open, "sqlite3_open_v2") && Resolve(close, "sqlite3_close_v2") &&
            Resolve(prepare, "sqlite3_prepare_v2") && Resolve(bindText,"sqlite3_bind_text") && Resolve(step, "sqlite3_step") && Resolve(finalize, "sqlite3_finalize") &&
            Resolve(integer, "sqlite3_column_int64") && Resolve(text, "sqlite3_column_text") && Resolve(bytes, "sqlite3_column_bytes") &&
            Resolve(busyTimeout, "sqlite3_busy_timeout") && Resolve(readOnly, "sqlite3_db_readonly") &&
            Resolve(progressHandler, "sqlite3_progress_handler");
        if (!loaded) { FreeLibrary(module); module = nullptr; }
    }
    ~SystemSqlite() { if (module) FreeLibrary(module); }
    SystemSqlite(const SystemSqlite&) = delete;
    SystemSqlite& operator=(const SystemSqlite&) = delete;
};
std::string Utf8Text(const std::wstring& text) {
    if (text.empty()) return {};
    const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!size) return {};
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}
std::wstring WideUtf8(const std::string& text) {
    if (text.empty()) return {};
    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!size) return {};
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}
int64_t UnixMsNow() {
    FILETIME fileTime{};
    GetSystemTimePreciseAsFileTime(&fileTime);
    ULARGE_INTEGER value{};
    value.LowPart = fileTime.dwLowDateTime;
    value.HighPart = fileTime.dwHighDateTime;
    return static_cast<int64_t>((value.QuadPart - 116444736000000000ULL) / 10000);
}
struct RecordDeadline {
    const SharedState* state;
    int64_t deadlineMs;
};
int SQLITE_CALLBACK StopRecordQuery(void* context) {
    const auto& deadline = *static_cast<RecordDeadline*>(context);
    return deadline.state->stop.load() || MonotonicMs() >= deadline.deadlineMs;
}
std::wstring RecordDatabasePath() {
    PWSTR localData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localData)) || !localData) return {};
    std::wstring path(localData);
    CoTaskMemFree(localData);
    // This is the one known client-owned record database. No recursive search,
    // general config/localdata/log parsing, account tables, cookies or tokens.
    path += L"\\Netease\\CloudMusic\\Library\\webdb.dat";
    return path;
}
SongRecord ReadClientSongRecord(SystemSqlite& api, const ClientProcess& client,
    const std::shared_ptr<SharedState>& state, const std::wstring& observedTitle,
    const std::wstring& observedArtist, bool sessionIdentityFresh,const ClientClockObservation& nativeIdentity) {
    SongRecord result;
    const auto mediaRevision = state->metadataRevision.load();
    if (!api.module) { result.diagnostic = L"系统 WinSQLite3 不可用；没有安装替代运行库"; return result; }
    const bool nativeIdentityAvailable=nativeIdentity.candidate&&nativeIdentity.stable&&
        SameSongId(nativeIdentity.songId,nativeIdentity.songId)&&nativeIdentity.sampledAtMs>0&&
        MonotonicMs()>=nativeIdentity.sampledAtMs&&MonotonicMs()-nativeIdentity.sampledAtMs<=750;
    DWORD ownerPid = 0;std::wstring initialCaption,captionTitle,captionArtist;
    bool captionIdentity=false;
    if(!nativeIdentityAvailable){
        if(client.hwnd)GetWindowThreadProcessId(client.hwnd,&ownerPid);
        if(client.hwnd)initialCaption=WindowTitle(client.hwnd);
        SplitCaption(initialCaption,captionTitle,captionArtist);
        captionIdentity=client.hwnd&&ownerPid==client.pid&&IsKnownMusicMainClass(client.windowClass)&&
            !captionTitle.empty()&&!captionArtist.empty();
    }
    if (client.path.empty()) {
        result.diagnostic = L"歌曲记录缺少已核验客户端进程路径，不读取本地资料";
        return result;
    }
    // FileVersion is only corroboration for the legacy title/session path.
    // A fresh, stable native ID already belongs to the PID/creation epoch and
    // selects an exact row; requiring an optional version resource here would
    // unnecessarily turn local ID metadata into a network-only dependency.
    if (!nativeIdentityAvailable && (client.version.empty() || observedTitle.empty() || observedArtist.empty())) {
        result.diagnostic = L"最近播放记录缺少可信客户端版本或当前标题歌手，不核验 ID";
        return result;
    }
    std::wstring identitySource;
    if(nativeIdentityAvailable){
        identitySource=L"已收录版本的连续原生歌曲 ID 与时长";
    }else if (captionIdentity) {
        // During a media transition, GSMTC and the native caption may briefly
        // refer to different tracks. Do not choose either side of a mismatch.
        if (captionTitle != observedTitle || !ArtistSourcesCompatible(captionArtist,observedArtist)) {
            result.diagnostic = L"客户端窗口标题与媒体会话元数据不一致，等待切歌完成后再核验 ID";
            return result;
        }
        identitySource = L"当前窗口标题及歌手";
    } else if (sessionIdentityFresh && !state->metadataDirty.load()) {
        // A unique NetEase GSMTC session remains available when its window is
        // minimized or in the tray. The event flag is checked again after the
        // bounded database query, so a concurrent track change invalidates it.
        identitySource = L"唯一网易云媒体会话的当前标题及歌手";
    } else {
        result.diagnostic = L"最近播放记录缺少可复核的窗口标题或当前媒体会话，不核验 ID";
        return result;
    }
    const bool observedIdentityFresh=!observedTitle.empty()&&!observedArtist.empty()&&
        ((captionIdentity&&captionTitle==observedTitle&&ArtistSourcesCompatible(captionArtist,observedArtist))||
         (!captionIdentity&&sessionIdentityFresh&&!state->metadataDirty.load()));
    // The current native ID, not a window caption or a media session, chooses
    // the static row. Never fall back to the latest row for this native ID.
    const bool lookupByNativeId=nativeIdentityAvailable;
    const auto path = RecordDatabasePath();
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes) ||
        (attributes.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        result.diagnostic = L"指定 Library/webdb.dat 不可读或为重解析目标";
        return result;
    }
    const uint64_t size = (static_cast<uint64_t>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
    if (size < 16 || size > 256ULL * 1024 * 1024) { result.diagnostic = L"歌曲记录数据库大小超出 16 字节至 256 MiB 上限"; return result; }
    sqlite3* database = nullptr;
    const auto pathUtf8 = Utf8Text(path);
    int status = api.open(pathUtf8.c_str(), &database, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr);
    if (status != SQLITE_OK || !database) {
        if (database) api.close(database);
        result.diagnostic = L"Library/webdb.dat 只读打开失败，SQLite=" + std::to_wstring(status);
        return result;
    }
    struct DatabaseClose { SystemSqlite& api; sqlite3* database; ~DatabaseClose() { api.close(database); } } databaseClose{api, database};
    if (api.readOnly(database, "main") != 1) { result.diagnostic = L"数据库不是只读连接，拒绝查询"; return result; }
    status = api.busyTimeout(database, 50);
    if (status != SQLITE_OK) { result.diagnostic = L"SQLite busy timeout 设置失败=" + std::to_wstring(status); return result; }
    RecordDeadline deadline{state.get(), MonotonicMs() + 200};
    api.progressHandler(database, 1000, StopRecordQuery, &deadline);
    // Static metadata for the exact native ID needs no current caption, SMTC,
    // or recent history timestamp. The caller rechecks a NEW native sample.
    // Raw jsonStr and privilege/account values never leave the SQLite engine.
    constexpr const char* projection = R"SQL(
SELECT playtime, id,
CASE WHEN length(jsonStr) <= 65536 AND json_valid(jsonStr)
THEN json_object(
  'id', json_extract(jsonStr, '$.id'),
  'title', json_extract(jsonStr, '$.name'),
  'artists', json(COALESCE((SELECT json_group_array(json_extract(value, '$.name'))
                           FROM json_each(historyTracks.jsonStr, '$.artists')), '[]')),
  'album', json_extract(jsonStr, '$.album.name'),
  'durationMs', COALESCE(json_extract(jsonStr, '$.duration'), json_extract(jsonStr, '$.dt'), 0)
) ELSE NULL END
FROM historyTracks
)SQL";
    const auto query=std::string(projection)+(lookupByNativeId?" WHERE id = ?1 LIMIT 1":" ORDER BY playtime DESC LIMIT 1");
    sqlite3_stmt* statement = nullptr;
    status = api.prepare(database, query.c_str(), -1, &statement, nullptr);
    if (status != SQLITE_OK || !statement) {
        if (statement) api.finalize(statement);
        result.diagnostic = L"historyTracks 白名单查询不可用，SQLite=" + std::to_wstring(status);
        return result;
    }
    struct StatementClose { SystemSqlite& api; sqlite3_stmt* statement; ~StatementClose() { api.finalize(statement); } } statementClose{api, statement};
    if(lookupByNativeId){
        const auto requestedId=Utf8Text(nativeIdentity.songId);
        if(requestedId.empty()||requestedId.size()>20){result.diagnostic=L"原生歌曲ID绑定值无效";return result;}
        status=api.bindText(statement,1,requestedId.c_str(),static_cast<int>(requestedId.size()),SQLITE_TRANSIENT);
        if(status!=SQLITE_OK){result.diagnostic=L"原生歌曲ID参数绑定失败，SQLite="+std::to_wstring(status);return result;}
    }
    status = api.step(statement);
    if (status != SQLITE_ROW) { result.diagnostic = std::wstring(lookupByNativeId?L"historyTracks 指定原生ID记录不可用／查询预算到期，SQLite=":L"historyTracks 无可读取的新记录／查询预算到期，SQLite=") + std::to_wstring(status); return result; }
    result.playtimeUtcMs = api.integer(statement, 0);
    const auto utcNow=UnixMsNow();
    if (!lookupByNativeId&&(result.playtimeUtcMs <= 0 || result.playtimeUtcMs>utcNow+5000 || result.playtimeUtcMs<utcNow-24*3600*1000LL)) {
        result.diagnostic = L"最近播放记录超过 24 小时或未来时钟容差 5 秒，不核验 ID";
        return result;
    }
    const int idBytes = api.bytes(statement, 1), payloadBytes = api.bytes(statement, 2);
    const auto id = api.text(statement, 1), payload = api.text(statement, 2);
    if (!id || !payload || idBytes < 1 || idBytes > 20 || payloadBytes < 1 || payloadBytes > 65536) {
        result.diagnostic = L"歌曲记录 ID／白名单 JSON 长度无效";
        return result;
    }
    const std::string rowId(reinterpret_cast<const char*>(id), idBytes);
    if (!std::all_of(rowId.begin(), rowId.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        result.diagnostic = L"歌曲记录 ID 不是十进制歌曲 ID";
        return result;
    }
    try {
        const auto data = nlohmann::json::parse(reinterpret_cast<const char*>(payload), reinterpret_cast<const char*>(payload) + payloadBytes,
            [](int depth, nlohmann::json::parse_event_t, nlohmann::json&) { if (depth > 16) throw std::runtime_error("Record JSON depth"); return true; });
        if (!data.is_object() || !data.contains("id") || !data.contains("title") || !data["title"].is_string() ||
            !data.contains("artists") || !data["artists"].is_array() || data["artists"].size() > 16) throw std::runtime_error("Record schema");
        const auto& jsonId = data["id"];
        std::string metadataId;
        if (jsonId.is_string()) metadataId = jsonId.get<std::string>();
        else if (jsonId.is_number_unsigned()) metadataId = std::to_string(jsonId.get<uint64_t>());
        else if (jsonId.is_number_integer() && jsonId.get<int64_t>() > 0) metadataId = std::to_string(jsonId.get<int64_t>());
        if (metadataId != rowId) { result.diagnostic = L"historyTracks 行 ID 与 JSON.id 不一致，拒绝核验"; return result; }
        result.title = Trim(WideUtf8(data["title"].get<std::string>()));
        for (const auto& artist : data["artists"]) {
            if (!artist.is_string()) throw std::runtime_error("Record artist");
            const auto name = Trim(WideUtf8(artist.get<std::string>()));
            if (name.empty()) throw std::runtime_error("Record empty artist");
            if(result.primaryArtist.empty())result.primaryArtist=name;
            if (!result.artist.empty()) result.artist += L" / ";
            result.artist += name;
        }
        const auto decodedRowId=WideUtf8(rowId);
        if(result.title.empty()||result.title.size()>1024||result.artist.empty()||result.artist.size()>2048)throw std::runtime_error("Record metadata bounds");
        const bool fullCreditConfirmed=SameArtistCredit(result.artist,observedArtist)||
            (captionIdentity&&SameArtistCredit(result.artist,captionArtist));
        const bool artistSourcesMatch=RecordArtistMatches(observedArtist,result.artist,result.primaryArtist)&&
            (!captionIdentity||RecordArtistMatches(captionArtist,result.artist,result.primaryArtist));
        const bool normalMetadataMatches=result.title==observedTitle&&observedIdentityFresh&&artistSourcesMatch&&
            (nativeIdentityAvailable||fullCreditConfirmed);
        bool useNativeIdentity=nativeIdentityAvailable&&SameSongId(decodedRowId,nativeIdentity.songId);
        if(lookupByNativeId&&!useNativeIdentity){
            result.diagnostic=L"原生ID定点记录与当前原生ID不一致，不核验";return result;
        }
        if(nativeIdentityAvailable&&!useNativeIdentity&&!normalMetadataMatches){
            result.diagnostic=L"最新歌曲记录与连续原生歌曲 ID 不一致，不恢复标题或核验 ID";return result;
        }
        if (!useNativeIdentity&&!normalMetadataMatches) {
            result.diagnostic = L"最新歌曲记录与当前歌曲标题／歌手不精确一致，不核验 ID";
            return result;
        }
        if (data.contains("album") && data["album"].is_string()) result.album = WideUtf8(data["album"].get<std::string>());
        if (data.contains("durationMs") && data["durationMs"].is_number_integer()) result.durationMs = data["durationMs"].get<int64_t>();
        if (result.durationMs < 0 || result.durationMs > 24 * 3600 * 1000LL) throw std::runtime_error("Record duration");
        if(lookupByNativeId&&!NativeDurationMatches(result.durationMs,nativeIdentity.durationMs)){
            result.diagnostic=L"原生ID定点记录与当前原生总时长不一致，不核验";return result;
        }
        if(useNativeIdentity&&result.durationMs>0&&std::abs(result.durationMs-nativeIdentity.durationMs)>
           std::max<int64_t>(2000,nativeIdentity.durationMs/50)){
            if(!normalMetadataMatches){result.diagnostic=L"最新歌曲记录与原生总时长不一致，不恢复标题或核验 ID";return result;}
            useNativeIdentity=false;
        }
        if(!useNativeIdentity)identitySource=captionIdentity?L"当前窗口标题及歌手":L"唯一网易云媒体会话的当前标题及歌手";
        if (!useNativeIdentity&&client.hwnd) GetWindowThreadProcessId(client.hwnd, &ownerPid);
        const bool windowChanged = !useNativeIdentity && client.hwnd &&
            (ownerPid != client.pid || WindowTitle(client.hwnd) != initialCaption);
        const bool sessionChanged = !useNativeIdentity && !captionIdentity &&
            state->metadataRevision.load() != mediaRevision;
        if (windowChanged || sessionChanged || state->stop.load()) {
            result.diagnostic = L"读取记录期间客户端窗口或媒体歌曲变化，丢弃结果";
            return result;
        }
        result.id = decodedRowId;
        result.metadataValidated = true;
        result.nativeCorroborated=useNativeIdentity;
        result.diagnostic = lookupByNativeId?
            L"资料来自 historyTracks 原生ID定点记录（绑定参数、行ID=JSON.id、原生ID与正时长交叉核验）；不要求窗口标题或SMTC，记录时间不作为当前播放证据，未用 playtime 推算进度":
            L"ID 来自客户端 Library/webdb.dat:historyTracks 最新 1 条（行ID=JSON.id，" +
                identitySource + L"精确一致，24小时内）；仅为需确认的版本候选，不证明当前播放ID；未用 playtime 推算进度";
    } catch (...) { result.diagnostic = L"歌曲记录白名单 JSON 格式／16 层深度／字段范围无效"; }
    return result;
}
SongRecord ReadNativeSongDetails(const ClientClockObservation& identity, std::stop_token stop) {
    SongRecord result;
    if (!identity.candidate || !identity.stable || !SameSongId(identity.songId, identity.songId) || stop.stop_requested()) return result;
    NativeHttpRequest request;
    request.host = L"music.163.com";
    request.path = L"/api/song/detail?id=" + identity.songId + L"&ids=%5B" + identity.songId + L"%5D";
    request.maxBytes = 512 * 1024;
    request.totalTimeoutMs = 5000;
    const auto response = NativeHttps(request, stop);
    if (stop.stop_requested() || response.cancelled()) { result.diagnostic = L"原生ID资料查询已取消"; return result; }
    if (!response.ok()) { result.diagnostic = L"原生ID歌曲资料查询失败：" + response.diagnostic; return result; }
    try {
        const auto document = nlohmann::json::parse(response.bytes.begin(), response.bytes.end(),
            [](int depth, nlohmann::json::parse_event_t, nlohmann::json&) {
                if (depth > 24) throw std::runtime_error("Song detail depth"); return true;
            });
        if (!document.is_object() || document.value("code", 0) != 200 || !document.contains("songs") ||
            !document["songs"].is_array() || document["songs"].size() > 8) throw std::runtime_error("Song detail envelope");
        for (const auto& song : document["songs"]) {
            if (!song.is_object() || !song.contains("id")) continue;
            const auto& value = song["id"];
            std::wstring id;
            if (value.is_string()) id = WideUtf8(value.get<std::string>());
            else if (value.is_number_unsigned()) id = std::to_wstring(value.get<uint64_t>());
            else if (value.is_number_integer() && value.get<int64_t>() > 0) id = std::to_wstring(value.get<int64_t>());
            if (!SameSongId(id, identity.songId)) continue;
            if (!song.contains("name") || !song["name"].is_string()) throw std::runtime_error("Song detail title");
            result.title = Trim(WideUtf8(song["name"].get<std::string>()));
            const auto artists = song.find(song.contains("artists") ? "artists" : "ar");
            if (artists == song.end() || !artists->is_array() || artists->empty() || artists->size() > 16)
                throw std::runtime_error("Song detail artists");
            for (const auto& artist : *artists) {
                if (!artist.is_object() || !artist.contains("name") || !artist["name"].is_string())
                    throw std::runtime_error("Song detail artist");
                const auto name = Trim(WideUtf8(artist["name"].get<std::string>()));
                if (name.empty() || name.size() > 1024) throw std::runtime_error("Song detail artist bound");
                if (result.primaryArtist.empty()) result.primaryArtist = name;
                if (!result.artist.empty()) result.artist += L" / ";
                result.artist += name;
            }
            const auto album = song.find(song.contains("album") ? "album" : "al");
            if (album != song.end() && album->is_object() && album->contains("name") && (*album)["name"].is_string())
                result.album = Trim(WideUtf8((*album)["name"].get<std::string>()));
            const auto duration = song.find(song.contains("duration") ? "duration" : "dt");
            if (duration == song.end() || !duration->is_number_integer()) throw std::runtime_error("Song detail duration");
            result.durationMs = duration->get<int64_t>();
            if (result.title.empty() || result.title.size() > 1024 || result.artist.empty() || result.artist.size() > 2048 ||
                result.album.size() > 2048 || result.durationMs > 24 * 3600 * 1000LL ||
                !NativeDurationMatches(result.durationMs, identity.durationMs)) throw std::runtime_error("Song detail bounds");
            result.id = identity.songId; result.metadataValidated = true; result.nativeCorroborated = true;
            result.diagnostic = L"资料来自网易云HTTPS歌曲详情：按原生ID定点获取，响应ID及正时长相符；不是标题搜索候选，查询后仍须新原生样本确认";
            return result;
        }
        result.diagnostic = L"网易云歌曲详情未返回当前原生ID对应资料";
    } catch (...) { result.diagnostic = L"原生ID歌曲详情的ID／时长／字段或JSON范围未通过核验"; }
    return result;
}
bool WaitFor(const std::shared_ptr<SharedState>& state, std::chrono::milliseconds period) {
    std::unique_lock lock(state->mutex);
    state->cv.wait_for(lock, period, [&] { return state->stop.load() || state->wake.exchange(false); });
    return !state->stop.load();
}
struct UiText {
    std::wstring text;
    RECT rect{};
    int controlType = 0;
    ComPtr<IUIAutomationElement> element;
    int64_t sampledAtMs = 0;
};
struct UiClockCache {
    DWORD pid = 0;
    HWND hwnd = nullptr;
    int64_t identifiedAtMs = 0;
    ComPtr<IUIAutomationElement> pair, left, right, slider;
    ComPtr<IUIAutomationElement> root;
    std::wstring rootCaption;
    std::wstring rootDiagnostic;
    bool rootCaptionFromNative = false;
    std::vector<ClientProcess> trustedClients;
    ClockPair lastClock;
    int64_t lastClockChangedAtMs = 0;
    std::vector<ComPtr<IUIAutomationElement>> transportButtons;
};
constexpr int64_t UiHotBudgetMs = 750;
constexpr int64_t UiColdBudgetMs = 8000;
bool IsSensitiveUiId(const std::wstring& id) {
    const auto lower = Lower(id);
    for (const auto* part : {L"password", L"account", L"username", L"login", L"avatar", L"profile", L"cookie", L"token"})
        if (lower.find(part) != std::wstring::npos) return true;
    return false;
}
std::optional<RECT> UiMainBounds(HWND hwnd) {
    RECT bounds{};
    if (!hwnd || !GetWindowRect(hwnd, &bounds)) return std::nullopt;
    if (IsIconic(hwnd)) {
        WINDOWPLACEMENT placement{sizeof(placement)};
        if (GetWindowPlacement(hwnd, &placement)) bounds = placement.rcNormalPosition;
    }
    if (bounds.right - bounds.left < 320 || bounds.bottom - bounds.top < 220) return std::nullopt;
    return bounds;
}
bool IsBottomUiControl(HWND hwnd, const RECT& rect, const RECT& bounds, bool central) {
    const auto dpi = GetDpiForWindow(hwnd);
    const LONG stripHeight = std::min<LONG>((bounds.bottom - bounds.top) / 4, MulDiv(128, dpi ? static_cast<int>(dpi) : 96, 96));
    if (rect.right <= rect.left || rect.bottom <= rect.top || rect.top < bounds.bottom - stripHeight || rect.bottom > bounds.bottom + 8 ||
        rect.left < bounds.left - 8 || rect.right > bounds.right + 8) return false;
    const auto center = (rect.left + rect.right) / 2;
    const auto width = bounds.right - bounds.left;
    return !central || (center >= bounds.left + width * 35 / 100 && center <= bounds.left + width * 65 / 100);
}
std::wstring BstrValue(BSTR value) {
    if (!value) return {};
    std::wstring result(value, SysStringLen(value));
    SysFreeString(value);
    return result;
}
// C++ adaptation of Widdit/now-playing-service NeteaseMusicHelper.cs,
// commit 2336c5fc6c57300bc7cf5fa85c7f273042bb86b1 (MIT, Copyright 2024 Widdit).
// Desktop lookup is restricted to our fresh, verified client process family;
// CEF virtual Text providers inside its trusted root need not share root PID.
bool IsSongUiWindowName(const std::wstring& name) {
    return name.find(L" - ") != std::wstring::npos && Lower(name).find(L"mediaplayer") == std::wstring::npos &&
        !IsAuxiliaryWindowText(name);
}
std::wstring UiHResult(HRESULT status) {
    wchar_t value[16]{};
    swprintf_s(value, L"0x%08X", static_cast<unsigned>(status));
    return value;
}
std::wstring UiHandle(HWND hwnd) {
    wchar_t value[24]{};
    swprintf_s(value, L"0x%llX", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hwnd)));
    return value;
}
bool UiChecked(HRESULT status, const std::wstring& operation, std::wstring& diagnostic, bool reportSuccess = false) {
    if (reportSuccess || FAILED(status)) AddDiagnostic(diagnostic, operation + L"=" + UiHResult(status));
    return SUCCEEDED(status);
}
ComPtr<IUIAutomationCacheRequest> UiCacheRequest(IUIAutomation* automation, const std::vector<PROPERTYID>& properties,
    std::wstring& diagnostic) {
    ComPtr<IUIAutomationCacheRequest> request;
    if (!UiChecked(automation->CreateCacheRequest(&request), L"CreateCacheRequest", diagnostic) || !request) return {};
    if (!UiChecked(request->put_TreeScope(TreeScope_Element), L"Cache.TreeScope", diagnostic) ||
        !UiChecked(request->put_AutomationElementMode(AutomationElementMode_Full), L"Cache.Mode", diagnostic)) return {};
    for (const auto property : properties)
        if (!UiChecked(request->AddProperty(property), L"Cache.AddProperty(" + std::to_wstring(property) + L")", diagnostic)) return {};
    return request;
}
ComPtr<IUIAutomationCondition> MakePropertyCondition(IUIAutomation* automation, PROPERTYID property, LONG value,
    std::wstring* diagnostic = nullptr) {
    VARIANT variant{};
    variant.vt = VT_I4;
    variant.lVal = value;
    ComPtr<IUIAutomationCondition> condition;
    const auto status = automation->CreatePropertyCondition(property, variant, &condition);
    if (diagnostic) UiChecked(status, L"CreatePropertyCondition(" + std::to_wstring(property) + L")", *diagnostic);
    return condition;
}
std::vector<DWORD> TrustedClientPids(const ClientProcess& expectedRoot,
    std::vector<ClientProcess>* trustedClients = nullptr) {
    if (trustedClients) trustedClients->clear();
    if (!expectedRoot.pid || !expectedRoot.createdAtTicks || expectedRoot.path.empty()) return {};
    bool complete=false;
    const auto clients = DiscoverClients(&complete);
    if (!complete) return {};
    const auto root = std::find_if(clients.begin(), clients.end(), [&](const auto& client) {
        return SameClientInstance(client, expectedRoot);
    });
    if (root == clients.end() || !IsRootClient(*root, clients)) return {};
    std::vector<const ClientProcess*> trustedInstances{&*root};
    const auto rootPath = Lower(root->path);
    for (size_t pass = 0; pass < clients.size(); ++pass) {
        bool changed = false;
        for (const auto& client : clients) {
            if (!client.pid || !client.createdAtTicks || Lower(client.path) != rootPath ||
                std::any_of(trustedInstances.begin(), trustedInstances.end(), [&](const auto* trusted) {
                    return SameClientInstance(client, *trusted);
                })) continue;
            const auto parent = std::find_if(trustedInstances.begin(), trustedInstances.end(), [&](const auto* trusted) {
                return trusted->pid != client.pid && trusted->pid == client.parentPid &&
                    trusted->name == client.name && trusted->createdAtTicks > 0 &&
                    trusted->createdAtTicks <= client.createdAtTicks;
            });
            if (parent == trustedInstances.end()) continue;
            trustedInstances.push_back(&client);
            changed = true;
        }
        if (!changed) break;
    }
    std::vector<DWORD> trusted;
    trusted.reserve(trustedInstances.size());
    for (const auto* client : trustedInstances) {
        trusted.push_back(client->pid);
        if (trustedClients) trustedClients->push_back(*client);
    }
    return trusted;
}
class TrustedClientLease {
    HANDLE process_ = nullptr;
public:
    TrustedClientLease(const std::vector<ClientProcess>& trustedClients, DWORD reportedPid) {
        // UIA PID 0 does not identify a process instance.  Falling back to the
        // root PID would prove the wrong object while accepting the element.
        if (trustedClients.empty() || !reportedPid) return;
        const DWORD pid = reportedPid;
        const auto trusted = std::find_if(trustedClients.begin(), trustedClients.end(), [=](const auto& client) {
            return client.pid == pid;
        });
        if (trusted == trustedClients.end() || !trusted->createdAtTicks || trusted->path.empty()) return;
        ScopedProcessHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
        if (!process) return;
        const bool matches = WaitForSingleObject(process.Get(), 0) == WAIT_TIMEOUT &&
            ProcessCreationTime(process.Get()) == trusted->createdAtTicks && Lower(ProcessImagePath(process.Get())) == Lower(trusted->path);
        if (!matches)return;
        process_ = process.Release();
    }
    TrustedClientLease(const TrustedClientLease&) = delete;
    TrustedClientLease& operator=(const TrustedClientLease&) = delete;
    ~TrustedClientLease() { if (process_) CloseHandle(process_); }
    explicit operator bool() const noexcept { return process_ != nullptr; }
    bool Running() const noexcept { return process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT; }
};
bool ReadTrustedCurrentBounds(IUIAutomationElement* element, const std::vector<ClientProcess>& trustedClients,
    const std::wstring& operation, RECT& bounds, std::wstring& diagnostic) {
    if (!element) return false;
    int sourcePid = 0;
    if (!UiChecked(element->get_CurrentProcessId(&sourcePid), operation + L".CurrentPID", diagnostic) || sourcePid <= 0)
        return false;
    TrustedClientLease lease(trustedClients, static_cast<DWORD>(sourcePid));
    return lease && UiChecked(element->get_CurrentBoundingRectangle(&bounds), operation + L".CurrentBounds", diagnostic) &&
        lease.Running();
}
ComPtr<IUIAutomationElement> FindDesktopUiWindow(IUIAutomation* automation, const std::vector<DWORD>& trusted, int64_t deadline,
    std::wstring& caption, std::wstring& diagnostic) {
    const auto started = MonotonicMs();
    if (trusted.empty()) { diagnostic = L"UIA 主进程已消失／身份变化"; return {}; }
    std::vector<ComPtr<IUIAutomationCondition>> pidConditions;
    std::vector<IUIAutomationCondition*> rawConditions;
    for (DWORD pid : trusted) {
        auto condition = MakePropertyCondition(automation, UIA_ProcessIdPropertyId, static_cast<LONG>(pid), &diagnostic);
        if (!condition) { diagnostic = L"UIA 无法创建受信进程条件"; return {}; }
        rawConditions.push_back(condition.Get());
        pidConditions.push_back(std::move(condition));
    }
    ComPtr<IUIAutomationCondition> condition;
    if (rawConditions.size() == 1) condition = pidConditions.front();
    else if (!UiChecked(automation->CreateOrConditionFromNativeArray(rawConditions.data(), static_cast<int>(rawConditions.size()), &condition), L"Desktop.PidOr", diagnostic)) return {};
    ComPtr<IUIAutomationElement> desktop;
    auto request = UiCacheRequest(automation, {UIA_NamePropertyId, UIA_ProcessIdPropertyId}, diagnostic);
    if (!condition || !UiChecked(automation->GetRootElement(&desktop), L"Desktop.GetRootElement", diagnostic, true) || !desktop || !request) {
        AddDiagnostic(diagnostic, L"UIA 桌面根节点／缓存请求不可用");
        return {};
    }
    if (MonotonicMs() >= deadline) { diagnostic = L"UIA 主窗口定位超过预算"; return {}; }
    ComPtr<IUIAutomationElementArray> windows;
    const HRESULT status = desktop->FindAllBuildCache(TreeScope_Children, condition.Get(), request.Get(), &windows);
    if (!UiChecked(status, L"Desktop.Children.FindAll", diagnostic, true) || !windows) {
        AddDiagnostic(diagnostic, L"UIA 桌面受信窗口 FindAll 未取得数组");
        return {};
    }
    int count = 0;
    if (!UiChecked(windows->get_Length(&count), L"Desktop.Array.Length", diagnostic, true)) return {};
    if (count > 64 || MonotonicMs() > deadline) { diagnostic = L"UIA 主窗口候选数／定位预算超限"; return {}; }
    ComPtr<IUIAutomationElement> selected;
    for (int i = 0; i < count; ++i) {
        ComPtr<IUIAutomationElement> element;
        BSTR name = nullptr;
        int elementPid = 0;
        if (!UiChecked(windows->GetElement(i, &element), L"Desktop.GetElement", diagnostic) || !element ||
            !UiChecked(element->get_CachedName(&name), L"Desktop.CachedName", diagnostic)) continue;
        auto value = Trim(BstrValue(name));
        if (!IsSongUiWindowName(value) || !UiChecked(element->get_CachedProcessId(&elementPid), L"Desktop.CachedPID", diagnostic) ||
            std::find(trusted.begin(), trusted.end(), static_cast<DWORD>(elementPid)) == trusted.end()) continue;
        if (selected) { diagnostic = L"UIA 有多个合适歌曲窗口，拒绝任意选择"; return {}; }
        selected = std::move(element);
        caption = std::move(value);
    }
    AddDiagnostic(diagnostic, selected ? L"UIA Desktop.Children 窗口数=" + std::to_wstring(count) + L"，唯一歌曲窗口；受信进程数=" + std::to_wstring(trusted.size()) :
        L"UIA Desktop.Children 窗口数=" + std::to_wstring(count) + L"，合适歌曲窗口数=0");
    AddDiagnostic(diagnostic, L"Desktop.Root 查找耗时=" + std::to_wstring(MonotonicMs() - started) + L"ms");
    return selected;
}
ComPtr<IUIAutomationElement> FindDirectUiWindow(IUIAutomation* automation, HWND hwnd, const std::vector<DWORD>& trusted,
    std::wstring& caption, bool& captionFromNative, std::wstring& diagnostic) {
    const auto started = MonotonicMs();
    DWORD nativePid = 0;
    wchar_t nativeClass[256]{};
    GetWindowThreadProcessId(hwnd, &nativePid);
    GetClassNameW(hwnd, nativeClass, static_cast<int>(std::size(nativeClass)));
    if (!IsWindow(hwnd) || std::find(trusted.begin(), trusted.end(), nativePid) == trusted.end() ||
        !IsKnownMusicMainClass(nativeClass) || IsAuxiliaryWindowText(nativeClass) || !IsSongUiWindowName(WindowTitle(hwnd))) {
        AddDiagnostic(diagnostic, L"Direct.HWND 身份／类名／歌曲标题不符合可信根条件，拒绝读取");
        return {};
    }
    auto request = UiCacheRequest(automation, {UIA_NamePropertyId, UIA_ProcessIdPropertyId, UIA_NativeWindowHandlePropertyId,
        UIA_ClassNamePropertyId, UIA_IsOffscreenPropertyId}, diagnostic);
    if (!request) return {};
    ComPtr<IUIAutomationElement> root;
    const auto status = automation->ElementFromHandleBuildCache(hwnd, request.Get(), &root);
    UiChecked(status, L"Direct.ElementFromHandleBuildCache", diagnostic, true);
    AddDiagnostic(diagnostic, L"Direct.Root 调用耗时=" + std::to_wstring(MonotonicMs() - started) + L"ms");
    if (FAILED(status) || !root) { AddDiagnostic(diagnostic, L"Direct.根不可用"); return {}; }
    int pid = 0;
    UIA_HWND handle = nullptr;
    BOOL offscreen = FALSE;
    BSTR name = nullptr, className = nullptr;
    const bool pidOk = UiChecked(root->get_CachedProcessId(&pid), L"Direct.CachedPID", diagnostic, true);
    const bool handleOk = UiChecked(root->get_CachedNativeWindowHandle(&handle), L"Direct.CachedHWND", diagnostic, true);
    const bool nameOk = UiChecked(root->get_CachedName(&name), L"Direct.CachedName", diagnostic, true);
    const bool classOk = UiChecked(root->get_CachedClassName(&className), L"Direct.CachedClass", diagnostic, true);
    const bool offscreenOk = UiChecked(root->get_CachedIsOffscreen(&offscreen), L"Direct.CachedOffscreen", diagnostic, true);
    auto rootName = Trim(BstrValue(name));
    auto rootClass = Trim(BstrValue(className));
    AddDiagnostic(diagnostic, L"Direct.Root PID=" + std::to_wstring(pid) + L" HWND=" + UiHandle(reinterpret_cast<HWND>(handle)) +
        L" class=" + rootClass + L" IsOffscreen=" + (offscreenOk ? std::to_wstring(offscreen != FALSE) : L"unknown"));
    if (!pidOk || !handleOk || !classOk || !nameOk || !offscreenOk ||
        reinterpret_cast<HWND>(handle) != hwnd || std::find(trusted.begin(), trusted.end(), static_cast<DWORD>(pid)) == trusted.end() ||
        (!rootClass.empty() && Lower(rootClass) != Lower(nativeClass) && !IsKnownMusicMainClass(rootClass))) {
        AddDiagnostic(diagnostic, L"Direct.Root 属性与可信 HWND/PID/class 不一致，拒绝使用");
        return {};
    }
    captionFromNative = rootName.empty();
    caption = captionFromNative ? WindowTitle(hwnd) : rootName;
    if (!IsSongUiWindowName(caption)) { AddDiagnostic(diagnostic, L"Direct.Root 缺少可确认的歌曲标题"); return {}; }
    AddDiagnostic(diagnostic, L"Direct.Root 已验证");
    return root;
}
bool ApplyChangedClock(UiClockCache& cache, UiObservation& result) {
    // Chromium can return the last hover tooltip text after the mouse leaves.
    // Re-reading an identical integer clock is not a new progress sample.
    if (!cache.lastClockChangedAtMs || cache.lastClock.position != result.clock.position || cache.lastClock.duration != result.clock.duration) {
        cache.lastClock = result.clock;
        cache.lastClockChangedAtMs = result.sampledAtMs;
    } else result.sampledAtMs = cache.lastClockChangedAtMs;
    const auto now = MonotonicMs();
    const bool pausedHold = result.state == PlayState::Paused && result.stateSampledAtMs > 0 &&
        now >= result.stateSampledAtMs && now - result.stateSampledAtMs <= 1500;
    if (now - result.sampledAtMs > 1500 && !pausedHold) {
        result.hasClock = false;
        AddDiagnostic(result.diagnostic, L"时间标签超过 1.5 秒未变化，可能为历史 hover 文本，拒绝标成新进度");
        return false;
    }
    if (pausedHold && now - result.sampledAtMs > 1500)
        AddDiagnostic(result.diagnostic, L"播放命令仍明确为暂停；保留同一窗口、同一歌曲的最后秒级位置，不刷新原采样时间");
    return true;
}
bool ReadCachedText(IUIAutomationElement* element, const std::vector<ClientProcess>& trusted, std::wstring& text, RECT& rect,
    int64_t& sampledAtMs, std::wstring& diagnostic) {
    if (!element) return false;
    int sourcePid = 0, type = 0;
    BOOL password = TRUE;
    BSTR rawId = nullptr;
    if (!UiChecked(element->get_CurrentProcessId(&sourcePid), L"Clock.CurrentPID", diagnostic) || sourcePid <= 0) return false;
    TrustedClientLease lease(trusted, static_cast<DWORD>(sourcePid));
    if (!lease) return false;
    if (!UiChecked(element->get_CurrentControlType(&type), L"Clock.CurrentType", diagnostic) ||
        !UiChecked(element->get_CurrentIsPassword(&password), L"Clock.CurrentPassword", diagnostic) ||
        !UiChecked(element->get_CurrentAutomationId(&rawId), L"Clock.CurrentID", diagnostic)) {
        if (rawId) SysFreeString(rawId);
        return false;
    }
    const auto id = BstrValue(rawId);
    if (password || IsSensitiveUiId(id) || type != UIA_TextControlTypeId) return false;
    BSTR raw = nullptr;
    if (!UiChecked(element->get_CurrentName(&raw), L"Clock.CurrentName", diagnostic)) { if (raw) SysFreeString(raw); return false; }
    text = Trim(BstrValue(raw));
    sampledAtMs = MonotonicMs();
    if (!UiChecked(element->get_CurrentBoundingRectangle(&rect), L"Clock.CurrentBounds", diagnostic)) return false;
    if (ParseClock(text) || ParsePair(text)) return lease.Running();
    ComPtr<IUIAutomationValuePattern> value;
    if (UiChecked(element->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&value)), L"Clock.ValuePattern", diagnostic) && value) {
        raw = nullptr;
        if (UiChecked(value->get_CurrentValue(&raw), L"Clock.CurrentValue", diagnostic)) { text = Trim(BstrValue(raw)); sampledAtMs = MonotonicMs(); }
    }
    return lease.Running() && (ParseClock(text).has_value() || ParsePair(text).has_value());
}
PlayState ButtonState(const std::wstring& text) {
    auto lower = Lower(Trim(text));
    lower.erase(std::remove_if(lower.begin(), lower.end(), [](wchar_t c) { return std::iswspace(c) != 0; }), lower.end());
    for (const auto* shortcut : {L"(p)", L"(space)", L"(空格)", L"（p）", L"（空格）"}) {
        const std::wstring suffix(shortcut);
        if (lower.ends_with(suffix)) { lower.resize(lower.size() - suffix.size()); break; }
    }
    if (lower == L"暂停" || lower == L"暂停播放" || lower == L"pause" || lower == L"pauseplayback") return PlayState::Playing;
    if (lower == L"播放" || lower == L"继续播放" || lower == L"play" || lower == L"resumeplayback") return PlayState::Paused;
    return PlayState::Unknown;
}
bool ReadUiTransportState(UiClockCache& cache, HWND hwnd, UiObservation& result,
    const std::shared_ptr<SharedState>& state, int64_t deadline, const RECT* clockRect = nullptr) {
    const auto bounds = UiMainBounds(hwnd);
    if (!bounds || cache.transportButtons.size() != 1 || state->stop.load() || MonotonicMs() >= deadline) return false;
    auto& control = cache.transportButtons.front();
    int pid = 0, type = 0;
    BOOL password = TRUE;
    RECT rect{};
    BSTR rawId = nullptr;
    if (!UiChecked(control->get_CurrentProcessId(&pid), L"Transport.CurrentPID", result.diagnostic) || pid <= 0) return false;
    TrustedClientLease lease(cache.trustedClients, static_cast<DWORD>(pid));
    if (!lease) return false;
    if (!UiChecked(control->get_CurrentControlType(&type), L"Transport.CurrentType", result.diagnostic) ||
        !UiChecked(control->get_CurrentIsPassword(&password), L"Transport.CurrentIsPassword", result.diagnostic) ||
        !UiChecked(control->get_CurrentAutomationId(&rawId), L"Transport.CurrentID", result.diagnostic)) {
        if (rawId) SysFreeString(rawId);
        return false;
    }
    const auto id = BstrValue(rawId);
    if (password || IsSensitiveUiId(id) || (type != UIA_ButtonControlTypeId && type != UIA_MenuItemControlTypeId) ||
        !UiChecked(control->get_CurrentBoundingRectangle(&rect), L"Transport.CurrentBounds", result.diagnostic) ||
        !IsBottomUiControl(hwnd, rect, *bounds, true) || (clockRect && std::abs(rect.bottom - clockRect->bottom) > 120) ||
        state->stop.load() || MonotonicMs() >= deadline) return false;
    BSTR raw = nullptr;
    if (!UiChecked(control->get_CurrentName(&raw), L"Transport.CurrentName", result.diagnostic)) { if (raw) SysFreeString(raw); return false; }
    const auto observed = ButtonState(BstrValue(raw));
    const auto sampledAt = MonotonicMs();
    if (observed == PlayState::Unknown || state->stop.load() || sampledAt > deadline || !lease.Running()) return false;
    result.state = observed;
    result.stateSampledAtMs = sampledAt;
    AddDiagnostic(result.diagnostic, L"底部中央唯一 MenuItem/Button 的明确播放／暂停命令；仅作为状态，type=" + std::to_wstring(type));
    return true;
}
std::optional<UiObservation> ObserveCachedClock(UiClockCache& cache, const ClientProcess& client, HWND hwnd,
    const std::shared_ptr<SharedState>& state, int64_t deadline) {
    const auto pid = client.pid;
    if (cache.pid != pid || cache.hwnd != hwnd || !cache.identifiedAtMs || MonotonicMs() - cache.identifiedAtMs > 10000) return std::nullopt;
    TrustedClientLease rootLease(cache.trustedClients, pid);
    if (!rootLease) return std::nullopt;
    UiObservation result;
    result.pid = pid;
    result.hwnd = hwnd;
    result.nativeCaption = WindowTitle(hwnd);
    if (!cache.root) return std::nullopt;
    int rootElementPid = 0;
    if (!UiChecked(cache.root->get_CurrentProcessId(&rootElementPid), L"HotRoot.CurrentPID", result.diagnostic) || rootElementPid <= 0)
        return std::nullopt;
    TrustedClientLease uiRootLease(cache.trustedClients, static_cast<DWORD>(rootElementPid));
    if (!uiRootLease) return std::nullopt;
    DWORD nativePid = 0;
    GetWindowThreadProcessId(hwnd, &nativePid);
    if (nativePid != pid || !IsSongUiWindowName(result.nativeCaption)) return std::nullopt;
    std::wstring caption;
    if (cache.rootCaptionFromNative) caption = WindowTitle(hwnd);
    else {
        BSTR rootName = nullptr;
        if (!UiChecked(cache.root->get_CurrentName(&rootName), L"HotRoot.CurrentName", result.diagnostic)) {
            if (rootName) SysFreeString(rootName);
            return std::nullopt;
        }
        caption = Trim(BstrValue(rootName));
        if (!uiRootLease.Running()) return std::nullopt;
    }
    if (!IsSongUiWindowName(caption) || caption != cache.rootCaption) return std::nullopt;
    std::wstring text;
    RECT clockRect{};
    std::optional<ClockPair> pair;
    int64_t clockSampledAtMs = 0;
    if (cache.pair && ReadCachedText(cache.pair.Get(), cache.trustedClients, text, clockRect, clockSampledAtMs, result.diagnostic)) pair = ParsePair(text);
    if (!pair && cache.left && cache.right && cache.slider && MonotonicMs() < deadline) {
        std::wstring leftText, rightText;
        RECT leftRect{}, rightRect{};
        int64_t leftSampledAtMs = 0, rightSampledAtMs = 0;
        if (ReadCachedText(cache.left.Get(), cache.trustedClients, leftText, leftRect, leftSampledAtMs, result.diagnostic) && MonotonicMs() < deadline &&
            ReadCachedText(cache.right.Get(), cache.trustedClients, rightText, rightRect, rightSampledAtMs, result.diagnostic) &&
            ReadTrustedCurrentBounds(cache.slider.Get(), cache.trustedClients, L"Clock.Slider", clockRect, result.diagnostic)) {
            const auto left = ParseClock(leftText), right = ParseClock(rightText);
            const auto sliderY = (clockRect.top + clockRect.bottom) / 2;
            const bool flank = leftRect.right <= clockRect.left + 20 && clockRect.left - leftRect.right <= 130 &&
                rightRect.left >= clockRect.right - 20 && rightRect.left - clockRect.right <= 130 &&
                std::abs((leftRect.top + leftRect.bottom) / 2 - sliderY) <= 36 &&
                std::abs((rightRect.top + rightRect.bottom) / 2 - sliderY) <= 36;
            if (left && right && *right > 0 && *left <= *right && flank) {
                pair = ClockPair{*left, *right};
                clockSampledAtMs = leftSampledAtMs;
            }
        }
    }
    if (state->stop.load() || MonotonicMs() > deadline) return std::nullopt;
    if (pair) {
        const auto bounds = UiMainBounds(hwnd);
        if (!bounds || !IsBottomUiControl(hwnd, clockRect, *bounds, false)) return std::nullopt;
    }
    if (!pair) {
        if (!cache.pair && !cache.left && !cache.right && ReadUiTransportState(cache, hwnd, result, state, deadline)) {
            if (!rootLease.Running() || !uiRootLease.Running() || WindowTitle(hwnd) != result.nativeCaption) return std::nullopt;
            AddDiagnostic(result.diagnostic, L"缓存播放状态可读，但没有时间控件；不给予时间轴可靠性");
            AddDiagnostic(result.diagnostic, cache.rootDiagnostic);
            return result;
        }
        return std::nullopt;
    }
    result.hasClock = true;
    result.clock = *pair;
    // Timestamp the clock read itself, not a later tree traversal or metadata.
    result.sampledAtMs = clockSampledAtMs;
    ReadUiTransportState(cache, hwnd, result, state, deadline, &clockRect);
    if (!rootLease.Running() || !uiRootLease.Running() || WindowTitle(hwnd) != result.nativeCaption) return std::nullopt;
    AddDiagnostic(result.diagnostic, L"只读 UI Automation 已识别时间控件缓存（每次读取 Current 属性，10 秒内重新扫描）；秒级时间轴");
    AddDiagnostic(result.diagnostic, cache.rootDiagnostic);
    if (result.state == PlayState::Unknown) AddDiagnostic(result.diagnostic, L"播放状态未知，禁止自动推进歌词");
    ApplyChangedClock(cache, result);
    return result;
}
UiObservation ObserveUi(IUIAutomation* automation, const ClientProcess& client, HWND hwnd,
    const std::shared_ptr<SharedState>& state, UiClockCache& cache) {
    const auto pid = client.pid;
    UiObservation result;
    result.pid = pid;
    result.hwnd = hwnd;
    result.nativeCaption = WindowTitle(hwnd);
    ComPtr<IUIAutomation2> automation2;
    if (SUCCEEDED(automation->QueryInterface(IID_PPV_ARGS(&automation2)))) automation2->put_TransactionTimeout(500);
    const auto hotDeadline = MonotonicMs() + UiHotBudgetMs;
    if (auto cached = ObserveCachedClock(cache, client, hwnd, state, hotDeadline)) return *cached;
    const auto previousCaption = cache.rootCaption;
    const auto previousClock = cache.lastClock;
    const auto previousClockChangedAt = cache.lastClockChangedAtMs;
    const bool sameSource = cache.pid == pid && cache.hwnd == hwnd;
    cache = {};
    if (MonotonicMs() >= hotDeadline || state->stop.load()) {
        result.diagnostic = L"缓存时间控件读取失败／超过预算，不复用旧进度";
        return result;
    }
    // Real client queries take 0.8–1.6 seconds for Text and 4.6–6.6 seconds
    // for a full metadata subtree. A detached worker may spend a bounded
    // cold budget without blocking app rendering or Stop. Hot reads retain
    // their small budget; completion never makes old clock text fresh.
    const auto deadline = MonotonicMs() + UiColdBudgetMs;
    if (automation2) automation2->put_TransactionTimeout(2000);
    const auto mainBounds = UiMainBounds(hwnd);
    if (!mainBounds || !IsSongUiWindowName(result.nativeCaption)) {
        result.diagnostic = L"当前受信主窗口没有可确认的歌曲标题／有效底部几何，不读取控件内容";
        return result;
    }
    std::wstring rootCaption, rootDiagnostic;
    std::vector<ClientProcess> trustedClients;
    auto trusted = TrustedClientPids(client, &trustedClients);
    TrustedClientLease rootLease(trustedClients, pid);
    if (!rootLease) {
        result.diagnostic = L"UIA 根进程实例已退出、被复用或无法继续核验";
        return result;
    }
    std::vector<std::unique_ptr<TrustedClientLease>> familyLeases;
    std::vector<DWORD> liveTrusted{trusted.front()};
    std::vector<ClientProcess> liveTrustedClients{trustedClients.front()};
    for (size_t i = 1; i < trustedClients.size(); ++i) {
        auto lease = std::make_unique<TrustedClientLease>(trustedClients, trustedClients[i].pid);
        if (!*lease) continue;
        liveTrusted.push_back(trustedClients[i].pid);
        liveTrustedClients.push_back(trustedClients[i]);
        familyLeases.push_back(std::move(lease));
    }
    trusted = std::move(liveTrusted);
    trustedClients = std::move(liveTrustedClients);
    std::wstring trustedList;
    for (DWORD trustedPid : trusted) { if (!trustedList.empty()) trustedList += L","; trustedList += std::to_wstring(trustedPid); }
    AddDiagnostic(rootDiagnostic, L"TrustedPID=[" + trustedList + L"]（精确实例句柄在本次冷扫描期间保持）");
    bool captionFromNative = false;
    auto direct = FindDirectUiWindow(automation, hwnd, trusted, rootCaption, captionFromNative, rootDiagnostic);
    if (state->stop.load()) return result;
    std::wstring desktopCaption, desktopDiagnostic;
    auto desktop = FindDesktopUiWindow(automation, trusted, MonotonicMs() + 750, desktopCaption, desktopDiagnostic);
    AddDiagnostic(rootDiagnostic, desktopDiagnostic);
    auto root = direct ? direct : desktop;
    if (!direct && desktop && desktopCaption != result.nativeCaption) {
        root = nullptr;
        AddDiagnostic(rootDiagnostic, L"Desktop 候选标题与当前受信主 HWND 不一致，拒绝作为源");
    }
    if (!direct && desktop) { rootCaption = desktopCaption; captionFromNative = false; }
    AddDiagnostic(rootDiagnostic, direct ? L"SelectedRoot=Direct.HWND" : (desktop ? L"SelectedRoot=Desktop.Children" : L"SelectedRoot=Unavailable"));
    if (!root || state->stop.load()) { result.diagnostic = rootDiagnostic; return result; }
    int selectedRootPid = 0;
    if (!UiChecked(root->get_CurrentProcessId(&selectedRootPid), L"SelectedRoot.CurrentPID", rootDiagnostic) || selectedRootPid <= 0) {
        result.diagnostic = rootDiagnostic;
        return result;
    }
    auto selectedRootLease = std::make_unique<TrustedClientLease>(trustedClients, static_cast<DWORD>(selectedRootPid));
    if (!*selectedRootLease) {
        AddDiagnostic(rootDiagnostic, L"所选 UIA 根节点的精确进程实例已经变化");
        result.diagnostic = rootDiagnostic;
        return result;
    }
    std::wstring selectedCurrentCaption;
    if (!captionFromNative) {
        BSTR selectedRootName = nullptr;
        if (!UiChecked(root->get_CurrentName(&selectedRootName), L"SelectedRoot.CurrentName", rootDiagnostic)) {
            if (selectedRootName) SysFreeString(selectedRootName);
            result.diagnostic = rootDiagnostic;
            return result;
        }
        selectedCurrentCaption = Trim(BstrValue(selectedRootName));
    }
    if ((!captionFromNative && selectedCurrentCaption != rootCaption) || !selectedRootLease->Running()) {
        AddDiagnostic(rootDiagnostic, L"所选 UIA 根节点标题或进程实例在冷扫描前已经变化");
        result.diagnostic = rootDiagnostic;
        return result;
    }
    AddDiagnostic(rootDiagnostic, L"冷扫描预算=8000ms；仅读取当前受信窗口底部控件，热缓存预算=750ms");
    std::vector<UiText> texts;
    std::vector<UiText> sliders;
    bool budgetExceeded = false;
    auto request = UiCacheRequest(automation, {UIA_ControlTypePropertyId, UIA_BoundingRectanglePropertyId,
        UIA_ProcessIdPropertyId, UIA_IsPasswordPropertyId, UIA_AutomationIdPropertyId}, rootDiagnostic);
    if (!request) { result.diagnostic = rootDiagnostic; return result; }
    auto collect = [&](int type) {
        bool passComplete = true;
        if (MonotonicMs() >= deadline || state->stop.load()) { budgetExceeded = true; return false; }
        auto condition = MakePropertyCondition(automation, UIA_ControlTypePropertyId, type, &rootDiagnostic);
        ComPtr<IUIAutomationElementArray> elements;
        if (!condition) return false;
        const auto queryStarted = MonotonicMs();
        const HRESULT status = root->FindAllBuildCache(TreeScope_Descendants, condition.Get(), request.Get(), &elements);
        const auto sampledAt = MonotonicMs();
        AddDiagnostic(rootDiagnostic, L"Subtree.FindAll(" + std::to_wstring(type) + L")=" + UiHResult(status) + L" 耗时=" + std::to_wstring(sampledAt - queryStarted) + L"ms");
        if (FAILED(status) || !elements) {
            wchar_t code[16]{};
            swprintf_s(code, L"0x%08X", static_cast<unsigned>(status));
            AddDiagnostic(rootDiagnostic, L"UIA subtree FindAll(" + std::to_wstring(type) + L")=" + code);
            return false;
        }
        int count = 0;
        if (!UiChecked(elements->get_Length(&count), L"Subtree.Array.Length(" + std::to_wstring(type) + L")", rootDiagnostic, true) || count < 0) return false;
        AddDiagnostic(rootDiagnostic, L"UIA type " + std::to_wstring(type) + L" count=" + std::to_wstring(count));
        if (count > 400) budgetExceeded = true;
        for (int i = 0; i < std::min(count, 400) && !state->stop.load(); ++i) {
            if (MonotonicMs() >= deadline) { budgetExceeded = true; break; }
            ComPtr<IUIAutomationElement> element;
            BSTR name = nullptr, rawId = nullptr;
            RECT rect{};
            int elementPid = 0;
            BOOL password = TRUE;
            if (!UiChecked(elements->GetElement(i, &element), L"Subtree.GetElement", rootDiagnostic) || !element ||
                !UiChecked(element->get_CachedProcessId(&elementPid), L"Subtree.CachedPID", rootDiagnostic) || elementPid <= 0) {
                passComplete = false;
                continue;
            }
            TrustedClientLease elementLease(trustedClients, static_cast<DWORD>(elementPid));
            if (!elementLease) continue;
            if (!UiChecked(element->get_CachedBoundingRectangle(&rect), L"Subtree.CachedBounds", rootDiagnostic) ||
                !UiChecked(element->get_CachedIsPassword(&password), L"Subtree.CachedPassword", rootDiagnostic) ||
                !UiChecked(element->get_CachedAutomationId(&rawId), L"Subtree.CachedID", rootDiagnostic)) {
                if (rawId) SysFreeString(rawId);
                passComplete = false;
                continue;
            }
            const auto id = BstrValue(rawId);
            const bool transport = type == UIA_ButtonControlTypeId || type == UIA_MenuItemControlTypeId;
            if (password || IsSensitiveUiId(id) ||
                std::find(trusted.begin(), trusted.end(), static_cast<DWORD>(elementPid)) == trusted.end() ||
                !IsBottomUiControl(hwnd, rect, *mainBounds, transport)) continue;
            if (type == UIA_SliderControlTypeId) {
                if (elementLease.Running()) sliders.push_back({{}, rect, type, element, sampledAt});
                continue;
            }
            if (MonotonicMs() >= deadline) { budgetExceeded = true; break; }
            int currentElementPid = 0;
            if (!UiChecked(element->get_CurrentProcessId(&currentElementPid), L"Subtree.Bottom.CurrentPID", rootDiagnostic) ||
                currentElementPid <= 0 || currentElementPid != elementPid) {
                passComplete = false;
                continue;
            }
            TrustedClientLease currentElementLease(trustedClients, static_cast<DWORD>(currentElementPid));
            if (!currentElementLease) continue;
            if (!UiChecked(element->get_CurrentName(&name), L"Subtree.Bottom.CurrentName", rootDiagnostic)) {
                if (name) SysFreeString(name);
                passComplete = false; continue;
            }
            const auto value = Trim(BstrValue(name));
            const auto nameSampledAt = MonotonicMs();
            if (elementLease.Running() && currentElementLease.Running() &&
                ((type == UIA_TextControlTypeId && (ParseClock(value) || ParsePair(value))) ||
                (transport && ButtonState(value) != PlayState::Unknown))) texts.push_back({value, rect, type, element, nameSampledAt});
        }
        if (MonotonicMs() > deadline) budgetExceeded = true;
        return passComplete && !budgetExceeded && !state->stop.load();
    };
    // Upstream's Text-only query avoids hundreds of individual provider RPCs.
    // Optional slider/button passes retain the prior native fallback behavior.
    bool clockScanComplete = collect(UIA_TextControlTypeId);
    const bool textHasPair = std::any_of(texts.begin(), texts.end(), [](const auto& text) { return ParsePair(text.text).has_value(); });
    if (!textHasPair) clockScanComplete = collect(UIA_SliderControlTypeId) && clockScanComplete;
    const bool menuScanComplete = collect(UIA_MenuItemControlTypeId);
    const bool buttonScanComplete = collect(UIA_ButtonControlTypeId);
    const bool stateScanComplete = menuScanComplete && buttonScanComplete;
    // A pair is accepted only if all observed pairs agree. Playlist track
    // lengths cannot be mistaken for two independent clock labels.
    std::optional<ClockPair> pair;
    RECT clockRect{};
    UiClockCache candidateCache;
    candidateCache.root = root;
    candidateCache.rootCaption = rootCaption;
    candidateCache.rootDiagnostic = rootDiagnostic;
    candidateCache.rootCaptionFromNative = captionFromNative;
    candidateCache.trustedClients = trustedClients;
    if (sameSource && previousCaption == rootCaption) {
        candidateCache.lastClock = previousClock;
        candidateCache.lastClockChangedAtMs = previousClockChangedAt;
    }
    int64_t clockSampledAtMs = 0;
    bool conflicting = false;
    for (const auto& text : texts) if (auto candidate = ParsePair(text.text)) {
        if (pair && (pair->position != candidate->position || pair->duration != candidate->duration)) conflicting = true;
        pair = candidate;
        clockRect = text.rect;
        candidateCache.pair = text.element;
        clockSampledAtMs = text.sampledAtMs;
    }
    if (!pair && !budgetExceeded) {
        // Independent mm:ss labels must flank a horizontal progress slider.
        // A percent-only slider has no known duration and is not converted to
        // seconds. Unknown arbitrary ranges are never inferred to be seconds.
        for (const auto& slider : sliders) {
            if (slider.rect.right - slider.rect.left < 120 || slider.rect.bottom <= slider.rect.top) continue;
            std::optional<int64_t> left, right;
            ComPtr<IUIAutomationElement> leftElement, rightElement;
            int64_t leftSampledAtMs = 0;
            for (const auto& text : texts) {
                auto time = ParseClock(text.text);
                if (!time) continue;
                const auto centerY = (text.rect.top + text.rect.bottom) / 2;
                const auto sliderY = (slider.rect.top + slider.rect.bottom) / 2;
                if (std::abs(centerY - sliderY) > 36) continue;
                if (text.rect.right <= slider.rect.left + 20 && slider.rect.left - text.rect.right <= 130) { left = time; leftElement = text.element; leftSampledAtMs = text.sampledAtMs; }
                if (text.rect.left >= slider.rect.right - 20 && text.rect.left - slider.rect.right <= 130) { right = time; rightElement = text.element; }
            }
            if (left && right && *right > 0 && *left <= *right) {
                const ClockPair candidate{*left, *right};
                if (pair && (pair->position != candidate.position || pair->duration != candidate.duration)) conflicting = true;
                pair = candidate;
                clockRect = slider.rect;
                candidateCache.left = leftElement;
                candidateCache.right = rightElement;
                candidateCache.slider = slider.element;
                clockSampledAtMs = leftSampledAtMs;
            }
        }
    }
    const auto sourceStillCurrent = [&] {
        return rootLease.Running() && selectedRootLease->Running() &&
            WindowTitle(hwnd) == result.nativeCaption && !state->stop.load();
    };
    const auto discardChangedSource = [&] {
        result.hasClock = false; result.clock = {}; result.state = PlayState::Unknown;
        result.sampledAtMs = 0; result.stateSampledAtMs = 0;
        result.diagnostic = L"冷读取期间主窗口、UIA 根节点或进程实例发生变化，丢弃时间及状态";
    };
    if (!sourceStillCurrent()) {
        discardChangedSource();
        return result;
    }
    candidateCache.pid = pid;
    candidateCache.hwnd = hwnd;
    candidateCache.identifiedAtMs = MonotonicMs();
    // The real 3.1.41 footer exposes Pause as MenuItem, with an Image child.
    // Read only MenuItem/Button, require a single central command, and ignore
    // the decorative Image duplicate. Never infer a clock from this state.
    if (stateScanComplete) for (const auto& text : texts) {
        if ((text.controlType == UIA_ButtonControlTypeId || text.controlType == UIA_MenuItemControlTypeId) &&
            ButtonState(text.text) != PlayState::Unknown && (!pair || conflicting || std::abs(text.rect.bottom - clockRect.bottom) <= 120))
            candidateCache.transportButtons.push_back(text.element);
    }
    if (candidateCache.transportButtons.size() > 1) {
        candidateCache.transportButtons.clear();
        AddDiagnostic(rootDiagnostic, L"底部播放／暂停命令不唯一，拒绝推断状态");
    }
    candidateCache.rootDiagnostic = rootDiagnostic;
    if (pair && !conflicting && clockScanComplete && !state->stop.load()) {
        result.hasClock = true;
        result.clock = *pair;
        result.sampledAtMs = clockSampledAtMs;
        result.diagnostic = L"只读 UI Automation 秒级时间标签；未证明歌曲 ID";
        AddDiagnostic(result.diagnostic, rootDiagnostic);
        ReadUiTransportState(candidateCache, hwnd, result, state, deadline, &clockRect);
        if (!sourceStillCurrent()) { discardChangedSource(); return result; }
        if (result.state == PlayState::Unknown) AddDiagnostic(result.diagnostic, L"播放状态未知，禁止自动推进歌词");
        ApplyChangedClock(candidateCache, result);
        cache = std::move(candidateCache);
    } else {
        result.diagnostic = conflicting ? L"UI 时间标签互相冲突，拒绝作为时间轴" : L"未发现可确认的 UI 秒级时间轴";
        AddDiagnostic(result.diagnostic, rootDiagnostic);
        candidateCache.pair.Reset(); candidateCache.left.Reset(); candidateCache.right.Reset(); candidateCache.slider.Reset();
        if (ReadUiTransportState(candidateCache, hwnd, result, state, deadline)) {
            if (!sourceStillCurrent()) { discardChangedSource(); return result; }
            AddDiagnostic(result.diagnostic, L"只有播放状态，无时间锚点；歌词保持静态，时间轴可靠性仍不可用");
            cache = std::move(candidateCache);
        }
    }
    if (budgetExceeded || MonotonicMs() >= deadline) AddDiagnostic(result.diagnostic, L"UI 冷读取超过 8000 毫秒软预算，拒绝未完成的候选；热读预算仍为 750 毫秒");
    return result;
}
void UiWorker(const std::shared_ptr<SharedState>& state) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) return;
    ComPtr<IUIAutomation> automation;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation))))
        CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation));
    if (automation) {
        ComPtr<IUIAutomation2> automation2;
        if (SUCCEEDED(automation.As(&automation2))) {
            automation2->put_ConnectionTimeout(1000);
            automation2->put_TransactionTimeout(500);
        }
        UiClockCache cache;
        uint64_t cachedGeneration = UINT64_MAX;
        while (!state->stop.load()) {
            ClientProcess client;
            HWND hwnd = nullptr;
            uint64_t generation = 0;
            {
                std::lock_guard lock(state->mutex);
                client = state->requestedClient;
                hwnd = state->requestedWindow;
                generation = state->uiGeneration;
            }
            if (generation != cachedGeneration) { cache = {}; cachedGeneration = generation; }
            if (client.pid) {
                auto observation = ObserveUi(automation.Get(), client, hwnd, state, cache);
                std::lock_guard lock(state->mutex);
                if (!state->stop.load() && generation == state->uiGeneration && SameClientInstance(client, state->requestedClient) &&
                    hwnd == state->requestedWindow &&
                    (observation.nativeCaption.empty() || observation.nativeCaption == state->requestedCaption))
                    state->ui = std::move(observation);
            }
            std::unique_lock lock(state->mutex);
            state->cv.wait_for(lock, 700ms, [&] { return state->stop.load() || state->uiGeneration != generation; });
        }
    }
    automation.Reset();
    CoUninitialize();
}
template <typename Async> bool AwaitBounded(const Async& operation, const std::shared_ptr<SharedState>& state, int64_t timeoutMs) {
    const auto deadline = MonotonicMs() + timeoutMs;
    while (!state->stop.load() && MonotonicMs() < deadline) {
        if (operation.Status() == winrt::Windows::Foundation::AsyncStatus::Completed) return true;
        if (operation.Status() != winrt::Windows::Foundation::AsyncStatus::Started) return false;
        if (!WaitFor(state, 20ms)) break;
    }
    operation.Cancel();
    return false;
}
int64_t FileTimeNowTicks() {
    FILETIME time{};
    GetSystemTimePreciseAsFileTime(&time);
    ULARGE_INTEGER value{};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return static_cast<int64_t>(value.QuadPart);
}
PlayState SessionState(GlobalSystemMediaTransportControlsSessionPlaybackStatus value) {
    switch (value) {
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing: return PlayState::Playing;
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Paused: return PlayState::Paused;
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Stopped: return PlayState::Stopped;
    default: return PlayState::Unknown;
    }
}
struct SessionEvents {
    GlobalSystemMediaTransportControlsSession session{nullptr};
    winrt::event_token media{}, playback{}, timeline{};
    std::weak_ptr<SharedState> owner;
    uint64_t generation = 0;
    bool mediaRegistered = false, playbackRegistered = false, timelineRegistered = false;
    void Reset() noexcept {
        // Never hold callbackMutex while asking a foreign WinRT source to
        // unsubscribe: a callback already in flight may need that mutex.
        if (session) {
            if (mediaRegistered) try { session.MediaPropertiesChanged(media); } catch (...) {}
            if (playbackRegistered) try { session.PlaybackInfoChanged(playback); } catch (...) {}
            if (timelineRegistered) try { session.TimelinePropertiesChanged(timeline); } catch (...) {}
        }
        if (auto shared = owner.lock()) {
            std::lock_guard delivery(shared->callbackMutex);
            if (generation && shared->smtcSessionGeneration.load() == generation) {
                ++shared->smtcSessionGeneration;
                shared->smtcMediaEventRevision = 0;
                ++shared->metadataRevision;
                shared->metadataDirty = true;
                shared->wake = true;
                shared->cv.notify_all();
            }
        }
        mediaRegistered = playbackRegistered = timelineRegistered = false;
        owner.reset();
        generation = 0;
        session = nullptr;
    }
    ~SessionEvents() { Reset(); }
    void Attach(const GlobalSystemMediaTransportControlsSession& value, const std::shared_ptr<SharedState>& state) {
        Reset();
        session = value;
        owner = state;
        {
            std::lock_guard delivery(state->callbackMutex);
            generation = ++state->smtcSessionGeneration;
            state->smtcMediaEventRevision = 0;
        }
        const std::weak_ptr<SharedState> weak(state);
        const auto attachedGeneration = generation;
        auto changed = [weak, attachedGeneration](const auto&, const auto&) {
            if (auto shared = weak.lock()) {
                std::lock_guard delivery(shared->callbackMutex);
                if (shared->smtcSessionGeneration.load() != attachedGeneration) return;
                shared->wake = true;
                shared->cv.notify_all();
            }
        };
        try {
            media = session.MediaPropertiesChanged([weak, attachedGeneration](const auto&, const auto&) {
                if (auto shared = weak.lock()) {
                    // Serialize invalidation with final snapshot validation.
                    // A delayed callback from an older attachment is ignored.
                    std::lock_guard delivery(shared->callbackMutex);
                    if (shared->smtcSessionGeneration.load() != attachedGeneration) return;
                    ++shared->smtcMediaEventRevision;
                    ++shared->metadataRevision; shared->metadataDirty = true; shared->wake = true; shared->cv.notify_all();
                }
            });
            mediaRegistered = true;
            playback = session.PlaybackInfoChanged(changed);
            playbackRegistered = true;
            timeline = session.TimelinePropertiesChanged(changed);
            timelineRegistered = true;
        } catch (...) {
            Reset();
            throw;
        }
    }
};
void Publish(const std::shared_ptr<SharedState>& state, const PlaybackSnapshot& snapshot) {
    std::lock_guard lock(state->callbackMutex);
    if (!state->stop.load() && state->callback) {
        // Keep the callable alive if a callback itself re-enters Stop/Start.
        const auto callback = state->callback;
        try { callback(snapshot); } catch (...) { /* a client callback cannot terminate the worker */ }
    }
}
// Caller holds SharedState::mutex. A fresh clock sample proves what this
// process is playing, not that an indefinitely old topology still selects it.
bool NativeOwnershipCurrent(const SharedState& state,DWORD pid,int64_t now) {
    constexpr int64_t ownershipBudgetMs=1500;
    const auto& discovery=state.discovery;
    return state.clientClockEnabled.load()&&state.nativePreferred&&pid!=0&&state.requestedPid==pid&&
        discovery.ready&&discovery.complete&&discovery.nativeOwnershipProven&&discovery.selected&&
        discovery.selected->pid==pid&&discovery.selected->createdAtTicks!=0&&
        discovery.selected->createdAtTicks==state.requestedClient.createdAtTicks&&
        discovery.scanStartedAtMs>0&&discovery.scanStartedAtMs<=discovery.observedAtMs&&
        discovery.observedAtMs<=now&&now-discovery.scanStartedAtMs<=ownershipBudgetMs;
}
bool NativeObservationCurrent(const SharedState& state,DWORD pid,int64_t now) {
    return state.nativeObservation.candidate&&state.nativeObservationPid==pid&&state.nativeObservationEpoch==state.nativeEpoch&&
        state.nativeObservation.sampledAtMs>0&&state.nativeObservation.sampledAtMs<=now&&
        now-state.nativeObservation.sampledAtMs<=500;
}
bool NativeIdentityCurrent(const SharedState& state,DWORD pid,int64_t now) {
    return NativeOwnershipCurrent(state,pid,now)&&state.nativeIdentityAvailability==NativeIdentityAvailability::Current&&
        NativeObservationCurrent(state,pid,now)&&state.nativeObservation.stable&&
        SameSongId(state.nativeIdentityId,state.nativeObservation.songId);
}
ClientClockObservation LatestNativeObservation(const std::shared_ptr<SharedState>& state,DWORD pid,int64_t after=0) {
    std::unique_lock lock(state->mutex);
    if(after>0)state->nativeCv.wait_for(lock,50ms,[&]{return state->stop.load()||!state->clientClockEnabled.load()||state->requestedPid!=pid||
        (state->nativeObservationPid==pid&&state->nativeObservationEpoch==state->nativeEpoch&&state->nativeObservation.sampledAtMs>after);});
    if(!state->clientClockEnabled.load()||state->requestedPid!=pid||state->nativeObservationEpoch!=state->nativeEpoch||state->nativeObservationPid!=pid)return {};
    if(after>0&&state->nativeObservation.sampledAtMs<=after)return {};
    if(state->nativeObservation.candidate&&!NativeObservationCurrent(*state,pid,MonotonicMs()))return {};
    return state->nativeObservation;
}
void ApplyNativeTime(PlaybackSnapshot& snapshot,const ClientClockObservation& observation) {
    const auto now=MonotonicMs();
    snapshot.stateSampledAtMs=observation.stateSampledAtMs;
    snapshot.state=observation.stateSampledAtMs>0&&observation.stateSampledAtMs<=now&&now-observation.stateSampledAtMs<=1500?
        observation.state:PlayState::Unknown;
    snapshot.positionMs=observation.positionMs;snapshot.durationMs=observation.durationMs;
    snapshot.sampledAtMs=observation.sampledAtMs;snapshot.observedAtMs=observation.sampledAtMs;
    snapshot.reliability=Reliability::NativeRead;snapshot.nativeClockAdvanceAllowed=observation.advanceAllowed;snapshot.metadataStale=false;
    if(snapshot.source.find(L"独立原生时钟采样")==std::wstring::npos)snapshot.source+=L" + 独立原生时钟采样";
}
void ApplyNativeIdentity(PlaybackSnapshot& snapshot,const ClientClockObservation& observation) {
    const bool sameIdentity=snapshot.songIdVerified&&SameSongId(snapshot.songId,observation.songId);
    if(!sameIdentity){
        snapshot.title.clear();snapshot.artist.clear();snapshot.album.clear();snapshot.catalogMetadataReady=false;
    }
    snapshot.songId=observation.songId;snapshot.songIdVerified=true;snapshot.metadataStale=false;
    snapshot.identityReacquiring=false;snapshot.recordCandidateId.clear();snapshot.durationMs=observation.durationMs;
    if(snapshot.source.find(L"独立原生歌曲身份")==std::wstring::npos)snapshot.source+=L" + 独立原生歌曲身份";
}
void InvalidateNativeIdentity(PlaybackSnapshot& snapshot,int64_t observedAt) {
    snapshot.reliability=Reliability::Unavailable;snapshot.state=PlayState::Unknown;
    snapshot.sampledAtMs=0;snapshot.stateSampledAtMs=0;snapshot.observedAtMs=observedAt;snapshot.songIdVerified=false;
    snapshot.songId.clear();snapshot.recordCandidateId.clear();snapshot.metadataStale=true;snapshot.catalogMetadataReady=false;
    snapshot.identityReacquiring=false;snapshot.nativeClockAdvanceAllowed=false;
    AddDiagnostic(snapshot.diagnostic,L"当前原生歌曲身份尚待确认；保留信息仅属此前歌曲，不显示实时逐字跟随");
}
void MarkNativeIdentityReacquiring(PlaybackSnapshot& snapshot,int64_t observedAt) {
    InvalidateNativeIdentity(snapshot,observedAt);snapshot.identityReacquiring=true;
    snapshot.diagnostic.clear();
    AddDiagnostic(snapshot.diagnostic,L"原生歌曲身份待重新确认，正在限时核验；此前歌词只作静态保留");
}
// Called with SharedState::mutex held. Metadata describes a song; only a fresh
// observation of this process/epoch can make that song CURRENT.
bool NativeRecordCurrent(const SharedState& state, DWORD pid, int64_t now) {
    return state.clientClockEnabled.load() && state.nativeRecordPid == pid && state.nativeRecordEpoch == state.nativeEpoch &&
        state.nativeRecord.metadataValidated && state.nativeRecord.nativeCorroborated && NativeIdentityCurrent(state, pid, now) &&
        SameSongId(state.nativeRecord.id, state.nativeObservation.songId) &&
        NativeDurationMatches(state.nativeRecord.durationMs, state.nativeObservation.durationMs);
}
void MergeNativeRecord(PlaybackSnapshot& snapshot, const SharedState& state) {
    snapshot.title = state.nativeRecord.title; snapshot.artist = state.nativeRecord.artist; snapshot.album = state.nativeRecord.album;
    snapshot.songId = state.nativeRecord.id; snapshot.songIdVerified = true; snapshot.metadataStale = false;snapshot.catalogMetadataReady=true;
    snapshot.recordCandidateId.clear();
    snapshot.durationMs = state.nativeRecord.durationMs;
    if (snapshot.source.find(L"原生ID资料") == std::wstring::npos) snapshot.source += L" + 原生ID资料";
    AddDiagnostic(snapshot.diagnostic, state.nativeRecord.diagnostic);
    if(NativeCacheOwnerCurrentLocked(state,state.nativeCacheDiagnostic.owner))
        AddDiagnostic(snapshot.diagnostic,state.nativeCacheDiagnostic.text);
}
void PublishMetadata(const std::shared_ptr<SharedState>& state,PlaybackSnapshot snapshot,bool preferNative,bool nativeIdentity,uint64_t epoch,
                     uint64_t selectionRevision,int64_t selectionObservedAt,uint64_t metadataNativeRevision,
                     bool metadataFromSmtc,uint64_t smtcMetadataRevision,uint64_t smtcSessionGeneration,
                     uint64_t smtcMediaEventRevision) {
    // Both producers take callbackMutex before mutex. The accepted seed and
    // its delivery form one ordered transaction, including reentrant Stop.
    std::lock_guard delivery(state->callbackMutex);
    bool suppress=false;
    {
        std::lock_guard lock(state->mutex);
        const auto publishedAt=MonotonicMs();
        if(state->stop.load()||snapshot.pid!=state->requestedPid||epoch!=state->nativeEpoch||
            snapshot.processCreatedAtTicks!=state->requestedClient.createdAtTicks||!state->discovery.ready||
            selectionRevision!=state->discovery.revision||selectionObservedAt<=0||selectionObservedAt>publishedAt||
            publishedAt-selectionObservedAt>1500)return;
        state->nativePreferred=preferNative;
        if(snapshot.stateSampledAtMs>=state->stateHintSampledAtMs){
            state->stateHintPid=snapshot.pid;state->stateHint=snapshot.state;state->stateHintSampledAtMs=snapshot.stateSampledAtMs;
        }
        const bool enabled=state->clientClockEnabled.load();
        // Once the independent reader has accepted an identity, only that
        // reader may publish song ownership and time. SMTC/caption data still
        // supplies a state hint above, but cannot overwrite or revive an ID.
        const auto availability=state->nativeIdentityAvailability;
        // After an independent identity expires, a non-stale fallback must
        // come from the current availability generation. SMTC additionally
        // needs a real media event after expiry, and every fallback must differ
        // from the last native song metadata. Internal refresh requests alone
        // therefore cannot revive the frozen old song.
        const auto currentSmtcSessionGeneration=state->smtcSessionGeneration.load();
        const auto currentSmtcEventRevision=state->smtcMediaEventRevision.load();
        const bool currentSmtcSnapshot=metadataFromSmtc&&smtcSessionGeneration!=0&&
            smtcSessionGeneration==currentSmtcSessionGeneration&&smtcMediaEventRevision==currentSmtcEventRevision;
        const bool smtcEventAfterNativeExpiry=currentSmtcSnapshot&&smtcMediaEventRevision>0&&
            ((smtcSessionGeneration==state->nativeExpiredSmtcSessionGeneration&&
              smtcMediaEventRevision>state->nativeExpiredAfterSmtcEventRevision)||
             smtcSessionGeneration>state->nativeExpiredSmtcSessionGeneration);
        std::wstring currentCaptionTitle,currentCaptionArtist;
        SplitCaption(state->requestedCaption,currentCaptionTitle,currentCaptionArtist);
        const bool stableCaptionBoundary=state->nativeStableCaptionPid==snapshot.pid&&
            state->nativeStableCaptionCreatedAt==snapshot.processCreatedAtTicks&&
            !state->nativeStableCaptionTitle.empty()&&!state->nativeStableCaptionArtist.empty();
        const bool nativeSeedBoundary=state->nativeSeedReady&&state->nativeSeed.pid==snapshot.pid&&
            state->nativeSeed.processCreatedAtTicks==snapshot.processCreatedAtTicks&&
            !state->nativeSeed.title.empty()&&!state->nativeSeed.artist.empty();
        const auto& fallbackBoundaryTitle=nativeSeedBoundary?state->nativeSeed.title:state->nativeStableCaptionTitle;
        const auto& fallbackBoundaryArtist=nativeSeedBoundary?state->nativeSeed.artist:state->nativeStableCaptionArtist;
        const bool fallbackBoundaryComparable=nativeSeedBoundary||stableCaptionBoundary;
        const bool snapshotDiffersFromFallbackBoundary=fallbackBoundaryComparable&&!snapshot.title.empty()&&!snapshot.artist.empty()&&
            (snapshot.title!=fallbackBoundaryTitle||!ArtistSourcesCompatible(snapshot.artist,fallbackBoundaryArtist));
        const bool expiredSmtcWithoutNewIdentity=availability==NativeIdentityAvailability::Expired&&metadataFromSmtc&&
            (!smtcEventAfterNativeExpiry||!snapshotDiffersFromFallbackBoundary);
        const bool currentCaptionAvailable=!currentCaptionTitle.empty()&&!currentCaptionArtist.empty();
        const bool snapshotMatchesCurrentCaption=currentCaptionAvailable&&snapshot.title==currentCaptionTitle&&
            ArtistSourcesCompatible(snapshot.artist,currentCaptionArtist);
        const bool captionChangedSinceStableNative=snapshotMatchesCurrentCaption&&fallbackBoundaryComparable&&
            (currentCaptionTitle!=fallbackBoundaryTitle||
             (nativeSeedBoundary?!ArtistSourcesCompatible(currentCaptionArtist,fallbackBoundaryArtist):
                !SameArtistCredit(currentCaptionArtist,fallbackBoundaryArtist)));
        // A later discovery timestamp proves only that the window still
        // exists. It does not prove that unchanged caption text belongs to a
        // new song. With no comparable stable-native caption, fail closed until
        // the native identity returns or SMTC supplies a real media event.
        const bool expiredCaptionWithoutObservedChange=availability==NativeIdentityAvailability::Expired&&
            !snapshot.metadataStale&&!metadataFromSmtc&&!captionChangedSinceStableNative;
        const bool expiredFallbackUnproven=availability==NativeIdentityAvailability::Expired&&!snapshot.metadataStale&&
            (metadataNativeRevision!=state->nativeAvailabilityRevision||expiredSmtcWithoutNewIdentity||
             expiredCaptionWithoutObservedChange);
        const bool smtcSnapshotSuperseded=metadataFromSmtc&&(!currentSmtcSnapshot||
            smtcMetadataRevision!=state->metadataRevision.load()||state->metadataDirty.load());
        const bool sameNativeInstance=enabled&&state->requestedPid==snapshot.pid&&
            snapshot.processCreatedAtTicks==state->requestedClient.createdAtTicks;
        const bool nativeCurrentOrReacquiring=availability==NativeIdentityAvailability::Current||
            availability==NativeIdentityAvailability::Reacquiring;
        // When ownership becomes temporarily unproven, a stale discovery seed
        // must still reach the app so its old clock freezes. A non-stale
        // caption/SMTC fallback remains fenced by the native availability
        // generation regardless of the transient ownership flag.
        const bool nativeFallbackSuppressed=sameNativeInstance&&
            ((preferNative&&nativeCurrentOrReacquiring)||
             (!snapshot.metadataStale&&(nativeCurrentOrReacquiring||expiredFallbackUnproven)));
        suppress=smtcSnapshotSuperseded||nativeFallbackSuppressed;
        (void)nativeIdentity;
        if(snapshot.reliability==Reliability::NativeRead){snapshot.reliability=Reliability::Unavailable;snapshot.sampledAtMs=0;}
        if(!snapshot.metadataStale&&!snapshot.songIdVerified&&!snapshot.title.empty())
            AddDiagnostic(snapshot.diagnostic,L"歌曲 ID 未证实，在线搜索只可作为候选");
    }
    if(!suppress)Publish(state,snapshot);
    state->nativeCv.notify_all();
}
void PublishNative(const std::shared_ptr<SharedState>& state,const ClientClockObservation& observation,DWORD pid,uint64_t epoch) {
    std::lock_guard delivery(state->callbackMutex);PlaybackSnapshot snapshot;
    {
        std::lock_guard lock(state->mutex);
        if(state->stop.load()||!state->clientClockEnabled.load()||state->requestedPid!=pid||state->nativeEpoch!=epoch||
           !state->nativePreferred)return;
        const auto now=MonotonicMs();
        const auto availability=state->nativeIdentityAvailability;
        if(availability==NativeIdentityAvailability::Current){
            if(!NativeIdentityCurrent(*state,pid,now))return;
            if(observation.sampledAtMs<=state->lastNativeDeliveredAtMs&&
               SameSongId(observation.songId,state->lastNativeDeliveredId)&&
               state->nativeAvailabilityDeliveredRevision==state->nativeAvailabilityRevision)return;
            const bool sameSeed=state->nativeSeedReady&&state->nativeSeed.pid==pid&&
                state->nativeSeed.processCreatedAtTicks==state->requestedClient.createdAtTicks&&
                SameSongId(state->nativeSeed.songId,observation.songId)&&
                (!state->nativeSeed.catalogMetadataReady||NativeRecordCurrent(*state,pid,now));
            snapshot=sameSeed?state->nativeSeed:PlaybackSnapshot{};
            snapshot.pid = pid; snapshot.processCreatedAtTicks = state->requestedClient.createdAtTicks;
            snapshot.clientVersion = state->requestedClient.version; snapshot.minimized = state->requestedClient.minimized;
            snapshot.source = L"独立原生歌曲身份／进度"; snapshot.diagnostic.clear();
            ApplyNativeIdentity(snapshot,observation);
            if(NativeRecordCurrent(*state,pid,now))MergeNativeRecord(snapshot,*state);
            else AddDiagnostic(snapshot.diagnostic,state->nativeLookupDiagnostic.empty()?L"歌曲ID与进度已确认，标题和歌手资料仍在加载":state->nativeLookupDiagnostic);
            ApplyNativeTime(snapshot,observation);
            state->nativeSeed = snapshot; state->nativeSeedReady = true; state->nativeSeedIdentity = true;
            state->lastNativeDeliveredAtMs=observation.sampledAtMs;state->lastNativeDeliveredId=observation.songId;
        }else{
            if(availability==NativeIdentityAvailability::None||!state->nativeSeedReady||!state->nativeSeedIdentity||
               state->nativeSeed.pid!=pid||state->nativeAvailabilityDeliveredRevision==state->nativeAvailabilityRevision)return;
            snapshot=state->nativeSeed;snapshot.source=L"独立原生歌曲身份／进度";
            if(availability==NativeIdentityAvailability::Reacquiring)MarkNativeIdentityReacquiring(snapshot,now);
            else InvalidateNativeIdentity(snapshot,now);
            AddDiagnostic(snapshot.diagnostic,observation.diagnostic);
            AddDiagnostic(snapshot.diagnostic,state->nativeLookupDiagnostic);
        }
        state->nativeAvailabilityDeliveredRevision=state->nativeAvailabilityRevision;
    }
    Publish(state,snapshot);
}
void NativeClockWorker(const std::shared_ptr<SharedState>& state) {
    constexpr int64_t lossDebounceMs=100;
    constexpr int64_t reacquireWindowMs=1000;
    ClientClockReader reader;uint64_t activeEpoch=UINT64_MAX;DWORD activePid=0;
    while(!state->stop.load()){
        DWORD pid=0;uint64_t epoch=0,processCreatedAt=0;bool enabled=false,preferred=true,sampleAvailable=false;
        PlayState playState=PlayState::Unknown;int64_t stateSampledAt=0,captionObservedAt=0;
        std::wstring captionBeforeNativeSample;
        {
            std::lock_guard lock(state->mutex);pid=state->requestedPid;epoch=state->nativeEpoch;processCreatedAt=state->requestedClient.createdAtTicks;
            enabled=state->clientClockEnabled.load();preferred=state->nativePreferred;
            captionObservedAt=state->discovery.observedAtMs;
            try{captionBeforeNativeSample=state->requestedCaption;}catch(...){captionObservedAt=0;}
            const auto now=MonotonicMs();
            if(state->stateHintPid==pid&&state->stateHintSampledAtMs>0&&state->stateHintSampledAtMs<=now&&now-state->stateHintSampledAtMs<=1500){playState=state->stateHint;stateSampledAt=state->stateHintSampledAtMs;}
            if(state->ui.pid==pid&&state->ui.hwnd==state->requestedWindow&&state->ui.nativeCaption==state->requestedCaption&&
               state->ui.stateSampledAtMs>stateSampledAt&&state->ui.stateSampledAtMs<=now&&now-state->ui.stateSampledAtMs<=1500){playState=state->ui.state;stateSampledAt=state->ui.stateSampledAtMs;}
        }
        if(epoch!=activeEpoch||pid!=activePid){reader.Reset();activeEpoch=epoch;activePid=pid;}
        if(enabled&&pid){
            ClientClockObservation observation;
            try{observation=reader.Observe(pid,playState,processCreatedAt);}
            catch(...){reader.Reset();observation.diagnostic=L"原生采样暂不可用，等待重试";}
            sampleAvailable=observation.candidate;
            observation.state=playState;observation.stateSampledAtMs=stateSampledAt;
            {
                // Identity availability and its first callback are one ordered
                // transaction. A metadata producer cannot observe Current and
                // publish an older SMTC/caption seed before this native sample.
                std::lock_guard delivery(state->callbackMutex);
                bool committed=false,identityChanged=false;
                {
                    std::lock_guard lock(state->mutex);
                    if(!state->stop.load()&&state->clientClockEnabled.load()&&state->requestedPid==pid&&state->nativeEpoch==epoch){
                        const auto now=MonotonicMs();
                        const bool ownershipCurrent=NativeOwnershipCurrent(*state,pid,now);
                        if(!ownershipCurrent)AddDiagnostic(observation.diagnostic,
                            L"当前进程播放所有权缺少新鲜的完整发现证据，原生采样暂不作为当前歌曲");
                        state->nativeObservation=observation;state->nativeObservationPid=pid;state->nativeObservationEpoch=epoch;committed=true;
                        const bool stable=ownershipCurrent&&observation.candidate&&observation.stable&&SameSongId(observation.songId,observation.songId)&&
                            NativeObservationCurrent(*state,pid,now);
                        if(stable){
                            const bool newIdentity=!SameSongId(state->nativeIdentityId,observation.songId);
                            if(newIdentity){
                                CancelNativeDetailsLocked(*state);
                                state->nativeLookupStop.request_stop();state->nativeLookupStop=std::stop_source{};
                                state->nativeIdentityId=observation.songId;++state->nativeIdentityRevision;
                                state->nativeStableCaptionPid=0;state->nativeStableCaptionCreatedAt=0;
                                state->nativeIdentityConfirmedAtMs=observation.sampledAtMs;
                                state->nativeStableCaptionTitle.clear();state->nativeStableCaptionArtist.clear();
                                state->nativeRecord={};state->nativeRecordPid=0;state->nativeRecordEpoch=0;
                                ClearNativeCacheDiagnosticLocked(*state);
                                state->nativeLookupDiagnostic=L"原生歌曲ID已由连续样本确认，正在补充该ID资料";
                                state->lastNativeDeliveredAtMs=0;state->lastNativeDeliveredId.clear();
                            }
                            if(newIdentity||state->nativeIdentityAvailability!=NativeIdentityAvailability::Current){
                                state->nativeIdentityAvailability=NativeIdentityAvailability::Current;
                                ++state->nativeAvailabilityRevision;identityChanged=true;
                            }
                            // Discovery completion is the conservative caption
                            // observation time. Never associate text committed
                            // after this native sample with its song ID.
                            if(!newIdentity&&captionObservedAt>state->nativeIdentityConfirmedAtMs&&
                                    captionObservedAt<observation.sampledAtMs){
                                std::wstring stableTitle,stableArtist;
                                SplitCaption(captionBeforeNativeSample,stableTitle,stableArtist);
                                if(!stableTitle.empty()&&!stableArtist.empty()){
                                    state->nativeStableCaptionPid=pid;
                                    state->nativeStableCaptionCreatedAt=state->requestedClient.createdAtTicks;
                                    state->nativeStableCaptionTitle=std::move(stableTitle);
                                    state->nativeStableCaptionArtist=std::move(stableArtist);
                                }
                            }
                            state->nativeLossStartedAtMs=0;
                        }else if(SameSongId(state->nativeIdentityId,state->nativeIdentityId)){
                            if(state->nativeIdentityAvailability==NativeIdentityAvailability::Current){
                                // A fresh different-ID candidate contradicts
                                // the old song immediately, even before the
                                // new ID earns continuous-sample confirmation.
                                // Only missing/invalid/same-ID reads debounce.
                                const bool conflictingCandidate=NativeObservationCurrent(*state,pid,now)&&
                                    SameSongId(observation.songId,observation.songId)&&
                                    !SameSongId(state->nativeIdentityId,observation.songId);
                                if(!state->nativeLossStartedAtMs)state->nativeLossStartedAtMs=now;
                                if(conflictingCandidate||now-state->nativeLossStartedAtMs>=lossDebounceMs){
                                    state->nativeIdentityAvailability=NativeIdentityAvailability::Reacquiring;
                                    ++state->nativeAvailabilityRevision;identityChanged=true;
                                }
                            }else if(state->nativeIdentityAvailability==NativeIdentityAvailability::Reacquiring&&
                                     state->nativeLossStartedAtMs>0&&now-state->nativeLossStartedAtMs>=reacquireWindowMs){
                                state->nativeIdentityAvailability=NativeIdentityAvailability::Expired;
                                ++state->nativeAvailabilityRevision;identityChanged=true;
                                // Invalidate any in-flight/cached SMTC media
                                // request. A fallback may resume only after a
                                // new observation in this availability generation.
                                ++state->metadataRevision;state->metadataDirty=true;
                                state->nativeExpiredSmtcSessionGeneration=state->smtcSessionGeneration.load();
                                state->nativeExpiredAfterSmtcEventRevision=state->smtcMediaEventRevision.load();
                                CancelNativeDetailsLocked(*state);
                                state->nativeLookupStop.request_stop();state->nativeLookupStop=std::stop_source{};
                                state->nativeIdentityId.clear();++state->nativeIdentityRevision;
                                state->nativeRecord={};state->nativeRecordPid=0;state->nativeRecordEpoch=0;
                                ClearNativeCacheDiagnosticLocked(*state);
                                state->nativeLookupDiagnostic=L"原生歌曲身份超过限时仍未恢复，等待新的连续稳定样本";
                            }
                        }
                    }
                }
                if(committed){state->nativeCv.notify_all();if(identityChanged){state->wake=true;state->cv.notify_all();}PublishNative(state,observation,pid,epoch);}
            }
        }else reader.Reset();
        std::unique_lock lock(state->mutex);
        state->nativeCv.wait_for(lock,enabled&&pid&&preferred&&sampleAvailable?33ms:250ms,[&]{return state->stop.load()||state->requestedPid!=pid||
            state->nativeEpoch!=epoch||state->clientClockEnabled.load()!=enabled||state->nativePreferred!=preferred;});
    }
}
void NativeDetailsWorker(const std::shared_ptr<SharedState>& state) {
    while (!state->stop.load()) {
        std::shared_ptr<NativeDetailsRequest> request;
        {
            std::unique_lock lock(state->mutex);
            state->detailsCv.wait(lock, [&] {
                return state->stop.load() || (state->nativeDetailsRequest && !state->nativeDetailsRequest->started);
            });
            if (state->stop.load()) break;
            request = state->nativeDetailsRequest; request->started = true;
        }
        SongRecord record;
        try {
            // Linking the generation to a private HTTP stop source lets local
            // recovery cancel this request without cancelling the song itself.
            std::stop_callback cancel(request->generationStop, [source = request->httpStop]() mutable { source.request_stop(); });
            const auto stop=request->httpStop.get_token();std::wstring cacheDiagnostic;
            const auto cached=request->identity.candidate&&request->identity.stable?
                LoadSongMetadataCache(request->identity.songId,request->identity.durationMs,stop,cacheDiagnostic):std::optional<SongMetadata>{};
            if(cached&&!stop.stop_requested()&&SameSongId(cached->id,request->identity.songId)&&NativeDurationMatches(cached->durationMs,request->identity.durationMs)){
                record.id=cached->id;record.title=cached->title;record.artist=cached->artist;record.album=cached->album;
                record.durationMs=cached->durationMs;record.metadataValidated=true;record.nativeCorroborated=true;record.fromMetadataCache=true;
                record.diagnostic=std::move(cacheDiagnostic);
            }else if(!stop.stop_requested()){
                record = ReadNativeSongDetails(request->identity, stop);
                if(!record.metadataValidated)AddDiagnostic(record.diagnostic,cacheDiagnostic);
            }else record.diagnostic=L"原生ID歌曲资料查询已取消";
        } catch (...) { record.diagnostic = L"原生ID歌曲详情请求异常，等待自动重试"; }
        {
            std::lock_guard lock(state->mutex);
            // A cancelled/obsolete request owns only its result object. It
            // cannot restore a removed slot or publish any playback identity.
            request->result = std::move(record); request->complete = true;
        }
        state->nativeCv.notify_all();
    }
}
void NativeMetadataCacheWorker(const std::shared_ptr<SharedState>& state) {
    while(!state->stop.load()){
        NativeCacheWrite request;
        {
            std::unique_lock lock(state->mutex);
            state->nativeCacheCv.wait(lock,[&]{return state->stop.load()||!state->nativeCacheWrites.empty();});
            if(state->stop.load())break;
            request=std::move(state->nativeCacheWrites.front());state->nativeCacheWrites.pop_front();
        }
        // Optional disk writes never hold playback locks or the details worker.
        // Stop suppresses callbacks immediately; ordinary file I/O may finish
        // later with shared state only, just like the other detached workers.
        std::wstring diagnostic;const bool saved=SaveSongMetadataCache(request.metadata,diagnostic);
        {
            std::lock_guard lock(state->mutex);
            if(!NativeCacheOwnerCurrentLocked(*state,request.owner))continue;
            if(saved){
                if(SameNativeCacheOwner(state->nativeCacheDiagnostic.owner,request.owner))ClearNativeCacheDiagnosticLocked(*state);
            }else state->nativeCacheDiagnostic=NativeCacheDiagnostic{std::move(request.owner),std::move(diagnostic)};
        }
    }
}
void NativeMetadataWorker(const std::shared_ptr<SharedState>& state) {
    struct PendingNativeMetadata {
        SongRecord record;
        DWORD pid=0;
        uint64_t epoch=0,revision=0;
        int64_t readyAtMs=0;
    };
    SystemSqlite recordApi;
    std::deque<SongRecord> cache;
    std::optional<PendingNativeMetadata> pending;
    DWORD activePid = 0; uint64_t activeEpoch = UINT64_MAX, activeRevision = UINT64_MAX;
    int64_t localRetryAt = 0, networkRetryAt = 0;
    unsigned failures = 0;
    std::wstring networkDiagnostic;
    while (!state->stop.load()) {
      try {
        ClientProcess client; ClientClockObservation identity;
        uint64_t epoch = 0, revision = 0; std::stop_token stop;
        std::optional<SongRecord> completedDetails;
        {
            std::unique_lock lock(state->mutex);
            state->nativeCv.wait_for(lock, 250ms, [&] {
                return state->stop.load() || (state->clientClockEnabled.load() && state->nativePreferred && state->requestedPid &&
                    NativeIdentityCurrent(*state, state->requestedPid, MonotonicMs()) &&
                    !NativeRecordCurrent(*state, state->requestedPid, MonotonicMs()));
            });
            if (state->stop.load()) break;
            if (!state->clientClockEnabled.load() || !state->nativePreferred || !state->requestedPid ||
                !NativeIdentityCurrent(*state, state->requestedPid, MonotonicMs()) ||
                NativeRecordCurrent(*state, state->requestedPid, MonotonicMs())) continue;
            client = state->requestedClient; identity = state->nativeObservation;
            epoch = state->nativeEpoch; revision = state->nativeIdentityRevision; stop = state->nativeLookupStop.get_token();
            if (client.pid != activePid || epoch != activeEpoch || revision != activeRevision) {
                activePid = client.pid; activeEpoch = epoch; activeRevision = revision;
                localRetryAt = 0; networkRetryAt = 0; failures = 0; networkDiagnostic.clear();
                pending.reset();
            }
            if(pending&&(pending->pid!=client.pid||pending->epoch!=epoch||pending->revision!=revision||
                !SameSongId(pending->record.id,identity.songId)||!NativeDurationMatches(pending->record.durationMs,identity.durationMs))){
                pending.reset();localRetryAt=0;
            }
            if (state->nativeDetailsRequest && NativeDetailsMatch(*state->nativeDetailsRequest, client.pid, epoch, revision) &&
                state->nativeDetailsRequest->complete) {
                completedDetails = std::move(state->nativeDetailsRequest->result);
                state->nativeDetailsRequest.reset();
            }
            if (!pending && !completedDetails && MonotonicMs() < localRetryAt) {
                state->nativeCv.wait_for(lock, 250ms, [&] { return state->stop.load() || state->nativeEpoch != epoch ||
                    state->nativeIdentityRevision != revision || (state->nativeDetailsRequest &&
                    NativeDetailsMatch(*state->nativeDetailsRequest, client.pid, epoch, revision) && state->nativeDetailsRequest->complete); });
                continue;
            }
        }
        SongRecord record;
        const bool metadataPrepared=pending.has_value();
        if(metadataPrepared)record=pending->record;
        else{
            const auto cached = std::find_if(cache.begin(), cache.end(), [&](const SongRecord& item) {
                return SameSongId(item.id, identity.songId) && NativeDurationMatches(item.durationMs, identity.durationMs);
            });
            if (cached != cache.end()) {
                record = *cached; cache.erase(cached);
            } else {
                record = ReadClientSongRecord(recordApi, client, state, {}, {}, false, identity);
            }
        }
        if (!metadataPrepared && !record.metadataValidated && completedDetails && !stop.stop_requested()) {
            if (completedDetails->metadataValidated && SameSongId(completedDetails->id, identity.songId) &&
                NativeDurationMatches(completedDetails->durationMs, identity.durationMs)) {
                const auto localDiagnostic = record.diagnostic;
                record = std::move(*completedDetails); AddDiagnostic(record.diagnostic, localDiagnostic);
            } else {
                constexpr int64_t backoff[]{3000, 8000, 15000, 30000, 60000};
                const auto index = std::min<size_t>(failures, std::size(backoff) - 1);
                if (failures < std::size(backoff)) ++failures;
                networkRetryAt = MonotonicMs() + backoff[index];
                networkDiagnostic = completedDetails->metadataValidated ? L"歌曲详情与当前原生ID或时长不符" : completedDetails->diagnostic;
            }
        }
        bool queuedDetails = false;
        if(!metadataPrepared){
            std::lock_guard lock(state->mutex);
            if (state->stop.load() || stop.stop_requested() || !state->clientClockEnabled.load() || !state->nativePreferred ||
                state->requestedPid != client.pid ||
                state->nativeEpoch != epoch || state->nativeIdentityRevision != revision) continue;
            if (record.metadataValidated) {
                if (state->nativeDetailsRequest && NativeDetailsMatch(*state->nativeDetailsRequest, client.pid, epoch, revision))
                    CancelNativeDetailsLocked(*state);
            } else {
                if (MonotonicMs() >= networkRetryAt && !state->nativeDetailsRequest &&
                    NativeIdentityCurrent(*state, client.pid, MonotonicMs())) {
                    auto request = std::make_shared<NativeDetailsRequest>();
                    request->pid = client.pid; request->epoch = epoch; request->revision = revision;
                    request->identity = identity; request->generationStop = stop;
                    state->nativeDetailsRequest = std::move(request); queuedDetails = true;
                }
                AddDiagnostic(record.diagnostic, networkDiagnostic);
                if (state->nativeDetailsRequest) AddDiagnostic(record.diagnostic, L"同ID的HTTPS资料查询在独立线程进行，本地继续重试");
            }
        }
        if (queuedDetails) state->detailsCv.notify_one();
        if(!metadataPrepared)localRetryAt = MonotonicMs() + 1000;
        if (stop.stop_requested() || state->stop.load()) continue;
        if (!metadataPrepared && record.metadataValidated) {
            // Bounded static metadata cache, never a cache of current playback.
            cache.push_front(record); if (cache.size() > 64) cache.pop_back();
        }
        // A sample taken DURING a slow query is not its completion barrier.
        // Retain both ready metadata and its ORIGINAL completion time when the
        // bounded sample wait misses. Sampling recovery must not redo I/O or
        // wait for the one-second missing-metadata retry interval.
        const auto queryCompletedAt = metadataPrepared?pending->readyAtMs:MonotonicMs();
        if(!metadataPrepared&&record.metadataValidated&&record.nativeCorroborated)
            pending=PendingNativeMetadata{record,client.pid,epoch,revision,queryCompletedAt};
        const auto after = record.metadataValidated ? LatestNativeObservation(state, client.pid, queryCompletedAt) : ClientClockObservation{};
        bool committed = false;
        {
            std::lock_guard lock(state->mutex);
            if (state->stop.load() || stop.stop_requested() || !state->clientClockEnabled.load() || !state->nativePreferred ||
                state->requestedPid != client.pid ||
                state->nativeEpoch != epoch || state->nativeIdentityRevision != revision) continue;
            state->nativeLookupDiagnostic = record.diagnostic;
            if (record.metadataValidated && record.nativeCorroborated && after.candidate && after.stable && after.sampledAtMs > queryCompletedAt &&
                SameSongId(record.id, after.songId) && NativeDurationMatches(record.durationMs, after.durationMs) &&
                NativeIdentityCurrent(*state, client.pid, MonotonicMs()) &&
                state->nativeObservation.sampledAtMs>queryCompletedAt&&
                SameSongId(record.id, state->nativeObservation.songId) &&
                NativeDurationMatches(record.durationMs, state->nativeObservation.durationMs)) {
                state->nativeRecord = std::move(record); state->nativeRecordPid = client.pid; state->nativeRecordEpoch = epoch;
                state->nativeLookupDiagnostic.clear(); committed = true;
                QueueNativeCacheWriteLocked(*state);
            }else if(pending)AddDiagnostic(state->nativeLookupDiagnostic,L"原生ID资料已就绪，等待资料完成后的新鲜稳定样本");
        }
        if (committed) { pending.reset();state->wake = true; state->cv.notify_all(); state->nativeCv.notify_all();state->nativeCacheCv.notify_one(); }
      } catch (...) {
        std::unique_lock lock(state->mutex);
        state->nativeLookupDiagnostic = L"原生歌曲资料暂不可读，保留身份校验并自动重试";
        state->nativeCv.wait_for(lock, 1s, [&] { return state->stop.load(); });
      }
    }
}
PlaybackSnapshot DiscoveryPlaybackSnapshot(const ClientDiscoverySnapshot& discovery) {
    PlaybackSnapshot snapshot;
    snapshot.source=L"独立本机进程／窗口发现";
    snapshot.observedAtMs=discovery.observedAtMs;
    AddDiagnostic(snapshot.diagnostic,discovery.diagnostic);
    if(discovery.selected){
        const auto& client=*discovery.selected;
        snapshot.pid=client.pid;snapshot.processCreatedAtTicks=client.createdAtTicks;
        snapshot.clientVersion=client.version;snapshot.minimized=client.minimized;
        SplitCaption(client.caption,snapshot.title,snapshot.artist);
        // A live process is not itself a current-song identity. In tray and
        // minimized states an empty caption must remain stale until native ID,
        // SMTC, or another independently corroborated identity is available.
        const bool ambiguousNativeOwner=discovery.complete&&RootClientCount(discovery.clients)>1&&
            !discovery.nativeOwnershipProven;
        snapshot.metadataStale=snapshot.title.empty()||snapshot.artist.empty()||ambiguousNativeOwner;
        if(ambiguousNativeOwner)AddDiagnostic(snapshot.diagnostic,L"多个主进程并存且播放所有权尚未核验，窗口文字只作静态显示");
        if(client.hwnd)AddDiagnostic(snapshot.diagnostic,L"所选网易云主窗口 class="+client.windowClass+L"，caption="+client.caption);
        else AddDiagnostic(snapshot.diagnostic,client.windowAmbiguous?L"多个主窗口候选同分，窗口标题歌曲不可识别":L"未确认网易云主窗口，窗口标题歌曲不可识别");
        if(client.hwnd&&snapshot.title.empty())AddDiagnostic(snapshot.diagnostic,L"窗口标题没有可确认的歌曲格式");
        if(client.minimized)AddDiagnostic(snapshot.diagnostic,L"主窗口已最小化");
        else if(!client.visible)AddDiagnostic(snapshot.diagnostic,L"无可见主窗口（可能位于托盘，尚未证明）");
    }else{
        AddDiagnostic(snapshot.diagnostic,!discovery.complete?L"进程发现未完成，且没有可复用的已核验存活实例":
            discovery.clients.empty()?L"未发现正在运行的网易云音乐":L"主进程关联有歧义或实例已变化");
        snapshot.metadataStale=!discovery.complete||!discovery.clients.empty();
    }
    // Discovery is metadata only; its timestamp never becomes a music clock.
    return snapshot;
}
bool SameDiscoveryCandidates(const std::vector<ClientProcess>& first,const std::vector<ClientProcess>& second) {
    return first.size()==second.size()&&std::all_of(first.begin(),first.end(),[&](const auto& client){
        return std::any_of(second.begin(),second.end(),[&](const auto& other){
            return client.pid==other.pid&&client.createdAtTicks==other.createdAtTicks&&client.path==other.path;
        });
    });
}
bool SameSelectedClient(const std::optional<ClientProcess>& first,const std::optional<ClientProcess>& second) {
    if(first.has_value()!=second.has_value())return false;
    return !first||(first->pid==second->pid&&first->createdAtTicks==second->createdAtTicks&&first->path==second->path&&
        first->version==second->version&&first->hwnd==second->hwnd&&first->caption==second->caption&&
        first->windowClass==second->windowClass&&first->visible==second->visible&&first->minimized==second->minimized&&
        first->windowAmbiguous==second->windowAmbiguous);
}
void ClientDiscoveryWorker(const std::shared_ptr<SharedState>& state) {
    BoundClientProcess boundClient;
    NativeClockTargetProbe nativeClockProbe;
    uint64_t observedClockModeRevision=state->clientClockModeRevision.load();
    while(!state->stop.load()){
      BoundClientProcess stagedBinding;
      std::optional<ClientProcess> stagedSelection;
      bool stagedSelectionReplacesBound=false,stagedSelectionAfterExpiry=false;
      bool stagedSelectionNativeOwnershipProven=false;
      ClientProcess scannedRequestedClient;
      uint64_t scannedNativeAvailabilityRevision=0;
      uint64_t scannedClientVersionRevision=0;
      NativeIdentityAvailability scannedAvailability=NativeIdentityAvailability::None;
      int64_t scannedNativeLossStartedAtMs=0;
      try {
        bool clockEnabledAtScan=false;uint64_t clockModeRevision=0;
        ClientDiscoverySnapshot next;next.ready=true;
        // Capture before EnumWindows/process discovery begins. If native
        // availability changes during the scan, publication sees an older
        // generation and rejects its non-stale caption as a fallback.
        {
            std::lock_guard lock(state->mutex);if(state->stop.load())return;
            next.scanStartedAtMs=MonotonicMs();
            scannedClientVersionRevision=state->clientVersionRevision;
            next.metadataNativeRevision=state->nativeAvailabilityRevision;
            scannedNativeAvailabilityRevision=state->nativeAvailabilityRevision;
            scannedAvailability=state->nativeIdentityAvailability;
            scannedNativeLossStartedAtMs=state->nativeLossStartedAtMs;
            scannedRequestedClient=state->requestedClient;
            clockEnabledAtScan=state->clientClockEnabled.load();
            clockModeRevision=state->clientClockModeRevision.load();
        }
        if(clockModeRevision!=observedClockModeRevision){
            nativeClockProbe.Reset();
            // Re-enabling native ownership starts a fresh election. Otherwise
            // a still-live process chosen while native evidence was disabled
            // could bypass the multi-root ownership probe indefinitely.
            if(clockEnabledAtScan)boundClient.Reset();
            observedClockModeRevision=clockModeRevision;
        }
        try {
            next.clients=DiscoverClients(&next.complete,&next.diagnostic);
            {
                std::lock_guard lock(state->mutex);
                ApplyClientVersionLocked(*state,next.clients);
            }
            if(next.complete){
                const auto preferred=PickClient(next.clients);
                const auto rootCount=RootClientCount(next.clients);
                // Keep the exact kernel-verified instance while it is still in
                // a complete scan. A different visible root cannot steal the
                // background/tray binding merely by owning the only window.
                next.selected=boundClient.ReuseListed(next.clients);
                if(next.selected&&!IsRootClient(*next.selected,next.clients)){
                    // A complete topology scan can reveal that the previously
                    // bound instance is now a child. It must not inherit the
                    // unique-root proof from a different process.
                    next.selected.reset();boundClient.Reset();
                    AddDiagnostic(next.diagnostic,L"既有进程绑定已不再属于主进程，重新选择并核验播放所有权");
                }
                const auto selectionNow=MonotonicMs();
                const bool canConsiderRebind=next.selected&&scannedAvailability==NativeIdentityAvailability::Expired&&
                    SameClientInstance(scannedRequestedClient,*next.selected)&&
                    scannedNativeLossStartedAtMs>0&&selectionNow-scannedNativeLossStartedAtMs>=3000;
                std::optional<ClientProcess> nativeClockCandidate;
                const bool nativeClockEnabled=clockEnabledAtScan;
                if(nativeClockEnabled&&rootCount>1){
                    auto choice=nativeClockProbe.Observe(next.clients);nativeClockCandidate=std::move(choice.selected);
                    AddDiagnostic(next.diagnostic,choice.diagnostic);
                }else if(!nativeClockEnabled||rootCount<=1)nativeClockProbe.Reset();
                const bool waitingForNativeOwnership=!next.selected&&nativeClockEnabled&&rootCount>1&&
                    !nativeClockCandidate&&nativeClockProbe.TopologyObservedAtMs()>0&&
                    selectionNow-nativeClockProbe.TopologyObservedAtMs()<1000;
                if(next.selected){
                    const bool preferredIsBound=preferred&&preferred->pid==next.selected->pid&&
                        preferred->createdAtTicks==next.selected->createdAtTicks&&Lower(preferred->path)==Lower(next.selected->path);
                    const bool uniqueRootIsBound=rootCount==1&&preferred&&SameClientInstance(*preferred,*next.selected)&&
                        IsRootClient(*next.selected,next.clients);
                    next.retained=!preferredIsBound||rootCount!=1;
                    next.nativeOwnershipProven=uniqueRootIsBound||
                        (nativeClockCandidate&&SameClientInstance(*nativeClockCandidate,*next.selected));
                    if(nativeClockCandidate&&!SameClientInstance(*nativeClockCandidate,*next.selected)){
                        if(stagedBinding.Select(*nativeClockCandidate)){
                            stagedSelection=*nativeClockCandidate;
                            stagedSelectionReplacesBound=true;
                            stagedSelectionAfterExpiry=canConsiderRebind;
                            stagedSelectionNativeOwnershipProven=true;
                        }
                        else AddDiagnostic(next.diagnostic,L"唯一原生时钟候选未能复核，继续保留旧进程绑定");
                    }
                }else{
                    // A unique advancing native clock is stronger than a
                    // window heuristic and remains available for tray startup.
                    // Give it a short bounded observation window; if ownership
                    // stays inconclusive, retain the ordinary unique-window
                    // selection policy rather than guessing from a DLL alone.
                    next.selected=nativeClockCandidate?nativeClockCandidate:
                        waitingForNativeOwnership?std::optional<ClientProcess>{}:preferred;
                    next.retained=next.selected.has_value()&&rootCount!=1;
                    next.nativeOwnershipProven=next.selected.has_value()&&
                        ((rootCount==1&&preferred&&SameClientInstance(*preferred,*next.selected)&&
                          IsRootClient(*next.selected,next.clients))||
                          (nativeClockCandidate&&SameClientInstance(*nativeClockCandidate,*next.selected)));
                    if(next.selected&&nativeClockCandidate&&preferred&&!SameClientInstance(*nativeClockCandidate,*preferred))
                        AddDiagnostic(next.diagnostic,L"窗口候选与实时进度证据不同，采用唯一通过连续前进核验的进程");
                    if(next.selected){
                        if(stagedBinding.Select(*next.selected)){
                            stagedSelection=*next.selected;
                            stagedSelectionNativeOwnershipProven=next.nativeOwnershipProven;
                        }else{
                            next.selected.reset();next.retained=false;next.nativeOwnershipProven=false;
                            AddDiagnostic(next.diagnostic,L"新选择的进程实例未能复核，不绑定旧PID");
                        }
                    }
                }
                // A first/new selection is not the retained binding until the
                // scan and state-generation checks below commit it atomically.
            }else{
                next.selected=boundClient.ReuseRunning();
                next.retained=next.selected.has_value();
                // An incomplete scan can hide an unreadable or newly started
                // root process. Keep the exact live handle only as a process
                // lease; current playback ownership must be established again
                // from a later complete topology sample.
                next.nativeOwnershipProven=false;
                if(next.selected)AddDiagnostic(next.diagnostic,
                    L"进程发现未完整，已保留实例句柄但撤销原生播放所有权，等待完整扫描重新核验");
            }
        }catch(...){
            stagedSelection.reset();stagedBinding.Reset();
            stagedSelectionReplacesBound=false;stagedSelectionAfterExpiry=false;
            stagedSelectionNativeOwnershipProven=false;
            next.complete=false;next.clients.clear();next.selected=boundClient.ReuseRunning();
            next.diagnostic=L"进程发现暂时失败，只允许复用仍存活的已核验实例";
            next.retained=next.selected.has_value();
            // The throw may have happened after contradictory multi-root
            // evidence was observed. Retain only the exact kernel binding;
            // native playback ownership must be proved again by a complete scan.
            next.nativeOwnershipProven=false;
        }
        next.observedAtMs=MonotonicMs();const auto observedAt=next.observedAtMs;
        const auto metadataNativeRevision=next.metadataNativeRevision;
        PlaybackSnapshot snapshot;ClientProcess requestedClient;std::wstring requestedCaption;
        DWORD requestedPid=0;HWND requestedWindow=nullptr;
        bool publishSeed=false,preferNative=false;uint64_t epoch=0,revision=0;
        // Match all other publishers' lock order. A new instance's discovery
        // seed is delivered before that instance's first native callback.
        std::lock_guard delivery(state->callbackMutex);
        {
            std::lock_guard lock(state->mutex);
            if(state->stop.load())return;
            const bool stagedSelectionListed=stagedSelection&&std::any_of(next.clients.begin(),next.clients.end(),[&](const auto& client){
                return SameClientInstance(client,*stagedSelection);
            });
            const bool selectionStillValid=next.complete&&stagedSelectionListed&&stagedBinding.Running()&&
                IsRootClient(*stagedSelection,next.clients)&&
                (!stagedSelectionReplacesBound||state->clientClockEnabled.load())&&
                SameClientInstance(state->requestedClient,scannedRequestedClient)&&
                state->nativeAvailabilityRevision==scannedNativeAvailabilityRevision&&
                state->clientClockModeRevision.load()==clockModeRevision;
            if(selectionStillValid){
                next.selected=*stagedSelection;next.retained=RootClientCount(next.clients)!=1;
                next.nativeOwnershipProven=stagedSelectionNativeOwnershipProven;
                if(stagedSelectionReplacesBound)AddDiagnostic(next.diagnostic,stagedSelectionAfterExpiry?
                    L"旧进程的原生身份持续过期，已换绑到唯一连续通过实时进度前进核验的实例":
                    L"另一进程提供了唯一且连续的实时进度前进证据，已更新原生绑定");
                boundClient.Swap(stagedBinding);
            }else if(stagedSelection&&!stagedSelectionReplacesBound){
                next.selected.reset();next.retained=false;next.nativeOwnershipProven=false;
                AddDiagnostic(next.diagnostic,L"进程候选在提交前已变化，本轮不建立绑定");
            }
            if(state->clientClockModeRevision.load()!=clockModeRevision)next.nativeOwnershipProven=false;
            if(next.retained)AddDiagnostic(next.diagnostic,next.complete?
                L"多个主进程候选并存；仅使用已核验的原生进程绑定，不尝试歧义的SMTC关联":
                L"仅凭仍存活的既有进程句柄保留原生绑定，本轮不使用窗口或SMTC关联");
            snapshot=DiscoveryPlaybackSnapshot(next);
            requestedClient=next.selected.value_or(ClientProcess{});requestedCaption=requestedClient.caption;
            requestedPid=requestedClient.pid;requestedWindow=requestedClient.hwnd;
            const auto& previous=state->discovery;
            const bool instanceChanged=!SameClientInstance(state->requestedClient,requestedClient);
            const bool selectionChanged=!previous.ready||!SameSelectedClient(previous.selected,next.selected)||
                previous.complete!=next.complete||previous.retained!=next.retained||
                previous.nativeOwnershipProven!=next.nativeOwnershipProven||!SameDiscoveryCandidates(previous.clients,next.clients);
            next.revision=previous.revision+(selectionChanged?1:0);revision=next.revision;
            publishSeed=!previous.ready||instanceChanged||(!next.selected&&selectionChanged)||
                (selectionChanged&&previous.nativeOwnershipProven!=next.nativeOwnershipProven);
            const bool uiChanged=instanceChanged||state->requestedWindow!=requestedWindow||state->requestedCaption!=requestedCaption||
                previous.complete!=next.complete||previous.retained!=next.retained;
            if(instanceChanged){
                CancelNativeDetailsLocked(*state);
                state->nativeLookupStop.request_stop();state->nativeLookupStop=std::stop_source{};
                ++state->nativeIdentityRevision;state->nativeIdentityId.clear();
                state->nativeRecord={};state->nativeRecordPid=0;state->nativeLookupDiagnostic.clear();
                ClearNativeCacheDiagnosticLocked(*state);
                state->nativeIdentityAvailability=NativeIdentityAvailability::None;++state->nativeAvailabilityRevision;
                state->nativeAvailabilityDeliveredRevision=0;state->nativeLossStartedAtMs=0;
                state->nativeExpiredSmtcSessionGeneration=state->smtcSessionGeneration.load();
                state->nativeExpiredAfterSmtcEventRevision=state->smtcMediaEventRevision.load();
                state->nativeStableCaptionPid=0;state->nativeStableCaptionCreatedAt=0;
                state->nativeIdentityConfirmedAtMs=0;
                state->nativeStableCaptionTitle.clear();state->nativeStableCaptionArtist.clear();
                ++state->nativeEpoch;state->nativeObservation={};state->nativeObservationPid=0;
                state->nativeSeed={};state->nativeSeedReady=false;state->nativeSeedIdentity=false;
                state->lastNativeDeliveredAtMs=0;state->lastNativeDeliveredId.clear();
                state->stateHintPid=0;state->stateHint=PlayState::Unknown;state->stateHintSampledAtMs=0;
            }
            state->requestedClient=std::move(requestedClient);state->requestedPid=requestedPid;
            state->requestedWindow=requestedWindow;state->requestedCaption=std::move(requestedCaption);
            preferNative=state->clientClockEnabled.load()&&requestedPid!=0&&next.nativeOwnershipProven;
            state->nativePreferred=preferNative;
            if(uiChanged){++state->uiGeneration;state->ui={};}
            epoch=state->nativeEpoch;state->discovery=std::move(next);
            RequestClientVersionLocked(*state);
            if(selectionChanged){
                ++state->metadataRevision;state->metadataDirty=true;
                state->wake=true;state->cv.notify_all();
            }
        }
        state->nativeCv.notify_all();
        if(publishSeed)PublishMetadata(state,std::move(snapshot),preferNative,false,epoch,revision,observedAt,metadataNativeRevision,false,0,0,0);
      }catch(...){
        // No new discovery sample is claimed if construction/commit fails.
        // Existing native readers still validate their own live instance.
      }
        std::unique_lock lock(state->mutex);
        state->discoveryCv.wait_for(lock,250ms,[&]{return state->stop.load()||
            state->clientClockModeRevision.load()!=observedClockModeRevision||
            state->clientVersionRevision!=scannedClientVersionRevision;});
    }
}
void MonitorWorker(const std::shared_ptr<SharedState>& state) {
    bool apartmentReady=false;
    try{winrt::init_apartment(winrt::apartment_type::multi_threaded);apartmentReady=true;}
    catch(...){} // Discovery/native playback have no dependency on this COM worker.
    GlobalSystemMediaTransportControlsSessionManager manager{nullptr};
    SessionEvents sessionEvents;
    GlobalSystemMediaTransportControlsSessionMediaProperties media{nullptr};
    int64_t managerRetry=0,lastMediaRead=0;
    uint64_t lastMediaRevision=0,lastDiscoveryRevision=0,mediaNativeRevision=0;
    uint64_t mediaSessionGeneration=0,mediaEventRevision=0;
    std::wstring lastSource;DWORD lastPid=0;uint64_t lastProcessCreatedAt=0;
    std::wstring lastMediaIdentity;int64_t lastTimelineTicks=0;
    PlayState previousState=PlayState::Unknown;bool timelineNeedsRefresh=false;
    SystemSqlite recordApi;SongRecord cachedRecord;DWORD recordPid=0;uint64_t recordCreatedAt=0;
    std::wstring recordIdentity;int64_t lastRecordRead=0;
    while(!state->stop.load()){
        ClientDiscoverySnapshot discovery;uint64_t nativeEpoch=0;
        {
            std::unique_lock lock(state->mutex);
            if(!state->discovery.ready){state->cv.wait_for(lock,250ms,[&]{return state->stop.load()||state->discovery.ready;});continue;}
            discovery=state->discovery;nativeEpoch=state->nativeEpoch;
        }
        const auto now=MonotonicMs();
        if(discovery.observedAtMs<=0||discovery.observedAtMs>now||now-discovery.observedAtMs>1500){
            if(!WaitFor(state,250ms))break;continue;
        }
        const auto& clients=discovery.clients;const auto& client=discovery.selected;
        const auto rootClientCount=RootClientCount(clients);
        const bool discoveryComplete=discovery.complete,retainedProcess=discovery.retained;
        if(lastDiscoveryRevision!=discovery.revision){
            // A request rejected at publication must not survive in this
            // worker's local cache and be published by the next iteration.
            media=nullptr;mediaNativeRevision=0;mediaSessionGeneration=0;mediaEventRevision=0;
            lastMediaRevision=0;lastMediaRead=0;lastDiscoveryRevision=discovery.revision;
            state->metadataDirty=true;
        }
        auto snapshot=DiscoveryPlaybackSnapshot(discovery);
        uint64_t snapshotNativeRevision=discovery.metadataNativeRevision,snapshotSmtcRevision=0;
        uint64_t snapshotSmtcSessionGeneration=0,snapshotSmtcEventRevision=0;
        bool snapshotFromSmtc=false;
        if(!apartmentReady)AddDiagnostic(snapshot.diagnostic,L"SMTC COM初始化失败，独立原生身份和进度仍继续");
        if (apartmentReady && !manager && now >= managerRetry) {
            try {
                const auto request = GlobalSystemMediaTransportControlsSessionManager::RequestAsync();
                if (AwaitBounded(request, state, 1000)) manager = request.GetResults();
            } catch (...) {}
            managerRetry = now + 5000;
        }
        GlobalSystemMediaTransportControlsSession selected{nullptr};
        std::wstring selectedSource;
        std::wstring sessionFailure;
        if (retainedProcess) sessionFailure=discoveryComplete?
            L"多个网易云主进程候选并存，本轮不尝试无法绑定到PID的SMTC会话":
            L"进程发现未完整，本轮只使用已绑定实例的原生ID与进度";
        else if(discoveryComplete&&rootClientCount!=1)
            sessionFailure=L"网易云主进程不唯一，SMTC无法可靠绑定到当前PID";
        if (manager && discoveryComplete && !retainedProcess && rootClientCount==1 && !state->stop.load()) try {
            std::vector<GlobalSystemMediaTransportControlsSession> matching;
            for (auto session : manager.GetSessions()) {
                const std::wstring source(session.SourceAppUserModelId());
                if (IsNeteaseName(source)) matching.push_back(session);
            }
            if (matching.size() == 1) {
                selected = matching.front();
                selectedSource = selected.SourceAppUserModelId().c_str();
            } else if (matching.size() > 1) sessionFailure = L"多个网易云媒体会话，未任意选择";
        } catch (...) { sessionFailure = L"媒体会话读取失败"; manager = nullptr; managerRetry = now + 5000; }
        if (selected && client) {
            // AUMID is not an OS PID. Accept the association only when its
            // app-name identifies the discovered unique root client; expose
            // this limitation rather than implying an undocumented PID API.
            const auto sourceLower = Lower(selectedSource);
            const bool associated = rootClientCount==1&&
                (sourceLower.find(L"cloudmusic") != std::wstring::npos||sourceLower.find(L"netease") != std::wstring::npos);
            if (!associated) { selected = nullptr; sessionFailure = L"媒体会话无法明确关联客户端主进程"; }
        } else if (selected) { selected = nullptr; sessionFailure = L"媒体会话存在，但无法明确关联本机网易云 PID"; }
        if (selected) try {
            if (!sessionEvents.session || sessionEvents.session != selected || lastSource != selectedSource || lastPid != snapshot.pid ||
                lastProcessCreatedAt!=client->createdAtTicks) {
                sessionEvents.Attach(selected, state);
                media = nullptr;mediaNativeRevision=0;mediaSessionGeneration=0;mediaEventRevision=0;lastMediaRevision=0;
                ++state->metadataRevision;
                state->metadataDirty = true;
                lastSource = selectedSource;
                lastPid = snapshot.pid;
                lastProcessCreatedAt=client->createdAtTicks;
                lastMediaIdentity.clear();
                lastTimelineTicks = 0;
                previousState = PlayState::Unknown;
                timelineNeedsRefresh = false;
            }
            if (state->metadataDirty.exchange(false) || now - lastMediaRead > 5000) {
                uint64_t requestedRevision=0,requestedSessionGeneration=0,requestedEventRevision=0;
                {
                    std::lock_guard delivery(state->callbackMutex);
                    requestedRevision=state->metadataRevision.load();
                    requestedSessionGeneration=state->smtcSessionGeneration.load();
                    requestedEventRevision=state->smtcMediaEventRevision.load();
                }
                uint64_t requestedNativeRevision=0;
                {std::lock_guard lock(state->mutex);requestedNativeRevision=state->nativeAvailabilityRevision;}
                const auto request = selected.TryGetMediaPropertiesAsync();
                bool requestStillCurrent=false;
                GlobalSystemMediaTransportControlsSessionMediaProperties result{nullptr};
                if(AwaitBounded(request,state,700)){
                    result=request.GetResults();
                    std::lock_guard delivery(state->callbackMutex);
                    requestStillCurrent=requestedRevision==state->metadataRevision.load()&&
                        requestedSessionGeneration==state->smtcSessionGeneration.load()&&
                        requestedEventRevision==state->smtcMediaEventRevision.load();
                }
                if (requestStillCurrent) {
                    media=std::move(result);lastMediaRevision=requestedRevision;mediaNativeRevision=requestedNativeRevision;
                    mediaSessionGeneration=requestedSessionGeneration;mediaEventRevision=requestedEventRevision;
                }
                else { media=nullptr;mediaNativeRevision=0;mediaSessionGeneration=0;mediaEventRevision=0;lastMediaRevision=0;
                    AddDiagnostic(snapshot.diagnostic,L"媒体元数据读取超时、会话已更换或内容已被更新"); }
                lastMediaRead = MonotonicMs();
            }
            if (media) {
                snapshot.title = media.Title().c_str();
                snapshot.artist = media.Artist().c_str();
                snapshot.album = media.AlbumTitle().c_str();
                snapshot.metadataStale=snapshot.title.empty()||snapshot.artist.empty();
                snapshotNativeRevision=mediaNativeRevision;
                snapshotFromSmtc=true;snapshotSmtcRevision=lastMediaRevision;
                snapshotSmtcSessionGeneration=mediaSessionGeneration;snapshotSmtcEventRevision=mediaEventRevision;
            }
            const auto info = selected.GetPlaybackInfo();
            snapshot.state = SessionState(info.PlaybackStatus());
            snapshot.stateSampledAtMs=MonotonicMs();
            const auto timeline = selected.GetTimelineProperties();
            const auto startMs = timeline.StartTime().count() / 10000;
            const auto endMs = timeline.EndTime().count() / 10000;
            const auto rawPosition = timeline.Position().count() / 10000;
            const auto updatedTicks = timeline.LastUpdatedTime().time_since_epoch().count();
            const auto ageMs = (FileTimeNowTicks() - updatedTicks) / 10000;
            const std::wstring identity = media ? snapshot.title + L"\n" + snapshot.artist + L"\n" + snapshot.album : std::wstring{};
            if (updatedTicks != lastTimelineTicks) timelineNeedsRefresh = false;
            if (lastTimelineTicks > 0 && updatedTicks == lastTimelineTicks) {
                // LastUpdatedTime is an anchor, not a heartbeat. Unchanged
                // anchors are normal during uninterrupted playback. A known
                // pause/resume or track change without a new anchor, however,
                // cannot be projected safely from the previous song/clock.
                if ((!lastMediaIdentity.empty() && !identity.empty() && identity != lastMediaIdentity) ||
                    (previousState != PlayState::Unknown && snapshot.state != previousState &&
                        (previousState == PlayState::Paused || snapshot.state == PlayState::Paused)))
                    timelineNeedsRefresh = true;
            }
            if (!identity.empty()) lastMediaIdentity = identity;
            lastTimelineTicks = updatedTicks;
            previousState = snapshot.state;
            snapshot.durationMs = std::max<int64_t>(0, endMs - startMs);
            snapshot.positionMs = std::max<int64_t>(0, rawPosition - startMs);
            snapshot.source = L"Windows GSMTC · " + selectedSource;
            AddDiagnostic(snapshot.diagnostic, L"会话应用标识与唯一网易云主进程关联；GSMTC 不直接公开 PID");
            if (updatedTicks > 0 && ageMs >= -500 && ageMs <= 24 * 3600 * 1000LL &&
                snapshot.durationMs > 0 && snapshot.positionMs <= snapshot.durationMs) {
                snapshot.sampledAtMs = MonotonicMs() - std::max<int64_t>(0, ageMs);
                if (timelineNeedsRefresh) {
                    AddDiagnostic(snapshot.diagnostic, L"切歌／暂停／恢复后时间轴锚点未更新，冻结歌词并尝试界面观测");
                } else if (snapshot.state == PlayState::Playing) {
                    const auto rate = info.PlaybackRate();
                    if (rate && std::abs(rate.Value() - 1.0) > 0.001) {
                        AddDiagnostic(snapshot.diagnostic, L"会话播放速度不是 1 倍，当前快照合同无法可靠投影");
                    } else if (snapshot.positionMs + std::max<int64_t>(0, ageMs) > snapshot.durationMs + 1000) {
                        AddDiagnostic(snapshot.diagnostic, L"锚点投影已超过歌曲末尾，等待新的时间轴，不循环推进旧歌曲");
                    } else {
                        snapshot.reliability = Reliability::Estimated;
                        AddDiagnostic(snapshot.diagnostic, L"位置按客户端 LastUpdatedTime 锚点及 1 倍速估算，锚点年龄 " + std::to_wstring(std::max<int64_t>(0, ageMs)) + L" 毫秒；尚未实测同步误差");
                    }
                } else if (snapshot.state == PlayState::Paused || snapshot.state == PlayState::Stopped) {
                    snapshot.reliability = Reliability::Precise;
                    AddDiagnostic(snapshot.diagnostic, L"保留客户端公布的静止位置；Precise 表示接口分辨率，不等于 250 毫秒实测通过");
                }
            } else {
                AddDiagnostic(snapshot.diagnostic, L"媒体时间轴缺少有效时长／更新时间");
            }
            if (snapshot.state == PlayState::Unknown) {
                snapshot.reliability = Reliability::Unavailable;
                AddDiagnostic(snapshot.diagnostic, L"会话播放状态未知");
            }
        } catch (...) {
            selected = nullptr;
            sessionFailure = L"网易云媒体会话读取异常，回退界面观测";
            snapshot.state = PlayState::Unknown;
            snapshot.stateSampledAtMs=0;
            snapshot.reliability = Reliability::Unavailable;
            sessionEvents.Reset();
            media=nullptr;mediaNativeRevision=0;mediaSessionGeneration=0;mediaEventRevision=0;lastMediaRevision=0;
        }
        if (!selected || snapshot.reliability == Reliability::Unavailable) {
            UiObservation observed;
            {
                std::lock_guard lock(state->mutex);
                observed = state->ui;
            }
            const auto uiObservedNow = MonotonicMs();
            const bool uiMatches = client && observed.pid == client->pid && observed.hwnd == client->hwnd && observed.nativeCaption == client->caption;
            const bool stateFresh = uiMatches && observed.stateSampledAtMs > 0 && uiObservedNow >= observed.stateSampledAtMs &&
                uiObservedNow - observed.stateSampledAtMs <= 1500;
            const bool clockFresh = uiMatches && observed.hasClock && observed.sampledAtMs > 0 &&
                uiObservedNow >= observed.sampledAtMs && uiObservedNow - observed.sampledAtMs <= 1500;
            const bool pausedClockHeld = uiMatches && observed.hasClock && observed.sampledAtMs > 0 && stateFresh &&
                observed.state == PlayState::Paused && uiObservedNow >= observed.sampledAtMs;
            if (clockFresh || pausedClockHeld) {
                snapshot.positionMs = observed.clock.position;
                snapshot.durationMs = observed.clock.duration;
                snapshot.sampledAtMs = observed.sampledAtMs;
                snapshot.reliability = Reliability::Observed;
                snapshot.source = L"只读 UI Automation／窗口标题";
                if (snapshot.state == PlayState::Unknown && stateFresh){snapshot.state=observed.state;snapshot.stateSampledAtMs=observed.stateSampledAtMs;}
                AddDiagnostic(snapshot.diagnostic, observed.diagnostic);
            } else if (client) {
                if (!selected){snapshot.state=PlayState::Unknown;snapshot.stateSampledAtMs=0;}
                AddDiagnostic(snapshot.diagnostic, observed.diagnostic.empty() ? L"无可靠时间轴；等待客户端可访问数据" : observed.diagnostic);
                if (observed.sampledAtMs > 0) AddDiagnostic(snapshot.diagnostic, L"UI 观测已过期，不复用旧进度");
            }
            if (snapshot.state == PlayState::Unknown && stateFresh && observed.state != PlayState::Unknown) {
                snapshot.state = observed.state;
                snapshot.stateSampledAtMs=observed.stateSampledAtMs;
                snapshot.source += L" + 只读 UIA 播放状态";
                AddDiagnostic(snapshot.diagnostic, L"状态来自当前受信底部唯一 MenuItem/Button，独立采样；没有时钟时不提升时间轴可靠性");
            }
            AddDiagnostic(snapshot.diagnostic, sessionFailure);
        }
        if (!selected && sessionEvents.session) { sessionEvents.Reset();media=nullptr;mediaNativeRevision=0;
            mediaSessionGeneration=0;mediaEventRevision=0;lastMediaRevision=0;lastSource.clear(); }
        const bool wantsNativeClock=state->clientClockEnabled.load()&&client.has_value()&&discovery.nativeOwnershipProven;
        ClientClockObservation nativeIdentity;
        if(wantsNativeClock)nativeIdentity=LatestNativeObservation(state,client->pid);
        snapshot.songId.clear();
        snapshot.recordCandidateId.clear();
        snapshot.songIdVerified = false;
        // Native ID metadata has its own worker. Legacy title/history matching
        // is supplemental only when no current native candidate is available.
        if (client && !nativeIdentity.candidate && !snapshot.title.empty()) {
            const auto currentRecordIdentity = client->caption + L"\n" + snapshot.title + L"\n" + snapshot.artist;
            if (recordPid != client->pid || recordCreatedAt!=client->createdAtTicks || recordIdentity != currentRecordIdentity || MonotonicMs() - lastRecordRead >= 1000) {
                const bool sessionIdentityFresh = selected && media && !state->metadataDirty.load() &&
                    lastMediaRevision == state->metadataRevision.load();
                cachedRecord = ReadClientSongRecord(recordApi, *client, state,
                    snapshot.title, snapshot.artist, sessionIdentityFresh, {});
                recordPid = client->pid;
                recordCreatedAt=client->createdAtTicks;
                recordIdentity = currentRecordIdentity;
                lastRecordRead = MonotonicMs();
            }
            if (cachedRecord.metadataValidated&&cachedRecord.title == snapshot.title &&
                RecordArtistMatches(snapshot.artist,cachedRecord.artist,cachedRecord.primaryArtist)) {
                snapshot.recordCandidateId = cachedRecord.id;
                snapshot.artist=cachedRecord.artist;
                if (snapshot.album.empty()) snapshot.album = cachedRecord.album;
                if (snapshot.durationMs <= 0) snapshot.durationMs = cachedRecord.durationMs;
                snapshot.source += L" + 最近播放记录候选（未证实当前ID）";
            }
            AddDiagnostic(snapshot.diagnostic, cachedRecord.diagnostic);
        } else { cachedRecord = {}; recordPid = 0; recordCreatedAt=0;recordIdentity.clear(); }
        AddDiagnostic(snapshot.diagnostic, nativeIdentity.diagnostic);
        if (client && !state->clientClockEnabled.load()) AddDiagnostic(snapshot.diagnostic, L"原生只读内部时钟已在设置中关闭");
        // Completion time proves this monitor is still observing the source.
        // It does not replace the media timeline's anchor or a UIA sample.
        snapshot.observedAtMs = MonotonicMs();
        PublishMetadata(state,snapshot,wantsNativeClock,false,nativeEpoch,discovery.revision,discovery.observedAtMs,
            snapshotNativeRevision,snapshotFromSmtc,snapshotSmtcRevision,snapshotSmtcSessionGeneration,
            snapshotSmtcEventRevision);
        if (!WaitFor(state, 250ms)) break;
    }
    sessionEvents.Reset();
    media = nullptr;
    manager = nullptr;
    if (apartmentReady) winrt::uninit_apartment();
}
} // namespace

struct NeteasePlaybackMonitor::Impl { std::shared_ptr<SharedState> state; };
NeteasePlaybackMonitor::NeteasePlaybackMonitor() : impl_(std::make_unique<Impl>()) {}
NeteasePlaybackMonitor::~NeteasePlaybackMonitor() { Stop(); }
void NeteasePlaybackMonitor::Start(Callback callback,bool clientClockEnabled) {
    Stop();
    auto state = std::make_shared<SharedState>();
    state->callback = std::move(callback);
    state->clientClockEnabled = clientClockEnabled;
    impl_->state = state;
    // External UIA providers and version-resource I/O may not be cancellable. Workers hold
    // shared state only, never this/Impl. Stop disables callbacks immediately
    // and does not wait indefinitely for an unresponsive foreign provider.
    std::thread([state] { ClientDiscoveryWorker(state); }).detach();
    std::thread([state] { UiWorker(state); }).detach();
    std::thread([state] { NativeClockWorker(state); }).detach();
    std::thread([state] { NativeDetailsWorker(state); }).detach();
    std::thread([state] { NativeMetadataCacheWorker(state); }).detach();
    std::thread([state] { NativeMetadataWorker(state); }).detach();
    std::thread([state] { MonitorWorker(state); }).detach();
    // Failure to create this optional worker must not prevent the native clock
    // and song-metadata workers above from starting.
    try{
        std::thread([state] { ClientVersionWorker(state); }).detach();
        {
            std::lock_guard lock(state->mutex);
            if(!state->stop.load()){
                state->clientVersionWorkerAvailable=true;++state->clientVersionRevision;
            }
        }
        state->discoveryCv.notify_all();
    }catch(...){/* version stays unavailable; independent playback continues */}
}
void NeteasePlaybackMonitor::SetClientClockEnabled(bool enabled) {
    const auto state=impl_->state;
    if(!state)return;
    std::lock_guard delivery(state->callbackMutex);PlaybackSnapshot fallback;bool publishFallback=false;
    {std::lock_guard lock(state->mutex);if(state->clientClockEnabled.load()==enabled)return;
        if(!enabled&&state->nativePreferred&&state->nativeSeed.pid==state->requestedPid){
            fallback=state->nativeSeed;publishFallback=true;
            if(state->nativeSeedIdentity)InvalidateNativeIdentity(fallback,MonotonicMs());
            else if(fallback.reliability==Reliability::NativeRead){fallback.reliability=Reliability::Unavailable;fallback.sampledAtMs=0;}
            AddDiagnostic(fallback.diagnostic,L"原生内部时钟已关闭，等待媒体会话或界面时间源");
        }
        state->clientClockEnabled=enabled;state->nativePreferred=false;++state->clientClockModeRevision;
        ++state->nativeEpoch;state->nativeObservation={};state->nativeObservationPid=0;
        CancelNativeDetailsLocked(*state);
        state->nativeLookupStop.request_stop(); state->nativeLookupStop = std::stop_source{};
        ++state->nativeIdentityRevision; state->nativeIdentityId.clear(); state->nativeRecord = {}; state->nativeRecordPid = 0;
        state->nativeLookupDiagnostic.clear();
        ClearNativeCacheDiagnosticLocked(*state);
        state->nativeIdentityAvailability=NativeIdentityAvailability::None;++state->nativeAvailabilityRevision;
        state->nativeAvailabilityDeliveredRevision=0;state->nativeLossStartedAtMs=0;
        state->nativeExpiredSmtcSessionGeneration=state->smtcSessionGeneration.load();
        state->nativeExpiredAfterSmtcEventRevision=state->smtcMediaEventRevision.load();
        state->nativeStableCaptionPid=0;state->nativeStableCaptionCreatedAt=0;
        state->nativeIdentityConfirmedAtMs=0;
        state->nativeStableCaptionTitle.clear();state->nativeStableCaptionArtist.clear();
        state->nativeSeedReady=false;state->nativeSeedIdentity=false;state->lastNativeDeliveredAtMs=0;state->lastNativeDeliveredId.clear();}
    if(publishFallback)Publish(state,fallback);
    state->wake=true;
    state->cv.notify_all();
    state->discoveryCv.notify_all();
    state->nativeCv.notify_all();
}
void NeteasePlaybackMonitor::Stop() {
    auto state = std::exchange(impl_->state, {});
    if (!state) return;
    state->stop = true;
    {
        std::lock_guard lock(state->mutex);
        CancelNativeDetailsLocked(*state);state->nativeLookupStop.request_stop();
        state->nativeCacheWrites.clear();ClearNativeCacheDiagnosticLocked(*state);
        state->clientVersionRequest.reset();
        state->clientVersionWorkerAvailable=false;
    }
    state->cv.notify_all();
    state->nativeCv.notify_all();
    state->detailsCv.notify_all();
    state->nativeCacheCv.notify_all();
    state->discoveryCv.notify_all();
    state->clientVersionCv.notify_all();
    std::lock_guard lock(state->callbackMutex);
    state->callback = {};
}
} // namespace stage
