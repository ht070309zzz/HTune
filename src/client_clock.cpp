#include "client_clock.h"

#include <windows.h>
#include <tlhelp32.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace stage {
namespace {

struct Layout {
    DWORD imageSize;
    uint64_t fileSize;
    uint64_t mtimeUnixSeconds;
    uintptr_t songRoot;
    uintptr_t current;
    uintptr_t total;
    DWORD peTimeDateStamp = 0;
    std::string_view sha256{};
};

struct ModuleInfo {
    uintptr_t base = 0;
    DWORD imageSize = 0;
    DWORD mappedTimeDateStamp = 0;
    uint64_t fileSize = 0;
    uint64_t mtimeUnixSeconds = 0;
    std::string sha256;
};

// Publicly observed cloudmusic.dll layouts. Legacy entries use image/file size
// and last-write second as an initial gate; their PE timestamp only cross-checks
// the opened disk file against the mapped image. A content-catalogued binary
// additionally pins the recorded PE timestamp and uses its exact
// SHA-256 and PE timestamp instead of mutable filesystem last-write time.
// Song token, ranges and temporal behavior are validated before a usable clock.
// Unknown binaries are never scanned.
constexpr std::array KnownLayouts{
    Layout{37257216, 33948160, 1775653816, 32097584, 31705392, 32100920},
    Layout{37257216, 33948160, 1776941630, 32097584, 31705392, 32100920},
    Layout{37281792, 33969664, 1780030708, 32118160, 31725888, 32121496},
    Layout{37515264, 34208256, 1780926412, 32343632, 31947096, 32346968},
    Layout{40964096, 37661696, 1788777282, 35649232, 35206248, 35652568},
    Layout{41054208, 37747192, 1791637600, 35732080, 35288280, 35735416},
    // Same disk contents recorded locally on 2026-10-13 and 2026-10-17.
    // Fixed offsets from the published 41054208/37747192 layout; each sample
    // still has to pass the existing ID, duration and temporal checks.
    Layout{41054208, 37747192, 0, 35732080, 35288280, 35735416, 1789741933,
        "7f667f876caf94bc3dac3b0070d3e495117005fafdcf18b6aaa51eb004edac8c"},
};
constexpr std::array<uintptr_t, 5> SongPointerOffsets{0x10, 0, 0x10, 0x68, 0};

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~ScopedHandle() {
        if (valid()) CloseHandle(handle_);
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    bool valid() const noexcept { return handle_ && handle_ != INVALID_HANDLE_VALUE; }
    HANDLE get() const noexcept { return handle_; }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

bool ValidUserPointer(uintptr_t value) {
    return value >= 0x10000 && value < 0x0000800000000000ULL;
}

std::wstring WindowsError(DWORD code) {
    return L"Windows " + std::to_wstring(code);
}

bool ReadFileBytes(
    HANDLE file,
    uint64_t fileSize,
    uint64_t offset,
    void* destination,
    size_t size,
    DWORD& error) {
    error = ERROR_SUCCESS;
    if (!file || file == INVALID_HANDLE_VALUE || !destination || !size ||
        size > std::numeric_limits<DWORD>::max()) {
        error = ERROR_INVALID_PARAMETER;
        return false;
    }
    const auto size64 = static_cast<uint64_t>(size);
    if (offset > fileSize || size64 > fileSize - offset ||
        offset > static_cast<uint64_t>(std::numeric_limits<LONGLONG>::max())) {
        error = ERROR_HANDLE_EOF;
        return false;
    }

    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) {
        error = GetLastError();
        return false;
    }
    DWORD read = 0;
    const DWORD requested = static_cast<DWORD>(size);
    if (!ReadFile(file, destination, requested, &read, nullptr)) {
        error = GetLastError();
        return false;
    }
    if (read != requested) {
        error = ERROR_HANDLE_EOF;
        return false;
    }
    return true;
}

bool NeedsContentFingerprint(DWORD imageSize, uint64_t fileSize, DWORD peTimestamp) {
    return std::any_of(KnownLayouts.begin(), KnownLayouts.end(), [&](const Layout& layout) {
        return !layout.sha256.empty() && layout.imageSize == imageSize &&
            layout.fileSize == fileSize && layout.peTimeDateStamp == peTimestamp;
    });
}

bool ModuleFileSha256(HANDLE file, uint64_t fileSize, std::string& digest, std::wstring& diagnostic) {
    // Hash only catalogued-size files, not arbitrary process memory. Ordinary
    // file I/O itself is not interruptible; the deadline bounds further reads.
    constexpr uint64_t MaxModuleBytes = 64ULL * 1024 * 1024;
    if (!fileSize || fileSize > MaxModuleBytes) {
        diagnostic = L"cloudmusic.dll 内容指纹文件大小越界"; return false;
    }
    struct HashHandles {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        ~HashHandles() {
            if (hash) BCryptDestroyHash(hash);
            if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        }
    } handles;
    const auto startedAt = MonotonicMs();
    if (BCryptOpenAlgorithmProvider(&handles.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptCreateHash(handles.algorithm, &handles.hash, nullptr, 0, nullptr, 0, 0) < 0) {
        diagnostic = L"无法初始化 cloudmusic.dll SHA-256 内容核验"; return false;
    }
    std::array<unsigned char, 64 * 1024> buffer{};
    for (uint64_t offset = 0; offset < fileSize;) {
        if (MonotonicMs() - startedAt > 500) {
            diagnostic = L"cloudmusic.dll 内容核验超时，等待重新绑定"; return false;
        }
        const auto size = static_cast<size_t>(std::min<uint64_t>(buffer.size(), fileSize - offset));
        DWORD error = ERROR_SUCCESS;
        if (!ReadFileBytes(file, fileSize, offset, buffer.data(), size, error)) {
            diagnostic = L"无法完整读取 cloudmusic.dll 内容指纹（" + WindowsError(error) + L"）";
            return false;
        }
        if (BCryptHashData(handles.hash, buffer.data(), static_cast<ULONG>(size), 0) < 0) {
            diagnostic = L"cloudmusic.dll SHA-256 内容计算失败"; return false;
        }
        offset += size;
    }
    std::array<unsigned char, 32> bytes{};
    if (MonotonicMs() - startedAt > 500 ||
        BCryptFinishHash(handles.hash, bytes.data(), static_cast<ULONG>(bytes.size()), 0) < 0) {
        diagnostic = L"cloudmusic.dll SHA-256 内容核验未完成"; return false;
    }
    constexpr char Hex[] = "0123456789abcdef";
    digest.resize(bytes.size() * 2);
    for (size_t index = 0; index < bytes.size(); ++index) {
        digest[index * 2] = Hex[bytes[index] >> 4];
        digest[index * 2 + 1] = Hex[bytes[index] & 15];
    }
    return true;
}

std::optional<uint64_t> FileTimeToUnixSeconds(const FILETIME& value) {
    constexpr uint64_t TicksPerSecond = 10000000ULL;
    constexpr uint64_t UnixEpochTicks = 11644473600ULL * TicksPerSecond;
    const uint64_t ticks = (static_cast<uint64_t>(value.dwHighDateTime) << 32) |
        static_cast<uint64_t>(value.dwLowDateTime);
    if (ticks < UnixEpochTicks) return std::nullopt;
    return (ticks - UnixEpochTicks) / TicksPerSecond;
}

bool IsReadableProtection(DWORD value) {
    if (value & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const DWORD access = value & 0xff;
    return access == PAGE_READONLY || access == PAGE_READWRITE || access == PAGE_WRITECOPY ||
        access == PAGE_EXECUTE_READ || access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY;
}

bool ReadBytes(HANDLE process, uintptr_t address, void* destination, size_t size) {
    if (!process || !ValidUserPointer(address) || !destination || !size ||
        address > std::numeric_limits<uintptr_t>::max() - size) return false;
    MEMORY_BASIC_INFORMATION memory{};
    if (!VirtualQueryEx(process, reinterpret_cast<const void*>(address), &memory, sizeof(memory)) ||
        memory.State != MEM_COMMIT || !IsReadableProtection(memory.Protect)) return false;
    const auto regionStart = reinterpret_cast<uintptr_t>(memory.BaseAddress);
    // Express the bound as offsets inside the region. Adding RegionSize to a
    // hostile/corrupt base can wrap uintptr_t before the comparison.
    if (address < regionStart || size > memory.RegionSize ||
        address - regionStart > memory.RegionSize - size) return false;
    SIZE_T read = 0;
    return ReadProcessMemory(process, reinterpret_cast<const void*>(address), destination, size, &read) && read == size;
}

template<class T>
bool ReadValue(HANDLE process, uintptr_t address, T& value) {
    return ReadBytes(process, address, &value, sizeof(value));
}

bool ReadModuleBytes(
    HANDLE process,
    uintptr_t moduleBase,
    DWORD imageSize,
    uintptr_t rva,
    void* destination,
    size_t size) {
    if (!process || !ValidUserPointer(moduleBase) || !imageSize || !destination || !size ||
        moduleBase > std::numeric_limits<uintptr_t>::max() - imageSize ||
        rva >= imageSize || size > static_cast<size_t>(imageSize - rva) ||
        moduleBase > std::numeric_limits<uintptr_t>::max() - rva) return false;

    auto cursor = moduleBase + rva;
    auto* output = static_cast<unsigned char*>(destination);
    size_t remaining = size;
    while (remaining) {
        MEMORY_BASIC_INFORMATION memory{};
        if (!VirtualQueryEx(process, reinterpret_cast<const void*>(cursor), &memory, sizeof(memory)) ||
            memory.State != MEM_COMMIT || memory.Type != MEM_IMAGE ||
            reinterpret_cast<uintptr_t>(memory.AllocationBase) != moduleBase ||
            !IsReadableProtection(memory.Protect)) return false;
        const auto regionStart = reinterpret_cast<uintptr_t>(memory.BaseAddress);
        if (cursor < regionStart || memory.RegionSize == 0 ||
            cursor - regionStart >= memory.RegionSize) return false;
        const size_t available = memory.RegionSize - static_cast<size_t>(cursor - regionStart);
        const size_t chunk = std::min(remaining, available);
        SIZE_T read = 0;
        if (!ReadProcessMemory(process, reinterpret_cast<const void*>(cursor), output, chunk, &read) ||
            read != chunk) return false;
        cursor += chunk;
        output += chunk;
        remaining -= chunk;
    }
    return true;
}

template<class T>
bool ReadModuleValue(HANDLE process, uintptr_t moduleBase, DWORD imageSize, uintptr_t rva, T& value) {
    return ReadModuleBytes(process, moduleBase, imageSize, rva, &value, sizeof(value));
}

std::wstring ParseSongToken(const char* bytes, size_t size) {
    if (!bytes || !size) return {};
    size_t length = 0;
    while (length < size && bytes[length]) ++length;
    if (!length || length == size) return {};
    size_t digits = 0;
    while (digits < length && bytes[digits] >= '0' && bytes[digits] <= '9') ++digits;
    if (!digits || digits > 19 || bytes[0] == '0') return {};
    if (digits < length) {
        if (bytes[digits] != '_' || digits + 1 == length) return {};
        for (size_t i = digits + 1; i < length; ++i)
            if (static_cast<unsigned char>(bytes[i]) < 0x21 || static_cast<unsigned char>(bytes[i]) > 0x7e) return {};
    }
    uint64_t id = 0;
    for (size_t i = 0; i < digits; ++i) {
        const unsigned digit = static_cast<unsigned>(bytes[i] - '0');
        if (id > (static_cast<uint64_t>(INT64_MAX) - digit) / 10) return {};
        id = id * 10 + digit;
    }
    return id ? std::to_wstring(id) : std::wstring{};
}

std::wstring ReadSongStringObject(HANDLE process, uintptr_t address) {
    std::array<unsigned char, 32> object{};
    if (!ReadBytes(process, address, object.data(), object.size())) return {};
    uint64_t length = 0, capacity = 0;
    std::memcpy(&length, object.data() + 16, sizeof(length));
    std::memcpy(&capacity, object.data() + 24, sizeof(capacity));
    if (length > 256 || capacity < length || capacity > 0x100000) return {};
    std::array<char, 257> text{};
    if (capacity == 15 && length <= 15) {
        if (object[static_cast<size_t>(length)] != '\0') return {};
        std::memcpy(text.data(), object.data(), static_cast<size_t>(length) + 1);
    } else if (capacity >= 16) {
        uintptr_t pointer = 0;
        std::memcpy(&pointer, object.data(), sizeof(pointer));
        if (!ValidUserPointer(pointer) || !ReadBytes(process, pointer, text.data(), static_cast<size_t>(length) + 1)) return {};
        if (text[static_cast<size_t>(length)] != '\0') return {};
    } else return {};
    return ParseSongToken(text.data(), static_cast<size_t>(length) + 1);
}

std::wstring ReadSongId(HANDLE process, uintptr_t moduleBase, DWORD imageSize, uintptr_t rootOffset) {
    uintptr_t address = 0;
    if (!ReadModuleValue(process, moduleBase, imageSize, rootOffset, address) || !ValidUserPointer(address)) return {};
    for (size_t index = 0; index + 1 < SongPointerOffsets.size(); ++index) {
        const uintptr_t slot = address + SongPointerOffsets[index];
        if (index + 2 == SongPointerOffsets.size() && SongPointerOffsets.back() == 0) {
            if (auto value = ReadSongStringObject(process, slot); !value.empty()) return value;
        }
        if (!ReadValue(process, slot, address) || !ValidUserPointer(address)) return {};
    }
    std::array<char, 64> raw{};
    const uintptr_t leaf = address + SongPointerOffsets.back();
    if (!ReadBytes(process, leaf, raw.data(), raw.size())) return {};
    return ParseSongToken(raw.data(), raw.size());
}

std::optional<ModuleInfo> CloudMusicModule(HANDLE process, DWORD pid, std::wstring& diagnostic) {
    ScopedHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
    if (!snapshot.valid()) {
        diagnostic = L"无法读取网易云模块清单（" + WindowsError(GetLastError()) + L"）";
        return std::nullopt;
    }
    MODULEENTRY32W module{sizeof(module)};
    std::optional<ModuleInfo> result;
    if (!Module32FirstW(snapshot.get(), &module)) {
        diagnostic = L"无法枚举网易云模块清单（" + WindowsError(GetLastError()) + L"）";
        return std::nullopt;
    }
    do {
        std::wstring name = module.szModule;
        std::transform(name.begin(), name.end(), name.begin(), towlower);
        if (name != L"cloudmusic.dll") continue;

        const auto moduleBase = reinterpret_cast<uintptr_t>(module.modBaseAddr);
        MEMORY_BASIC_INFORMATION memory{};
        if (!moduleBase || !module.modBaseSize ||
            !VirtualQueryEx(process, module.modBaseAddr, &memory, sizeof(memory)) ||
            memory.State != MEM_COMMIT || memory.Type != MEM_IMAGE ||
            reinterpret_cast<uintptr_t>(memory.AllocationBase) != moduleBase) {
            diagnostic = L"已定位 cloudmusic.dll，但目标进程中的模块基址不是有效映像";
            break;
        }

        IMAGE_DOS_HEADER dos{};
        if (!ReadModuleValue(process, moduleBase, module.modBaseSize, 0, dos) ||
            dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < static_cast<LONG>(sizeof(dos))) {
            diagnostic = L"已定位 cloudmusic.dll，但目标进程中的 DOS 头无效";
            break;
        }
        const auto ntRva = static_cast<uintptr_t>(dos.e_lfanew);
        IMAGE_NT_HEADERS64 mappedNt{};
        if (!ReadModuleValue(process, moduleBase, module.modBaseSize, ntRva, mappedNt) ||
            mappedNt.Signature != IMAGE_NT_SIGNATURE ||
            mappedNt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            mappedNt.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) ||
            mappedNt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            diagnostic = L"已定位 cloudmusic.dll，但目标进程中的 PE64/AMD64 头无效";
            break;
        }
        if (mappedNt.OptionalHeader.SizeOfImage != module.modBaseSize) {
            diagnostic = L"已定位 cloudmusic.dll，但内存 PE64 映像大小与模块清单不一致";
            break;
        }

        ScopedHandle file(CreateFileW(
            module.szExePath,
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr));
        if (!file.valid()) {
            diagnostic = L"已验证内存中的 cloudmusic.dll，但无法打开对应磁盘文件（" +
                WindowsError(GetLastError()) + L"）";
            break;
        }

        LARGE_INTEGER diskSize{};
        if (!GetFileSizeEx(file.get(), &diskSize)) {
            diagnostic = L"已打开磁盘 cloudmusic.dll，但无法读取文件大小（" +
                WindowsError(GetLastError()) + L"）";
            break;
        }
        if (diskSize.QuadPart < 0) {
            diagnostic = L"磁盘 cloudmusic.dll 报告了无效的负文件大小";
            break;
        }
        const uint64_t fileSize = static_cast<uint64_t>(diskSize.QuadPart);
        const bool contentRequired = NeedsContentFingerprint(
            module.modBaseSize, fileSize, mappedNt.FileHeader.TimeDateStamp);
        FILETIME lastWrite{};
        if (!GetFileTime(file.get(), nullptr, nullptr, &lastWrite) && !contentRequired) {
            diagnostic = L"已打开磁盘 cloudmusic.dll，但无法读取最后写入时间（" +
                WindowsError(GetLastError()) + L"）";
            break;
        }
        const auto mtimeUnixSeconds = FileTimeToUnixSeconds(lastWrite);
        if (!mtimeUnixSeconds && !contentRequired) {
            diagnostic = L"磁盘 cloudmusic.dll 的最后写入时间早于 Unix 纪元，无法作为布局指纹";
            break;
        }

        DWORD readError = ERROR_SUCCESS;
        IMAGE_DOS_HEADER diskDos{};
        if (!ReadFileBytes(file.get(), fileSize, 0, &diskDos, sizeof(diskDos), readError)) {
            diagnostic = L"无法完整读取磁盘 cloudmusic.dll 的 DOS 头（" +
                WindowsError(readError) + L"）";
            break;
        }
        if (diskDos.e_magic != IMAGE_DOS_SIGNATURE ||
            diskDos.e_lfanew < static_cast<LONG>(sizeof(diskDos))) {
            diagnostic = L"磁盘 cloudmusic.dll 的 DOS 头无效";
            break;
        }

        IMAGE_NT_HEADERS64 diskNt{};
        const auto diskNtOffset = static_cast<uint64_t>(diskDos.e_lfanew);
        if (!ReadFileBytes(
                file.get(), fileSize, diskNtOffset, &diskNt, sizeof(diskNt), readError)) {
            diagnostic = L"无法完整读取磁盘 cloudmusic.dll 的 PE64 头（" +
                WindowsError(readError) + L"）";
            break;
        }
        if (diskNt.Signature != IMAGE_NT_SIGNATURE ||
            diskNt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            diskNt.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) ||
            diskNt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            diagnostic = L"磁盘 cloudmusic.dll 的 PE64/AMD64 头无效";
            break;
        }
        // Comparing these fields rejects an obvious path replacement after the
        // target loaded its image. Equality is only a consistency check, not a
        // known release timestamp or proof of file identity.
        if (diskNt.FileHeader.TimeDateStamp != mappedNt.FileHeader.TimeDateStamp) {
            diagnostic = L"磁盘 cloudmusic.dll 的 PE 时间戳与进程已映射映像不一致（disk=" +
                std::to_wstring(diskNt.FileHeader.TimeDateStamp) + L"，mapped=" +
                std::to_wstring(mappedNt.FileHeader.TimeDateStamp) + L"），拒绝套用布局";
            break;
        }
        if (diskNt.OptionalHeader.SizeOfImage != mappedNt.OptionalHeader.SizeOfImage ||
            diskNt.OptionalHeader.SizeOfImage != module.modBaseSize) {
            diagnostic = L"磁盘 cloudmusic.dll 的 PE64 映像大小与进程已映射映像不一致";
            break;
        }

        std::string contentHash;
        if (contentRequired &&
            !ModuleFileSha256(file.get(), fileSize, contentHash, diagnostic)) break;
        result = ModuleInfo{
            moduleBase,
            module.modBaseSize,
            mappedNt.FileHeader.TimeDateStamp,
            fileSize,
            mtimeUnixSeconds.value_or(0), std::move(contentHash)};
        break;
    } while (Module32NextW(snapshot.get(), &module));
    if (!result && diagnostic.empty()) {
        const DWORD error = GetLastError();
        diagnostic = error == ERROR_NO_MORE_FILES
            ? L"网易云进程中没有可识别的 cloudmusic.dll"
            : L"网易云模块枚举中断（" + WindowsError(error) + L"）";
    }
    return result;
}

const Layout* FindLayout(const ModuleInfo& module) {
    if (NeedsContentFingerprint(module.imageSize, module.fileSize, module.mappedTimeDateStamp)) {
        for (const auto& layout : KnownLayouts)
            if (!layout.sha256.empty() && layout.imageSize == module.imageSize &&
                layout.fileSize == module.fileSize && layout.peTimeDateStamp == module.mappedTimeDateStamp &&
                layout.sha256 == module.sha256) return &layout;
        return nullptr; // Required content verification must never fall back to mtime.
    }
    for (const auto& layout : KnownLayouts)
        if (layout.sha256.empty() && layout.imageSize == module.imageSize &&
            layout.fileSize == module.fileSize &&
            layout.mtimeUnixSeconds == module.mtimeUnixSeconds) return &layout;
    return nullptr;
}

bool DurationConsistent(int64_t first, int64_t second) {
    const int64_t allowance = std::max<int64_t>(1000, std::max(first, second) / 200);
    return std::abs(first - second) <= allowance;
}

} // namespace

