#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <limits>
#include <vector>

namespace stage::detail {

// Keeps every verified directory component open without write/delete sharing.
// This rejects reparse traversal and prevents the checked directory chain from
// being renamed or converted in place during a path-based transaction.
class DirectoryTreeLock {
public:
    using HandleValidator = bool (*)(HANDLE, const void*, DWORD&) noexcept;

    DirectoryTreeLock() = default;
    ~DirectoryTreeLock() { Reset(); }
    DirectoryTreeLock(const DirectoryTreeLock&) = delete;
    DirectoryTreeLock& operator=(const DirectoryTreeLock&) = delete;

    bool Acquire(const std::filesystem::path& rawDirectory, DWORD& error,
        bool createMissing = false, HandleValidator validator = nullptr,
        const void* validatorContext = nullptr) {
        Reset();
        error = ERROR_SUCCESS;
        std::error_code pathError;
        const auto directory = std::filesystem::absolute(rawDirectory, pathError).lexically_normal();
        if (pathError || directory.empty() || !directory.is_absolute()) {
            error = ERROR_BAD_PATHNAME;
            return false;
        }
        const auto rootName = directory.root_name().wstring();
        const auto root = directory.root_path().wstring();
        if (rootName.size() != 2 || rootName[1] != L':' || root.size() < 3) {
            error = ERROR_BAD_NETPATH;
            return false;
        }
        const auto driveType = GetDriveTypeW(root.c_str());
        if (driveType != DRIVE_FIXED && driveType != DRIVE_REMOVABLE && driveType != DRIVE_RAMDISK) {
            error = ERROR_BAD_NETPATH;
            return false;
        }

        std::filesystem::path current;
        for (const auto& component : directory) {
            current /= component;
            // A drive designator ("C:") is not openable by itself; the root
            // directory ("C:\\") is. Keep the root handle too so callers
            // exporting directly to a removable-drive root still receive a
            // real final-directory identity rather than an empty lock.
            if (current == directory.root_name()) continue;
            const DWORD openFlags = FILE_FLAG_BACKUP_SEMANTICS |
                FILE_FLAG_OPEN_REPARSE_POINT |
                (validator ? FILE_FLAG_OPEN_NO_RECALL : 0);
            HANDLE handle = CreateFileW(current.c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                openFlags, nullptr);
            if (handle == INVALID_HANDLE_VALUE && createMissing) {
                const auto openError = GetLastError();
                if (openError == ERROR_FILE_NOT_FOUND || openError == ERROR_PATH_NOT_FOUND) {
                    if (!CreateDirectoryW(current.c_str(), nullptr)) {
                        const auto createError = GetLastError();
                        if (createError != ERROR_ALREADY_EXISTS) {
                            error = createError;
                            Reset();
                            return false;
                        }
                    }
                    handle = CreateFileW(current.c_str(), FILE_READ_ATTRIBUTES,
                        FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                        openFlags, nullptr);
                }
            }
            if (handle == INVALID_HANDLE_VALUE) {
                error = GetLastError();
                Reset();
                return false;
            }
            BY_HANDLE_FILE_INFORMATION information{};
            if (!GetFileInformationByHandle(handle, &information)) {
                error = GetLastError();
                CloseHandle(handle);
                Reset();
                return false;
            }
            if (!(information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                error = (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                    ? ERROR_REPARSE_TAG_INVALID : ERROR_DIRECTORY;
                CloseHandle(handle);
                Reset();
                return false;
            }
            if (validator && !validator(handle, validatorContext, error)) {
                if (error == ERROR_SUCCESS) error = ERROR_ACCESS_DENIED;
                CloseHandle(handle);
                Reset();
                return false;
            }
            try {
                handles_.push_back(handle);
            } catch (...) {
                CloseHandle(handle);
                Reset();
                throw;
            }
        }
        return true;
    }

    HANDLE FinalHandle() const noexcept {
        return handles_.empty() ? INVALID_HANDLE_VALUE : handles_.back();
    }

private:
    void Reset() noexcept {
        for (const auto handle : handles_)
            if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        handles_.clear();
    }
    std::vector<HANDLE> handles_;
};

enum class FileObjectComparison { Same, Different, Unavailable };

inline FileObjectComparison CompareFileObjects(HANDLE left, HANDLE right) noexcept {
    if (!left || left == INVALID_HANDLE_VALUE || !right || right == INVALID_HANDLE_VALUE)
        return FileObjectComparison::Unavailable;
    FILE_ID_INFO leftId{}, rightId{};
    const bool leftExtended = GetFileInformationByHandleEx(
        left, FileIdInfo, &leftId, sizeof(leftId)) != FALSE;
    const bool rightExtended = GetFileInformationByHandleEx(
        right, FileIdInfo, &rightId, sizeof(rightId)) != FALSE;
    if (leftExtended && rightExtended) {
        return leftId.VolumeSerialNumber == rightId.VolumeSerialNumber &&
            std::memcmp(leftId.FileId.Identifier, rightId.FileId.Identifier,
                sizeof(leftId.FileId.Identifier)) == 0
            ? FileObjectComparison::Same : FileObjectComparison::Different;
    }
    // A one-sided extended identity failure cannot be downgraded safely. In
    // particular, the legacy 64-bit index is not guaranteed to identify a
    // ReFS object uniquely. Callers use Unavailable to retain data and stop a
    // replace/delete/protection decision instead of guessing.
    if (leftExtended != rightExtended) return FileObjectComparison::Unavailable;
    // FileIdInfo is not implemented by every local filesystem driver. The
    // older stable volume/file-index identity remains available on ordinary
    // Win32 disk handles and keeps protection decisions fail-closed.
    BY_HANDLE_FILE_INFORMATION leftLegacy{}, rightLegacy{};
    if (!GetFileInformationByHandle(left, &leftLegacy) ||
        !GetFileInformationByHandle(right, &rightLegacy))
        return FileObjectComparison::Unavailable;
    return leftLegacy.dwVolumeSerialNumber == rightLegacy.dwVolumeSerialNumber &&
        leftLegacy.nFileIndexHigh == rightLegacy.nFileIndexHigh &&
        leftLegacy.nFileIndexLow == rightLegacy.nFileIndexLow
        ? FileObjectComparison::Same : FileObjectComparison::Different;
}

inline bool SameFileObject(HANDLE left, HANDLE right) noexcept {
    return CompareFileObjects(left, right) == FileObjectComparison::Same;
}

// Rename the exact object represented by an open DELETE handle. Unlike a
// source-path MoveFile/DeleteFile pair, another process cannot substitute a
// different temporary object between close and commit/cleanup.
inline bool RenameOpenFile(HANDLE file, const std::filesystem::path& target,
    bool replaceExisting, DWORD& error) {
    error = ERROR_SUCCESS;
    if (!file || file == INVALID_HANDLE_VALUE) {
        error = ERROR_INVALID_HANDLE;
        return false;
    }
    const auto name = target.wstring();
    constexpr size_t prefix = offsetof(FILE_RENAME_INFO, FileName);
    if (name.empty() || name.find(L'\0') != std::wstring::npos) {
        error = ERROR_INVALID_NAME;
        return false;
    }
    if (name.size() >
        (std::numeric_limits<DWORD>::max() - prefix - sizeof(wchar_t)) / sizeof(wchar_t)) {
        error = ERROR_FILENAME_EXCED_RANGE;
        return false;
    }
    // FileName is consumed as a NUL-terminated Win32 path before the native
    // counted rename is issued. Reserve and initialize the terminator even
    // though FileNameLength itself excludes it. Without it this API can read
    // beyond our buffer, reject a valid path or rename to a garbage suffix.
    const size_t bytes = prefix + (name.size() + 1) * sizeof(wchar_t);
    std::vector<std::max_align_t> storage(
        (bytes + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
    auto* information = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    ZeroMemory(information, bytes);
    information->ReplaceIfExists = replaceExisting ? TRUE : FALSE;
    information->RootDirectory = nullptr;
    information->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
    std::memcpy(information->FileName, name.data(), information->FileNameLength);
    if (!SetFileInformationByHandle(file, FileRenameInfo, information, static_cast<DWORD>(bytes))) {
        error = GetLastError();
        return false;
    }
    return true;
}

inline bool DeleteOpenFile(HANDLE file, DWORD* error = nullptr) noexcept {
    if (error) *error = ERROR_SUCCESS;
    if (!file || file == INVALID_HANDLE_VALUE) {
        if (error) *error = ERROR_INVALID_HANDLE;
        return false;
    }
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (!SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition))) {
        if (error) *error = GetLastError();
        return false;
    }
    return true;
}

} // namespace stage::detail
