#pragma once
#include "playback.h"
namespace stage {
// Internal adapter; PlaybackMonitor owns source arbitration.
class NeteasePlaybackMonitor {
public:
    using Callback=PlaybackMonitor::Callback;
    NeteasePlaybackMonitor(); ~NeteasePlaybackMonitor();
    void Start(Callback callback,bool clientClockEnabled=true);
    void Stop();
    void SetClientClockEnabled(bool enabled);
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};
}
