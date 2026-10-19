#include "desktop_host.h"
#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <iterator>
#include <utility>
#include <vector>

#pragma comment(lib, "User32.lib")

// Original implementation; no third-party source code copied.
// Official SetParent/DPI semantics:
// https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setparent
// https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-aredpiawarenesscontextsequal
// https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setlayeredwindowattributes
// Undocumented 24H2 topology is informed by Windows-shell engineering discussion:
// https://github.com/lively-community/lively/issues/2074
// That discussion is topology evidence, not a stable/public Windows contract.

namespace stage {
namespace {
struct ExplorerIdentity {
    DWORD pid = 0;
    uint64_t created = 0;
    HWND progman = nullptr;
};
struct Target {
    ExplorerIdentity explorer;
    HWND parent = nullptr, icons = nullptr, background = nullptr, iconRoot = nullptr;
    bool layered = false;
    DPI_AWARENESS_CONTEXT dpi = nullptr;
    std::wstring layout;
    bool backgroundRequested = false;
};
class DpiScope {
public:
    explicit DpiScope(DPI_AWARENESS_CONTEXT context) {
        if (context) previous_ = SetThreadDpiAwarenessContext(context);
    }
    ~DpiScope() { if (previous_) SetThreadDpiAwarenessContext(previous_); }
    DpiScope(const DpiScope&) = delete;
    DpiScope& operator=(const DpiScope&) = delete;
private:
    DPI_AWARENESS_CONTEXT previous_ = nullptr;
};
std::wstring ClassName(HWND window) {
    wchar_t name[128]{};
    if (GetClassNameW(window, name, static_cast<int>(std::size(name))) == 0) return {};
    return name;
}
DWORD ProcessId(HWND window) {
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    return pid;
}
std::wstring HandleText(HWND window) {
    wchar_t text[32]{};
    swprintf_s(text, L"%p", static_cast<void*>(window));
    return text;
}
std::wstring ErrorText(const wchar_t* action, DWORD error) {
    wchar_t text[32]{};
    swprintf_s(text, L"0x%08X", error);
    return std::wstring(action) + L" · Win32=" + text;
}
const wchar_t* DpiName(DPI_AWARENESS_CONTEXT context) {
    if (!context) return L"unknown";
    if (AreDpiAwarenessContextsEqual(context, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        return L"per-monitor-v2";
    if (AreDpiAwarenessContextsEqual(context, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE))
        return L"per-monitor-v1";
    if (AreDpiAwarenessContextsEqual(context, DPI_AWARENESS_CONTEXT_SYSTEM_AWARE))
        return L"system-aware";
    if (AreDpiAwarenessContextsEqual(context, DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED))
        return L"unaware-gdi-scaled";
    return L"unaware";
}
bool ReadExplorer(ExplorerIdentity& identity, std::wstring& diagnostic) {
    HWND shell = GetShellWindow();
    if (!shell) { diagnostic = L"Explorer桌面尚未就绪，保留窗口模式"; return false; }
    const DWORD pid = ProcessId(shell);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        diagnostic = ErrorText(L"无法核对Explorer身份", GetLastError());
        return false;
    }
    wchar_t path[32768]{};
    DWORD length = static_cast<DWORD>(std::size(path));
    FILETIME created{}, exit{}, kernel{}, user{};
    const bool read = QueryFullProcessImageNameW(process, 0, path, &length) &&
        GetProcessTimes(process, &created, &exit, &kernel, &user);
    const DWORD error = read ? ERROR_SUCCESS : GetLastError();
    CloseHandle(process);
    if (!read) { diagnostic = ErrorText(L"无法核对Explorer进程", error); return false; }
    const wchar_t* file = wcsrchr(path, L'\\');
    file = file ? file + 1 : path;
    if (_wcsicmp(file, L"explorer.exe") != 0) {
        diagnostic = L"当前桌面宿主不是Windows Explorer，保留窗口模式";
        return false;
    }
    HWND progman = ClassName(shell) == L"Progman" ? shell : FindWindowW(L"Progman", nullptr);
    if (!progman || ProcessId(progman) != pid || ClassName(progman) != L"Progman") {
        diagnostic = L"没有找到同一Explorer进程的合法Progman桌面";
        return false;
    }
    identity = {pid, (uint64_t{created.dwHighDateTime} << 32) | created.dwLowDateTime, progman};
    return true;
}
bool SameExplorer(const ExplorerIdentity& left, const ExplorerIdentity& right) {
    return left.pid == right.pid && left.created == right.created &&
        left.progman == right.progman;
}
bool BehindSibling(HWND lower, HWND upper) {
    // GA_PARENT excludes top-level owners, unlike GetParent.
    if (!lower || !upper ||
        GetAncestor(lower, GA_PARENT) != GetAncestor(upper, GA_PARENT)) return false;
    HWND next = GetWindow(upper, GW_HWNDNEXT);
    for (unsigned count = 0; next && count < 4096; ++count) {
        if (next == lower) return true;
        next = GetWindow(next, GW_HWNDNEXT);
    }
    return false;
}
struct Descendants {
    HWND defview = nullptr;
    DWORD pid = 0;
    unsigned count = 0;
    bool incomplete = false;
};
BOOL CALLBACK FindDefView(HWND window, LPARAM value) {
    auto& data = *reinterpret_cast<Descendants*>(value);
    if (++data.count > 4096) { data.incomplete = true; return FALSE; }
    wchar_t name[128]{};
    GetClassNameW(window, name, static_cast<int>(std::size(name)));
    if (wcscmp(name, L"SHELLDLL_DefView") == 0 && ProcessId(window) == data.pid) {
        data.defview = window;
        return FALSE;
    }
    return TRUE;
}
Descendants DefView(HWND parent, DWORD pid) {
    Descendants result;
    result.pid = pid;
    EnumChildWindows(parent, FindDefView, reinterpret_cast<LPARAM>(&result));
    return result;
}
struct TopWindows {
    DWORD pid = 0;
    std::vector<HWND> windows;
    bool incomplete = false;
};
BOOL CALLBACK EnumerateTop(HWND window, LPARAM value) {
    auto& data = *reinterpret_cast<TopWindows*>(value);
    if (ProcessId(window) != data.pid) return TRUE;
    wchar_t name[128]{};
    GetClassNameW(window, name, static_cast<int>(std::size(name)));
    if (wcscmp(name, L"WorkerW") == 0 || wcscmp(name, L"Progman") == 0) {
        if (data.windows.size() >= 1024) { data.incomplete = true; return FALSE; }
        try { data.windows.push_back(window); }
        catch (...) { data.incomplete = true; return FALSE; }
    }
    return TRUE;
}
bool ParentCovers(HWND parent, const RECT& screen) {
    RECT local = screen, client{};
    SetLastError(ERROR_SUCCESS);
    const int mapped = MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&local), 2);
    if (!mapped && GetLastError() != ERROR_SUCCESS) return false;
    if (!GetClientRect(parent, &client)) return false;
    return local.left >= client.left && local.top >= client.top &&
        local.right <= client.right && local.bottom <= client.bottom;
}
bool SelectTarget(const ExplorerIdentity& explorer, const RECT& monitorRect, Target& target) {
    // 24H2 raised desktop: Progman directly owns both the icon DefView and a
    // background WorkerW. Our own layered HWND is inserted below DefView,
    // above that WorkerW. We never edit the WorkerW itself.
    HWND icons = FindWindowExW(explorer.progman, nullptr, L"SHELLDLL_DefView", nullptr);
    if (icons && ProcessId(icons) == explorer.pid) {
        for (HWND worker = FindWindowExW(explorer.progman, nullptr, L"WorkerW", nullptr);
            worker; worker = FindWindowExW(explorer.progman, worker, L"WorkerW", nullptr)) {
            if (ProcessId(worker) != explorer.pid || !BehindSibling(worker, icons)) continue;
            const auto childView = DefView(worker, explorer.pid);
            if (childView.defview || childView.incomplete) continue;
            DpiScope context(GetWindowDpiAwarenessContext(explorer.progman));
            if (!IsWindowVisible(explorer.progman) || !ParentCovers(explorer.progman, monitorRect)) continue;
            target = {explorer, explorer.progman, icons, worker, explorer.progman, true,
                GetWindowDpiAwarenessContext(explorer.progman), L"24H2 Progman child-layer"};
            return target.dpi != nullptr;
        }
    }
    // Legacy layout: discover the real icon-root first, then an Explorer-owned
    // top-level WorkerW behind it without DefView. Never assume "first WorkerW".
    TopWindows data;
    data.pid = explorer.pid;
    EnumWindows(EnumerateTop, reinterpret_cast<LPARAM>(&data));
    if (data.incomplete) return false;
    for (size_t iconIndex = 0; iconIndex < data.windows.size(); ++iconIndex) {
        const HWND root = data.windows[iconIndex];
        const auto view = DefView(root, explorer.pid);
        if (!view.defview || view.incomplete) continue;
        for (size_t workerIndex = iconIndex + 1; workerIndex < data.windows.size(); ++workerIndex) {
            const HWND worker = data.windows[workerIndex];
            if (ClassName(worker) != L"WorkerW" || (GetWindowLongPtrW(worker, GWL_STYLE) & WS_CHILD) ||
                !IsWindowVisible(worker) || !BehindSibling(worker, root)) continue;
            const auto nestedView = DefView(worker, explorer.pid);
            if (nestedView.defview || nestedView.incomplete) continue;
            DpiScope context(GetWindowDpiAwarenessContext(worker));
            if (!ParentCovers(worker, monitorRect)) continue;
            target = {explorer, worker, view.defview, worker, root, false,
                GetWindowDpiAwarenessContext(worker), L"legacy top-level WorkerW"};
            return target.dpi != nullptr;
        }
    }
    return false;
}
std::wstring Describe(const Target& target) {
    return L"layout=" + target.layout + L" · ExplorerPID=" + std::to_wstring(target.explorer.pid) +
        L" · parent=" + HandleText(target.parent) + L" · icons=" + HandleText(target.icons) +
        L" · background=" + HandleText(target.background) + L" · DPI=" + DpiName(target.dpi) +
        (target.layered ? L" · own layered alpha=255 after parenting / BLT presentation required" : L"") +
        (target.backgroundRequested ? L" · requested existing desktop layer 0x052C/0/0" : L"");
}
bool Discover(const RECT& monitorRect, bool allowCreate, Target& target, std::wstring& diagnostic) {
    ExplorerIdentity explorer;
    if (!ReadExplorer(explorer, diagnostic)) return false;
    if (SelectTarget(explorer, monitorRect, target)) {
        if (allowCreate && target.layered) {
            // An existing 24H2 child topology is not a receipt that the shell
            // has prepared the raised background for this attachment. Request
            // it once here; a healthy Poll does not enter Discover again.
            // Only request the background layer, never the 0xD/0 teardown.
            DWORD_PTR reply = 0;
            SetLastError(ERROR_SUCCESS);
            const LRESULT sent = SendMessageTimeoutW(explorer.progman, 0x052C, 0, 0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT, 500, &reply);
            if (!sent) {
                const DWORD error = GetLastError();
                diagnostic = ErrorText(L"Explorer未完成桌面背景层准备，保留窗口模式",
                    error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error);
                return false;
            }
            ExplorerIdentity current;
            std::wstring reason;
            if (!ReadExplorer(current, reason) || !SameExplorer(current, explorer)) {
                diagnostic = L"Explorer在准备既有桌面层时发生变化，保留窗口模式";
                return false;
            }
            // The request may rebuild the shell's children. Never carry the
            // pre-request HWNDs into SetParent or the prepared-target cache.
            Target refreshed;
            if (!SelectTarget(current, monitorRect, refreshed)) {
                diagnostic = L"准备后未找到可验证的桌面背景层，保留窗口模式";
                return false;
            }
            target = std::move(refreshed);
            target.backgroundRequested = true;
            diagnostic = Describe(target);
            return true;
        }
        diagnostic = Describe(target);
        return true;
    }
    if (allowCreate) {
        // Bounded requests to create a desktop layer only. No WorkerW is hidden,
        // destroyed, restyled, moved, or z-ordered, and no 0xD/0 teardown request.
        for (const WPARAM parameter : {WPARAM{0xD}, WPARAM{0}}) {
            DWORD_PTR ignored = 0;
            SetLastError(ERROR_SUCCESS);
            const LRESULT sent = SendMessageTimeoutW(explorer.progman, 0x052C,
                parameter, parameter == 0xD ? LPARAM{1} : LPARAM{0},
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &ignored);
            if (!sent && GetLastError() == ERROR_ACCESS_DENIED) {
                diagnostic = ErrorText(L"Explorer拒绝创建桌面背景层", ERROR_ACCESS_DENIED);
                return false;
            }
            ExplorerIdentity current;
            std::wstring reason;
            if (!ReadExplorer(current, reason) || !SameExplorer(current, explorer)) {
                diagnostic = L"Explorer在准备桌面层时发生变化，保留窗口模式";
                return false;
            }
            if (SelectTarget(explorer, monitorRect, target)) {
                diagnostic = Describe(target) + L" · requested desktop layer 0x052C";
                return true;
            }
        }
    }
    diagnostic = L"当前Explorer拓扑没有可验证的图标下方背景层，保留窗口模式";
    return false;
}
bool TargetValid(const Target& target, std::wstring& diagnostic) {
    ExplorerIdentity current;
    if (!ReadExplorer(current, diagnostic) || !SameExplorer(current, target.explorer)) return false;
    if (!IsWindow(target.parent) || !IsWindow(target.icons) || !IsWindow(target.background) ||
        ProcessId(target.parent) != current.pid || ProcessId(target.icons) != current.pid ||
        ProcessId(target.background) != current.pid || ClassName(target.icons) != L"SHELLDLL_DefView" ||
        ClassName(target.background) != L"WorkerW") return false;
    if (target.layered) {
        return target.parent == current.progman && GetParent(target.icons) == target.parent &&
            GetParent(target.background) == target.parent &&
            BehindSibling(target.background, target.icons);
    }
    return ClassName(target.parent) == L"WorkerW" &&
        !(GetWindowLongPtrW(target.parent, GWL_STYLE) & WS_CHILD) &&
        GetAncestor(target.icons, GA_ROOT) == target.iconRoot &&
        BehindSibling(target.parent, target.iconRoot);
}
bool SetLongChecked(HWND window, int index, LONG_PTR value) {
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR old = SetWindowLongPtrW(window, index, value);
    return old != 0 || GetLastError() == ERROR_SUCCESS;
}
bool SetParentChecked(HWND window, HWND parent) {
    const HWND expected = parent ? parent : GetDesktopWindow();
    if (GetAncestor(window, GA_PARENT) == expected) return true;
    SetLastError(ERROR_SUCCESS);
    SetParent(window, parent);
    const DWORD error = GetLastError();
    if (GetAncestor(window, GA_PARENT) == expected) return true;
    SetLastError(error == ERROR_SUCCESS ? ERROR_INVALID_STATE : error);
    return false;
}
bool MonitorInfo(HMONITOR monitor, MONITORINFOEXW& info) {
    info = {};
    info.cbSize = sizeof(info);
    return monitor && GetMonitorInfoW(monitor, &info);
}
struct MonitorSearch {
    const std::wstring* device = nullptr;
    HMONITOR found = nullptr;
};
BOOL CALLBACK FindMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM value) {
    auto& search = *reinterpret_cast<MonitorSearch*>(value);
    MONITORINFOEXW info{};
    if (MonitorInfo(monitor, info) && *search.device == info.szDevice) {
        search.found = monitor;
        return FALSE;
    }
    return TRUE;
}
HMONITOR MonitorByDevice(const std::wstring& device) {
    MonitorSearch search{&device};
    EnumDisplayMonitors(nullptr, nullptr, FindMonitor, reinterpret_cast<LPARAM>(&search));
    return search.found;
}
} // namespace

