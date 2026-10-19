#pragma once

#include "model.h"
#include <memory>

namespace stage {

struct ClientClockObservation {
    bool candidate = false;
    bool stable = false;
    bool advanceAllowed=false;
    std::wstring songId;
    int64_t positionMs = 0;
    int64_t durationMs = 0;
    int64_t sampledAtMs = 0;
    PlayState state=PlayState::Unknown;
    int64_t stateSampledAtMs=0;
    std::wstring source;
    std::wstring diagnostic;
};

// Reads a small, version-fingerprinted set of values from the already-running
// NetEase process. The reader requests query and PROCESS_VM_READ rights only:
// it never writes, injects, hooks, resumes, suspends, or starts remote threads.
class ClientClockReader {
public:
    ClientClockReader();
    ~ClientClockReader();
    ClientClockReader(const ClientClockReader&) = delete;
    ClientClockReader& operator=(const ClientClockReader&) = delete;

    ClientClockObservation Observe(DWORD pid, PlayState state, uint64_t processCreatedAtTicks);
    void Reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace stage
