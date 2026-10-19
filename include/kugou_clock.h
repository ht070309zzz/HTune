#pragma once
#include "model.h"
#include <optional>
namespace stage {
struct KugouClockSample {
    int64_t positionMs=0,durationMs=0,observedAtMs=0;
    uintptr_t durationObject=0; // Read consistency token, never a song ID.
};
class KugouClockReader {
public:
    KugouClockReader(); ~KugouClockReader();
    std::optional<KugouClockSample> Read(DWORD pid,uint64_t created);
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
