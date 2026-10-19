#include "kugou_playback.h"
#include "kugou_clock.h"
#include "background.h"
#include <tlhelp32.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <cmath>
#include <utility>

namespace stage {
namespace {
using namespace winrt::Windows::Media::Control;
using namespace winrt::Windows::Storage::Streams;
using namespace std::chrono_literals;
using Microsoft::WRL::ComPtr;
using Session=GlobalSystemMediaTransportControlsSession;
using Manager=GlobalSystemMediaTransportControlsSessionManager;
using Media=GlobalSystemMediaTransportControlsSessionMediaProperties;
struct Handle {
    HANDLE value=nullptr;
    ~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
};
uint64_t Ticks(FILETIME value){return (uint64_t(value.dwHighDateTime)<<32)|value.dwLowDateTime;}
struct Process {DWORD pid=0;uint64_t created=0;};
Process FindKugouProcess() {
    Handle list{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0)};
    if(list.value==INVALID_HANDLE_VALUE)return {};
    std::vector<PROCESSENTRY32W> candidates;
    PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);
    if(Process32FirstW(list.value,&entry))do{
        if(_wcsicmp(entry.szExeFile,L"KuGou.exe")==0)candidates.push_back(entry);
    }while(Process32NextW(list.value,&entry));
    Process found;
    for(const auto& candidate:candidates){
        // Kugou helper processes are not independent player roots.
        if(std::any_of(candidates.begin(),candidates.end(),[&](const auto& parent){return parent.th32ProcessID==candidate.th32ParentProcessID;}))continue;
        Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,candidate.th32ProcessID)};
        FILETIME created{},exit{},kernel{},user{};wchar_t path[32768]{};DWORD length=32768;
        if(!process.value||WaitForSingleObject(process.value,0)!=WAIT_TIMEOUT||
            !GetProcessTimes(process.value,&created,&exit,&kernel,&user)||
            !QueryFullProcessImageNameW(process.value,0,path,&length))continue;
        const auto name=wcsrchr(path,L'\\');
        if(_wcsicmp(name?name+1:path,L"KuGou.exe")!=0)continue;
        if(found.pid)return {}; // Ambiguous roots must not own one another's audio.
        found={candidate.th32ProcessID,Ticks(created)};
    }
    return found;
}
template<class Operation> auto Await(Operation operation,std::chrono::milliseconds timeout=800ms) {
    if(operation.wait_for(timeout)!=winrt::Windows::Foundation::AsyncStatus::Completed){
        operation.Cancel();throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    }
    return operation.get();
}
PlayState StateOf(GlobalSystemMediaTransportControlsSessionPlaybackStatus state){
    switch(state){
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing:return PlayState::Playing;
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Paused:return PlayState::Paused;
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Stopped:return PlayState::Stopped;
    default:return PlayState::Unknown;
    }
}
std::shared_ptr<const BackgroundFrame> DecodeCover(const IRandomAccessStreamReference& reference,uint64_t sequence){
    if(!reference)return {};
    auto stream=Await(reference.OpenReadAsync(),250ms);
    const auto size=stream.Size();
    if(!size||size>8*1024*1024)return {};
    DataReader reader(stream.GetInputStreamAt(0));
    if(Await(reader.LoadAsync(static_cast<uint32_t>(size)),250ms)!=size)return {};
    std::vector<uint8_t> bytes(static_cast<size_t>(size));reader.ReadBytes(bytes);
    ComPtr<IWICImagingFactory> factory;
    winrt::check_hresult(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)));
    ComPtr<IWICStream> memory;winrt::check_hresult(factory->CreateStream(&memory));
    winrt::check_hresult(memory->InitializeFromMemory(bytes.data(),static_cast<DWORD>(bytes.size())));
    ComPtr<IWICBitmapDecoder> decoder;winrt::check_hresult(factory->CreateDecoderFromStream(memory.Get(),nullptr,WICDecodeMetadataCacheOnDemand,&decoder));
    ComPtr<IWICBitmapFrameDecode> frame;winrt::check_hresult(decoder->GetFrame(0,&frame));
    UINT width=0,height=0;winrt::check_hresult(frame->GetSize(&width,&height));
    if(!width||!height||width>16384||height>16384||uint64_t(width)*height>64000000)return {};
    const double scale=std::min(1.,1024./std::max(width,height));
    const UINT targetWidth=std::max(1u,static_cast<UINT>(width*scale));
    const UINT targetHeight=std::max(1u,static_cast<UINT>(height*scale));
    ComPtr<IWICBitmapScaler> scaler;winrt::check_hresult(factory->CreateBitmapScaler(&scaler));
    winrt::check_hresult(scaler->Initialize(frame.Get(),targetWidth,targetHeight,WICBitmapInterpolationModeFant));
    ComPtr<IWICFormatConverter> converter;winrt::check_hresult(factory->CreateFormatConverter(&converter));
    winrt::check_hresult(converter->Initialize(scaler.Get(),GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,nullptr,0.,WICBitmapPaletteTypeCustom));
    auto result=std::make_shared<BackgroundFrame>();result->width=targetWidth;result->height=targetHeight;
    result->stride=targetWidth*4;result->sequence=sequence;result->bgra.resize(size_t(result->stride)*targetHeight);
    winrt::check_hresult(converter->CopyPixels(nullptr,result->stride,static_cast<UINT>(result->bgra.size()),result->bgra.data()));
    return result;
}
}