struct ClientClockReader::Impl {
    DWORD pid = 0;
    HANDLE process = nullptr;
    uintptr_t moduleBase = 0;
    const Layout* layout = nullptr;
    bool previousValid = false;
    std::wstring previousSongId;
    int64_t previousPositionMs = 0;
    int64_t previousDurationMs = 0;
    int64_t previousAtMs = 0;
    int64_t lastMovementAtMs=0;
    int64_t movementAnchorAtMs=0,movementAnchorPositionMs=0;
    PlayState previousState=PlayState::Unknown;
    DWORD retryPid = 0;
    uint64_t expectedCreatedAtTicks = 0;
    int64_t retryAtMs = 0;
    std::wstring retryDiagnostic;
    DWORD moduleImageSize = 0;
    DWORD moduleMappedTimeDateStamp = 0;
    uint64_t moduleFileSize = 0;
    uint64_t moduleMtimeUnixSeconds = 0;
    int64_t moduleRecheckAtMs = 0;
    unsigned consecutiveReadFailures = 0;
    unsigned moduleProbeFailures = 0;
    unsigned processAttachFailures = 0;

    static int64_t NoteBoundedFailure(unsigned& failures) noexcept {
        if (failures < std::numeric_limits<unsigned>::max()) ++failures;
        if (failures <= 3) return 250;
        if (failures <= 5) return 1000;
        return 5000;
    }

