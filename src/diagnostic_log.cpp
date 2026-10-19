#include "diagnostic_log.h"
#include "model.h"
#include "version.h"
#include "third_party/json.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace stage {
namespace {
using Json=nlohmann::json;
constexpr uint64_t MaxFileBytes=16ULL*1024*1024,ControlReserve=8*1024;
constexpr size_t MaxRows=128,SampleRows=96,MaxQueueBytes=512*1024,SampleQueueBytes=384*1024,MaxLineBytes=32*1024;
std::atomic<uint64_t> sessionSerial{0};
void Increment(uint64_t& value) noexcept {if(value!=std::numeric_limits<uint64_t>::max())++value;}
std::string LogUtc() {
    SYSTEMTIME t{};GetSystemTime(&t);char b[64]{};
    sprintf_s(b,"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds);return b;
}
struct Loss {
    uint64_t samples=0,events=0,serialization=0,oversized=0;
    bool operator==(const Loss&) const=default;
};
enum class WriteResult { Written,Limit,Failed };
}
struct DiagnosticLogWriter::Impl {
    struct Row {std::string line;DiagnosticLogKind kind;};
    std::filesystem::path path;
    const int64_t started;
    const std::string session;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::deque<Row> queue;
    std::optional<DiagnosticLogKind> activeKind;
    size_t queuedBytes=0;
    bool stopping=false,accepting=true;
    DiagnosticLogStatus status;
    std::thread thread;

