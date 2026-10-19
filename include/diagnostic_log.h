#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace stage {
enum class DiagnosticLogKind { Sample,Event };
enum class DiagnosticLogState { Opening,Active,Stopped,Failed,LimitReached };
struct DiagnosticLogStatus {
    DiagnosticLogState state=DiagnosticLogState::Failed;
    uint64_t queuedRows=0,queuedBytes=0,acceptedRows=0,writtenRows=0;
    uint64_t droppedSamples=0,droppedEvents=0,serializationFailures=0,oversizedRows=0;
};
class DiagnosticLogWriter {
public:
    explicit DiagnosticLogWriter(const std::filesystem::path& path,int64_t startedAtMs) noexcept;
    ~DiagnosticLogWriter();
    DiagnosticLogWriter(const DiagnosticLogWriter&)=delete;
    DiagnosticLogWriter& operator=(const DiagnosticLogWriter&)=delete;
    const std::string& SessionId() const noexcept;
    bool Append(std::string jsonLine,DiagnosticLogKind kind) noexcept;
    void SerializationFailed(DiagnosticLogKind kind) noexcept;
    DiagnosticLogStatus Status() const noexcept;
    // Only shutdown waits for accepted rows. Ordinary file I/O cannot be
    // guaranteed interruptible; successful enqueue never means saved to disk.
    void Stop() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