    int64_t NoteModuleProbeFailure() noexcept {
        return NoteBoundedFailure(moduleProbeFailures);
    }

    void ClearModuleProbeFailures() noexcept { moduleProbeFailures = 0; }
    int64_t NoteProcessAttachFailure() noexcept {
        return NoteBoundedFailure(processAttachFailures);
    }
    void ClearProcessAttachFailures() noexcept { processAttachFailures = 0; }

    void ReleaseActive() {
        if (process) CloseHandle(process);
        pid = 0; process = nullptr; moduleBase = 0; layout = nullptr;
        moduleImageSize=0;moduleMappedTimeDateStamp=0;moduleFileSize=0;moduleMtimeUnixSeconds=0;
        moduleRecheckAtMs=0;consecutiveReadFailures=0;
        previousValid = false; previousSongId.clear(); previousPositionMs = 0;
        previousDurationMs = 0; previousAtMs = 0;lastMovementAtMs=0;
        movementAnchorAtMs=0;movementAnchorPositionMs=0;previousState=PlayState::Unknown;
    }

    void Defer(DWORD failedPid, int64_t delayMs, std::wstring diagnostic) {
        ReleaseActive();
        retryPid = failedPid;
        retryAtMs = MonotonicMs() + delayMs;
        retryDiagnostic = std::move(diagnostic);
    }