struct DesktopHost::Impl {
    DWORD uiThread = 0;
    HWND window = nullptr, originalParent = nullptr, originalOwner = nullptr;
    LONG_PTR originalStyle = 0, originalExStyle = 0;
    WINDOWPLACEMENT originalPlacement{sizeof(WINDOWPLACEMENT)};
    RECT originalRect{};
    COLORREF originalColorKey = 0;
    BYTE originalAlpha = 255;
    DWORD originalLayerFlags = 0;
    bool originalVisible = false, saved = false, attached = false, restorePending = false;
    Target prepared, target;
    std::wstring monitorDevice;
    RECT monitorRect{};

    bool ThreadCorrect(std::wstring& diagnostic, HWND own = nullptr) {
        const DWORD thread = GetCurrentThreadId();
        if (uiThread && thread != uiThread) {
            diagnostic = L"桌面宿主操作必须在创建舞台的UI线程执行";
            return false;
        }
        if (own) {
            DWORD pid = 0;
            const DWORD ownerThread = GetWindowThreadProcessId(own, &pid);
            if (!ownerThread || ownerThread != thread || pid != GetCurrentProcessId()) {
                diagnostic = L"桌面宿主只接受当前进程、当前UI线程自己的舞台窗口";
                return false;
            }
        }
        if (!uiThread) uiThread = thread;
        return true;
    }
    bool Capture(HWND own, std::wstring& diagnostic) {
        SetLastError(ERROR_SUCCESS);
        originalStyle = GetWindowLongPtrW(own, GWL_STYLE);
        if (!originalStyle && GetLastError() != ERROR_SUCCESS) {
            diagnostic = ErrorText(L"读取舞台窗口style失败", GetLastError());
            return false;
        }
        SetLastError(ERROR_SUCCESS);
        originalExStyle = GetWindowLongPtrW(own, GWL_EXSTYLE);
        if (!originalExStyle && GetLastError() != ERROR_SUCCESS) {
            diagnostic = ErrorText(L"读取舞台窗口exstyle失败", GetLastError());
            return false;
        }
        originalPlacement = {sizeof(WINDOWPLACEMENT)};
        if (!GetWindowPlacement(own, &originalPlacement) || !GetWindowRect(own, &originalRect)) {
            diagnostic = ErrorText(L"读取舞台原始位置失败", GetLastError());
            return false;
        }
        originalParent = (originalStyle & WS_CHILD) ? GetParent(own) : nullptr;
        originalOwner = (originalStyle & WS_CHILD) ? nullptr : GetWindow(own, GW_OWNER);
        // Preserve the window's own visibility bit, including children whose
        // original parent happens to be hidden at capture time.
        originalVisible = (originalStyle & WS_VISIBLE) != 0;
        if (originalExStyle & WS_EX_LAYERED) {
            if (!GetLayeredWindowAttributes(own, &originalColorKey, &originalAlpha, &originalLayerFlags)) {
                diagnostic = L"舞台原有分层状态无法完整保存，保留窗口模式";
                return false;
            }
        }
        window = own;
        saved = true;
        return true;
    }
    bool Place(const Target& selected, const RECT& screen, std::wstring& diagnostic) {
        DpiScope dpi(GetWindowDpiAwarenessContext(window));
        RECT client = screen;
        SetLastError(ERROR_SUCCESS);
        const int mapped = MapWindowPoints(nullptr, selected.parent,
            reinterpret_cast<POINT*>(&client), 2);
        if (!mapped && GetLastError() != ERROR_SUCCESS) {
            diagnostic = ErrorText(L"桌面坐标转换失败", GetLastError());
            return false;
        }
        if (!ParentCovers(selected.parent, screen)) {
            diagnostic = L"背景宿主不能完整覆盖所选显示器，保留窗口模式";
            return false;
        }
        const HWND after = selected.layered ? selected.icons : HWND_TOP;
        if (!SetWindowPos(window, after, client.left, client.top,
            client.right - client.left, client.bottom - client.top,
            SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW)) {
            diagnostic = ErrorText(L"设置本应用桌面舞台位置失败", GetLastError());
            return false;
        }
        if (selected.layered && (!BehindSibling(window, selected.icons) ||
            !BehindSibling(selected.background, window))) {
            diagnostic = L"本应用窗口没有稳定处于图标与系统背景之间，已停止挂载";
            return false;
        }
        return true;
    }
    bool Mount(const Target& selected, const RECT& screen, std::wstring& diagnostic) {
        std::wstring verification;
        if (!TargetValid(selected, verification)) {
            diagnostic = L"桌面宿主身份或窗口拓扑已改变，保留窗口模式";
            return false;
        }
        const auto ownDpi = GetWindowDpiAwarenessContext(window);
        const auto parentDpi = GetWindowDpiAwarenessContext(selected.parent);
        // Do not attempt cross-process SetParent with mismatched contexts:
        // Windows can force-reset the child's entire process DPI awareness.
        if (!ownDpi || !parentDpi || !AreDpiAwarenessContextsEqual(ownDpi, parentDpi)) {
            diagnostic = L"舞台与桌面DPI上下文不一致，请按Prepare结果重建独立舞台 · own=" +
                std::wstring(DpiName(ownDpi)) + L" · desktop=" + DpiName(parentDpi);
            return false;
        }
        const LONG_PTR classStyle = GetClassLongPtrW(window, GCL_STYLE);
        if (selected.layered && (classStyle & (CS_OWNDC | CS_CLASSDC))) {
            diagnostic = L"舞台窗口类使用独占DC，不能安全启用24H2分层桌面模式";
            return false;
        }
        DpiScope dpi(ownDpi);
        ShowWindow(window, SW_HIDE);
        const LONG_PTR style = (originalStyle & ~static_cast<LONG_PTR>(
            WS_OVERLAPPEDWINDOW | WS_POPUP | WS_MINIMIZE | WS_MAXIMIZE | WS_VISIBLE)) |
            WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
        LONG_PTR exStyle = (originalExStyle & ~static_cast<LONG_PTR>(WS_EX_APPWINDOW | WS_EX_TOPMOST)) |
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT;
        if (selected.layered)
            exStyle = (exStyle & ~static_cast<LONG_PTR>(WS_EX_NOREDIRECTIONBITMAP)) | WS_EX_LAYERED;
        // Establish the final parent before creating the layered redirection
        // surface. On recovery, discard our previous surface while hidden;
        // do not carry a top-level/old-parent surface into the new shell tree.
        const LONG_PTR parentingExStyle = selected.layered ?
            exStyle & ~static_cast<LONG_PTR>(WS_EX_LAYERED) : exStyle;
        if (!SetLongChecked(window, GWL_STYLE, style) ||
            !SetLongChecked(window, GWL_EXSTYLE, parentingExStyle)) {
            diagnostic = ErrorText(L"设置本应用桌面style失败", GetLastError());
            return false;
        }
        if (!SetParentChecked(window, selected.parent)) {
            diagnostic = ErrorText(L"SetParent桌面挂载失败", GetLastError());
            return false;
        }
        // Record a real parent change immediately, even if a later DPI/z-order
        // check fails. Rollback failure must not falsely report "unattached".
        target = selected;
        attached = true;
        if (!TargetValid(selected, verification)) {
            diagnostic = L"SetParent期间Explorer宿主身份或拓扑发生变化，已停止挂载";
            return false;
        }
        if (!AreDpiAwarenessContextsEqual(GetWindowDpiAwarenessContext(window), ownDpi)) {
            diagnostic = L"SetParent后舞台DPI上下文发生意外变化，已停止挂载";
            return false;
        }
        if (selected.layered && (!SetLongChecked(window, GWL_EXSTYLE, exStyle) ||
            !SetLayeredWindowAttributes(window, 0, 255, LWA_ALPHA))) {
            diagnostic = ErrorText(L"在桌面父级下建立本应用分层显示失败", GetLastError());
            return false;
        }
        if (!Place(selected, screen, diagnostic)) return false;
        target = selected;
        monitorRect = screen;
        attached = true;
        restorePending = false;
        return true;
    }
    bool Restore(std::wstring& diagnostic) {
        if (!saved) { attached = restorePending = false; return true; }
        restorePending = true;
        if (!ThreadCorrect(diagnostic)) return false;
        if (!IsWindow(window) || ProcessId(window) != GetCurrentProcessId()) {
            diagnostic = L"桌面舞台HWND已被销毁；控制/托盘窗口应继续运行并重建舞台";
            saved = attached = restorePending = false;
            window = nullptr;
            return false;
        }
        DpiScope dpi(GetWindowDpiAwarenessContext(window));
        ShowWindow(window, SW_HIDE);
        auto failed = [&](const wchar_t* action) {
            DWORD error = GetLastError();
            if (error == ERROR_SUCCESS) error = ERROR_INVALID_STATE;
            diagnostic = ErrorText(action, error) +
                L" · 本应用舞台保持隐藏，保存状态仍可重试；也可销毁自己的舞台";
            if (IsWindow(window)) ShowWindow(window, SW_HIDE);
            attached = target.parent && IsWindow(target.parent) &&
                GetAncestor(window, GA_PARENT) == target.parent;
            return false;
        };
        HWND restoreParent = originalParent && IsWindow(originalParent) ? originalParent : nullptr;
        // Do not restore top-level style/owner while still parented to Explorer.
        // Stop at the first failed step, retaining exact originals for retry.
        if (!SetParentChecked(window, restoreParent)) return failed(L"恢复舞台原父级失败");
        LONG_PTR style = originalStyle;
        if (originalParent && !restoreParent)
            style = (style & ~static_cast<LONG_PTR>(WS_CHILD)) | WS_POPUP;
        if (!SetLongChecked(window, GWL_STYLE, style & ~static_cast<LONG_PTR>(WS_VISIBLE)))
            return failed(L"恢复舞台原style失败");
        if (!SetLongChecked(window, GWL_EXSTYLE, originalExStyle))
            return failed(L"恢复舞台原exstyle失败");
        if (!restoreParent) {
            HWND owner = originalOwner && IsWindow(originalOwner) ? originalOwner : nullptr;
            // A newly created standalone stage normally already has no owner.
            // Avoid an unnecessary owner mutation, but verify real changes.
            if (GetWindow(window, GW_OWNER) != owner &&
                !SetLongChecked(window, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner)))
                return failed(L"恢复舞台原owner失败");
            if (!IsWindow(window) || GetWindow(window, GW_OWNER) != owner) {
                SetLastError(ERROR_INVALID_STATE);
                return failed(L"恢复舞台owner后核验失败");
            }
        }
        if ((originalExStyle & WS_EX_LAYERED) &&
            !SetLayeredWindowAttributes(window, originalColorKey, originalAlpha, originalLayerFlags))
            return failed(L"恢复舞台原分层参数失败");
        if (!SetWindowPlacement(window, &originalPlacement))
            return failed(L"恢复舞台原WINDOWPLACEMENT失败");
        if (!SetWindowPos(window, (originalExStyle & WS_EX_TOPMOST) ? HWND_TOPMOST : HWND_NOTOPMOST,
            0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED))
            return failed(L"恢复舞台原窗口框架失败");
        ShowWindow(window, originalVisible ? SW_SHOWNA : SW_HIDE);
        saved = attached = restorePending = false;
        window = nullptr;
        target = {};
        monitorDevice.clear();
        diagnostic = originalParent && !restoreParent ?
            L"原父窗口已失效，本应用舞台已退回独立窗口" :
            L"已恢复本应用舞台的原始窗口状态";
        return true;
    }
};

