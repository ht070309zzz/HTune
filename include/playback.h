#pragma once
#include "model.h"
#include <functional>
#include <memory>
namespace stage {
class PlaybackMonitor {
public:
    using Callback = std::function<void(const PlaybackSnapshot&)>;
    PlaybackMonitor(); ~PlaybackMonitor();
    PlaybackMonitor(const PlaybackMonitor&) = delete;
    PlaybackMonitor& operator=(const PlaybackMonitor&) = delete;
    void Start(Callback callback,bool clientClockEnabled=true); void Stop();
    void SetClientClockEnabled(bool enabled);
private: struct Impl; std::unique_ptr<Impl> impl_;
};
}
