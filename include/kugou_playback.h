#pragma once
#include "playback.h"
namespace stage {
class KugouPlaybackMonitor {
public:
    KugouPlaybackMonitor(); ~KugouPlaybackMonitor();
    void Start(PlaybackMonitor::Callback callback);
    void Stop();
private:
    struct State; std::shared_ptr<State> state_;
};
}