    void Reset() {
        ReleaseActive();
        ClearModuleProbeFailures();
        ClearProcessAttachFailures();
        expectedCreatedAtTicks = 0;
        retryPid = 0; retryAtMs = 0; retryDiagnostic.clear();
    }

    ~Impl() { Reset(); }
};

ClientClockReader::ClientClockReader() : impl_(std::make_unique<Impl>()) {}
ClientClockReader::~ClientClockReader() = default;
void ClientClockReader::Reset() { impl_->Reset(); }

ClientClockObservation ClientClockReader::Observe(DWORD pid, PlayState state, uint64_t processCreatedAtTicks) {
    ClientClockObservation result;
    result.source = L"网易云内部时钟（只读）";
    if (!pid) { impl_->Reset(); result.diagnostic = L"没有网易云进程可供只读时钟关联"; return result; }
    if (!processCreatedAtTicks) { impl_->Reset(); result.diagnostic = L"缺少已核验的进程创建标记，不按旧PID读取时钟"; return result; }
    if (impl_->expectedCreatedAtTicks != processCreatedAtTicks) {
        impl_->Reset(); impl_->expectedCreatedAtTicks = processCreatedAtTicks;
    }
    const auto now = MonotonicMs();
    if (impl_->retryPid == pid && now < impl_->retryAtMs) {
        result.diagnostic = impl_->retryDiagnostic + L"；已暂停重复探测，将稍后重试";
        return result;
    }
    if (impl_->retryPid != pid || now >= impl_->retryAtMs) {
        impl_->retryPid = 0;
        impl_->retryAtMs = 0;
        impl_->retryDiagnostic.clear();
    }
    if (impl_->pid != pid || !impl_->process) {
        impl_->ReleaseActive();
        impl_->pid = pid;
        // VirtualQueryEx requires PROCESS_QUERY_INFORMATION by contract. This
        // remains a read-only handle: no write, operation, thread, or injection
        // rights are requested.
        impl_->process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
        if (!impl_->process) {
            result.diagnostic = L"只读时钟无法打开网易云进程（" + WindowsError(GetLastError()) + L"）";
            impl_->Defer(pid, impl_->NoteProcessAttachFailure(), result.diagnostic); return result;
        }
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!GetProcessTimes(impl_->process, &created, &exited, &kernel, &user)) {
            result.diagnostic = L"原生时钟无法复核所打开进程的创建标记";
            impl_->Defer(pid, impl_->NoteProcessAttachFailure(), result.diagnostic); return result;
        }
        impl_->ClearProcessAttachFailures();
        const auto openedCreatedAt = (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        if (openedCreatedAt != processCreatedAtTicks) {
            result.diagnostic = L"所打开PID已是不同进程实例，等待监视器重新绑定";
            impl_->Defer(pid, 250, result.diagnostic); return result;
        }
        std::wstring moduleDiagnostic;
        const auto module = CloudMusicModule(impl_->process, pid, moduleDiagnostic);
        if (!module) {
            result.diagnostic = moduleDiagnostic;
            impl_->Defer(pid, impl_->NoteModuleProbeFailure(), result.diagnostic); return result;
        }
        impl_->ClearModuleProbeFailures();
        impl_->moduleBase = module->base;
        impl_->layout = FindLayout(*module);
        if (!impl_->layout) {
            result.diagnostic = L"cloudmusic.dll 指纹未收录（image=" + std::to_wstring(module->imageSize) +
                L"，file=" + std::to_wstring(module->fileSize) +
                L"，mtime=" + std::to_wstring(module->mtimeUnixSeconds) +
                L"），为避免误读已停用内部时钟；mappedPE=" +
                std::to_wstring(module->mappedTimeDateStamp) +
                L"；内容收录项还须SHA-256完全一致，旧收录项须修改时间一致";
            if (!moduleDiagnostic.empty()) result.diagnostic += L"；" + moduleDiagnostic;
            // Keep a long but finite retry so a late-loaded or replaced module
            // can recover without reopening and enumerating it four times per
            // second.
            impl_->Defer(pid, 60000, result.diagnostic); return result;
        }
        impl_->moduleImageSize=module->imageSize;
        impl_->moduleMappedTimeDateStamp=module->mappedTimeDateStamp;
        impl_->moduleFileSize=module->fileSize;
        impl_->moduleMtimeUnixSeconds=module->mtimeUnixSeconds;
        impl_->moduleRecheckAtMs=now+30000;
    }
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(impl_->process, &exitCode) || exitCode != STILL_ACTIVE) {
        result.diagnostic = L"网易云进程已退出或无法继续读取";
        impl_->Defer(pid, 1000, result.diagnostic); return result;
    }
    if(now>=impl_->moduleRecheckAtMs){
        std::wstring moduleDiagnostic;const auto module=CloudMusicModule(impl_->process,pid,moduleDiagnostic);
        if(!module){
            result.diagnostic=moduleDiagnostic+L"；正在重新绑定只读布局";
            impl_->Defer(pid,impl_->NoteModuleProbeFailure(),result.diagnostic);return result;
        }
        impl_->ClearModuleProbeFailures();
        if(module->base!=impl_->moduleBase||module->imageSize!=impl_->moduleImageSize||
           module->mappedTimeDateStamp!=impl_->moduleMappedTimeDateStamp||
           module->fileSize!=impl_->moduleFileSize||
           (impl_->layout->sha256.empty()&&module->mtimeUnixSeconds!=impl_->moduleMtimeUnixSeconds)||
           FindLayout(*module)!=impl_->layout){
            result.diagnostic=moduleDiagnostic.empty() ? L"cloudmusic.dll 已在同一进程内变化，正在重新绑定只读布局"
                : moduleDiagnostic+L"；正在重新绑定只读布局";
            impl_->Defer(pid,500,result.diagnostic);return result;
        }
        impl_->moduleRecheckAtMs=now+30000;
    }
    const auto failedSample=[&](std::wstring diagnostic){
        impl_->previousValid=false;impl_->lastMovementAtMs=0;impl_->movementAnchorAtMs=0;
        if(++impl_->consecutiveReadFailures>=3){
            diagnostic+=L"；连续失败，释放旧模块基址并重新枚举";
            impl_->Defer(pid,100,diagnostic);
        }
        result.diagnostic=std::move(diagnostic);return result;
    };
    const auto readStartedAt=MonotonicMs();
    const auto initialSongId=ReadSongId(
        impl_->process,impl_->moduleBase,impl_->moduleImageSize,impl_->layout->songRoot);
    double currentSeconds = 0, totalSeconds = 0;
    if (!ReadModuleValue(
            impl_->process,impl_->moduleBase,impl_->moduleImageSize,impl_->layout->current,currentSeconds) ||
        !ReadModuleValue(
            impl_->process,impl_->moduleBase,impl_->moduleImageSize,impl_->layout->total,totalSeconds)) {
        return failedSample(L"已识别版本，但内部时钟地址当前不可读");
    }
    const auto songId = ReadSongId(
        impl_->process,impl_->moduleBase,impl_->moduleImageSize,impl_->layout->songRoot);
    if (!SameSongId(initialSongId,songId)||MonotonicMs()-readStartedAt>100||!std::isfinite(currentSeconds) || !std::isfinite(totalSeconds) ||
        totalSeconds < 1 || totalSeconds > 24 * 3600 || currentSeconds < -0.5 || currentSeconds > totalSeconds + 3) {
        return failedSample(L"内部时钟的歌曲标识或数值范围未通过校验");
    }
    impl_->consecutiveReadFailures=0;
    result.candidate = true;
    result.songId = songId;
    result.positionMs = std::max<int64_t>(0, std::llround(currentSeconds * 1000.0));
    result.durationMs = std::max<int64_t>(1000, std::llround(totalSeconds * 1000.0));
    result.sampledAtMs = MonotonicMs();

    if (impl_->previousValid && SameSongId(impl_->previousSongId,result.songId) &&
        DurationConsistent(impl_->previousDurationMs, result.durationMs)) {
        const int64_t elapsed = std::max<int64_t>(1, result.sampledAtMs - impl_->previousAtMs);
        const int64_t advance = result.positionMs - impl_->previousPositionMs;
        bool temporal = false;
        if (state == PlayState::Playing) temporal = advance >= -250 && advance <= elapsed + 1500 && std::abs(advance - elapsed) <= 1500;
        else if (state == PlayState::Paused || state == PlayState::Stopped) temporal = std::abs(advance) <= 500;
        else temporal = advance >= -250 && advance <= elapsed + 1500;
        result.stable=temporal&&elapsed<=750;
        if(!result.stable||state!=impl_->previousState){impl_->lastMovementAtMs=0;impl_->movementAnchorAtMs=0;}
        else if(impl_->movementAnchorAtMs>0&&result.sampledAtMs-impl_->movementAnchorAtMs>=100){
            // Accumulate a real interval, independent of the sampling period.
            // At 33 ms, per-read thresholds would never confirm forward motion.
            const auto interval=result.sampledAtMs-impl_->movementAnchorAtMs;
            const auto movement=result.positionMs-impl_->movementAnchorPositionMs;
            if(state==PlayState::Playing&&interval<=750&&movement>25&&movement>=interval/2&&std::abs(movement-interval)<=250)
                impl_->lastMovementAtMs=result.sampledAtMs;
            else impl_->lastMovementAtMs=0;
            impl_->movementAnchorAtMs=result.sampledAtMs;impl_->movementAnchorPositionMs=result.positionMs;
        }
    }else{impl_->lastMovementAtMs=0;impl_->movementAnchorAtMs=0;}
    if(!impl_->movementAnchorAtMs){impl_->movementAnchorAtMs=result.sampledAtMs;impl_->movementAnchorPositionMs=result.positionMs;}
    result.advanceAllowed=result.stable&&state==PlayState::Playing&&impl_->lastMovementAtMs>0&&
        result.sampledAtMs-impl_->lastMovementAtMs<=750;
    result.diagnostic = result.stable
        ? L"版本指纹、歌曲 ID、范围与连续样本一致；尚需实际同步误差验收"
        : L"内部时钟取得候选值，等待下一份连续样本确认（拖动或切歌后会重新确认）";
    impl_->previousValid = true;
    impl_->previousSongId = result.songId;
    impl_->previousPositionMs = result.positionMs;
    impl_->previousDurationMs = result.durationMs;
    impl_->previousAtMs = result.sampledAtMs;
    impl_->previousState=state;
    return result;
}

} // namespace stage
