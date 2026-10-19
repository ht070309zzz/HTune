#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "cover.h"
#include "file_transaction.h"
#include "native_http.h"
#include "third_party/json.hpp"
#include <windows.h>
#include <winhttp.h>
#include <wincodec.h>
#include <shlobj.h>
#include <bcrypt.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <optional>
#include <set>
#include <string_view>
#include <stdexcept>
#include <vector>

#pragma comment(lib, "Winhttp.lib")
#pragma comment(lib, "Windowscodecs.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Bcrypt.lib")

namespace stage {
namespace {
using CoverJson = nlohmann::json;
using Microsoft::WRL::ComPtr;
constexpr size_t CoverMaxImageBytes = 10 * 1024 * 1024;
constexpr size_t CoverMaxMetadataBytes = 2 * 1024 * 1024;
constexpr size_t CoverMaxManifestBytes = 16 * 1024;
constexpr uint32_t CoverMaxDimension = 4096;
constexpr uint64_t CoverMaxPixels = 16 * 1024 * 1024;
constexpr uint64_t CoverCacheQuotaBytes = 512ull * 1024 * 1024;
constexpr size_t CoverCacheRecordLimit = 1024;
constexpr size_t CoverMaxPruneAttempts = 32;
constexpr unsigned CoverRecoverySlots = 8;

class CoverFileHandle {
    HANDLE value_;
public:
    explicit CoverFileHandle(HANDLE value) : value_(value) {}
    ~CoverFileHandle() { Close(); }
    CoverFileHandle(const CoverFileHandle&) = delete;
    CoverFileHandle& operator=(const CoverFileHandle&) = delete;
    HANDLE get() const { return value_; }
    void Close() { if (value_ != INVALID_HANDLE_VALUE && value_) CloseHandle(value_); value_ = INVALID_HANDLE_VALUE; }
};

class CoverCacheWriteLock {
    CoverFileHandle mutex_;
    DWORD creationError_ = ERROR_SUCCESS;
    bool owned_ = false;
public:
    CoverCacheWriteLock()
        : mutex_(CreateMutexW(nullptr, FALSE,
            L"Local\\MusicStageNativePrototype.CoverCache.Transaction.v1")) {
        if (!mutex_.get() || mutex_.get() == INVALID_HANDLE_VALUE)
            creationError_ = GetLastError();
    }
    ~CoverCacheWriteLock() { if (owned_) ReleaseMutex(mutex_.get()); }
    CoverCacheWriteLock(const CoverCacheWriteLock&) = delete;
    CoverCacheWriteLock& operator=(const CoverCacheWriteLock&) = delete;

    bool Acquire(std::stop_token stop, std::wstring& error) {
        if (!mutex_.get() || mutex_.get() == INVALID_HANDLE_VALUE) {
            error = L"无法建立封面缓存写入锁（Windows " + std::to_wstring(creationError_) + L"）";
            return false;
        }
        DWORD waited = 0;
        for (;;) {
            if (stop.stop_requested()) {
                error = L"封面获取已取消";
                return false;
            }
            const DWORD wait = WaitForSingleObject(mutex_.get(), 50);
            if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED_0) {
                owned_ = true;
                return true;
            }
            if (wait == WAIT_TIMEOUT) {
                waited += 50;
                if (waited < 30000) continue;
                error = L"封面缓存正在由另一项任务写入，等待已超过30秒";
                return false;
            }
            const DWORD waitError = GetLastError();
            error = L"无法取得封面缓存写入锁（Windows " + std::to_wstring(waitError) + L"）";
            return false;
        }
    }
};

std::wstring CoverError(const wchar_t* action) {
    return std::wstring(action) + L"失败（Windows " + std::to_wstring(GetLastError()) + L"）";
}
std::wstring CoverFromUtf8(const std::string& text) {
    if (text.empty() || text.size() > CoverMaxMetadataBytes) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring value(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), value.data(), length);
    if (value.find(L'\0') != std::wstring::npos) return {};
    return value;
}
std::string CoverAscii(const std::wstring& text) {
    std::string result;
    result.reserve(text.size());
    for (wchar_t c : text) {
        if (c > 0x7F) return {};
        result.push_back(static_cast<char>(c));
    }
    return result;
}
bool CoverNumericId(const std::wstring& text, uint64_t& value) {
    if (text.empty() || text.size() > 19) return false;
    value = 0;
    for (wchar_t c : text) {
        if (c < L'0' || c > L'9') return false;
        if (value > (static_cast<uint64_t>(INT64_MAX) - (c - L'0')) / 10) return false;
        value = value * 10 + (c - L'0');
    }
    return value != 0;
}

CoverJson CoverParseJson(const std::vector<BYTE>& data, std::wstring& error) {
    try {
        auto check = [](int depth, CoverJson::parse_event_t, CoverJson&) {
            if (depth > 32) throw std::runtime_error("cover JSON depth limit");
            return true;
        };
        auto value = CoverJson::parse(data.begin(), data.end(), check);
        if (!value.is_object()) throw std::runtime_error("cover JSON object expected");
        return value;
    } catch (...) { error = L"封面元数据不是有效 JSON"; return {}; }
}
const CoverJson* CoverField(const CoverJson& value, const char* field) {
    if (!value.is_object()) return nullptr;
    const auto item = value.find(field);
    return item == value.end() ? nullptr : &*item;
}
std::string CoverString(const CoverJson& value, const char* field) {
    const auto* item = CoverField(value, field);
    return item && item->is_string() ? item->get<std::string>() : std::string{};
}
uint64_t CoverNumber(const CoverJson& value, const char* field) {
    const auto* item = CoverField(value, field);
    if (!item) return 0;
    if (item->is_number_unsigned()) return item->get<uint64_t>();
    if (item->is_number_integer()) { const auto number = item->get<int64_t>(); return number > 0 ? static_cast<uint64_t>(number) : 0; }
    return 0;
}

