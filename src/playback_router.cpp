#include "playback.h"
#include "netease_playback.h"
#include "qq_playback.h"
#include "kugou_playback.h"
#include "soda_playback.h"
#include <mutex>
#include <thread>
#include <condition_variable>
#include <optional>
#include <utility>

namespace stage {
namespace {
struct Sources {
    std::recursive_mutex mutex;
    PlaybackMonitor::Callback callback;
    std::array<PlaybackSnapshot,4> latest;
    std::array<int64_t,4> received{};
    size_t selected=0;
    bool stopped=false;
    std::mutex waitMutex;std::condition_variable wake;
    static bool Available(const PlaybackSnapshot& p,int64_t now){
        return p.pid&&p.processCreatedAtTicks&&!p.metadataStale&&!p.identityReacquiring&&
            (!p.title.empty()||p.songIdVerified)&&p.observedAtMs>0&&now>=p.observedAtMs&&now-p.observedAtMs<=1500;
    }
    void Deliver(std::optional<size_t> changed){
        const auto now=MonotonicMs();const auto before=selected;
        const bool ready=Available(latest[selected],now);
        if(!ready||latest[selected].state!=PlayState::Playing){
            std::optional<size_t> playing,available,present;
            for(size_t i=0;i<latest.size();++i){
                if(Available(latest[i],now)){
                    if(!available)available=i;
                    if(latest[i].state==PlayState::Playing&&!playing)playing=i;
                }
                if(latest[i].pid&&!present)present=i;
            }
            if(playing)selected=*playing;
            else if(!ready&&available)selected=*available;
            else if(!ready&&!latest[selected].pid&&present)selected=*present;
        }
        if(stopped||!callback)return;
        // Other-source updates must not refresh the selected source's observation.
        if(changed&&*changed!=selected&&before==selected)return;
        auto result=latest[selected];
        if(received[selected]&&now-received[selected]>1500){
            result.metadataStale=true;result.reliability=Reliability::Unavailable;
            result.sampledAtMs=0;result.mediaCover.reset();result.diagnostic=L"播放器信息暂时中断，等待自动恢复";
        }
        try{callback(result);}catch(...){/* A consumer failure must not terminate a producer. */}
    }
    void Accept(size_t index,const PlaybackSnapshot& snapshot){
        std::lock_guard lock(mutex);if(stopped)return;
        latest[index]=snapshot;received[index]=MonotonicMs();Deliver(index);
    }
};
}
struct PlaybackMonitor::Impl {
    NeteasePlaybackMonitor netease;
    QqPlaybackMonitor qq;
    KugouPlaybackMonitor kugou;
    SodaPlaybackMonitor soda;
    std::shared_ptr<Sources> sources;
};
PlaybackMonitor::PlaybackMonitor():impl_(std::make_unique<Impl>()){}
PlaybackMonitor::~PlaybackMonitor(){Stop();}
void PlaybackMonitor::Start(Callback callback,bool clientClockEnabled){
    Stop();auto sources=std::make_shared<Sources>();sources->callback=std::move(callback);
    sources->latest[1].provider=MusicProvider::QqMusic;sources->latest[2].provider=MusicProvider::Kugou;sources->latest[3].provider=MusicProvider::SodaMusic;impl_->sources=sources;
    impl_->netease.Start([sources](const auto& snapshot){sources->Accept(0,snapshot);},clientClockEnabled);
    impl_->qq.Start([sources](const auto& snapshot){sources->Accept(1,snapshot);});
    impl_->kugou.Start([sources](const auto& snapshot){sources->Accept(2,snapshot);});
    impl_->soda.Start([sources](const auto& snapshot){sources->Accept(3,snapshot);});
    std::thread([sources]{
        for(;;){
            {std::lock_guard lock(sources->mutex);if(sources->stopped)break;sources->Deliver({});}
            std::unique_lock wait(sources->waitMutex);sources->wake.wait_for(wait,std::chrono::milliseconds(250));
        }
    }).detach();
}
void PlaybackMonitor::SetClientClockEnabled(bool enabled){impl_->netease.SetClientClockEnabled(enabled);}
void PlaybackMonitor::Stop(){
    if(auto sources=std::exchange(impl_->sources,{})){
        {std::lock_guard lock(sources->mutex);sources->stopped=true;sources->callback={};}
        sources->wake.notify_all();
    }
    impl_->soda.Stop();impl_->kugou.Stop();impl_->qq.Stop();impl_->netease.Stop();
}
}
