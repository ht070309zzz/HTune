#include "background.h"
#include "file_transaction.h"
#include "local_file_limits.h"
#include <winternl.h>
#include <cfapi.h>
#include <wincodec.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propvarutil.h>
#include <wrl.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <new>
#include <thread>
#include <utility>

#pragma comment(lib, "Windowscodecs.lib")
#pragma comment(lib, "Mfreadwrite.lib")
#pragma comment(lib, "Mfplat.lib")
#pragma comment(lib, "Mfuuid.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "CldApi.lib")

// Microsoft interface references (implementation is original):
// https://learn.microsoft.com/en-us/windows/win32/medfound/using-the-source-reader-in-asynchronous-mode
// https://learn.microsoft.com/en-us/windows/win32/medfound/mf-mt-default-stride-attribute
// https://learn.microsoft.com/en-us/windows/win32/medfound/mf-source-reader-enable-video-processing
// https://learn.microsoft.com/en-us/windows/win32/medfound/mf-mt-minimum-display-aperture-attribute
// https://learn.microsoft.com/en-us/windows/win32/api/wincodec/nf-wincodec-iwicbitmapdecoder-getframe

namespace stage {
namespace {
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
constexpr uint32_t kMaxEdge = 3840;
constexpr uint64_t kMaxPixels = uint64_t{3840} * 2160;
constexpr uint64_t kMaxFrameBytes = kMaxPixels * 4;
constexpr uint64_t kMaxImageSourcePixels = uint64_t{8192} * 8192;
constexpr DWORD kFirstVideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
constexpr DWORD kAllStreams = static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now().time_since_epoch()).count();
}
std::wstring ErrorText(const wchar_t* message, HRESULT hr) {
    wchar_t hex[24]{};
    swprintf_s(hex, L"0x%08X", static_cast<unsigned>(hr));
    return std::wstring(message) + L" · " + hex;
}
std::wstring FileName(const std::wstring& path) {
    const auto split = path.find_last_of(L"\\/");
    return split == std::wstring::npos ? path : path.substr(split + 1);
}
bool FrameSizeValid(uint32_t width, uint32_t height) {
    return width != 0 && height != 0 && width <= kMaxEdge &&
        height <= kMaxEdge && uint64_t{width} * height <= kMaxPixels;
}
uint16_t PhotoOrientation(IWICBitmapFrameDecode* image) {
    ComPtr<IWICMetadataQueryReader> reader;
    if(!image||FAILED(image->GetMetadataQueryReader(&reader)))return 1;
    // Read only orientation; never enumerate camera, GPS or personal metadata.
    // https://learn.microsoft.com/en-us/windows/win32/wic/-wic-native-image-format-metadata-queries
    for(const auto* query:{L"System.Photo.Orientation",L"/app1/ifd/{ushort=274}",L"/ifd/{ushort=274}"}){
        PROPVARIANT value{};const auto status=reader->GetMetadataByName(query,&value);
        uint32_t orientation=0;
        if(SUCCEEDED(status)){if(value.vt==VT_UI2)orientation=value.uiVal;else if(value.vt==VT_UI4)orientation=value.ulVal;}
        PropVariantClear(&value);
        if(orientation>=1&&orientation<=8)return (uint16_t)orientation;
    }
    return 1;
}
HRESULT VideoOrientation(IMFMediaType* type,uint16_t fallback,uint16_t& result) {
    UINT32 rotation=0;const auto status=type->GetUINT32(MF_MT_VIDEO_ROTATION,&rotation);
    if(status==MF_E_ATTRIBUTENOTFOUND){result=fallback;return S_OK;}
    if(FAILED(status))return status;
    // MF describes the content's counter-clockwise rotation. Normalize using
    // the corresponding clockwise display correction (D2D positive angles).
    // https://learn.microsoft.com/en-us/windows/win32/medfound/mf-mt-video-rotation
    switch(rotation){case 0:result=1;break;case 90:result=6;break;case 180:result=3;break;case 270:result=8;break;default:return MF_E_INVALIDMEDIATYPE;}
    return S_OK;
}
struct LockedLocalFile {
    std::wstring path;
    detail::DirectoryTreeLock directories;
    HANDLE handle=INVALID_HANDLE_VALUE;
    ~LockedLocalFile(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}
    LockedLocalFile()=default;
    LockedLocalFile(const LockedLocalFile&)=delete;
    LockedLocalFile& operator=(const LockedLocalFile&)=delete;
};
struct ProbeHandle {
    HANDLE value=INVALID_HANDLE_VALUE;
    ~ProbeHandle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    ProbeHandle()=default;
    ProbeHandle(const ProbeHandle&)=delete;
    ProbeHandle& operator=(const ProbeHandle&)=delete;
};
HRESULT ValidateLocalHandle(HANDLE handle, bool expectDirectory, bool checkCloudPlaceholder) noexcept {
    BY_HANDLE_FILE_INFORMATION information{};
    if(!GetFileInformationByHandle(handle,&information))return HRESULT_FROM_WIN32(GetLastError());
    const bool directory=(information.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
    if(directory!=expectDirectory)return HRESULT_FROM_WIN32(expectDirectory?ERROR_DIRECTORY:ERROR_FILE_INVALID);
    constexpr DWORD remoteDataAttributes=FILE_ATTRIBUTE_OFFLINE|FILE_ATTRIBUTE_RECALL_ON_OPEN|
        FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS;
    if(information.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)
        return HRESULT_FROM_WIN32(ERROR_REPARSE_TAG_INVALID);
    if(information.dwFileAttributes&remoteDataAttributes)
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    if(GetFileType(handle)!=FILE_TYPE_DISK)return HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE);
    if(checkCloudPlaceholder){
        CF_PLACEHOLDER_BASIC_INFO placeholder{};DWORD placeholderBytes=0;
        const HRESULT placeholderStatus=CfGetPlaceholderInfo(handle,CF_PLACEHOLDER_INFO_BASIC,
            &placeholder,sizeof(placeholder),&placeholderBytes);
        if(SUCCEEDED(placeholderStatus)||placeholderStatus==HRESULT_FROM_WIN32(ERROR_MORE_DATA))
            return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        if(placeholderStatus!=HRESULT_FROM_WIN32(ERROR_NOT_A_CLOUD_FILE))return placeholderStatus;
    }
    return S_OK;
}
bool ValidateBackgroundDirectory(HANDLE handle, const void* context, DWORD& error) noexcept {
    const bool checkCloudPlaceholder=context&&*static_cast<const bool*>(context);
    const HRESULT status=ValidateLocalHandle(handle,true,checkCloudPlaceholder);
    if(SUCCEEDED(status)){error=ERROR_SUCCESS;return true;}
    error=HRESULT_FACILITY(status)==FACILITY_WIN32
        ? static_cast<DWORD>(HRESULT_CODE(status)) : ERROR_NOT_SUPPORTED;
    return false;
}
HRESULT OpenLocalFile(const std::wstring& input, bool readFromGuard, LockedLocalFile& output) {
    if (input.empty() || input.size() > MaxLocalFilePathChars || input.find(L"://") != std::wstring::npos)
        return E_INVALIDARG;
    const DWORD length = GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
    if (!length || length > MaxLocalFilePathChars + 1) return HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
    std::wstring path(length, L'\0');
    const DWORD written = GetFullPathNameW(input.c_str(), length, path.data(), nullptr);
    if (!written || written >= length) return HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
    path.resize(written);
    // The module deliberately accepts local drive files only. A UNC path,
    // device name or URL is never passed to the media-source resolver.
    if (path.size() < 3 || path[1] != L':' || path[2] != L'\\')
        return HRESULT_FROM_WIN32(ERROR_BAD_NETPATH);
    // A second colon selects an NTFS alternate data stream. Background media
    // is always a normal user-selected file, never a hidden stream or device.
    if(path.find(L':',2)!=std::wstring::npos)
        return HRESULT_FROM_WIN32(ERROR_INVALID_NAME);
    const std::wstring drive = path.substr(0, 3);
    const UINT driveType = GetDriveTypeW(drive.c_str());
    if (driveType != DRIVE_FIXED && driveType != DRIVE_REMOVABLE && driveType != DRIVE_RAMDISK)
        return HRESULT_FROM_WIN32(ERROR_BAD_NETPATH);
    wchar_t fileSystemName[MAX_PATH+1]{};
    if(!GetVolumeInformationW(drive.c_str(),nullptr,0,nullptr,nullptr,nullptr,
            fileSystemName,MAX_PATH+1))
        return HRESULT_FROM_WIN32(GetLastError());
    // The Windows Cloud Files minifilter supports NTFS volumes only. Avoid
    // treating the API's unsupported-filesystem result as a rejection of an
    // ordinary local FAT/exFAT removable-drive file.
    const bool cloudPlaceholdersSupported=lstrcmpiW(fileSystemName,L"NTFS")==0;
    const std::filesystem::path filePath(path);
    if(filePath.filename().empty())return HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
    DWORD error=ERROR_SUCCESS;
    if(!output.directories.Acquire(filePath.parent_path(),error,false,
            ValidateBackgroundDirectory,&cloudPlaceholdersSupported))
        return HRESULT_FROM_WIN32(error);
    // Inspect through an attribute-only, no-recall handle first. Cloud Files
    // deliberately hides its reparse tag from normal applications, and opening
    // an image for data before this check could hydrate it from the network.
    ProbeHandle probe;
    probe.value=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_OPEN_NO_RECALL,nullptr);
    if(probe.value==INVALID_HANDLE_VALUE)return HRESULT_FROM_WIN32(GetLastError());
    const HRESULT probeStatus=ValidateLocalHandle(probe.value,false,cloudPlaceholdersSupported);
    if(FAILED(probeStatus))return probeStatus;

    // Images are decoded through this exact data handle. Media Foundation
    // still resolves videos by path, so its long-lived guard requests no data
    // access: the restrictive share mask prevents writers/deletion while not
    // requiring the source reader to share our own read access.
    const DWORD desiredAccess=readFromGuard?GENERIC_READ:0;
    output.handle=CreateFileW(path.c_str(),desiredAccess,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_OPEN_NO_RECALL|
        (readFromGuard?FILE_FLAG_SEQUENTIAL_SCAN:0),nullptr);
    if(output.handle==INVALID_HANDLE_VALUE)return HRESULT_FROM_WIN32(GetLastError());
    if(detail::CompareFileObjects(probe.value,output.handle)!=detail::FileObjectComparison::Same)
        return HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
    output.path=std::move(path);
    return S_OK;
}