struct CoverCdnUrl { std::wstring host, path, normalized; };
bool CoverTrustedUrl(const std::wstring& input, CoverCdnUrl& result) {
    if (input.empty() || input.size() > 4096) return false;
    for (wchar_t c : input) if (c < 0x21 || c > 0x7E || c == L'\\' || c == L'#') return false;
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength =
        parts.dwUserNameLength = parts.dwPasswordLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(input.c_str(), static_cast<DWORD>(input.size()), ICU_REJECT_USERPWD, &parts) ||
        parts.dwUserNameLength || parts.dwPasswordLength) return false;
    if (parts.nScheme != INTERNET_SCHEME_HTTPS && parts.nScheme != INTERNET_SCHEME_HTTP) return false;
    if ((parts.nScheme == INTERNET_SCHEME_HTTPS && parts.nPort != 443) ||
        (parts.nScheme == INTERNET_SCHEME_HTTP && parts.nPort != 80)) return false;
    result.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    for (auto& c : result.host) if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
    if (result.host != L"music.126.net" && !result.host.ends_with(L".music.126.net")) return false;
    if (result.host.size() > 253) return false;
    size_t labelStart = 0;
    for (size_t i = 0; i <= result.host.size(); ++i) {
        if (i == result.host.size() || result.host[i] == L'.') {
            if (i == labelStart || i - labelStart > 63 || result.host[labelStart] == L'-' || result.host[i - 1] == L'-') return false;
            labelStart = i + 1;
        } else if (!((result.host[i] >= L'a' && result.host[i] <= L'z') ||
            (result.host[i] >= L'0' && result.host[i] <= L'9') || result.host[i] == L'-')) return false;
    }
    result.path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) result.path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (result.path.empty() || result.path.front() != L'/' || result.path.starts_with(L"//")) return false;
    // Only known HTTPS CDN host/path is reconstructed. An HTTP value returned
    // by the catalog is upgraded; its scheme/port/credentials are never used.
    result.normalized = L"https://" + result.host + result.path;
    return true;
}

std::vector<BYTE> CoverHttp(const std::wstring& host, const std::wstring& path, size_t limit,
    std::wstring& error, std::stop_token stop) {
    constexpr wchar_t headers[] = L"Referer: https://music.163.com/\r\n";
    auto response = NativeHttps({host, path, headers, {}, limit, 15000}, stop);
    if (!response.ok()) { error = response.diagnostic; return {}; }
    auto data = std::move(response.bytes);
    if (data.empty()) error = L"封面服务返回空数据";
    return data;
}

std::wstring CoverHash(const std::vector<BYTE>& data) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    struct Cleanup {
        BCRYPT_ALG_HANDLE& algorithm; BCRYPT_HASH_HANDLE& hash;
        ~Cleanup() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
    } cleanup{algorithm, hash};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0 ||
        BCryptHashData(hash, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0) < 0) return {};
    BYTE digest[32];
    if (BCryptFinishHash(hash, digest, sizeof(digest), 0) < 0) return {};
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring value;
    for (BYTE byte : digest) { value += hex[byte >> 4]; value += hex[byte & 15]; }
    return value;
}

bool CoverDecode(const std::vector<BYTE>& bytes, uint32_t& width, uint32_t& height,
    std::wstring& extension, std::wstring& error) {
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    ComPtr<IWICStream> stream;
    if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()));
    ComPtr<IWICBitmapDecoder> decoder;
    if (SUCCEEDED(hr)) hr = factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    GUID format{};
    if (SUCCEEDED(hr)) hr = decoder->GetContainerFormat(&format);
    if (FAILED(hr)) { error = L"封面不能由 Windows WIC 解码"; return false; }
    if (IsEqualGUID(format, GUID_ContainerFormatJpeg)) extension = L"jpg";
    else if (IsEqualGUID(format, GUID_ContainerFormatPng)) extension = L"png";
    else { error = L"封面必须是静态 JPEG 或 PNG"; return false; }
    UINT frames = 0;
    hr = decoder->GetFrameCount(&frames);
    if (FAILED(hr) || frames != 1) { error = L"不接受多帧封面"; return false; }
    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = frame->GetSize(&width, &height);
    if (FAILED(hr) || !width || !height || width > CoverMaxDimension || height > CoverMaxDimension ||
        static_cast<uint64_t>(width) * height > CoverMaxPixels) { error = L"封面像素尺寸超过 4096×4096 上限或无效"; return false; }
    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
    const UINT stride = width * 4;
    std::vector<BYTE> pixels(static_cast<size_t>(stride) * height);
    if (SUCCEEDED(hr)) hr = converter->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data());
    if (FAILED(hr)) { error = L"封面像素未能完整解码"; return false; }
    return true;
}

bool CoverCacheDirectory(std::filesystem::path& directory,
    detail::DirectoryTreeLock& directoryLock, std::wstring& error) {
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) { error = L"无法取得独立封面缓存目录"; return false; }
    directory = std::filesystem::path(local) / L"MusicStageNativePrototype" / L"cache" / L"covers";
    CoTaskMemFree(local);
    DWORD directoryError = ERROR_SUCCESS;
    if (!directoryLock.Acquire(directory, directoryError, true)) {
        error = L"封面缓存目录不可用、不是本地普通目录或是重解析路径（Windows " +
            std::to_wstring(directoryError) + L"）";
        return false;
    }
    return true;
}

bool CoverReadHandle(HANDLE file, size_t limit, std::vector<BYTE>& bytes) {
    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER size{};
    if (!file || file == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
        !GetFileSizeEx(file, &size) || size.QuadPart < 1 || static_cast<uint64_t>(size.QuadPart) > limit) return false;
    bytes.resize(static_cast<size_t>(size.QuadPart));
    LARGE_INTEGER beginning{};
    if (!SetFilePointerEx(file, beginning, nullptr, FILE_BEGIN)) return false;
    DWORD read = 0;
    return ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size();
}

