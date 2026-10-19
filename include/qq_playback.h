#pragma once
#include "playback.h"
namespace stage {
class QqPlaybackMonitor {
public:
    QqPlaybackMonitor(); ~QqPlaybackMonitor();
    void Start(PlaybackMonitor::Callback callback);
    void Stop();
private:
    struct State; std::shared_ptr<State> state_;
};
}