struct ReadPacket {
    HRESULT status = S_OK;
    DWORD flags = 0;
    LONGLONG pts = 0;
    ComPtr<IMFSample> sample;
};
struct ReadState {
    std::mutex mutex;
    std::condition_variable changed;
    bool ready = false, cancelled = false;
    HRESULT terminalStatus = S_OK;
    ReadPacket packet;
};
void CancelPendingRead(const std::shared_ptr<ReadState>& state) {
    if(!state)return;
    {std::lock_guard lock(state->mutex);state->cancelled=true;}
    // No decoder calls or sample releases on the command/UI thread.
    state->changed.notify_all();
}

// This callback owns shared state only. It never refers to a controller or UI;
// late MF callbacks after switching files cannot touch destroyed app objects.
class ReaderCallback final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
    Microsoft::WRL::FtmBase, IMFSourceReaderCallback> {
public:
    explicit ReaderCallback(std::shared_ptr<ReadState> state) : state_(std::move(state)) {}
    STDMETHOD(OnReadSample)(HRESULT status, DWORD, DWORD flags,
        LONGLONG pts, IMFSample* sample) override {
        ComPtr<IMFSample> incoming=sample;
        {
            std::lock_guard lock(state_->mutex);
            if (state_->cancelled || FAILED(state_->terminalStatus)) return S_OK;
            if(FAILED(status)||(flags&MF_SOURCE_READERF_ERROR))state_->terminalStatus=FAILED(status)?status:E_FAIL;
            state_->packet.status = status;
            state_->packet.flags = flags;
            state_->packet.pts = pts;
            state_->packet.sample.Swap(incoming);
            state_->ready = true;
        }
        state_->changed.notify_all();
        return S_OK;
    }
    STDMETHOD(OnFlush)(DWORD) override {
        state_->changed.notify_all();
        return S_OK;
    }
    STDMETHOD(OnEvent)(DWORD, IMFMediaEvent* event) override {
        HRESULT status = S_OK;
        if (event && SUCCEEDED(event->GetStatus(&status)) && FAILED(status)) {
            ReadPacket retired;
            {
                std::lock_guard lock(state_->mutex);
                if (state_->cancelled) return S_OK;
                if(FAILED(state_->terminalStatus))return S_OK;
                state_->terminalStatus = status;
                retired=std::move(state_->packet);state_->packet={};
                state_->packet.status = status;
                state_->ready = true;
            }
            state_->changed.notify_all();
        }
        return S_OK;
    }
private:
    std::shared_ptr<ReadState> state_;
};
struct ReaderSession {
    std::shared_ptr<ReadState> state = std::make_shared<ReadState>();
    ComPtr<ReaderCallback> callback;
    ComPtr<IMFSourceReader> reader;
    ~ReaderSession() {
        ReadPacket retired;
        {
            std::lock_guard lock(state->mutex);
            state->cancelled = true;
            retired=std::move(state->packet);state->packet={};
        }
        state->changed.notify_all();
        retired.sample.Reset();
        // An ERROR callback forbids further source-reader calls, including
        // Flush. Release directly for every exit, so a racing late error
        // cannot turn otherwise ordinary cleanup into an invalid method call.
        // The callback owns only shared, cancelled state and may outlive us.
        reader.Reset();
        callback.Reset();
    }
};
struct VideoFormat {
    uint32_t width = 0, height = 0;
    uint32_t codedWidth = 0, codedHeight = 0, cropX = 0, cropY = 0;
    uint32_t nativeWidth = 0, nativeHeight = 0;
    uint32_t pixelAspectNumerator=1,pixelAspectDenominator=1;
    LONG stride = 0;
    LONGLONG frameDuration = 333333;
    uint16_t nativeOrientation=1,orientation=1;
};
HRESULT ReadNativeVideoFormat(IMFSourceReader* reader, VideoFormat& result) {
    ComPtr<IMFMediaType> type;
    HRESULT hr = reader->GetNativeMediaType(kFirstVideoStream,
        static_cast<DWORD>(MF_SOURCE_READER_CURRENT_TYPE_INDEX), &type);
    if (FAILED(hr)) return hr;
    GUID major{};
    if (FAILED(hr = type->GetGUID(MF_MT_MAJOR_TYPE, &major))) return hr;
    if (major != MFMediaType_Video) return MF_E_INVALIDMEDIATYPE;
    UINT32 width = 0, height = 0;
    if (FAILED(hr = MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height)))
        return hr;
    // Bound the native input before negotiation as well as its decoded output.
    if (!FrameSizeValid(width, height)) return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    if (FAILED(hr = VideoOrientation(type.Get(), 1, result.nativeOrientation))) return hr;
    result.nativeWidth = width;
    result.nativeHeight = height;
    return S_OK;
}
// Fill only an unpublished candidate; callers commit native and output
// information together after every field has passed validation.
HRESULT ReadVideoOutputFormat(IMFSourceReader* reader, VideoFormat& result) {
    ComPtr<IMFMediaType> type;
    HRESULT hr = reader->GetCurrentMediaType(kFirstVideoStream, &type);
    if (FAILED(hr)) return hr;
    GUID major{}, subtype{};
    if (FAILED(hr = type->GetGUID(MF_MT_MAJOR_TYPE, &major)) ||
        FAILED(hr = type->GetGUID(MF_MT_SUBTYPE, &subtype))) return hr;
    if (major != MFMediaType_Video || subtype != MFVideoFormat_RGB32)
        return MF_E_INVALIDMEDIATYPE;
    if(FAILED(hr=VideoOrientation(type.Get(),result.nativeOrientation,result.orientation)))return hr;
    UINT32 pixelAspectNumerator=1,pixelAspectDenominator=1;
    const auto aspectStatus=MFGetAttributeRatio(type.Get(),MF_MT_PIXEL_ASPECT_RATIO,
        &pixelAspectNumerator,&pixelAspectDenominator);
    if(FAILED(aspectStatus)){
        if(aspectStatus!=MF_E_ATTRIBUTENOTFOUND)return aspectStatus;
        pixelAspectNumerator=pixelAspectDenominator=1;
    }
    if(!pixelAspectNumerator||!pixelAspectDenominator)return MF_E_INVALIDMEDIATYPE;
    result.pixelAspectNumerator=pixelAspectNumerator;
    result.pixelAspectDenominator=pixelAspectDenominator;
    UINT32 width = 0, height = 0;
    if (FAILED(hr = MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height)))
        return hr;
    // H264/MF can pad the coded surface (e.g. 180 to 192 lines). The
    // published frame must use the display aperture, never those padding rows.
    if (!width || !height || width > kMaxEdge + 32 || height > kMaxEdge + 32 ||
        uint64_t{width} * height > kMaxPixels + uint64_t{kMaxEdge} * 64 + 1024)
        return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    uint32_t displayWidth = width, displayHeight = height, cropX = 0, cropY = 0;
    MFVideoArea aperture{};
    UINT32 apertureBytes = 0;
    HRESULT apertureResult = type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE,
        reinterpret_cast<UINT8*>(&aperture), static_cast<UINT32>(sizeof(aperture)), &apertureBytes);
    if (FAILED(apertureResult)) {
        apertureResult = type->GetBlob(MF_MT_GEOMETRIC_APERTURE,
            reinterpret_cast<UINT8*>(&aperture), static_cast<UINT32>(sizeof(aperture)), &apertureBytes);
    }
    if (SUCCEEDED(apertureResult)) {
        if (apertureBytes != sizeof(aperture) || aperture.OffsetX.value < 0 ||
            aperture.OffsetY.value < 0 || aperture.OffsetX.fract || aperture.OffsetY.fract ||
            aperture.Area.cx <= 0 || aperture.Area.cy <= 0)
            return MF_E_INVALIDMEDIATYPE;
        cropX = static_cast<uint32_t>(aperture.OffsetX.value);
        cropY = static_cast<uint32_t>(aperture.OffsetY.value);
        displayWidth = static_cast<uint32_t>(aperture.Area.cx);
        displayHeight = static_cast<uint32_t>(aperture.Area.cy);
        if (uint64_t{cropX} + displayWidth > width ||
            uint64_t{cropY} + displayHeight > height) return MF_E_INVALIDMEDIATYPE;
    } else if (result.nativeWidth && result.nativeHeight &&
        width >= result.nativeWidth && height >= result.nativeHeight &&
        width - result.nativeWidth <= 32 && height - result.nativeHeight <= 32) {
        // Some converters remove the aperture attribute but retain native-size
        // padding. Native display size is a conservative fallback only when
        // the difference fits decoder alignment, not arbitrary resize/crops.
        displayWidth = result.nativeWidth;
        displayHeight = result.nativeHeight;
    }
    if (!FrameSizeValid(displayWidth, displayHeight))
        return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    UINT32 strideBits = 0;
    LONG stride = 0;
    if (SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &strideBits)))
        stride = static_cast<LONG>(strideBits);
    else if (FAILED(hr = MFGetStrideForBitmapInfoHeader(subtype.Data1, width, &stride)))
        return hr;
    const int64_t absoluteStride = stride < 0 ? -int64_t{stride} : int64_t{stride};
    if (absoluteStride < int64_t{width} * 4 ||
        absoluteStride * height > int64_t{kMaxFrameBytes} + 4096 * int64_t{height})
        return MF_E_INVALIDMEDIATYPE;
    result.width = displayWidth;
    result.height = displayHeight;
    result.codedWidth = width;
    result.codedHeight = height;
    result.cropX = cropX;
    result.cropY = cropY;
    result.stride = stride;
    UINT32 numerator = 0, denominator = 0;
    if (SUCCEEDED(MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &numerator, &denominator)) &&
        numerator && denominator) {
        const LONGLONG duration = 10000000LL * denominator / numerator;
        result.frameDuration = std::clamp(duration, 10000LL, 10000000LL);
    }
    return S_OK;
}
HRESULT RefreshVideoFormat(IMFSourceReader* reader, VideoFormat& result) {
    VideoFormat candidate;
    HRESULT hr = ReadNativeVideoFormat(reader, candidate);
    if (FAILED(hr)) return hr;
    if (FAILED(hr = ReadVideoOutputFormat(reader, candidate))) return hr;
    result = candidate;
    return S_OK;
}
HRESULT ConfigureVideoFormat(IMFSourceReader* reader, VideoFormat& result) {
    VideoFormat nativeInput;
    HRESULT hr = ReadNativeVideoFormat(reader, nativeInput);
    if (FAILED(hr)) return hr;
    ComPtr<IMFMediaType> requested;
    if (FAILED(hr = MFCreateMediaType(&requested)) ||
        FAILED(hr = requested->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
        FAILED(hr = requested->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32)) ||
        FAILED(hr = reader->SetCurrentMediaType(kFirstVideoStream,
            nullptr, requested.Get()))) return hr;
    // Negotiation can select a different native type. Re-read it instead of
    // reusing pre-negotiation hints or values retained from the previous frame.
    return RefreshVideoFormat(reader, result);
}

