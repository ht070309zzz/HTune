#include "soda_playback.h"
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
Process FindSodaProcess() {
    Handle list{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0)};
    if(list.value==INVALID_HANDLE_VALUE)return {};
    std::vector<PROCESSENTRY32W> candidates;
    PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);
    if(Process32FirstW(list.value,&entry))do{
        if(_wcsicmp(entry.szExeFile,L"SodaMusic.exe")==0)candidates.push_back(entry);
    }while(Process32NextW(list.value,&entry));
    Process found;
    for(const auto& candidate:candidates){
        // Soda helper processes are not independent player roots.
        if(std::any_of(candidates.begin(),candidates.end(),[&](const auto& parent){return parent.th32ProcessID==candidate.th32ParentProcessID;}))continue;
        Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,candidate.th32ProcessID)};
        FILETIME created{},exit{},kernel{},user{};wchar_t path[32768]{};DWORD length=32768;
        if(!process.value||WaitForSingleObject(process.value,0)!=WAIT_TIMEOUT||
            !GetProcessTimes(process.value,&created,&exit,&kernel,&user)||
            !QueryFullProcessImageNameW(process.value,0,path,&length))continue;
        const auto name=wcsrchr(path,L'\\');
        if(_wcsicmp(name?name+1:path,L"SodaMusic.exe")!=0)continue;
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

