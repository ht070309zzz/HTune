#pragma once
#include "model.h"
#include <functional>
#include <memory>
namespace stage {
class ProcessAudioCapture {
public:
    using Callback = std::function<void(const AudioFeatures&)>;
    ProcessAudioCapture(); ~ProcessAudioCapture();
    ProcessAudioCapture(const ProcessAudioCapture&) = delete;
    ProcessAudioCapture& operator=(const ProcessAudioCapture&) = delete;
    // Stop closes the callback generation immediately and signals cancellation;
    // COM cleanup remains on the session worker, without joining on the UI.
    // The caller supplies the already-verified instance. Missing/mismatched
    // creation identity is rejected; no PID-only fallback opens a new target.
    void Start(DWORD pid, Callback callback, uint64_t processCreatedAtTicks); void Stop();
private: struct Impl; std::unique_ptr<Impl> impl_;
};
}