DesktopHost::DesktopHost() : impl_(std::make_unique<Impl>()) {}
DesktopHost::~DesktopHost() {
    if (impl_ && impl_->saved && GetCurrentThreadId() == impl_->uiThread) {
        std::wstring ignored;
        try { impl_->Restore(ignored); } catch (...) {}
    }
}
DPI_AWARENESS_CONTEXT DesktopHost::Prepare(std::wstring& diagnostic) {
    diagnostic.clear();
    if (!impl_->ThreadCorrect(diagnostic)) return nullptr;
    MONITORINFOEXW monitor{};
    POINT origin{};
    if (!MonitorInfo(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), monitor)) {
        diagnostic = ErrorText(L"读取主显示器失败", GetLastError());
        return nullptr;
    }
    Target target;
    if (!Discover(monitor.rcMonitor, true, target, diagnostic)) {
        impl_->prepared = {};
        return nullptr;
    }
    impl_->prepared = std::move(target);
    return impl_->prepared.dpi;
}
bool DesktopHost::Attach(HWND ownStage, HMONITOR preferred, std::wstring& diagnostic) {
    diagnostic.clear();
    if (!IsWindow(ownStage) || !impl_->ThreadCorrect(diagnostic, ownStage)) {
        if (diagnostic.empty()) diagnostic = L"舞台窗口无效，保留窗口模式";
        return false;
    }
    if (impl_->saved && impl_->window != ownStage) {
        // Explorer may already have destroyed the previous stage. Its original
        // state cannot be restored, but it must not block a fresh stage.
        if (!IsWindow(impl_->window) || ProcessId(impl_->window) != GetCurrentProcessId()) {
            impl_->saved = impl_->attached = impl_->restorePending = false;
            impl_->window = nullptr;
            impl_->target = {};
            impl_->monitorDevice.clear();
        }
    }
    if (impl_->saved && impl_->window != ownStage) {
        std::wstring detached;
        if (!impl_->Restore(detached)) { diagnostic = detached; return false; }
    }
    MONITORINFOEXW monitor{};
    if (!MonitorInfo(preferred ? preferred : MonitorFromWindow(ownStage, MONITOR_DEFAULTTONEAREST), monitor)) {
        diagnostic = ErrorText(L"读取所选显示器失败", GetLastError());
        return false;
    }
    Target target;
    std::wstring verification;
    if (impl_->prepared.parent && TargetValid(impl_->prepared, verification)) {
        DpiScope context(impl_->prepared.dpi);
        if (ParentCovers(impl_->prepared.parent, monitor.rcMonitor)) target = impl_->prepared;
    }
    if (!target.parent && !Discover(monitor.rcMonitor, true, target, diagnostic)) return false;
    if (!impl_->saved && !impl_->Capture(ownStage, diagnostic)) return false;
    std::wstring failure;
    if (!impl_->Mount(target, monitor.rcMonitor, failure)) {
        std::wstring restore;
        const bool restored = impl_->Restore(restore);
        diagnostic = failure + (restored ? L" · 已回滚原始窗口状态" : L" · " + restore);
        return false;
    }
    impl_->monitorDevice = monitor.szDevice;
    impl_->prepared = target;
    diagnostic = L"本应用桌面舞台已挂载，图标层在上 · " + Describe(target);
    return true;
}
bool DesktopHost::Detach(std::wstring* diagnostic) {
    std::wstring result;
    if (!impl_->ThreadCorrect(result)) {
        if (diagnostic) *diagnostic = result;
        return false;
    }
    const bool restored = impl_->Restore(result);
    if (diagnostic) *diagnostic = result;
    return restored;
}
bool DesktopHost::IsAttached() const {
    return impl_->attached && IsWindow(impl_->window) && IsWindow(impl_->target.parent) &&
        GetParent(impl_->window) == impl_->target.parent;
}
bool DesktopHost::Poll(std::wstring& diagnostic) {
    diagnostic.clear();
    if (!impl_->ThreadCorrect(diagnostic)) return false;
    if (!impl_->saved) return false;
    if (!IsWindow(impl_->window) || ProcessId(impl_->window) != GetCurrentProcessId()) {
        impl_->saved = impl_->attached = impl_->restorePending = false;
        impl_->window = nullptr;
        impl_->prepared = {};
        impl_->target = {};
        impl_->monitorDevice.clear();
        diagnostic = L"Explorer重启后舞台HWND已失效，请重建独立舞台；控制/托盘窗口继续运行";
        return false;
    }
    if (impl_->restorePending) {
        // A failed explicit detach/rollback is a request for window mode.
        // Poll may retry restoration, but must never silently remount desktop.
        impl_->Restore(diagnostic);
        return false;
    }
    const HMONITOR monitor = MonitorByDevice(impl_->monitorDevice);
    MONITORINFOEXW info{};
    if (!MonitorInfo(monitor, info)) {
        std::wstring restored;
        const bool success = impl_->Restore(restored);
        diagnostic = L"原显示器已移除 · " + restored;
        if (!success) diagnostic += L" · 原始窗口恢复尚未完成";
        return false;
    }
    std::wstring verify;
    const bool hostReady = TargetValid(impl_->target, verify) &&
        GetParent(impl_->window) == impl_->target.parent;
    if (!hostReady) {
        Target replacement;
        if (Discover(info.rcMonitor, true, replacement, verify)) {
            std::wstring reason;
            if (impl_->Mount(replacement, info.rcMonitor, reason)) {
                impl_->prepared = replacement;
                diagnostic = L"Explorer桌面宿主已恢复 · " + Describe(replacement);
                return true;
            }
            verify = reason;
        }
        std::wstring restored;
        impl_->Restore(restored);
        diagnostic = L"桌面宿主恢复失败，退回原始窗口 · " + verify + L" · " + restored;
        return false;
    }
    RECT actual{};
    DpiScope dpi(GetWindowDpiAwarenessContext(impl_->window));
    if (!GetWindowRect(impl_->window, &actual) || !EqualRect(&actual, &info.rcMonitor)) {
        if (!impl_->Place(impl_->target, info.rcMonitor, verify)) {
            std::wstring restored;
            impl_->Restore(restored);
            diagnostic = L"显示器布局恢复失败，退回原始窗口 · " + verify + L" · " + restored;
            return false;
        }
        impl_->monitorRect = info.rcMonitor;
        diagnostic = L"本应用桌面舞台已适配显示器布局变化";
    }
    // If Explorer changed sibling order, adjust only our own window.
    if (impl_->target.layered && (!BehindSibling(impl_->window, impl_->target.icons) ||
        !BehindSibling(impl_->target.background, impl_->window))) {
        if (!impl_->Place(impl_->target, info.rcMonitor, verify)) {
            std::wstring restored;
            impl_->Restore(restored);
            diagnostic = L"图标层级恢复失败，退回原始窗口 · " + verify + L" · " + restored;
            return false;
        }
        diagnostic = L"已将本应用窗口恢复到图标层下方";
    }
    impl_->attached = true;
    return true;
}

} // namespace stage