struct SodaPlaybackMonitor::State {
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
        Process owner;PlaybackSnapshot previous;uint64_t acceptedEvents=0,identity=0;
        int64_t lastMetadata=0,lastCoverAttempt=0,lastTicks=0,blockedTicks=0,anchorTicks=0,anchorAt=0;
        bool reconnectCheck=false;
        bool waitingTimeline=false;std::shared_ptr<const BackgroundFrame> cover;
        try{winrt::init_apartment(winrt::apartment_type::multi_threaded);apartment=true;}catch(...){return;}
        do{
            PlaybackSnapshot snapshot;snapshot.provider=MusicProvider::SodaMusic;
            snapshot.source=L"汽水音乐 · Windows 媒体会话";
            try{
                const auto process=FindSodaProcess();snapshot.pid=process.pid;snapshot.processCreatedAtTicks=process.created;
                if(!process.pid){detach();owner={};previous={};cover.reset();anchorTicks=0;anchorAt=0;snapshot.diagnostic=L"未找到唯一汽水音乐进程";state->Emit(snapshot);continue;}
                if(!manager)manager=Await(Manager::RequestAsync());
                Session selected{nullptr};size_t count=0;
                for(const auto& candidate:manager.GetSessions()){
                    if(candidate.SourceAppUserModelId()==L"汽水音乐"||_wcsicmp(candidate.SourceAppUserModelId().c_str(),L"SodaMusic.exe")==0){selected=candidate;++count;}
                }
                if(count!=1){detach();cover.reset();snapshot.diagnostic=count?L"汽水媒体会话不唯一，等待重新确认":L"汽水音乐未提供媒体会话，等待恢复";
                    snapshot.metadataStale=true;snapshot.observedAtMs=MonotonicMs();state->Emit(snapshot);continue;}
                if(selected!=session||process.pid!=owner.pid||process.created!=owner.created){
                    const bool sameOwner=process.pid==owner.pid&&process.created==owner.created;
                    const auto priorTicks=sameOwner?lastTicks:0;
                    reconnectCheck=sameOwner&&previous.observedAtMs>0&&MonotonicMs()-previous.observedAtMs<=1500;
                    detach();session=selected;owner=process;if(!sameOwner){previous={};anchorTicks=0;anchorAt=0;}cover.reset();lastMetadata=0;lastCoverAttempt=0;
                    lastTicks=priorTicks;blockedTicks=priorTicks;waitingTimeline=priorTicks>0;++identity;
                    const std::weak_ptr<State> weak=state;
                    mediaToken=session.MediaPropertiesChanged([weak](const auto&,const auto&){if(auto shared=weak.lock())++shared->mediaEvents;});subscribed=true;
                    controlToken=session.PlaybackInfoChanged([weak](const auto&,const auto&){if(auto shared=weak.lock())++shared->controlEvents;});controlSubscribed=true;
                    acceptedEvents=state->mediaEvents.load();
                }
                const auto events=state->mediaEvents.load();
                if(!media||events!=acceptedEvents||MonotonicMs()-lastMetadata>=1000){
                    auto next=Await(session.TryGetMediaPropertiesAsync());
                    if(events!=state->mediaEvents.load())throw winrt::hresult_error(E_PENDING);
                    const bool changed=media&&(next.Title()!=media.Title()||next.Artist()!=media.Artist()||next.AlbumTitle()!=media.AlbumTitle()||events!=acceptedEvents);
                    if(changed){++identity;cover.reset();lastCoverAttempt=0;waitingTimeline=true;blockedTicks=lastTicks;}
                    media=std::move(next);acceptedEvents=events;lastMetadata=MonotonicMs();
                }
                snapshot.title=media.Title();snapshot.artist=media.Artist();snapshot.album=media.AlbumTitle();
                snapshot.mediaIdentityRevision=identity;snapshot.mediaCover=cover;
                if(snapshot.title.empty()||snapshot.title.size()>512||snapshot.artist.size()>512||snapshot.album.size()>1024)
                    throw winrt::hresult_error(E_PENDING);
                const auto control=state->controlEvents.load();
                const auto info=session.GetPlaybackInfo();const auto timeline=session.GetTimelineProperties();
                snapshot.state=StateOf(info.PlaybackStatus());
                snapshot.positionMs=(timeline.Position()-timeline.StartTime()).count()/10000;
                snapshot.durationMs=(timeline.EndTime()-timeline.StartTime()).count()/10000;
                const auto ticks=timeline.LastUpdatedTime().time_since_epoch().count();
                FILETIME wall{};GetSystemTimeAsFileTime(&wall);
                const auto wallTicks=static_cast<int64_t>(Ticks(wall));
                const auto now=MonotonicMs();snapshot.observedAtMs=now;snapshot.stateSampledAtMs=now;
                if(previous.state!=PlayState::Unknown&&previous.state!=snapshot.state){waitingTimeline=true;blockedTicks=lastTicks;}
                if(reconnectCheck){
                    if(ticks==lastTicks&&previous.title==snapshot.title&&previous.artist==snapshot.artist&&
                        previous.album==snapshot.album&&previous.durationMs==snapshot.durationMs&&previous.state==snapshot.state)
                        waitingTimeline=false;
                    reconnectCheck=false;
                }
                if(waitingTimeline&&ticks!=blockedTicks)waitingTimeline=false;
                const bool wallValid=ticks>0&&static_cast<uint64_t>(ticks)>=owner.created&&ticks<=wallTicks+2000000&&wallTicks-ticks<=864000000000LL;
                if(ticks!=anchorTicks&&wallValid){anchorTicks=ticks;anchorAt=now-std::max<int64_t>(0,(wallTicks-ticks)/10000);}
                const bool validTime=ticks==anchorTicks&&anchorAt>0&&anchorAt<=now;
                const int64_t age=validTime?now-anchorAt:INT64_MAX;
                const auto rate=info.PlaybackRate();const bool normalRate=!rate||std::abs(rate.Value()-1.)<0.001;
                // Soda 3.8 publishes anchors at transport changes, not every second.
                // Re-read the live session and metadata, and bound interpolation
                // by this recording. Never renew the original anchor timestamp.
                if(!waitingTimeline&&validTime&&snapshot.positionMs>=0&&snapshot.durationMs>0&&
                    snapshot.durationMs<=86400000&&snapshot.positionMs<=snapshot.durationMs&&
                    snapshot.state!=PlayState::Unknown&&normalRate&&
                    (snapshot.state!=PlayState::Playing||(age<=snapshot.durationMs-snapshot.positionMs+500))){
                    snapshot.sampledAtMs=snapshot.state==PlayState::Playing?now-age:now;
                    snapshot.reliability=snapshot.state==PlayState::Playing?Reliability::Estimated:Reliability::Precise;
                }else snapshot.diagnostic=L"汽水时间轴等待新锚点，歌词暂时静态";
                if(events!=state->mediaEvents.load())throw winrt::hresult_error(E_PENDING);
                // Recheck process identity after external calls before binding audio.
                const auto current=FindSodaProcess();
                if(current.pid!=owner.pid||current.created!=owner.created||events!=state->mediaEvents.load()||control!=state->controlEvents.load())throw winrt::hresult_error(E_PENDING);
                snapshot.catalogMetadataReady=true;snapshot.observedAtMs=MonotonicMs();lastTicks=ticks;previous=snapshot;state->Emit(snapshot);
                if(!cover&&now-lastCoverAttempt>=3000&&!state->stop){
                    lastCoverAttempt=now;
                    try{auto decoded=DecodeCover(media.Thumbnail(),identity);
                        if(events==state->mediaEvents.load())cover=std::move(decoded);
                    }catch(...){/* Missing artwork must not interrupt lyrics or progress. */}
                }
            }catch(...){
                snapshot.title.clear();snapshot.artist.clear();snapshot.album.clear();snapshot.mediaCover.reset();
                snapshot.songId.clear();snapshot.songIdVerified=false;
                snapshot.identityReacquiring=false;snapshot.catalogMetadataReady=false;
                snapshot.state=PlayState::Unknown;snapshot.sampledAtMs=0;snapshot.reliability=Reliability::Unavailable;
                snapshot.metadataStale=true;snapshot.observedAtMs=MonotonicMs();
                snapshot.diagnostic=L"汽水信息暂时不可用，正在重新连接";state->Emit(snapshot);
                // Recreate the manager after transport failure; do not retain old
                // metadata or a stalled event subscription across reconnection.
                detach();manager=nullptr;cover.reset();
            }
        }while(!state->stop&&state->Wait());
        detach();manager=nullptr;if(apartment)winrt::uninit_apartment();
    }
};
SodaPlaybackMonitor::SodaPlaybackMonitor()=default;
SodaPlaybackMonitor::~SodaPlaybackMonitor(){Stop();}
void SodaPlaybackMonitor::Start(PlaybackMonitor::Callback callback){
    Stop();auto state=std::make_shared<State>();state->callback=std::move(callback);state_=state;
    std::thread([state]{State::Run(state);}).detach();
}
void SodaPlaybackMonitor::Stop(){
    auto state=std::exchange(state_,{});if(!state)return;
    state->stop=true;state->wake.notify_all();std::lock_guard lock(state->callbackMutex);state->callback={};
}
}