bool CoverAtomicCreate(const std::filesystem::path& target, const std::vector<BYTE>& bytes, std::wstring& error) {
    GUID value{}; wchar_t suffix[40]{};
    if (FAILED(CoCreateGuid(&value)) || !StringFromGUID2(value, suffix, 40)) { error = L"无法创建封面临时记录"; return false; }
    auto temporary = target;
    temporary += std::wstring(L".") + suffix + L".tmp";
    CoverFileHandle file(CreateFileW(temporary.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) { error = CoverError(L"创建封面缓存临时文件"); return false; }
    DWORD written = 0;
    bool success = WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
        written == bytes.size() && FlushFileBuffers(file.get());
    DWORD transactionError = ERROR_SUCCESS;
    // Deliberately refuse replacement: unknown/older records win. Rename and
    // cleanup both address the exact application-created open object, so a
    // path substitution can never make failure cleanup delete another file.
    if (success) success = detail::RenameOpenFile(file.get(), target, false, transactionError);
    if (!success) {
        detail::DeleteOpenFile(file.get());
        error = L"缓存写入失败或记录已经存在；旧文件已保留";
    }
    return success;
}

enum class CoverCacheState { Missing, Hit, Conflict };
struct CoverFileSnapshot {
    BY_HANDLE_FILE_INFORMATION information{};
    FILE_ID_INFO extendedId{};
    uint64_t bytes = 0;
    bool extendedAvailable = false;
};
bool CoverCaptureSnapshot(HANDLE file, CoverFileSnapshot& snapshot) {
    snapshot = {};
    LARGE_INTEGER size{};
    if (!file || file == INVALID_HANDLE_VALUE || GetFileType(file) != FILE_TYPE_DISK ||
        !GetFileInformationByHandle(file, &snapshot.information) ||
        snapshot.information.nNumberOfLinks != 1 ||
        (snapshot.information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        !GetFileSizeEx(file, &size) || size.QuadPart < 0) return false;
    snapshot.bytes = static_cast<uint64_t>(size.QuadPart);
    snapshot.extendedAvailable = GetFileInformationByHandleEx(file, FileIdInfo,
        &snapshot.extendedId, sizeof(snapshot.extendedId)) != FALSE;
    return true;
}
bool CoverSameSnapshot(const CoverFileSnapshot& left, const CoverFileSnapshot& right) {
    if (left.extendedAvailable != right.extendedAvailable || left.bytes != right.bytes ||
        left.information.dwFileAttributes != right.information.dwFileAttributes ||
        left.information.nNumberOfLinks != right.information.nNumberOfLinks ||
        CompareFileTime(&left.information.ftLastWriteTime, &right.information.ftLastWriteTime) != 0)
        return false;
    if (left.extendedAvailable) {
        return left.extendedId.VolumeSerialNumber == right.extendedId.VolumeSerialNumber &&
            std::memcmp(left.extendedId.FileId.Identifier, right.extendedId.FileId.Identifier,
                sizeof(left.extendedId.FileId.Identifier)) == 0;
    }
    return left.information.dwVolumeSerialNumber == right.information.dwVolumeSerialNumber &&
        left.information.nFileIndexHigh == right.information.nFileIndexHigh &&
        left.information.nFileIndexLow == right.information.nFileIndexLow;
}
struct CoverCacheMetadata {
    std::filesystem::path directory, marker, file;
    std::wstring songId, hash, imageUrl;
    uint64_t generation = 0, bytes = 0, writtenAt = 0;
    CoverFileSnapshot markerSnapshot, imageSnapshot;
};
uint64_t CoverFileTimeValue(const FILETIME& value) {
    return (uint64_t{value.dwHighDateTime} << 32) | value.dwLowDateTime;
}
std::wstring CoverPathKey(const std::filesystem::path& input) {
    std::error_code error;
    auto value = std::filesystem::absolute(input, error).lexically_normal().wstring();
    if (error) value = input.lexically_normal().wstring();
    if (!value.empty()) CharLowerBuffW(value.data(), static_cast<DWORD>(value.size()));
    return value;
}
bool CoverManifest(const std::vector<BYTE>& metadata, const std::filesystem::path& directory,
    const std::wstring& songId, CoverCacheMetadata& result) {
    std::wstring error;
    const auto record = CoverParseJson(metadata, error);
    const auto schema = CoverNumber(record, "schema");
    if (!error.empty() || CoverString(record, "owner") != "MusicStageNativePrototype.CoverCache" ||
        CoverString(record, "songId") != CoverAscii(songId) || (schema != 1 && schema != 2)) return false;
    const auto format = CoverString(record, "format");
    if (format != "jpg" && format != "png") return false;
    const auto hash = CoverString(record, "sha256");
    if (hash.size() != 64 || std::any_of(hash.begin(), hash.end(), [](char value) {
        return !((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'));
    })) return false;
    CoverCdnUrl url;
    if (!CoverTrustedUrl(CoverFromUtf8(CoverString(record, "imageUrl")), url)) return false;
    const auto metadataUrl = "https://music.163.com/api/song/detail?id=" + CoverAscii(songId) +
        "&ids=%5B" + CoverAscii(songId) + "%5D";
    if (CoverString(record, "metadataUrl") != metadataUrl) return false;
    const auto generation = schema == 2 ? CoverNumber(record, "generation") : 0;
    if (schema == 2 && !generation) return false;
    result.directory = directory;
    result.marker = directory / (songId + L".source.json");
    result.file = directory / (songId + L"." + CoverFromUtf8(format));
    result.songId = songId;
    result.hash = CoverFromUtf8(hash);
    result.imageUrl = url.normalized;
    result.generation = generation;
    return true;
}
bool CoverLoadManifest(const std::filesystem::path& directory, const std::wstring& songId,
    CoverCacheMetadata& result) {
    const auto marker = directory / (songId + L".source.json");
    CoverFileHandle markerFile(CreateFileW(marker.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    CoverFileSnapshot markerBefore, markerAfter;
    std::vector<BYTE> metadata;
    CoverCacheMetadata parsed;
    if (markerFile.get() == INVALID_HANDLE_VALUE ||
        !CoverCaptureSnapshot(markerFile.get(), markerBefore) || !markerBefore.bytes ||
        markerBefore.bytes > CoverMaxManifestBytes ||
        !CoverReadHandle(markerFile.get(), CoverMaxManifestBytes, metadata) ||
        !CoverCaptureSnapshot(markerFile.get(), markerAfter) ||
        !CoverSameSnapshot(markerBefore, markerAfter) ||
        !CoverManifest(metadata, directory, songId, parsed)) return false;
    CoverFileHandle imageFile(CreateFileW(parsed.file.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    CoverFileSnapshot image;
    if (imageFile.get() == INVALID_HANDLE_VALUE || !CoverCaptureSnapshot(imageFile.get(), image) ||
        !image.bytes || image.bytes > CoverMaxImageBytes)
        return false;
    parsed.markerSnapshot = markerBefore;
    parsed.imageSnapshot = image;
    parsed.bytes = markerBefore.bytes + image.bytes;
    parsed.writtenAt = std::max(CoverFileTimeValue(markerBefore.information.ftLastWriteTime),
        CoverFileTimeValue(image.information.ftLastWriteTime));
    result = std::move(parsed);
    return true;
}
CoverCacheState CoverReadCache(const std::filesystem::path& directory, CoverResult& result,
    CoverCacheMetadata* details = nullptr) {
    const auto marker = directory / (result.songId + L".source.json");
    const bool hasMarker = GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!hasMarker) {
        for (const auto* format : {L"jpg", L"png"})
            if (GetFileAttributesW((directory / (result.songId + L"." + format)).c_str()) != INVALID_FILE_ATTRIBUTES)
                return CoverCacheState::Conflict;
        return CoverCacheState::Missing;
    }
    CoverCacheMetadata metadata;
    if (!CoverLoadManifest(directory, result.songId, metadata)) return CoverCacheState::Conflict;
    CoverFileHandle markerFile(CreateFileW(metadata.marker.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    CoverFileHandle imageFile(CreateFileW(metadata.file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    CoverFileSnapshot markerBefore, markerAfter, imageBefore, imageAfter;
    std::vector<BYTE> bytes;
    std::wstring actualFormat, error;
    if (markerFile.get() == INVALID_HANDLE_VALUE || imageFile.get() == INVALID_HANDLE_VALUE ||
        !CoverCaptureSnapshot(markerFile.get(), markerBefore) ||
        !CoverCaptureSnapshot(imageFile.get(), imageBefore) ||
        !CoverSameSnapshot(metadata.markerSnapshot, markerBefore) ||
        !CoverSameSnapshot(metadata.imageSnapshot, imageBefore) ||
        !CoverReadHandle(imageFile.get(), CoverMaxImageBytes, bytes) ||
        !CoverCaptureSnapshot(markerFile.get(), markerAfter) ||
        !CoverCaptureSnapshot(imageFile.get(), imageAfter) ||
        !CoverSameSnapshot(markerBefore, markerAfter) ||
        !CoverSameSnapshot(imageBefore, imageAfter) ||
        CoverHash(bytes) != metadata.hash ||
        !CoverDecode(bytes, result.width, result.height, actualFormat, error) ||
        metadata.file.extension().wstring() != L"." + actualFormat)
        return CoverCacheState::Conflict;
    result.file = metadata.file;
    result.imageUrl = metadata.imageUrl;
    result.fromCache = true;
    if (details) *details = std::move(metadata);
    return CoverCacheState::Hit;
}

struct CoverCacheHit {
    CoverResult result;
    CoverCacheMetadata metadata;
};

std::vector<std::filesystem::path> CoverSongDirectories(const std::filesystem::path& root,
    const std::wstring& songId) {
    std::vector<std::filesystem::path> directories;
    directories.reserve(CoverRecoverySlots + 1);
    directories.push_back(root);
    for (unsigned slot = 0; slot < CoverRecoverySlots; ++slot)
        directories.push_back(root / (L"recovered-" + songId + L"-" + std::to_wstring(slot)));
    return directories;
}

bool CoverScanSong(const std::filesystem::path& root, const CoverResult& prototype,
    std::vector<CoverCacheHit>& hits, std::vector<std::filesystem::path>& available,
    uint64_t& maxGeneration, std::stop_token stop) {
    for (const auto& directory : CoverSongDirectories(root, prototype.songId)) {
        if (stop.stop_requested()) return false;
        const auto attributes = GetFileAttributesW(directory.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const auto error = GetLastError();
            if (directory != root && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND))
                available.push_back(directory);
            continue;
        }
        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            continue;
        CoverCacheHit hit{prototype, {}};
        const auto state = CoverReadCache(directory, hit.result, &hit.metadata);
        if (state == CoverCacheState::Hit) {
            maxGeneration = std::max(maxGeneration, hit.metadata.generation);
            hits.push_back(std::move(hit));
        } else if (state == CoverCacheState::Missing) {
            available.push_back(directory);
        }
    }
    return !stop.stop_requested();
}

const CoverCacheHit* CoverNewestHit(const std::vector<CoverCacheHit>& hits) {
    if (hits.empty()) return nullptr;
    return &*std::max_element(hits.begin(), hits.end(), [](const auto& left, const auto& right) {
        if (left.metadata.generation != right.metadata.generation)
            return left.metadata.generation < right.metadata.generation;
        return left.metadata.writtenAt < right.metadata.writtenAt;
    });
}

struct CoverCacheUsage {
    uint64_t bytes = 0;
    size_t ownedRecords = 0;
    bool complete = true;
    std::vector<CoverCacheMetadata> candidates;
};

bool CoverMarkerSongId(const std::filesystem::path& path, std::wstring& songId) {
    constexpr std::wstring_view suffix = L".source.json";
    const auto name = path.filename().wstring();
    if (!name.ends_with(suffix) || name.size() <= suffix.size()) return false;
    songId = name.substr(0, name.size() - suffix.size());
    uint64_t numeric = 0;
    return CoverNumericId(songId, numeric) && songId == std::to_wstring(numeric);
}

bool CoverOwnedDirectory(const std::filesystem::path& root,
    const std::filesystem::path& directory, const std::wstring& songId) {
    const auto key = CoverPathKey(directory);
    for (const auto& expected : CoverSongDirectories(root, songId))
        if (CoverPathKey(expected) == key) return true;
    return false;
}

bool CoverRecoveryDirectoryName(const std::filesystem::path& root,
    const std::filesystem::path& directory) {
    constexpr std::wstring_view prefix = L"recovered-";
    if (CoverPathKey(directory.parent_path()) != CoverPathKey(root)) return false;
    const auto name = directory.filename().wstring();
    if (!name.starts_with(prefix)) return false;
    const auto separator = name.rfind(L'-');
    if (separator == std::wstring::npos || separator <= prefix.size() || separator + 2 != name.size())
        return false;
    const auto songId = name.substr(prefix.size(), separator - prefix.size());
    uint64_t numeric = 0;
    const wchar_t slot = name[separator + 1];
    return CoverNumericId(songId, numeric) && songId == std::to_wstring(numeric) &&
        slot >= L'0' && slot < L'0' + static_cast<wchar_t>(CoverRecoverySlots);
}

bool CoverEnumerateCacheFiles(const std::filesystem::path& root, std::stop_token stop,
    std::vector<std::filesystem::path>& files) {
    constexpr size_t scanEntryLimit = 8192;
    files.clear();
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(root,
        std::filesystem::directory_options::none, error), end;
    if (error) return false;
    size_t entries = 0;
    for (; iterator != end; iterator.increment(error)) {
        if (stop.stop_requested() || error || ++entries > scanEntryLimit) return false;
        const auto path = iterator->path();
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) return false;
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            if (attributes & FILE_ATTRIBUTE_DIRECTORY) iterator.disable_recursion_pending();
            return false;
        }
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!CoverRecoveryDirectoryName(root, path)) {
                iterator.disable_recursion_pending();
                return false;
            }
            continue;
        }
        files.push_back(path.lexically_normal());
    }
    if (error) return false;
    std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
        return left.wstring() < right.wstring();
    });
    return true;
}

CoverCacheUsage CoverMeasureCache(const std::filesystem::path& root, std::stop_token stop) {
    CoverCacheUsage usage;
    std::vector<std::filesystem::path> files, verifiedFiles;
    if (!CoverEnumerateCacheFiles(root, stop, files)) { usage.complete = false; return usage; }
    std::vector<CoverFileSnapshot> snapshots;
    snapshots.reserve(files.size());
    std::set<std::wstring> observedFiles, ownedFiles;
    for (const auto& path : files) {
        if (stop.stop_requested()) { usage.complete = false; return usage; }
        CoverFileHandle file(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        CoverFileSnapshot snapshot;
        if (file.get() == INVALID_HANDLE_VALUE || !CoverCaptureSnapshot(file.get(), snapshot) ||
            snapshot.bytes > std::numeric_limits<uint64_t>::max() - usage.bytes) {
            usage.complete = false;
            return usage;
        }
        usage.bytes += snapshot.bytes;
        snapshots.push_back(snapshot);
        if (!observedFiles.insert(CoverPathKey(path)).second) {
            usage.complete = false;
            return usage;
        }
        std::wstring songId;
        if (!CoverMarkerSongId(path, songId)) continue;
        CoverResult validated;
        validated.songId = songId;
        CoverCacheMetadata metadata;
        if (CoverOwnedDirectory(root, path.parent_path(), songId) &&
            CoverReadCache(path.parent_path(), validated, &metadata) == CoverCacheState::Hit) {
            if (!ownedFiles.insert(CoverPathKey(metadata.marker)).second ||
                !ownedFiles.insert(CoverPathKey(metadata.file)).second) {
                usage.complete = false;
                return usage;
            }
            ++usage.ownedRecords;
            usage.candidates.push_back(std::move(metadata));
        }
    }
    if (!CoverEnumerateCacheFiles(root, stop, verifiedFiles) || verifiedFiles != files ||
        observedFiles != ownedFiles) {
        usage.complete = false;
        return usage;
    }
    for (size_t index = 0; index < files.size(); ++index) {
        if (stop.stop_requested()) { usage.complete = false; return usage; }
        CoverFileHandle verification(CreateFileW(files[index].c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        CoverFileSnapshot current;
        if (verification.get() == INVALID_HANDLE_VALUE || !CoverCaptureSnapshot(verification.get(), current) ||
            !CoverSameSnapshot(snapshots[index], current)) {
            usage.complete = false;
            return usage;
        }
    }
    return usage;
}

bool CoverProtected(const CoverCacheMetadata& metadata, const std::set<std::wstring>& protectedFiles) {
    return protectedFiles.contains(CoverPathKey(metadata.file));
}

enum class CoverDeleteState { NoChange, Removed, Partial };

CoverDeleteState CoverDeleteOwnedRecord(const CoverCacheMetadata& candidate,
    const std::set<std::wstring>& protectedFiles, std::stop_token stop) {
    if (stop.stop_requested() || CoverProtected(candidate, protectedFiles))
        return CoverDeleteState::NoChange;
    DWORD lockError = ERROR_SUCCESS;
    detail::DirectoryTreeLock directoryLock;
    if (!directoryLock.Acquire(candidate.directory, lockError)) return CoverDeleteState::NoChange;
    CoverFileHandle marker(CreateFileW(candidate.marker.c_str(), GENERIC_READ | DELETE,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (marker.get() == INVALID_HANDLE_VALUE) return CoverDeleteState::NoChange;
    CoverFileSnapshot markerBefore, markerAfter;
    std::vector<BYTE> markerBytes;
    if (!CoverCaptureSnapshot(marker.get(), markerBefore) ||
        !CoverSameSnapshot(candidate.markerSnapshot, markerBefore) ||
        !CoverReadHandle(marker.get(), CoverMaxManifestBytes, markerBytes))
        return CoverDeleteState::NoChange;
    CoverCacheMetadata exact;
    if (!CoverManifest(markerBytes, candidate.directory, candidate.songId, exact) ||
        CoverPathKey(exact.directory) != CoverPathKey(candidate.directory) ||
        CoverPathKey(exact.marker) != CoverPathKey(candidate.marker) ||
        CoverPathKey(exact.file) != CoverPathKey(candidate.file) ||
        exact.songId != candidate.songId || exact.hash != candidate.hash ||
        exact.imageUrl != candidate.imageUrl || exact.generation != candidate.generation)
        return CoverDeleteState::NoChange;
    CoverFileHandle image(CreateFileW(exact.file.c_str(), GENERIC_READ | DELETE,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (image.get() == INVALID_HANDLE_VALUE) return CoverDeleteState::NoChange;
    CoverFileSnapshot imageBefore, imageAfter;
    std::vector<BYTE> imageBytes;
    std::wstring actualFormat, decodeError;
    uint32_t width = 0, height = 0;
    if (!CoverCaptureSnapshot(image.get(), imageBefore) ||
        !CoverSameSnapshot(candidate.imageSnapshot, imageBefore) ||
        !CoverReadHandle(image.get(), CoverMaxImageBytes, imageBytes) ||
        !CoverCaptureSnapshot(marker.get(), markerAfter) ||
        !CoverCaptureSnapshot(image.get(), imageAfter) ||
        !CoverSameSnapshot(markerBefore, markerAfter) ||
        !CoverSameSnapshot(imageBefore, imageAfter) ||
        markerBefore.bytes > std::numeric_limits<uint64_t>::max() - imageBefore.bytes ||
        markerBefore.bytes + imageBefore.bytes != candidate.bytes ||
        std::max(CoverFileTimeValue(markerBefore.information.ftLastWriteTime),
            CoverFileTimeValue(imageBefore.information.ftLastWriteTime)) != candidate.writtenAt ||
        CoverHash(imageBytes) != exact.hash) return CoverDeleteState::NoChange;
    if (stop.stop_requested()) return CoverDeleteState::NoChange;
    if (!CoverDecode(imageBytes, width, height, actualFormat, decodeError) ||
        exact.file.extension().wstring() != L"." + actualFormat)
        return CoverDeleteState::NoChange;
    if (stop.stop_requested()) return CoverDeleteState::NoChange;
    // Both exact open objects have now passed owner, identity, hash and full
    // decode validation. A partial delete remains bounded failure evidence and
    // is never followed by path-based cleanup.
    if (!detail::DeleteOpenFile(image.get())) return CoverDeleteState::NoChange;
    return detail::DeleteOpenFile(marker.get())
        ? CoverDeleteState::Removed : CoverDeleteState::Partial;
}

bool CoverEnsureCacheRoom(const std::filesystem::path& root, uint64_t incomingBytes,
    const std::set<std::wstring>& protectedFiles, std::stop_token stop, std::wstring& error) {
    if (incomingBytes > CoverCacheQuotaBytes) {
        error = L"封面记录超过本地缓存配额";
        return false;
    }
    size_t attempts = 0;
    for (;;) {
        if (stop.stop_requested()) { error = L"封面获取已取消"; return false; }
        auto usage = CoverMeasureCache(root, stop);
        if (!usage.complete) {
            error = L"无法完整核对封面缓存，未知、损坏或竞争对象已保留并停止写入";
            return false;
        }
        if (usage.bytes <= CoverCacheQuotaBytes - incomingBytes &&
            usage.ownedRecords < CoverCacheRecordLimit) return true;
        std::sort(usage.candidates.begin(), usage.candidates.end(), [](const auto& left, const auto& right) {
            if (left.writtenAt != right.writtenAt) return left.writtenAt < right.writtenAt;
            return CoverPathKey(left.marker) < CoverPathKey(right.marker);
        });
        bool removed = false;
        for (const auto& candidate : usage.candidates) {
            if (stop.stop_requested()) { error = L"封面获取已取消"; return false; }
            if (CoverProtected(candidate, protectedFiles)) continue;
            if (attempts >= CoverMaxPruneAttempts) break;
            ++attempts;
            const auto deletion = CoverDeleteOwnedRecord(candidate, protectedFiles, stop);
            if (deletion == CoverDeleteState::Partial) {
                error = L"封面缓存清理未完整完成，已保留现场并停止继续淘汰";
                return false;
            }
            if (deletion == CoverDeleteState::Removed) {
                removed = true;
                break;
            }
        }
        if (!removed) {
            error = L"封面缓存已满；当前封面、未知文件和失败证据均已保留";
            return false;
        }
    }
}

} // namespace

CoverResult CoverClient::Fetch(const std::wstring& songId, CoverIdOrigin origin,
    CoverFetchOptions options) {
    CoverResult result;
    result.idOrigin = origin;
    auto cancelled = [&] {
        if (!stop_.stop_requested()) return false;
        result.file.clear(); result.diagnostic = L"封面获取已取消";
        return true;
    };
    if (cancelled()) return result;
    uint64_t id = 0;
    if (!CoverNumericId(songId, id)) { result.diagnostic = L"封面歌曲 ID 必须是有效正整数"; return result; }
    result.songId = std::to_wstring(id);
    result.metadataUrl = L"https://music.163.com/api/song/detail?id=" + result.songId + L"&ids=%5B" + result.songId + L"%5D";
    result.source = L"网易云 album.picUrl · " + std::wstring(origin == CoverIdOrigin::VerifiedPlaybackId ?
        L"调用者提供的已证实播放 ID" : L"用户选择的搜索候选 ID");
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) { result.diagnostic = L"封面 WIC 的 COM 初始化失败"; return result; }
    struct CoverComScope { HRESULT result; ~CoverComScope() { if (SUCCEEDED(result)) CoUninitialize(); } } scope{com};
    try {
        std::filesystem::path directory;
        detail::DirectoryTreeLock directoryLock;
        if (cancelled()) return result;
        if (!CoverCacheDirectory(directory, directoryLock, result.diagnostic)) return result;
        std::vector<CoverCacheHit> hits;
        std::vector<std::filesystem::path> available;
        uint64_t maxGeneration = 0;
        if (!CoverScanSong(directory, result, hits, available, maxGeneration, stop_)) {
            cancelled();
            return result;
        }
        if (cancelled()) return result;
        const auto* newest = CoverNewestHit(hits);
        std::optional<CoverCacheHit> fallback;
        if (newest) fallback = *newest;
        if (fallback && !options.forceRevalidate) {
            // The first scan is intentionally lock-free so network refreshes do
            // not hold the cache-wide mutex. Before returning a path, wait for
            // an active writer and repeat the scan inside the write boundary.
            CoverCacheWriteLock cacheReadLock;
            std::wstring readLockError;
            if (!cacheReadLock.Acquire(stop_, readLockError)) {
                if (cancelled()) return result;
                result.file.clear();
                result.refreshPending = true;
                result.diagnostic = std::move(readLockError);
                return result;
            }
            std::vector<CoverCacheHit> currentHits;
            std::vector<std::filesystem::path> currentAvailable;
            uint64_t currentMaxGeneration = 0;
            if (!CoverScanSong(directory, result, currentHits, currentAvailable,
                currentMaxGeneration, stop_)) {
                cancelled();
                return result;
            }
            if (cancelled()) return result;
            newest = CoverNewestHit(currentHits);
            if (newest) {
                result = newest->result;
                result.diagnostic = newest->metadata.directory == directory
                    ? L"已读取校验过的本地封面缓存"
                    : L"已读取校验过的封面恢复副本，旧缓存保持原样";
                return result;
            }
            fallback.reset();
            hits.swap(currentHits);
            available.swap(currentAvailable);
            maxGeneration = currentMaxGeneration;
        }
        bool fallbackRevalidatedUnderWriteLock = false;
        auto onlineFailure = [&](std::wstring diagnostic) {
            if (cancelled()) return result;
            // A fallback discovered before the network request may have been
            // replaced while this Fetch was online. Re-scan under the native
            // write mutex immediately before returning any cached path.
            if (fallback && !fallbackRevalidatedUnderWriteLock) {
                CoverCacheWriteLock validationLock;
                std::wstring validationLockError;
                if (!validationLock.Acquire(stop_, validationLockError)) {
                    if (cancelled()) return result;
                    result.file.clear();
                    result.refreshPending = true;
                    result.diagnostic = L"在线封面刷新未完成；本地缓存未能在写入锁内重新确认，继续保留当前显示";
                    if (!diagnostic.empty()) result.diagnostic += L"：" + diagnostic;
                    if (!validationLockError.empty()) result.diagnostic += L"；" + validationLockError;
                    return result;
                }
                std::vector<CoverCacheHit> currentHits;
                std::vector<std::filesystem::path> currentAvailable;
                uint64_t currentMaxGeneration = 0;
                if (!CoverScanSong(directory, result, currentHits, currentAvailable,
                    currentMaxGeneration, stop_)) {
                    cancelled();
                    return result;
                }
                if (cancelled()) return result;
                const auto* currentNewest = CoverNewestHit(currentHits);
                if (currentNewest) fallback = *currentNewest;
                else fallback.reset();
            }
            if (!fallback) {
                result.diagnostic = std::move(diagnostic);
                return result;
            }
            auto retained = fallback->result;
            retained.refreshPending = true;
            retained.diagnostic = L"在线封面刷新未完成，继续使用已校验的本地封面";
            if (!diagnostic.empty()) retained.diagnostic += L"：" + diagnostic;
            return retained;
        };
        const auto metadata = CoverHttp(L"music.163.com", L"/api/song/detail?id=" + result.songId +
            L"&ids=%5B" + result.songId + L"%5D", CoverMaxMetadataBytes, result.diagnostic, stop_);
        if (cancelled()) return result;
        if (!result.diagnostic.empty()) return onlineFailure(result.diagnostic);
        const auto document = CoverParseJson(metadata, result.diagnostic);
        if (cancelled()) return result;
        if (!result.diagnostic.empty()) return onlineFailure(result.diagnostic);
        if (CoverNumber(document, "code") != 200)
            return onlineFailure(L"网易云歌曲元数据请求被拒绝");
        const auto* songs = CoverField(document, "songs");
        std::wstring image;
        if (songs && songs->is_array()) for (const auto& song : *songs) {
            if (CoverNumber(song, "id") != id) continue;
            const auto* album = CoverField(song, "album");
            if (!album) album = CoverField(song, "al");
            if (album) image = CoverFromUtf8(CoverString(*album, "picUrl"));
            break;
        }
        CoverCdnUrl url;
        if (!CoverTrustedUrl(image, url))
            return onlineFailure(L"歌曲未提供封面或 URL 不在网易云 HTTPS CDN 白名单中");
        result.imageUrl = url.normalized;
        const auto bytes = CoverHttp(url.host, url.path, CoverMaxImageBytes, result.diagnostic, stop_);
        if (cancelled()) return result;
        if (!result.diagnostic.empty()) return onlineFailure(result.diagnostic);
        std::wstring extension;
        if (!CoverDecode(bytes, result.width, result.height, extension, result.diagnostic))
            return onlineFailure(result.diagnostic);
        if (cancelled()) return result;
        const auto hash = CoverHash(bytes);
        if (cancelled()) return result;
        if (hash.empty()) return onlineFailure(L"无法校验封面 SHA-256");

        // Capacity, ownership, replacement and publication form one cache-wide
        // write transaction. Re-scan after acquiring the native mutex so a
        // concurrent Fetch cannot make an earlier song scan or quota decision
        // stale before this request publishes its pair.
        CoverCacheWriteLock cacheWriteLock;
        std::wstring writeLockError;
        if (!cacheWriteLock.Acquire(stop_, writeLockError)) {
            if (cancelled()) return result;
            // The earlier fallback scan predates the writer that currently
            // owns the mutex, so do not return that path as freshly retained.
            // The App keeps its already displayed cover while retrying.
            result.file.clear();
            result.refreshPending = true;
            result.diagnostic = std::move(writeLockError);
            return result;
        }
        std::vector<CoverCacheHit> lockedHits;
        std::vector<std::filesystem::path> lockedAvailable;
        uint64_t lockedMaxGeneration = 0;
        if (!CoverScanSong(directory, result, lockedHits, lockedAvailable, lockedMaxGeneration, stop_)) {
            cancelled();
            return result;
        }
        if (cancelled()) return result;
        const auto* lockedNewest = CoverNewestHit(lockedHits);
        if (lockedNewest) fallback = *lockedNewest;
        else fallback.reset();
        fallbackRevalidatedUnderWriteLock = true;
        hits.swap(lockedHits);
        available.swap(lockedAvailable);
        maxGeneration = lockedMaxGeneration;
        if (fallback && fallback->metadata.hash == hash) {
            result = fallback->result;
            result.imageUrl = url.normalized;
            result.refreshPending = false;
            result.diagnostic = L"在线封面已重新校验，继续使用内容一致的本地缓存";
            if (cancelled()) return result;
            return result;
        }
        if (maxGeneration == std::numeric_limits<uint64_t>::max())
            return onlineFailure(L"封面缓存代次已耗尽，原记录已保留");
        const uint64_t generation = maxGeneration + 1;
        CoverJson record{{"schema", 2}, {"owner", "MusicStageNativePrototype.CoverCache"},
            {"songId", CoverAscii(result.songId)}, {"format", CoverAscii(extension)},
            {"imageUrl", CoverAscii(result.imageUrl)}, {"metadataUrl", CoverAscii(result.metadataUrl)},
            {"sha256", CoverAscii(hash)}, {"generation", generation}};
        const auto serialized = record.dump();
        const std::vector<BYTE> markerBytes(serialized.begin(), serialized.end());
        const uint64_t incomingBytes = static_cast<uint64_t>(bytes.size()) + markerBytes.size();
        const auto imageName = result.songId + L"." + extension;
        const auto markerName = result.songId + L".source.json";

        std::set<std::wstring> protectedFiles;
        if (!options.protectedFile.empty()) protectedFiles.insert(CoverPathKey(options.protectedFile));
        if (fallback) protectedFiles.insert(CoverPathKey(fallback->metadata.file));
        std::filesystem::path destination, file, markerFile;
        if (!available.empty()) {
            destination = available.front();
            file = destination / imageName;
            markerFile = destination / markerName;
            protectedFiles.insert(CoverPathKey(file));
        }
        if (destination.empty()) {
            // Do not discard a validated recovery slot before the global cache
            // has proved that every existing object is complete and owned.
            // The later quota pass repeats this check after the exact delete.
            const auto preflight = CoverMeasureCache(directory, stop_);
            if (!preflight.complete) {
                if (cancelled()) return result;
                return onlineFailure(L"无法完整核对封面缓存，未知、损坏或竞争对象已保留");
            }
            std::vector<CoverCacheMetadata> replaceable;
            for (const auto& hit : hits)
                if (!CoverProtected(hit.metadata, protectedFiles)) replaceable.push_back(hit.metadata);
            std::sort(replaceable.begin(), replaceable.end(), [](const auto& left, const auto& right) {
                if (left.generation != right.generation) return left.generation < right.generation;
                return left.writtenAt < right.writtenAt;
            });
            for (const auto& candidate : replaceable) {
                if (cancelled()) return result;
                auto candidateDirectory = candidate.directory;
                auto candidateFile = candidateDirectory / imageName;
                auto candidateMarker = candidateDirectory / markerName;
                auto candidateProtected = protectedFiles;
                candidateProtected.insert(CoverPathKey(candidateFile));
                const auto deletion = CoverDeleteOwnedRecord(candidate, protectedFiles, stop_);
                if (deletion == CoverDeleteState::Partial)
                    return onlineFailure(L"封面恢复记录清理未完整完成，已保留现场并停止继续替换");
                if (deletion == CoverDeleteState::Removed) {
                    destination.swap(candidateDirectory);
                    file.swap(candidateFile);
                    markerFile.swap(candidateMarker);
                    protectedFiles.swap(candidateProtected);
                    break;
                }
            }
        }
        if (destination.empty() || file.empty() || markerFile.empty())
            return onlineFailure(L"原封面缓存及8个恢复位置均不可替换，旧文件和失败证据已保留");
        if (!CoverEnsureCacheRoom(directory, incomingBytes, protectedFiles, stop_, result.diagnostic)) {
            if (cancelled()) return result;
            return onlineFailure(result.diagnostic);
        }
        if (cancelled()) return result;
        DWORD destinationError = ERROR_SUCCESS;
        detail::DirectoryTreeLock destinationLock;
        if (!destinationLock.Acquire(destination, destinationError, true))
            return onlineFailure(L"封面缓存目录不可用或身份已变化（Windows " +
                std::to_wstring(destinationError) + L"），已停止写入并保留原记录");
        if (cancelled()) return result;
        if (!CoverAtomicCreate(file, bytes, result.diagnostic)) return onlineFailure(result.diagnostic);
        const bool recorded = CoverAtomicCreate(markerFile, markerBytes, result.diagnostic);
        result.file = file;
        result.refreshPending = !recorded;
        if (recorded) result.diagnostic = destination == directory
            ? L"封面已通过完整 WIC 解码和像素校验，写入独立缓存"
            : L"封面已重新获取并校验，原记录保留于原处";
        else result.diagnostic = L"封面文件已保存，但来源记录写入失败；文件作为失败证据保留并将重试";
        cancelled();
        return result;
    } catch (...) {
        if (cancelled()) return result;
        result.file.clear();
        result.diagnostic = L"封面查询、解码或缓存发生异常";
        return result;
    }
}
} // namespace stage