struct KugouPlaybackMonitor::State {
    std::atomic_bool stop=false;
    std::atomic<uint64_t> mediaEvents=0;
    std::atomic<uint64_t> controlEvents=0;
    std::recursive_mutex callbackMutex;
    PlaybackMonitor::Callback callback;
    std::mutex waitMutex;std::condition_variable wake;
    void Emit(const PlaybackSnapshot& snapshot){
        std::lock_guard lock(callbackMutex);if(!stop&&callback){try{callback(snapshot);}catch(...){}}
    }
    bool Wait(){std::unique_lock lock(waitMutex);return !wake.wait_for(lock,250ms,[&]{return stop.load();});}
    static void Run(const std::shared_ptr<State>& state){
        bool apartment=false;
        Manager manager{nullptr};Session session{nullptr};Media media{nullptr};
        winrt::event_token mediaToken{},controlToken{};bool subscribed=false,controlSubscribed=false;
        auto detach=[&]{
            if(subscribed&&session){try{session.MediaPropertiesChanged(mediaToken);}catch(...){}}
            if(controlSubscribed&&session){try{session.PlaybackInfoChanged(controlToken);}catch(...){}}
            subscribed=false;controlSubscribed=false;session=nullptr;media=nullptr;
        };
        Process owner;PlaybackSnapshot previous;uint64_t acceptedEvents=0,acceptedControl=0,identity=0;
        int64_t lastMetadata=0,lastCoverAttempt=0,settleAt=0,lastMovementAt=0;
        bool clockBound=false;std::shared_ptr<const BackgroundFrame> cover;
        KugouClockReader nativeReader;std::optional<KugouClockSample> clockCandidate;
        try{winrt::init_apartment(winrt::apartment_type::multi_threaded);apartment=true;}catch(...){return;}
        do{
            PlaybackSnapshot snapshot;snapshot.provider=MusicProvider::Kugou;
            snapshot.source=L"酷狗音乐 · Windows 媒体会话";
            try{
                const auto process=FindKugouProcess();snapshot.pid=process.pid;snapshot.processCreatedAtTicks=process.created;
                if(!process.pid){detach();owner={};previous={};cover.reset();clockCandidate.reset();clockBound=false;lastMovementAt=0;snapshot.diagnostic=L"未找到唯一 酷狗音乐进程";state->Emit(snapshot);continue;}
                if(!manager)manager=Await(Manager::RequestAsync());
                Session selected{nullptr};size_t count=0;
                for(const auto& candidate:manager.GetSessions()){
                    if(_wcsicmp(candidate.SourceAppUserModelId().c_str(),L"kugou")==0){selected=candidate;++count;}
                }
                if(count!=1){detach();cover.reset();snapshot.diagnostic=count?L"酷狗媒体会话不唯一，等待重新确认":L"酷狗音乐未提供媒体会话，等待恢复";
                    snapshot.metadataStale=true;snapshot.observedAtMs=MonotonicMs();state->Emit(snapshot);continue;}
                if(selected!=session||process.pid!=owner.pid||process.created!=owner.created){
                    detach();session=selected;owner=process;previous={};cover.reset();lastMetadata=0;lastCoverAttempt=0;
                    clockCandidate.reset();clockBound=false;lastMovementAt=0;settleAt=MonotonicMs()+1000;++identity;
                    const std::weak_ptr<State> weak=state;
                    mediaToken=session.MediaPropertiesChanged([weak](const auto&,const auto&){if(auto shared=weak.lock())++shared->mediaEvents;});subscribed=true;
                    controlToken=session.PlaybackInfoChanged([weak](const auto&,const auto&){if(auto shared=weak.lock())++shared->controlEvents;});controlSubscribed=true;
                    acceptedEvents=state->mediaEvents.load();
                    acceptedControl=state->controlEvents.load();
                }
                const auto nativeBefore=nativeReader.Read(process.pid,process.created);
                const auto events=state->mediaEvents.load();
                if(!media||events!=acceptedEvents||MonotonicMs()-lastMetadata>=1000){
                    auto next=Await(session.TryGetMediaPropertiesAsync());
                    if(events!=state->mediaEvents.load())throw winrt::hresult_error(E_PENDING);
                    const bool changed=media&&(next.Title()!=media.Title()||next.Artist()!=media.Artist()||next.AlbumTitle()!=media.AlbumTitle()||events!=acceptedEvents);
                    if(changed){++identity;cover.reset();lastCoverAttempt=0;clockCandidate.reset();clockBound=false;lastMovementAt=0;settleAt=MonotonicMs()+1000;}
                    media=std::move(next);acceptedEvents=events;lastMetadata=MonotonicMs();
                }
                snapshot.title=media.Title();snapshot.artist=media.Artist();snapshot.album=media.AlbumTitle();
                snapshot.mediaIdentityRevision=identity;snapshot.mediaCover=cover;
                if(snapshot.title.empty()||snapshot.title.size()>512||snapshot.artist.size()>512||snapshot.album.size()>1024)
                    throw winrt::hresult_error(E_PENDING);
                const auto control=state->controlEvents.load();
                const auto info=session.GetPlaybackInfo();
                snapshot.state=StateOf(info.PlaybackStatus());
                const auto nativeAfter=nativeReader.Read(process.pid,process.created);
                const auto now=MonotonicMs(),identityNow=now;snapshot.observedAtMs=now;snapshot.stateSampledAtMs=now;
                if(previous.state!=PlayState::Unknown&&(previous.state!=snapshot.state||control!=acceptedControl)){
                    lastMovementAt=0;settleAt=std::max(settleAt,now+500);
                    if(snapshot.state!=PlayState::Paused)clockBound=false;
                }
                const auto rate=info.PlaybackRate();const bool normalRate=!rate||std::abs(rate.Value()-1.)<0.001;
                const bool coherent=nativeBefore&&nativeAfter&&nativeBefore->durationObject==nativeAfter->durationObject&&
                    nativeBefore->durationMs==nativeAfter->durationMs&&
                    nativeAfter->observedAtMs>=nativeBefore->observedAtMs&&nativeAfter->observedAtMs-nativeBefore->observedAtMs<=1500&&
                    std::abs(nativeAfter->positionMs-nativeBefore->positionMs)<=nativeAfter->observedAtMs-nativeBefore->observedAtMs+250;
                if(coherent){
                    if(clockCandidate&&(clockCandidate->durationObject!=nativeAfter->durationObject||clockCandidate->durationMs!=nativeAfter->durationMs)){
                        ++identity;cover.reset();snapshot.mediaCover.reset();lastCoverAttempt=0;
                        clockCandidate.reset();clockBound=false;lastMovementAt=0;settleAt=now+1000;
                    }
                    const auto gap=clockCandidate?nativeAfter->observedAtMs-clockCandidate->observedAtMs:0;
                    const auto delta=clockCandidate?nativeAfter->positionMs-clockCandidate->positionMs:0;
                    const bool adjacent=clockCandidate&&gap>=100&&gap<=1500;
                    if(adjacent&&(delta < -100||delta>gap+750)){
                        // A seek invalidates interpolation immediately, but it does
                        // not invent a different catalog identity.
                        lastMovementAt=0;settleAt=std::max(settleAt,now+500);
                    }else if(adjacent&&delta>10&&delta<=gap+250)lastMovementAt=now;
                    const bool moving=snapshot.state==PlayState::Playing&&lastMovementAt>0&&now-lastMovementAt<=750;
                    const bool stationary=snapshot.state==PlayState::Paused&&clockBound;
                    if(adjacent&&now>=settleAt&&normalRate&&(moving||stationary)){
                        clockBound=true;
                        snapshot.sampledAtMs=nativeAfter->observedAtMs;
                        snapshot.reliability=Reliability::Estimated;
                    }
                    if(clockBound){snapshot.positionMs=nativeAfter->positionMs;snapshot.durationMs=nativeAfter->durationMs;}
                    snapshot.identityReacquiring=!clockBound;
                    clockCandidate=nativeAfter;
                    snapshot.clientVersion=L"20.1.51.27967";
                    snapshot.source=L"酷狗音乐 · 媒体信息 / 原生播放进度";
                }else{clockCandidate.reset();clockBound=false;lastMovementAt=0;snapshot.identityReacquiring=true;}
                if(snapshot.sampledAtMs==0)snapshot.diagnostic=L"酷狗原生进度等待确认，歌词暂时静态（适配版本 20.1.51.27967）";
                snapshot.mediaIdentityRevision=identity;
                if(events!=state->mediaEvents.load())throw winrt::hresult_error(E_PENDING);
                // Recheck process identity after external calls before binding audio.
                const auto current=FindKugouProcess();
                if(current.pid!=owner.pid||current.created!=owner.created||events!=state->mediaEvents.load()||control!=state->controlEvents.load())throw winrt::hresult_error(E_PENDING);
                snapshot.catalogMetadataReady=true;snapshot.observedAtMs=identityNow;acceptedControl=control;previous=snapshot;state->Emit(snapshot);
                if(!cover&&now-lastCoverAttempt>=3000&&!state->stop){
                    lastCoverAttempt=now;
                    try{auto decoded=DecodeCover(media.Thumbnail(),identity);
                        if(events==state->mediaEvents.load())cover=std::move(decoded);
                    }catch(...){/* Missing artwork must not interrupt lyrics or progress. */}
                }
            }catch(...){
                snapshot.title.clear();snapshot.artist.clear();snapshot.album.clear();snapshot.mediaCover.reset();
                snapshot.songId.clear();snapshot.songIdVerified=false;clockCandidate.reset();clockBound=false;lastMovementAt=0;
                snapshot.identityReacquiring=false;snapshot.catalogMetadataReady=false;
                snapshot.state=PlayState::Unknown;snapshot.sampledAtMs=0;snapshot.reliability=Reliability::Unavailable;
                snapshot.metadataStale=true;snapshot.observedAtMs=MonotonicMs();
                snapshot.diagnostic=L"酷狗信息暂时不可用，正在重新连接";state->Emit(snapshot);
                // Recreate the manager after transport failure; do not retain old
                // metadata or a stalled event subscription across reconnection.
                detach();manager=nullptr;cover.reset();
            }
        }while(!state->stop&&state->Wait());
        detach();manager=nullptr;if(apartment)winrt::uninit_apartment();
    }
};
KugouPlaybackMonitor::KugouPlaybackMonitor()=default;
KugouPlaybackMonitor::~KugouPlaybackMonitor(){Stop();}
void KugouPlaybackMonitor::Start(PlaybackMonitor::Callback callback){
    Stop();auto state=std::make_shared<State>();state->callback=std::move(callback);state_=state;
    std::thread([state]{State::Run(state);}).detach();
}
void KugouPlaybackMonitor::Stop(){
    auto state=std::exchange(state_,{});if(!state)return;
    state->stop=true;state->wake.notify_all();std::lock_guard lock(state->callbackMutex);state->callback={};
}
}
