#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "audio.h"
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <ks.h>
#include <ksmedia.h>
#include <wrl.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstring>
#include <mutex>
#include <numbers>
#include <thread>

#pragma comment(lib, "Mmdevapi.lib")
#pragma comment(lib, "Ole32.lib")

namespace stage {
namespace {
using Microsoft::WRL::ComPtr;

class Handle {
public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle() { if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
private:
    HANDLE value_;
};

std::wstring HResultText(const wchar_t* action, HRESULT value) {
    wchar_t number[32]{};
    swprintf_s(number, L"0x%08X", static_cast<unsigned>(value));
    return std::wstring(action) + L" · " + number;
}

struct AudioDelivery {
    std::recursive_mutex mutex;
    uint64_t generation=0;
    std::atomic<unsigned> workers=0;
};
struct CaptureSession {
    DWORD pid;
    const uint64_t processCreatedAtTicks;
    const uint64_t generation;
    const std::shared_ptr<AudioDelivery> delivery;
    ProcessAudioCapture::Callback callback;
    Handle stop{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    CaptureSession(DWORD value,uint64_t createdAt,uint64_t epoch,std::shared_ptr<AudioDelivery> target,ProcessAudioCapture::Callback fn)
        :pid(value),processCreatedAtTicks(createdAt),generation(epoch),delivery(std::move(target)),callback(std::move(fn)){}
    void Emit(AudioFeatures features) const noexcept {
        features.pid = pid;
        features.processCreatedAtTicks=processCreatedAtTicks;
        features.observedAtMs=MonotonicMs();
        std::lock_guard lock(delivery->mutex);
        if(generation!=delivery->generation)return;
        if(callback){try{const auto current=callback;current(features);}catch(...){}}
    }
    void Error(const std::wstring& message) const {
        AudioFeatures result;
        result.diagnostic = message;
        Emit(std::move(result));
    }
    bool IsStopped() const { return !stop.get()||WaitForSingleObject(stop.get(),0)==WAIT_OBJECT_0; }
};

bool TargetAlive(const CaptureSession& session,HANDLE target) {
    if(session.IsStopped())return false;
    const auto wait=WaitForSingleObject(target,0);
    if(wait==WAIT_TIMEOUT)return true;
    session.Error(wait==WAIT_OBJECT_0?L"目标音乐进程已退出":L"不能确认目标音乐进程仍在运行，音频响应已停止");
    return false;
}

bool TargetInstanceMatches(const CaptureSession& session,HANDLE target) {
    FILETIME created{},exited{},kernel{},user{};
    if(!session.processCreatedAtTicks||!GetProcessTimes(target,&created,&exited,&kernel,&user)){
        session.Error(L"不能核验目标音乐进程实例，等待重新识别");return false;
    }
    const uint64_t actual=(static_cast<uint64_t>(created.dwHighDateTime)<<32)|created.dwLowDateTime;
    if(actual!=session.processCreatedAtTicks){
        session.Error(L"音乐进程实例已经变化，等待歌曲识别重新确认后连接音频");return false;
    }
    return TargetAlive(session,target);
}

// GetBuffer supplies the first frame's QPC value in 100-nanosecond units.
// Map packet end to the local monotonic clock without mixing its epoch with QPC.
int64_t PacketSampleAt(UINT64 qpc,UINT32 frames,unsigned rate,const LARGE_INTEGER& frequency) {
    if(!qpc||!frames||!rate||frequency.QuadPart<=0)return 0;
    LARGE_INTEGER counter{};if(!QueryPerformanceCounter(&counter))return 0;
    const auto observed=MonotonicMs();
    const long double now=static_cast<long double>(counter.QuadPart)*10000000.L/frequency.QuadPart;
    const long double end=static_cast<long double>(qpc)+static_cast<long double>(frames)*10000000.L/rate;
    const auto age=now-end;
    if(!std::isfinite(age)||age< -500000.L||age>static_cast<long double>(AudioResponseFreshnessMs)*10000.L)return 0;
    const auto ageMs=static_cast<int64_t>(std::max(0.L,age)/10000.L);
    return observed>ageMs?observed-ageMs:0;
}

// The handler owns its completion event and COM result. It never points back to
// the capture object, so stopping during asynchronous activation cannot leave a
// late Windows callback with a dangling pointer.
class Activation final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
    Microsoft::WRL::FtmBase, IActivateAudioInterfaceCompletionHandler> {
public:
    Handle completed{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    HRESULT result = E_PENDING;
    ComPtr<IAudioClient> client;
    STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation* operation) override {
        HRESULT activationResult = E_FAIL;
        ComPtr<IUnknown> audioInterface;
        result = operation->GetActivateResult(&activationResult, &audioInterface);
        if (SUCCEEDED(result)) result = activationResult;
        if (SUCCEEDED(result)) result = audioInterface.As(&client);
        SetEvent(completed.get());
        return S_OK;
    }
};

struct Format {
    unsigned channels = 2, bytes = 4, blockAlign = 8, validBits = 32;
    unsigned sampleRate = 48000;
    bool floating = true;
};

bool DescribeFormat(const WAVEFORMATEX& input, Format& format) {
    format.channels = input.nChannels;
    format.bytes = input.wBitsPerSample / 8;
    format.validBits = input.wBitsPerSample;
    format.blockAlign = input.nBlockAlign;
    format.sampleRate = input.nSamplesPerSec;
    if (!format.channels || format.channels > 32 || !format.sampleRate ||
        format.blockAlign != format.channels * format.bytes) return false;
    if (input.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) format.floating = true;
    else if (input.wFormatTag == WAVE_FORMAT_PCM) format.floating = false;
    else if (input.wFormatTag == WAVE_FORMAT_EXTENSIBLE && input.cbSize >= 22) {
        const auto& extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(input);
        format.validBits = extended.Samples.wValidBitsPerSample;
        if (IsEqualGUID(extended.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) format.floating = true;
        else if (IsEqualGUID(extended.SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) format.floating = false;
        else return false;
    } else return false;
    return format.floating ? format.bytes == 4 :
        (format.bytes >= 1 && format.bytes <= 4 && format.validBits && format.validBits <= format.bytes * 8);
}

float ReadSample(const BYTE* data, const Format& format) {
    if (format.floating) {
        float value;
        std::memcpy(&value, data, sizeof(value));
        return std::isfinite(value) ? std::clamp(value, -1.0f, 1.0f) : 0.0f;
    }
    if (format.bytes == 1) return (static_cast<float>(*data) - 128.0f) / 128.0f;
    if (format.bytes == 2) {
        int16_t value;
        std::memcpy(&value, data, sizeof(value));
        return value / 32768.0f;
    }
    if (format.bytes == 3) {
        int32_t value = static_cast<int32_t>(data[0]) | (static_cast<int32_t>(data[1]) << 8) |
            (static_cast<int32_t>(data[2]) << 16);
        if (value & 0x800000) value |= static_cast<int32_t>(0xFF000000);
        return value / 8388608.0f;
    }
    int32_t value;
    std::memcpy(&value, data, sizeof(value));
    return static_cast<float>(value / 2147483648.0);
}

// Original small radix-2 FFT; no audio samples are written to disk or network.
class Analyzer {
    static constexpr size_t Size = 2048;
    std::array<float, Size> samples_{};
    size_t cursor_ = 0;
    std::array<std::complex<float>, Size> fft_{};
public:
    void Reset() { samples_.fill(0); cursor_ = 0; }
    void Feed(float value) { samples_[cursor_] = value; cursor_ = (cursor_ + 1) % Size; }
    void Spectrum(unsigned sampleRate, std::array<float, 32>& output) {
        for (size_t i = 0; i < Size; ++i) {
            const float window = 0.5f - 0.5f * std::cos(2.0f * std::numbers::pi_v<float> *
                static_cast<float>(i) / static_cast<float>(Size - 1));
            fft_[i] = {samples_[(cursor_ + i) % Size] * window, 0};
        }
        for (size_t i = 1, j = 0; i < Size; ++i) {
            size_t bit = Size >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(fft_[i], fft_[j]);
        }
        for (size_t length = 2; length <= Size; length *= 2) {
            const float angle = -2.0f * std::numbers::pi_v<float> / static_cast<float>(length);
            const std::complex<float> step(std::cos(angle), std::sin(angle));
            for (size_t offset = 0; offset < Size; offset += length) {
                std::complex<float> twiddle(1, 0);
                for (size_t j = 0; j < length / 2; ++j) {
                    const auto even = fft_[offset + j];
                    const auto odd = fft_[offset + j + length / 2] * twiddle;
                    fft_[offset + j] = even + odd;
                    fft_[offset + j + length / 2] = even - odd;
                    twiddle *= step;
                }
            }
        }
        const float maxFrequency = std::min(16000.0f, sampleRate * 0.5f);
        for (size_t band = 0; band < output.size(); ++band) {
            const float low = 35.0f * std::pow(maxFrequency / 35.0f, static_cast<float>(band) / 32.0f);
            const float high = 35.0f * std::pow(maxFrequency / 35.0f, static_cast<float>(band + 1) / 32.0f);
            const size_t begin = std::clamp<size_t>(static_cast<size_t>(low * Size / sampleRate), 1, Size / 2 - 1);
            const size_t end = std::clamp<size_t>(static_cast<size_t>(std::ceil(high * Size / sampleRate)), begin + 1, Size / 2);
            float magnitude = 0;
            for (size_t bin = begin; bin < end; ++bin) magnitude = std::max(magnitude, std::abs(fft_[bin]) * 4.0f / Size);
            output[band] = std::clamp(std::log1p(magnitude * 24.0f) / std::log(25.0f), 0.0f, 1.0f);
        }
    }
};

void Capture(const std::shared_ptr<CaptureSession>& session) {
    if(session->IsStopped())return;
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(comResult)) { session->Error(HResultText(L"音频 COM 初始化失败", comResult)); return; }
    struct ComScope { ~ComScope() { CoUninitialize(); } } comScope;
    Handle target(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, session->pid));
    if (!target.get()) { session->Error(L"不能访问目标进程；进程可能已退出或权限不足"); return; }
    if(!TargetInstanceMatches(*session,target.get()))return;
    auto activation = Microsoft::WRL::Make<Activation>();
    if (!activation || !activation->completed.get()) { session->Error(L"创建音频激活事件失败"); return; }
    AUDIOCLIENT_ACTIVATION_PARAMS parameters{};
    parameters.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    parameters.ProcessLoopbackParams.TargetProcessId = session->pid;
    parameters.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT arguments{};
    arguments.vt = VT_BLOB;
    arguments.blob.cbSize = sizeof(parameters);
    arguments.blob.pBlobData = reinterpret_cast<BYTE*>(&parameters);
    ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    HRESULT result = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
        __uuidof(IAudioClient), &arguments, activation.Get(), &operation);
    if (FAILED(result)) { session->Error(HResultText(L"进程音频激活失败（需要 Windows 11）", result)); return; }
    HANDLE activationEvents[]{session->stop.get(), activation->completed.get(), target.get()};
    DWORD wait = WaitForMultipleObjects(3, activationEvents, FALSE, 10000);
    if (wait == WAIT_OBJECT_0) return;
    if (wait == WAIT_OBJECT_0 + 2) { session->Error(L"目标音乐进程已退出"); return; }
    if (wait != WAIT_OBJECT_0 + 1) { session->Error(L"进程音频激活超时或等待失败"); return; }
    // Completion may win the wait when process exit is signalled as well.
    if(!TargetAlive(*session,target.get()))return;
    if (FAILED(activation->result)) { session->Error(HResultText(L"进程音频激活失败", activation->result)); return; }
    if(session->IsStopped())return;
    ComPtr<IAudioClient> client = activation->client;
    Handle sampleReady(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!sampleReady.get()) { session->Error(L"创建音频采样事件失败"); return; }
    WAVEFORMATEX captureFormat{};
    captureFormat.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    captureFormat.nChannels = 2;
    captureFormat.nSamplesPerSec = 48000;
    captureFormat.wBitsPerSample = 32;
    captureFormat.nBlockAlign = 8;
    captureFormat.nAvgBytesPerSec = 384000;
    constexpr DWORD streamFlags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    result = client->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags, 0, 0, &captureFormat, nullptr);
    // Some providers only accept PCM. Keep 48 kHz, avoiding endpoint-dependent
    // GetMixFormat behavior on the virtual process-loopback device.
    if (result == AUDCLNT_E_UNSUPPORTED_FORMAT) {
        captureFormat.wFormatTag = WAVE_FORMAT_PCM;
        captureFormat.wBitsPerSample = 16;
        captureFormat.nBlockAlign = 4;
        captureFormat.nAvgBytesPerSec = 192000;
        result = client->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags, 0, 0, &captureFormat, nullptr);
    }
    if (FAILED(result)) { session->Error(HResultText(L"音频格式初始化失败", result)); return; }
    Format format;
    if (!DescribeFormat(captureFormat, format)) { session->Error(L"不支持该音频采样格式"); return; }
    ComPtr<IAudioCaptureClient> capture;
    result = client->GetService(IID_PPV_ARGS(&capture));
    if (SUCCEEDED(result)) result = client->SetEventHandle(sampleReady.get());
    if(!TargetAlive(*session,target.get()))return;
    if (SUCCEEDED(result)) result = client->Start();
    if (FAILED(result)) { session->Error(HResultText(L"启动音频采集失败", result)); return; }
    struct StopScope { IAudioClient* client; ~StopScope() { client->Stop(); } } stopScope{client.Get()};
    std::array<Analyzer, 2> channelAnalyzers;
    enum class AnalysisTiming { Empty,Timestamped,Arrival };
    AnalysisTiming historyTiming=AnalysisTiming::Empty;
    const auto resetAnalyzers = [&channelAnalyzers,&historyTiming] {
        for (auto& analyzer : channelAnalyzers) analyzer.Reset();
        historyTiming=AnalysisTiming::Empty;
    };
    LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);
    AudioFeatures feature;
    feature.active = true;
    const std::wstring formatDescription=L"进程树隔离采样 · 48 kHz · "+std::wstring(format.floating?L"float32":L"PCM16");
    feature.diagnostic=formatDescription+L" · 等待音频数据";
    if(!TargetAlive(*session,target.get()))return;
    session->Emit(feature);
    HANDLE captureEvents[]{session->stop.get(), sampleReady.get(), target.get()};
    bool morePackets=false,discardingBacklog=false;
    while (!session->IsStopped()) {
        wait=morePackets?WAIT_OBJECT_0+1:WaitForMultipleObjects(3,captureEvents,FALSE,250);
        if (wait == WAIT_OBJECT_0) break;
        if (wait == WAIT_OBJECT_0 + 2) { session->Error(L"目标音乐进程已退出"); return; }
        if(!TargetAlive(*session,target.get()))return;
        UINT32 pending = 0;
        if (wait == WAIT_TIMEOUT) {
            // A disconnected process-loopback stream may stop signalling its
            // sample event. Probe the capture client before publishing another
            // silent heartbeat so device/service invalidation reaches the
            // reconnect policy instead of looking healthy forever.
            result=capture->GetNextPacketSize(&pending);
            if(FAILED(result)){
                session->Error(HResultText(L"音频采样失效，请重新连接音乐进程",result));
                return;
            }
            if(pending)wait=WAIT_OBJECT_0+1;
            else{
                resetAnalyzers();
                feature.rms = feature.peak = 0;
                feature.spectrum.fill(0);
                feature.sampledAtMs=0;feature.timingTrusted=false;feature.diagnostic=formatDescription+L" · 当前无音频数据";
                session->Emit(feature);
                continue;
            }
        }
        if (wait != WAIT_OBJECT_0 + 1) { session->Error(L"音频事件等待失败"); return; }
        double sumSquares = 0;
        uint64_t sampleCount = 0;
        float peak = 0;
        int64_t latestSampleAt=0;
        bool timingTrusted=false,rejected=false;
        const bool discardBatch=discardingBacklog;
        const auto batchStarted=MonotonicMs();unsigned packets=0;
        if(!pending)result = capture->GetNextPacketSize(&pending);
        while(SUCCEEDED(result)&&pending&&!session->IsStopped()&&packets<64&&MonotonicMs()-batchStarted<20){
            BYTE* bytes = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 packetQpc=0;
            result=capture->GetBuffer(&bytes,&frames,&flags,nullptr,&packetQpc);
            if (FAILED(result)) break;
            if(!frames){pending=0;break;}
            packets++;
            if(discardBatch){
                result=capture->ReleaseBuffer(frames);
                if(SUCCEEDED(result))result=capture->GetNextPacketSize(&pending);
                continue;
            }
            const auto sampleAt=(flags&AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)?0:PacketSampleAt(packetQpc,frames,format.sampleRate,frequency);
            const bool reject=(flags&AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)||frames>format.sampleRate/2||(packetQpc&&!sampleAt);
            if(reject){
                resetAnalyzers();sumSquares=0;sampleCount=0;peak=0;latestSampleAt=0;timingTrusted=false;rejected=true;
                result=capture->ReleaseBuffer(frames);
                if(SUCCEEDED(result))result=capture->GetNextPacketSize(&pending);
                continue;
            }
            const auto packetTiming=sampleAt>0?AnalysisTiming::Timestamped:AnalysisTiming::Arrival;
            if((flags&AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)||packetTiming==AnalysisTiming::Arrival||
                    (historyTiming!=AnalysisTiming::Empty&&historyTiming!=packetTiming)){
                // An arrival-only packet owns a zero-padded FFT window as well
                // as its RMS/peak. Mode changes also clear cross-batch history;
                // a new QPC stamp must not relabel earlier arrival-only audio.
                resetAnalyzers();
                sumSquares=0;sampleCount=0;peak=0;
            }
            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) || !bytes;
            for (UINT32 frame = 0; frame < frames; ++frame) {
                for (unsigned channel = 0; channel < format.channels; ++channel) {
                    const float value = silent ? 0 : ReadSample(bytes + static_cast<size_t>(frame) * format.blockAlign +
                        channel * format.bytes, format);
                    sumSquares += static_cast<double>(value) * value;
                    peak = std::max(peak, std::abs(value));
                    if (channel < channelAnalyzers.size()) channelAnalyzers[channel].Feed(value);
                }
            }
            sampleCount += static_cast<uint64_t>(frames) * format.channels;
            historyTiming=packetTiming;
            latestSampleAt=sampleAt;timingTrusted=sampleAt>0;
            result = capture->ReleaseBuffer(frames);
            if (SUCCEEDED(result)) result = capture->GetNextPacketSize(&pending);
        }
        if (FAILED(result)) { session->Error(HResultText(L"音频采样失效，请重新连接音乐进程", result)); return; }
        if(!TargetAlive(*session,target.get()))return;
        morePackets=pending>0;
        if(discardBatch){
            if(!morePackets){discardingBacklog=false;resetAnalyzers();}
            else if(WaitForSingleObject(session->stop.get(),1)==WAIT_OBJECT_0)break;
            continue;
        }
        if(morePackets){
            // Anything processed in the budget-limited batch may already be
            // stale. Clear both FFT histories and drain the remaining queue
            // without feeding analyzers before accepting a new event.
            discardingBacklog=true;resetAnalyzers();
            feature.rms=feature.peak=0;feature.spectrum.fill(0);feature.sampledAtMs=0;feature.timingTrusted=false;
            feature.diagnostic=formatDescription+L" · 正在丢弃积压数据，等待当前采样";
            session->Emit(feature);
        }else if(sampleCount){
            feature.rms = static_cast<float>(std::sqrt(sumSquares / sampleCount));
            feature.peak = peak;
            feature.sampledAtMs=latestSampleAt;feature.timingTrusted=timingTrusted;
            feature.diagnostic=formatDescription+(timingTrusted?L" · 采样时间有效":L" · 按数据到达时间响应，采样时标未提供");
            std::array<float, 32> leftSpectrum{}, rightSpectrum{};
            channelAnalyzers[0].Spectrum(format.sampleRate, leftSpectrum);
            channelAnalyzers[1].Spectrum(format.sampleRate, rightSpectrum);
            for(size_t band=0;band<feature.spectrum.size();++band){
                const float left=leftSpectrum[band],right=rightSpectrum[band];
                feature.spectrum[band]=std::sqrt((left*left+right*right)*0.5f);
            }
            session->Emit(feature);
        }else if(rejected){
            feature.rms=feature.peak=0;feature.spectrum.fill(0);feature.sampledAtMs=0;feature.timingTrusted=false;
            feature.diagnostic=formatDescription+L" · 采样时间无效，暂不驱动动效";
            session->Emit(feature);
        }
    }
}
} // namespace