// Validate every row against the actual locked buffer. Negative stride means
// scanline0 points at the last memory row; output is always positive top-down.
HRESULT CopyRows(BackgroundFrame& output, const BYTE* scanline0, LONG pitch,
    const BYTE* bufferStart, DWORD bufferLength, uint32_t cropX, uint32_t cropY) {
    if (!scanline0 || !bufferStart) return E_POINTER;
    const uint64_t rowBytes = uint64_t{output.width} * 4;
    const int64_t absolutePitch = pitch < 0 ? -int64_t{pitch} : int64_t{pitch};
    if (absolutePitch < static_cast<int64_t>(rowBytes)) return MF_E_BUFFERTOOSMALL;
    const auto begin = reinterpret_cast<uintptr_t>(bufferStart);
    if (bufferLength > std::numeric_limits<uintptr_t>::max() - begin)
        return MF_E_BUFFERTOOSMALL;
    const auto end = begin + bufferLength;
    const auto first = reinterpret_cast<uintptr_t>(scanline0);
    for (uint32_t y = 0; y < output.height; ++y) {
        const int64_t offset = int64_t{pitch} * (int64_t{y} + cropY) +
            int64_t{cropX} * 4;
        uintptr_t address = first;
        if (offset < 0) {
            if (static_cast<uint64_t>(-offset) > first) return MF_E_BUFFERTOOSMALL;
            address -= static_cast<uintptr_t>(-offset);
        } else {
            if (static_cast<uint64_t>(offset) > std::numeric_limits<uintptr_t>::max() - first)
                return MF_E_BUFFERTOOSMALL;
            address += static_cast<uintptr_t>(offset);
        }
        if (address < begin || address > end || rowBytes > end - address)
            return MF_E_BUFFERTOOSMALL;
        BYTE* destination = output.bgra.data() + size_t{y} * output.stride;
        std::memcpy(destination, reinterpret_cast<const void*>(address), static_cast<size_t>(rowBytes));
        // RGB32 has an unused alpha byte. Make it explicitly opaque BGRA.
        for (uint32_t x = 0; x < output.width; ++x) destination[size_t{x} * 4 + 3] = 255;
    }
    return S_OK;
}
HRESULT DecodeVideoFrame(IMFSample* sample, const VideoFormat& format,
    std::shared_ptr<BackgroundFrame>& result) {
    if (!sample || !FrameSizeValid(format.width, format.height)) return E_INVALIDARG;
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = sample->ConvertToContiguousBuffer(&buffer);
    if (FAILED(hr)) return hr;
    auto frame = std::make_shared<BackgroundFrame>();
    frame->width = format.width;
    frame->orientation=format.orientation;
    frame->pixelAspectNumerator=format.pixelAspectNumerator;
    frame->pixelAspectDenominator=format.pixelAspectDenominator;
    frame->height = format.height;
    frame->stride = format.width * 4;
    frame->bgra.resize(size_t{frame->stride} * frame->height);
    ComPtr<IMF2DBuffer2> twoDimensional;
    if (SUCCEEDED(buffer.As(&twoDimensional))) {
        BYTE* scanline = nullptr;
        BYTE* start = nullptr;
        DWORD length = 0;
        LONG pitch = 0;
        hr = twoDimensional->Lock2DSize(MF2DBuffer_LockFlags_Read,
            &scanline, &pitch, &start, &length);
        if (SUCCEEDED(hr)) {
            hr = CopyRows(*frame, scanline, pitch, start, length, format.cropX, format.cropY);
            twoDimensional->Unlock2D();
        }
    } else {
        BYTE* bytes = nullptr;
        DWORD maximum = 0, current = 0;
        hr = buffer->Lock(&bytes, &maximum, &current);
        if (SUCCEEDED(hr)) {
            const int64_t pitch = format.stride;
            const uint64_t absolutePitch = pitch < 0 ? static_cast<uint64_t>(-pitch) :
                static_cast<uint64_t>(pitch);
            const uint64_t offset = pitch < 0 ? absolutePitch * (format.codedHeight - 1) : 0;
            if (!bytes || current > maximum || offset > current)
                hr = MF_E_BUFFERTOOSMALL;
            else hr = CopyRows(*frame, bytes + static_cast<size_t>(offset),
                format.stride, bytes, current, format.cropX, format.cropY);
            buffer->Unlock();
        }
    }
    if (SUCCEEDED(hr)) result = std::move(frame);
    return hr;
}
HRESULT DecodeImage(HANDLE file, std::shared_ptr<BackgroundFrame>& result) {
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;
    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFileHandle(reinterpret_cast<ULONG_PTR>(file),nullptr,
        WICDecodeMetadataCacheOnDemand,&decoder);
    if (FAILED(hr)) return hr;
    ComPtr<IWICBitmapFrameDecode> image;
    if (FAILED(hr = decoder->GetFrame(0, &image))) return hr;
    UINT width = 0, height = 0;
    if (FAILED(hr = image->GetSize(&width, &height))) return hr;
    if (!width || !height || width > 16384 || height > 16384 ||
        uint64_t{width} * height > kMaxImageSourcePixels)
        return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    double scale = std::min(1.0, double{kMaxEdge} / std::max(width, height));
    scale = std::min(scale, std::sqrt(static_cast<double>(kMaxPixels) /
        (static_cast<double>(width) * height)));
    const UINT scaledWidth = std::max(1U, static_cast<UINT>(std::floor(width * scale)));
    const UINT scaledHeight = std::max(1U, static_cast<UINT>(std::floor(height * scale)));
    if (!FrameSizeValid(scaledWidth, scaledHeight)) return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    ComPtr<IWICBitmapSource> source;
    if (FAILED(hr = image.As(&source))) return hr;
    ComPtr<IWICBitmapScaler> scaler;
    if (scaledWidth != width || scaledHeight != height) {
        if (FAILED(hr = factory->CreateBitmapScaler(&scaler)) ||
            FAILED(hr = scaler->Initialize(image.Get(), scaledWidth, scaledHeight,
                WICBitmapInterpolationModeFant))) return hr;
        if (FAILED(hr = scaler.As(&source))) return hr;
    }
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(hr = factory->CreateFormatConverter(&converter))) return hr;
    WICPixelFormatGUID sourceFormat{};
    BOOL convertible = FALSE;
    if (FAILED(hr = source->GetPixelFormat(&sourceFormat)) ||
        FAILED(hr = converter->CanConvert(sourceFormat, GUID_WICPixelFormat32bppPBGRA,
            &convertible))) return hr;
    if (!convertible) return WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    hr = converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return hr;
    auto frame = std::make_shared<BackgroundFrame>();
    frame->width = scaledWidth;
    frame->orientation=PhotoOrientation(image.Get());
    frame->height = scaledHeight;
    frame->stride = scaledWidth * 4;
    frame->bgra.resize(size_t{frame->stride} * scaledHeight);
    hr = converter->CopyPixels(nullptr, frame->stride,
        static_cast<UINT>(frame->bgra.size()), frame->bgra.data());
    if (SUCCEEDED(hr)) result = std::move(frame);
    return hr;
}
} // namespace