    Impl(const std::filesystem::path& file,int64_t origin):path(file),started(origin),
        session(std::to_string(GetCurrentProcessId())+"-"+std::to_string(origin)+"-"+std::to_string(++sessionSerial)) {
        status.state=DiagnosticLogState::Opening;thread=std::thread([this]{Run();});
    }
    ~Impl(){Stop();}
    Loss LossLocked() const {return {status.droppedSamples,status.droppedEvents,status.serializationFailures,status.oversizedRows};}
    void DropLocked(DiagnosticLogKind kind) noexcept {
        if(kind==DiagnosticLogKind::Sample)Increment(status.droppedSamples);else Increment(status.droppedEvents);
    }
    void NoteFailure(DiagnosticLogKind kind,bool serialization) noexcept {
        try{{std::lock_guard lock(mutex);DropLocked(kind);if(serialization)Increment(status.serializationFailures);}cv.notify_all();}catch(...){}
    }
    DiagnosticLogStatus Snapshot() const {
        std::lock_guard lock(mutex);auto value=status;value.queuedRows=queue.size();value.queuedBytes=queuedBytes;return value;
    }
    bool Append(std::string line,DiagnosticLogKind kind) noexcept {
        try{
            {
                std::lock_guard lock(mutex);
                if(!accepting||stopping){DropLocked(kind);cv.notify_all();return false;}
                if(line.empty()||line.size()>MaxLineBytes){DropLocked(kind);Increment(status.oversizedRows);cv.notify_all();return false;}
                if(kind==DiagnosticLogKind::Sample){
                    if(queue.size()>=SampleRows||queuedBytes+line.size()>SampleQueueBytes){DropLocked(kind);cv.notify_all();return false;}
                }else{
                    while(queue.size()>=MaxRows||queuedBytes+line.size()>MaxQueueBytes){
                        const auto sample=std::find_if(queue.begin(),queue.end(),[](const Row& row){return row.kind==DiagnosticLogKind::Sample;});
                        if(sample==queue.end()){DropLocked(kind);cv.notify_all();return false;}
                        queuedBytes-=sample->line.size();DropLocked(sample->kind);queue.erase(sample);
                    }
                }
                const auto count=line.size();queue.push_back({std::move(line),kind});queuedBytes+=count;Increment(status.acceptedRows);
            }
            cv.notify_all();return true;
        }catch(...){NoteFailure(kind,false);return false;}
    }
    Json Marker(const char* event) const {
        return {{"event",event},{"logSchema",1},{"logSession",session},{"appVersion",MUSIC_STAGE_VERSION},
            {"utc",LogUtc()},{"elapsedMs",MonotonicMs()-started}};
    }
    Json LossMarker(const DiagnosticLogStatus& value) const {
        auto row=Marker("log-loss");
        row["droppedSamples"]=value.droppedSamples;row["droppedEvents"]=value.droppedEvents;
        row["serializationFailures"]=value.serializationFailures;row["oversizedRows"]=value.oversizedRows;
        row["detail"]="Cumulative omissions for this log session. Marker time does not locate missing observations.";return row;
    }
    Json EndMarker(const DiagnosticLogStatus& value,const char* reason) const {
        auto row=Marker("log-end");row["reason"]=reason;row["queueDrained"]=true;
        row["acceptedRows"]=value.acceptedRows;row["writtenRows"]=value.writtenRows;
        row["droppedSamples"]=value.droppedSamples;row["droppedEvents"]=value.droppedEvents;
        row["serializationFailures"]=value.serializationFailures;row["oversizedRows"]=value.oversizedRows;
        row["detail"]="Writer queue closed; this is not proof of durable storage, complete observations, or playback acceptance.";return row;
    }
    void Terminal(DiagnosticLogState state,const std::optional<Row>& current={}) {
        std::deque<Row> retired;
        {
            std::lock_guard lock(mutex);accepting=false;status.state=state;
            if(activeKind){DropLocked(*activeKind);activeKind.reset();}
            else if(current)DropLocked(current->kind);
            for(const auto& row:queue)DropLocked(row.kind);
            retired.swap(queue);queuedBytes=0;
        }
        cv.notify_all();
    }
    static WriteResult Write(std::ofstream& output,uint64_t& bytes,const std::string& line,bool finalControl=false) {
        const auto limit=finalControl?MaxFileBytes:MaxFileBytes-ControlReserve;
        if(bytes>limit||line.size()>limit-bytes)return WriteResult::Limit;
        output.write(line.data(),static_cast<std::streamsize>(line.size()));output.flush();
        if(!output)return WriteResult::Failed;
        bytes+=line.size();return WriteResult::Written;
    }
    static WriteResult WriteJson(std::ofstream& output,uint64_t& bytes,Json row,bool finalControl=false) {
        auto line=row.dump();line.push_back('\n');return Write(output,bytes,line,finalControl);
    }
    void FinishLimit(std::ofstream& output,uint64_t& bytes,const std::optional<Row>& current={}) {
        Terminal(DiagnosticLogState::LimitReached,current);const auto value=Snapshot();
        // Reserved control space is best effort. An already-full old file can
        // have no room even for these markers; it is never truncated to fit.
        auto loss=WriteJson(output,bytes,LossMarker(value),true);
        auto limit=Marker("log-limit");limit["detail"]="This log session reached its 16 MiB file budget; later records are omitted.";
        const auto capped=loss==WriteResult::Written?WriteJson(output,bytes,std::move(limit),true):loss;
        const auto ended=capped==WriteResult::Written?WriteJson(output,bytes,EndMarker(value,"limit"),true):capped;
        if(ended==WriteResult::Failed)Terminal(DiagnosticLogState::Failed);
    }
    void RunFile() {
        std::error_code error;
        if(!path.parent_path().empty()){std::filesystem::create_directories(path.parent_path(),error);if(error)throw std::runtime_error("Log directory unavailable");}
        const bool exists=std::filesystem::exists(path,error);if(error)throw std::runtime_error("Log file status unavailable");
        uint64_t bytes=0;
        if(exists){
            if(!std::filesystem::is_regular_file(path,error)||error)throw std::runtime_error("Log target is not a regular file");
            bytes=std::filesystem::file_size(path,error);if(error)throw std::runtime_error("Log file size unavailable");
        }
        std::ofstream output(path,std::ios::binary|std::ios::app);if(!output)throw std::runtime_error("Log file unavailable");
        // Keep an old partial tail intact and separate from the new session.
        if(bytes){const auto separator=Write(output,bytes,"\n",true);if(separator==WriteResult::Failed)throw std::runtime_error("Log separator failed");}
        if(bytes>=MaxFileBytes-ControlReserve){FinishLimit(output,bytes);return;}
        {std::lock_guard lock(mutex);status.state=DiagnosticLogState::Active;}
        auto start=Marker("log-start");start["detail"]="Asynchronous diagnostic recording; enqueue is not a disk-write receipt.";
        auto opened=WriteJson(output,bytes,std::move(start));
        if(opened==WriteResult::Failed)throw std::runtime_error("Log session start failed");
        if(opened==WriteResult::Limit){FinishLimit(output,bytes);return;}
        Loss reported;
        while(true){
            std::optional<Row> current;Loss loss;bool done=false;
            {
                std::unique_lock lock(mutex);cv.wait(lock,[&]{return stopping||!queue.empty()||!(LossLocked()==reported);});
                loss=LossLocked();
                if(!queue.empty()){current=std::move(queue.front());queuedBytes-=current->line.size();queue.pop_front();activeKind=current->kind;}
                else done=stopping;
            }
            if(!(loss==reported)){
                const auto observed=Snapshot();
                const auto recorded=WriteJson(output,bytes,LossMarker(observed));
                if(recorded==WriteResult::Failed){Terminal(DiagnosticLogState::Failed,current);return;}
                if(recorded==WriteResult::Limit){FinishLimit(output,bytes,current);return;}
                reported={observed.droppedSamples,observed.droppedEvents,observed.serializationFailures,observed.oversizedRows};
            }
            if(current){
                const auto recorded=Write(output,bytes,current->line);
                if(recorded==WriteResult::Failed){Terminal(DiagnosticLogState::Failed,current);return;}
                if(recorded==WriteResult::Limit){FinishLimit(output,bytes,current);return;}
                std::lock_guard lock(mutex);Increment(status.writtenRows);activeKind.reset();
            }
            if(done){
                const auto recorded=WriteJson(output,bytes,EndMarker(Snapshot(),"stop"),true);
                if(recorded==WriteResult::Failed){Terminal(DiagnosticLogState::Failed);return;}
                if(recorded==WriteResult::Limit){FinishLimit(output,bytes);return;}
                output.close();if(!output){Terminal(DiagnosticLogState::Failed);return;}
                {std::lock_guard lock(mutex);status.state=DiagnosticLogState::Stopped;accepting=false;}
                return;
            }
        }
    }
    void Run() noexcept {try{RunFile();}catch(...){try{Terminal(DiagnosticLogState::Failed);}catch(...){}}}
    void Stop() noexcept {
        try{{std::lock_guard lock(mutex);stopping=true;accepting=false;}cv.notify_all();if(thread.joinable())thread.join();}catch(...){}
    }
};
DiagnosticLogWriter::DiagnosticLogWriter(const std::filesystem::path& path,int64_t started) noexcept {
    try{impl_=std::make_unique<Impl>(path,started);}catch(...){}
}
DiagnosticLogWriter::~DiagnosticLogWriter()=default;
const std::string& DiagnosticLogWriter::SessionId() const noexcept {static const std::string empty;return impl_?impl_->session:empty;}
bool DiagnosticLogWriter::Append(std::string line,DiagnosticLogKind kind) noexcept {return impl_&&impl_->Append(std::move(line),kind);}
void DiagnosticLogWriter::SerializationFailed(DiagnosticLogKind kind) noexcept {if(impl_)impl_->NoteFailure(kind,true);}
DiagnosticLogStatus DiagnosticLogWriter::Status() const noexcept {try{if(impl_)return impl_->Snapshot();}catch(...){}return {};}
void DiagnosticLogWriter::Stop() noexcept {if(impl_)impl_->Stop();}
}