struct ProcessAudioCapture::Impl {
    std::shared_ptr<AudioDelivery> delivery=std::make_shared<AudioDelivery>();
    std::shared_ptr<CaptureSession> session;
    void Stop() {
        std::lock_guard lock(delivery->mutex);++delivery->generation;
        if(session&&session->stop.get())SetEvent(session->stop.get());
        session.reset();
    }
};

ProcessAudioCapture::ProcessAudioCapture() : impl_(std::make_unique<Impl>()) {}
ProcessAudioCapture::~ProcessAudioCapture() { Stop(); }
void ProcessAudioCapture::Stop() { impl_->Stop(); }
void ProcessAudioCapture::Start(DWORD pid, Callback callback, uint64_t processCreatedAtTicks) {
    std::lock_guard lock(impl_->delivery->mutex);impl_->Stop();
    auto session=std::make_shared<CaptureSession>(pid,processCreatedAtTicks,impl_->delivery->generation,impl_->delivery,std::move(callback));
    impl_->session=session;
    if(!pid){session->Error(L"尚未选择有效的音乐进程");return;}
    if(!processCreatedAtTicks){session->Error(L"音乐进程实例尚未核验，等待歌曲识别后连接音频");return;}
    if(!session->stop.get()){session->Error(L"创建音频停止事件失败");return;}
    if(impl_->delivery->workers.load()>=2){session->Error(L"Windows 音频组件仍在收尾，等待旧采集线程返回后再连接");return;}
    AudioFeatures starting;starting.connecting=true;starting.diagnostic=L"正在连接播放器进程音频，等待 Windows 激活完成";session->Emit(std::move(starting));
    if(session->IsStopped())return;
    impl_->delivery->workers.fetch_add(1);
    try {
        std::thread([session] {
            struct WorkerScope { std::shared_ptr<AudioDelivery> state;~WorkerScope(){state->workers.fetch_sub(1);} } workerScope{session->delivery};
            try { Capture(session); }
            catch (...) { session->Error(L"音频分析发生异常，采集已停止"); }
        }).detach();
    } catch (...) {impl_->delivery->workers.fetch_sub(1);session->Error(L"不能创建音频采集线程");}
}
} // namespace stage