struct BackgroundController::Impl {
    struct Job {
        BackgroundKind kind = BackgroundKind::None;
        std::wstring path;
        bool loop = false;
        uint64_t generation = 0;
    };
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::atomic<uint64_t> generation{0};
    bool pending = false, closing = false, playbackDemand = true;
    Clock::time_point playbackSuspendedAt{};
    Clock::duration completedPlaybackSuspension{};
    Job job;
    BackgroundSnapshot snapshot;
    std::weak_ptr<ReadState> activeRead;
    uint64_t frameSequence = 0;
    std::thread worker;

    Impl() : worker([this] { Run(); }) {}
    ~Impl() {
        std::shared_ptr<ReadState> reading;
        {
            std::lock_guard lock(mutex);
            closing = true;
            generation.fetch_add(1);
            reading=activeRead.lock();
        }
        CancelPendingRead(reading);
        wake.notify_all();
        if (worker.joinable()) worker.join();
    }
    bool Cancelled(const Job& current) const {
        return generation.load() != current.generation;
    }
    void SetPlaybackDemand(bool value) {
        {
            std::lock_guard lock(mutex);
            if(closing||playbackDemand==value)return;
            const auto changedAt=Clock::now();
            if(value)completedPlaybackSuspension+=changedAt-playbackSuspendedAt;
            else playbackSuspendedAt=changedAt;
            playbackDemand=value;
            if(!playbackDemand&&snapshot.kind==BackgroundKind::Video)snapshot.playing=false;
        }
        wake.notify_all();
    }
    void Queue(BackgroundKind kind, const std::wstring& path, bool loop) {
        // Complete every allocation before invalidating the active generation.
        // If the caller cannot copy a long path, the current source keeps
        // running instead of being cancelled without a replacement job.
        Job replacement{kind,path,loop,0};
        BackgroundSnapshot replacementSnapshot;
        replacementSnapshot.kind=kind;
        replacementSnapshot.filePath=path;
        replacementSnapshot.fileName=FileName(path);
        replacementSnapshot.loading=kind!=BackgroundKind::None;
        replacementSnapshot.loop=kind==BackgroundKind::Video&&loop;
        std::shared_ptr<ReadState> reading;
        {
            std::lock_guard lock(mutex);
            if (closing) return;
            const uint64_t next = generation.fetch_add(1) + 1;
            replacement.generation=next;
            replacementSnapshot.sourceRevision=next;
            reading=activeRead.lock();
            job=std::move(replacement);
            pending = true;
            snapshot=std::move(replacementSnapshot);
        }
        CancelPendingRead(reading);
        wake.notify_all();
    }
    void Fail(const Job& current, const wchar_t* message, HRESULT hr) {
        std::lock_guard lock(mutex);
        if (Cancelled(current)) return;
        snapshot.loading = false;
        snapshot.playing = false;
        snapshot.errorCode = hr;
        try { snapshot.error = ErrorText(message, hr); }
        catch (...) { snapshot.error.clear(); }
    }
    bool Retry(uint64_t expectedRevision) {
        std::shared_ptr<ReadState> reading;
        {
            std::lock_guard lock(mutex);
            if(closing||snapshot.sourceRevision!=expectedRevision||snapshot.kind==BackgroundKind::None||
                snapshot.filePath.empty()||snapshot.loading||SUCCEEDED(snapshot.errorCode))return false;
            // Copy the path before invalidating the worker. Allocation failure
            // must not cancel a source without leaving a replacement queued.
            Job retry{snapshot.kind,snapshot.filePath,snapshot.loop,0};
            reading=activeRead.lock();
            retry.generation=generation.fetch_add(1)+1;
            job=std::move(retry);pending=true;
            snapshot.sourceRevision=job.generation;snapshot.loading=true;snapshot.playing=false;
        }
        CancelPendingRead(reading);
        wake.notify_all();return true;
    }
    void Publish(const Job& current, std::shared_ptr<BackgroundFrame> frame) {
        std::lock_guard lock(mutex);
        if (Cancelled(current)) return;
        frame->sequence = ++frameSequence;
        frame->presentedAtMs = NowMs();
        snapshot.frame = std::move(frame);
        snapshot.loading = false;
        snapshot.playing = current.kind == BackgroundKind::Video&&playbackDemand;
        snapshot.error.clear();
        snapshot.errorCode = S_OK;
    }
    bool WaitForPlaybackDemand(const Job& current,Clock::duration& observedSuspension,Clock::duration& suspended,
        Clock::time_point* synchronizedAt=nullptr) {
        suspended=Clock::duration::zero();
        std::unique_lock lock(mutex);
        if(closing||Cancelled(current))return true;
        if(current.kind!=BackgroundKind::Video)return false;
        if(!playbackDemand){
            snapshot.playing=false;
            wake.wait(lock,[&]{return closing||Cancelled(current)||playbackDemand;});
        }
        if(completedPlaybackSuspension>observedSuspension){
            suspended=completedPlaybackSuspension-observedSuspension;
            observedSuspension=completedPlaybackSuspension;
        }
        if(synchronizedAt)*synchronizedAt=Clock::now();
        if(!closing&&!Cancelled(current)&&snapshot.frame&&!snapshot.loading&&SUCCEEDED(snapshot.errorCode))snapshot.playing=true;
        return closing||Cancelled(current);
    }
    bool WaitUntil(const Job& current, Clock::time_point when,Clock::duration& observedSuspension,Clock::duration& suspended) {
        suspended=Clock::duration::zero();
        std::unique_lock lock(mutex);
        for(;;){
            if(closing||Cancelled(current))return true;
            if(!playbackDemand){
                snapshot.playing=false;
                wake.wait(lock,[&]{return closing||Cancelled(current)||playbackDemand;});
                continue;
            }
            if(completedPlaybackSuspension>observedSuspension){
                const auto held=completedPlaybackSuspension-observedSuspension;
                observedSuspension=completedPlaybackSuspension;suspended+=held;when+=held;
                if(snapshot.frame&&!snapshot.loading&&SUCCEEDED(snapshot.errorCode))snapshot.playing=true;
            }
            if(Clock::now()>=when)return false;
            wake.wait_until(lock,when);
        }
    }
    HRESULT NextPacket(const Job& current, ReaderSession& session, ReadPacket& packet) {
        if (Cancelled(current)) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        ReadPacket retired;
        {
            std::lock_guard lock(session.state->mutex);
            if(session.state->cancelled||Cancelled(current))return HRESULT_FROM_WIN32(ERROR_CANCELLED);
            if(FAILED(session.state->terminalStatus))return session.state->terminalStatus;
            retired=std::move(session.state->packet);session.state->packet={};
            session.state->ready = false;
        }
        retired.sample.Reset();
        HRESULT hr = session.reader->ReadSample(kFirstVideoStream,
            0, nullptr, nullptr, nullptr, nullptr);
        if (FAILED(hr)) return hr;
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        std::unique_lock lock(session.state->mutex);
        if(!session.state->changed.wait_until(lock,deadline,[&]{return session.state->ready||session.state->cancelled||
            Cancelled(current)||FAILED(session.state->terminalStatus);}))return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        if (Cancelled(current) || session.state->cancelled)
            return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        if(FAILED(session.state->terminalStatus))return session.state->terminalStatus;
        retired = std::move(session.state->packet);
        session.state->ready = false;
        lock.unlock();packet=std::move(retired);
        return packet.status;
    }
    HRESULT OpenVideo(const Job& current, ReaderSession& session, VideoFormat& format) {
        {std::lock_guard lock(mutex);if(Cancelled(current))return HRESULT_FROM_WIN32(ERROR_CANCELLED);activeRead=session.state;}
        session.callback = Microsoft::WRL::Make<ReaderCallback>(session.state);
        if (!session.callback) return E_OUTOFMEMORY;
        ComPtr<IMFAttributes> attributes;
        HRESULT hr = MFCreateAttributes(&attributes, 3);
        if (FAILED(hr)) return hr;
        if (FAILED(hr = attributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK,
            static_cast<IMFSourceReaderCallback*>(session.callback.Get()))) ||
            FAILED(hr = attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE)))
            return hr;
        hr = MFCreateSourceReaderFromURL(current.path.c_str(), attributes.Get(), &session.reader);
        if (FAILED(hr)) return hr;
        if (Cancelled(current)) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        if (FAILED(hr = session.reader->SetStreamSelection(kAllStreams, FALSE)) ||
            FAILED(hr = session.reader->SetStreamSelection(kFirstVideoStream, TRUE)))
            return hr;
        return ConfigureVideoFormat(session.reader.Get(), format);
    }
    void PlayVideo(const Job& current) {
        Clock::duration observedSuspension{},initialSuspension{};
        if(WaitForPlaybackDemand(current,observedSuspension,initialSuspension))return;
        ReaderSession session;
        VideoFormat format;
        HRESULT hr = OpenVideo(current, session, format);
        if (FAILED(hr)) {
            if (!Cancelled(current)) Fail(current,
                L"视频打开失败：文件可能损坏、超过4K限制，或系统缺少对应编解码器", hr);
            return;
        }
        bool anchored = false, nextOriginProvided = false, gotFrame = false;
        LONGLONG firstPts = 0, previousPts = 0;
        unsigned emptyPackets = 0;
        Clock::time_point origin{}, lastEnd{};
        while (!Cancelled(current)) {
            Clock::duration suspension{};
            if(WaitForPlaybackDemand(current,observedSuspension,suspension))return;
            if((anchored||nextOriginProvided)&&suspension>Clock::duration::zero()){origin+=suspension;lastEnd+=suspension;}
            ReadPacket packet;
            hr = NextPacket(current, session, packet);
            if (FAILED(hr)) {
                if (!Cancelled(current)) Fail(current, L"视频后台解码失败或读取超时", hr);
                return;
            }
            suspension=Clock::duration::zero();
            if(WaitForPlaybackDemand(current,observedSuspension,suspension))return;
            if((anchored||nextOriginProvided)&&suspension>Clock::duration::zero()){origin+=suspension;lastEnd+=suspension;}
            if (packet.flags & MF_SOURCE_READERF_ERROR) {
                Fail(current, L"视频解码器报告错误", E_FAIL);
                return;
            }
            if (packet.flags & MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED) {
                hr = ConfigureVideoFormat(session.reader.Get(), format);
                if (FAILED(hr)) {
                    Fail(current, L"视频原生格式变化后无法重新配置有效的4K以内RGB32输出", hr);
                    return;
                }
                // This sample preceded output renegotiation. Do not interpret
                // its buffer using the newly selected layout. Preserve the
                // published image, timeline and EOS flag until the next frame.
                packet.sample.Reset();
            } else if (packet.flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
                hr = RefreshVideoFormat(session.reader.Get(), format);
                if (FAILED(hr)) {
                    Fail(current, L"视频输出不是有效的4K以内RGB32帧", hr);
                    return;
                }
            }
            if (packet.sample) {
                emptyPackets = 0;
                if (packet.pts < -100000000LL || packet.pts > 6048000000000LL) {
                    Fail(current, L"视频时间戳超出支持范围", MF_E_INVALID_TIMESTAMP);
                    return;
                }
                LONGLONG duration = format.frameDuration;
                LONGLONG sampleDuration = 0;
                if (SUCCEEDED(packet.sample->GetSampleDuration(&sampleDuration)) &&
                    sampleDuration > 0 && sampleDuration <= 10000000LL)
                    duration = sampleDuration;
                // Consume every completed visibility pause and sample the clock
                // under the same lock. A fast hide/show cannot make this frame
                // look late against an origin that has not yet been shifted.
                Clock::time_point synchronizedAt{};
                suspension=Clock::duration::zero();
                if(WaitForPlaybackDemand(current,observedSuspension,suspension,&synchronizedAt))return;
                if((anchored||nextOriginProvided)&&suspension>Clock::duration::zero()){origin+=suspension;lastEnd+=suspension;}
                if (!anchored) {
                    firstPts = packet.pts;
                    previousPts = packet.pts;
                    if (!nextOriginProvided) origin = synchronizedAt;
                    anchored = true;
                    nextOriginProvided = false;
                }
                if (packet.pts < firstPts || packet.pts < previousPts ||
                    packet.pts - previousPts > 300000000LL) {
                    Fail(current, L"视频时间戳不连续，无法可靠播放", MF_E_INVALID_TIMESTAMP);
                    return;
                }
                previousPts = packet.pts;
                const auto due = origin + std::chrono::nanoseconds((packet.pts - firstPts) * 100);
                lastEnd = due + std::chrono::nanoseconds(duration * 100);
                gotFrame = true;
                // Decode late packets but avoid uploading stale pictures. The media
                // clock is never reset to each callback, so decoder delay cannot
                // accumulate as playback drift.
                if (synchronizedAt - due <= std::chrono::milliseconds(200)) {
                    std::shared_ptr<BackgroundFrame> frame;
                    hr = DecodeVideoFrame(packet.sample.Get(), format, frame);
                    if (FAILED(hr)) {
                        Fail(current, L"视频RGB32像素、行距或帧缓冲无效", hr);
                        return;
                    }
                    frame->ptsMs = packet.pts / 10000;
                    auto scheduledDue=due;suspension=Clock::duration::zero();
                    if (WaitUntil(current, scheduledDue, observedSuspension, suspension)) return;
                    if(suspension>Clock::duration::zero()){origin+=suspension;lastEnd+=suspension;}
                    Publish(current, std::move(frame));
                }
            } else if (++emptyPackets > 1000) {
                Fail(current, L"视频未产生有效画面", MF_E_INVALIDMEDIATYPE);
                return;
            }
            if (packet.flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                if (!gotFrame) {
                    Fail(current, L"文件没有可显示的视频帧", MF_E_INVALIDMEDIATYPE);
                    return;
                }
                auto scheduledEnd=lastEnd;suspension=Clock::duration::zero();
                if (WaitUntil(current, scheduledEnd, observedSuspension, suspension)) return;
                if(suspension>Clock::duration::zero()){origin+=suspension;lastEnd+=suspension;}
                if (!current.loop) {
                    std::lock_guard lock(mutex);
                    if (!Cancelled(current)) snapshot.playing = false;
                    return;
                }
                PROPVARIANT beginning{};
                beginning.vt = VT_I8;
                beginning.hVal.QuadPart = 0;
                hr = session.reader->SetCurrentPosition(GUID_NULL, beginning);
                if (FAILED(hr)) {
                    Fail(current, L"该视频不能回到开头循环播放", hr);
                    return;
                }
                const auto loopSpan=lastEnd-origin;
                origin = lastEnd;
                anchored = false;
                nextOriginProvided = true;
                Clock::time_point synchronizedAt{};
                suspension=Clock::duration::zero();
                if(WaitForPlaybackDemand(current,observedSuspension,suspension,&synchronizedAt))return;
                if(suspension>Clock::duration::zero()){origin+=suspension;lastEnd+=suspension;}
                const auto overdue=synchronizedAt-origin;
                if(loopSpan>Clock::duration::zero()&&overdue>=loopSpan){
                    // Skip fully expired repetitions in one step. Preserve
                    // phase instead of decoding every missed loop or adding
                    // each ordinary seek/decoder delay to the media clock.
                    origin+=loopSpan*(overdue/loopSpan);
                }
                gotFrame = false;
            }
        }
    }
    void Run() noexcept {
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool mfStarted = false;
        HRESULT mfResult = S_OK;
        for (;;) {
            Job current;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] { return closing || pending; });
                if (closing) break;
                current = std::move(job);
                pending = false;
            }
            if (current.kind == BackgroundKind::None) continue;
            if (FAILED(comResult)) {
                Fail(current, L"原生背景组件初始化失败", comResult);
                continue;
            }
            try {
                LockedLocalFile local;
                HRESULT hr = OpenLocalFile(current.path,current.kind==BackgroundKind::Image,local);
                if (FAILED(hr)) {
                    Fail(current, L"请选择可读取的本地图片或视频文件", hr);
                    continue;
                }
                current.path = std::move(local.path);
                {
                    std::lock_guard lock(mutex);
                    if (Cancelled(current)) continue;
                    snapshot.filePath = current.path;
                    snapshot.fileName = FileName(current.path);
                }
                if (current.kind == BackgroundKind::Image) {
                    std::shared_ptr<BackgroundFrame> frame;
                    hr = DecodeImage(local.handle, frame);
                    if (FAILED(hr)) Fail(current,
                        L"图片读取失败：格式不支持、文件损坏或原始尺寸超过安全限制", hr);
                    else Publish(current, std::move(frame));
                } else {
                    if (!mfStarted) {
                        mfResult = MFStartup(MF_VERSION, MFSTARTUP_FULL);
                        mfStarted = SUCCEEDED(mfResult);
                    }
                    if (!mfStarted) Fail(current,
                        L"Windows Media Foundation不可用，无法播放视频背景", mfResult);
                    else PlayVideo(current);
                }
            } catch (const std::bad_alloc&) {
                Fail(current, L"背景帧内存分配失败", E_OUTOFMEMORY);
            } catch (...) {
                Fail(current, L"背景读取发生异常", E_FAIL);
            }
        }
        if (mfStarted) MFShutdown();
        if (SUCCEEDED(comResult)) CoUninitialize();
    }
};

BackgroundController::BackgroundController() : impl_(std::make_unique<Impl>()) {}
BackgroundController::~BackgroundController() = default;
void BackgroundController::SetImage(const std::wstring& path) {
    impl_->Queue(BackgroundKind::Image, path, false);
}
void BackgroundController::SetVideo(const std::wstring& path, bool loop) {
    impl_->Queue(BackgroundKind::Video, path, loop);
}
void BackgroundController::SetPlaybackDemand(bool visibleStage) {impl_->SetPlaybackDemand(visibleStage);}
bool BackgroundController::Retry(uint64_t expectedSourceRevision) {return impl_->Retry(expectedSourceRevision);}
void BackgroundController::Clear() { impl_->Queue(BackgroundKind::None, {}, false); }
void BackgroundController::Stop() { Clear(); }
BackgroundSnapshot BackgroundController::GetSnapshot() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->snapshot;
}

} // namespace stage
