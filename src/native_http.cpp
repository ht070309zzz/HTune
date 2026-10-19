#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "native_http.h"
#include <windows.h>
#include <winhttp.h>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <utility>

#pragma comment(lib, "Winhttp.lib")

namespace stage {
namespace {
using Clock = std::chrono::steady_clock;
#ifdef STAGE_NATIVE_HTTP_TEST
std::atomic<size_t> outstandingContexts{0};
std::atomic<size_t> pendingReads{0}, pendingHeaders{0}, pendingSend{0};
struct TestPendingOperation {
    std::atomic<size_t>& count;
    explicit TestPendingOperation(std::atomic<size_t>& value) : count(value) { ++count; }
    ~TestPendingOperation() { --count; }
};
#endif
struct HttpState {
    HINTERNET session = nullptr, connection = nullptr;
    HANDLE completed = nullptr, cancelled = nullptr;
    std::mutex mutex;
    DWORD expected = 0, error = ERROR_SUCCESS, read = 0;
    bool ready = false;
    std::atomic<bool> callbackFailed{false};
    // WinHTTP can borrow both buffers until its final callback after cancellation.
    std::string body;
    std::array<uint8_t, 16384> buffer{};
    ~HttpState() {
        if (connection) WinHttpCloseHandle(connection);
        if (session) WinHttpCloseHandle(session);
        if (completed) CloseHandle(completed);
        if (cancelled) CloseHandle(cancelled);
    }
    void Prepare(DWORD status) {
        std::lock_guard lock(mutex);
        expected = status; ready = false; error = ERROR_SUCCESS; read = 0;
        ResetEvent(completed);
    }
};
struct CallbackLease {
    std::shared_ptr<HttpState> state;
    explicit CallbackLease(std::shared_ptr<HttpState> value) : state(std::move(value)) {
#ifdef STAGE_NATIVE_HTTP_TEST
        ++outstandingContexts;
#endif
    }
    ~CallbackLease() {
#ifdef STAGE_NATIVE_HTTP_TEST
        --outstandingContexts;
#endif
    }
};
void CALLBACK HttpCallback(HINTERNET, DWORD_PTR context, DWORD status, void* information,
    DWORD informationLength) noexcept {
    if (!context) return;
    auto* lease = reinterpret_cast<CallbackLease*>(context);
    auto state = lease->state;
    if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
        // WinHTTP guarantees this is last and does not overlap other callbacks
        // for this request. Keep the local strong ref while releasing its anchor.
        delete lease;
        return;
    }
    try {
        bool signal = false;
        {
            std::lock_guard lock(state->mutex);
            if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) {
                state->error = information && informationLength >= sizeof(WINHTTP_ASYNC_RESULT)
                    ? static_cast<WINHTTP_ASYNC_RESULT*>(information)->dwError : ERROR_INVALID_DATA;
                state->ready = true; signal = true;
            } else if (status == state->expected) {
                state->read = status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE ? informationLength : 0;
                state->ready = true; signal = true;
            }
        }
        if (signal) SetEvent(state->completed);
    } catch (...) {
        state->callbackFailed = true;
        SetEvent(state->completed);
    }
}
class RequestOwner {
    HINTERNET request_;
    std::shared_ptr<HttpState> state_;
public:
    RequestOwner(HINTERNET request, std::shared_ptr<HttpState> state)
        : request_(request), state_(std::move(state)) {}
    ~RequestOwner() { Close(); }
    RequestOwner(const RequestOwner&) = delete;
    RequestOwner& operator=(const RequestOwner&) = delete;
    HINTERNET get() const { return request_; }
    void Close() {
        // Only the calling worker owns/uses this request, never the stop callback.
        // Every initiating API has returned before Close. No second close/use of
        // the old numeric value is possible even when Windows reuses handles.
        const auto request = std::exchange(request_, nullptr);
        if (request) WinHttpCloseHandle(request);
    }
};
void Fail(NativeHttpResponse& result, NativeHttpFailure failure, const std::wstring& text) {
    result.bytes.clear(); result.failure = failure; result.diagnostic = text;
}
void WindowsFailure(NativeHttpResponse& result, DWORD error, const wchar_t* action) {
    if (error == ERROR_WINHTTP_OPERATION_CANCELLED)
        Fail(result, NativeHttpFailure::Cancelled, L"网络请求已取消");
    else if (error == ERROR_WINHTTP_TIMEOUT)
        Fail(result, NativeHttpFailure::Timeout, L"网络请求已超时");
    else Fail(result, NativeHttpFailure::Network,
        std::wstring(action) + L"失败（WinHTTP " + std::to_wstring(error) + L"）");
}
bool Interrupted(std::stop_token stop, Clock::time_point deadline, NativeHttpResponse& result) {
    if (stop.stop_requested()) { Fail(result, NativeHttpFailure::Cancelled, L"网络请求已取消"); return true; }
    if (Clock::now() >= deadline) { Fail(result, NativeHttpFailure::Timeout, L"网络请求超过总时限"); return true; }
    return false;
}
bool Wait(const std::shared_ptr<HttpState>& state, std::stop_token stop,
    Clock::time_point deadline, NativeHttpResponse& result) {
    for (;;) {
        if (Interrupted(stop, deadline, result)) return false;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        const DWORD waitMs = static_cast<DWORD>(remaining > 0 ? remaining : 1);
        const HANDLE events[]{state->cancelled, state->completed};
        const DWORD outcome = WaitForMultipleObjects(2, events, FALSE, waitMs);
        // A late successful completion never overrides cancellation/deadline.
        if (Interrupted(stop, deadline, result)) return false;
        if (outcome == WAIT_OBJECT_0) { Fail(result, NativeHttpFailure::Cancelled, L"网络请求已取消"); return false; }
        if (outcome == WAIT_TIMEOUT) continue;
        if (outcome != WAIT_OBJECT_0 + 1) { WindowsFailure(result, GetLastError(), L"等待网络操作"); return false; }
        if (state->callbackFailed) { WindowsFailure(result, ERROR_INVALID_DATA, L"网络回调"); return false; }
        std::lock_guard lock(state->mutex);
        if (!state->ready) continue;
        if (state->error != ERROR_SUCCESS) { WindowsFailure(result, state->error, L"异步网络操作"); return false; }
        return true;
    }
}
bool ValidRequest(const NativeHttpRequest& request) {
    if (request.host.empty() || request.host.size() > 253 || request.path.empty() || request.path.size() > 32768 ||
        request.path.front() != L'/' || request.path.starts_with(L"//") || request.headers.size() > 8192 ||
        request.maxBytes == 0 || request.maxBytes > 10 * 1024 * 1024 || request.body.size() > 2 * 1024 * 1024 ||
        request.totalTimeoutMs == 0 || request.totalTimeoutMs > 120000) return false;
    for (wchar_t c : request.host) if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
        (c >= L'0' && c <= L'9') || c == L'.' || c == L'-')) return false;
    for (wchar_t c : request.path) if (c <= 32 || c == 127) return false;
    return request.headers.find(L'\0') == std::wstring::npos;
}
NativeHttpResponse Perform(const NativeHttpRequest& input, std::stop_token stop,
    bool secure, INTERNET_PORT port, DWORD proxy) {
    NativeHttpResponse result;
    if (stop.stop_requested()) { Fail(result, NativeHttpFailure::Cancelled, L"网络请求已取消"); return result; }
    if (!ValidRequest(input)) { Fail(result, NativeHttpFailure::InvalidRequest, L"网络请求参数或限额无效"); return result; }
    try {
        const auto deadline = Clock::now() + std::chrono::milliseconds(input.totalTimeoutMs);
        auto state = std::make_shared<HttpState>();
        state->body = input.body;
        state->completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        state->cancelled = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!state->completed || !state->cancelled) { WindowsFailure(result, GetLastError(), L"创建网络事件"); return result; }
        // Unregistered before local state ownership is released. Its only action
        // is SetEvent; it never closes handles or accesses application/UI state.
        std::stop_callback cancellation(stop, [state] { SetEvent(state->cancelled); });
        if (Interrupted(stop, deadline, result)) return result;
        state->session = WinHttpOpen(L"MusicStageNative/0.3", proxy, WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
        if (!state->session) { WindowsFailure(result, GetLastError(), L"初始化网络"); return result; }
        if (!WinHttpSetTimeouts(state->session, 2000, 3000, 3000, 5000)) {
            WindowsFailure(result, GetLastError(), L"配置网络超时"); return result;
        }
        if (Interrupted(stop, deadline, result)) return result;
        state->connection = WinHttpConnect(state->session, input.host.c_str(), port, 0);
        if (!state->connection) { WindowsFailure(result, GetLastError(), L"创建网络连接"); return result; }
        if (Interrupted(stop, deadline, result)) return result;
        const auto request = WinHttpOpenRequest(state->connection, state->body.empty() ? L"GET" : L"POST",
            input.path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
        if (!request) { WindowsFailure(result, GetLastError(), L"创建网络请求"); return result; }
        RequestOwner owner(request, state);
        DWORD disable = WINHTTP_DISABLE_COOKIES, redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        if (!WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable)) ||
            !WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect))) {
            WindowsFailure(result, GetLastError(), L"配置网络安全选项"); return result;
        }
        auto lease = std::make_unique<CallbackLease>(state);
        DWORD_PTR context = reinterpret_cast<DWORD_PTR>(lease.get());
        if (!WinHttpSetOption(request, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context))) {
            WindowsFailure(result, GetLastError(), L"配置网络回调上下文"); return result;
        }
        if (WinHttpSetStatusCallback(request, HttpCallback, WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS |
            WINHTTP_CALLBACK_FLAG_HANDLES, 0) == WINHTTP_INVALID_STATUS_CALLBACK) {
            WindowsFailure(result, GetLastError(), L"配置网络回调"); return result;
        }
        // The request now owns this anchor until HANDLE_CLOSING, including
        // error/stop before SendRequest. No App reference is stored anywhere.
        lease.release();
        if (Interrupted(stop, deadline, result)) return result;
        state->Prepare(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE);
        {
#ifdef STAGE_NATIVE_HTTP_TEST
            TestPendingOperation pending(pendingSend);
#endif
            if (!WinHttpSendRequest(request, input.headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : input.headers.c_str(),
                input.headers.empty() ? 0 : static_cast<DWORD>(-1), state->body.empty() ? WINHTTP_NO_REQUEST_DATA : state->body.data(),
                static_cast<DWORD>(state->body.size()), static_cast<DWORD>(state->body.size()), context)) {
                WindowsFailure(result, GetLastError(), L"发送请求"); return result;
            }
            if (!Wait(state, stop, deadline, result)) return result;
        }
        state->Prepare(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE);
        {
#ifdef STAGE_NATIVE_HTTP_TEST
            TestPendingOperation pending(pendingHeaders);
#endif
            if (!WinHttpReceiveResponse(request, nullptr)) { WindowsFailure(result, GetLastError(), L"接收响应"); return result; }
            if (!Wait(state, stop, deadline, result)) return result;
        }
        DWORD status = 0, length = sizeof(status);
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX)) {
            WindowsFailure(result, GetLastError(), L"读取响应状态"); return result;
        }
        result.status = status;
        if (status != 200) { Fail(result, NativeHttpFailure::HttpStatus, L"网络服务返回 HTTP " + std::to_wstring(status)); return result; }
        DWORD contentLength = 0; length = sizeof(contentLength);
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &contentLength, &length, WINHTTP_NO_HEADER_INDEX) && contentLength > input.maxBytes) {
            Fail(result, NativeHttpFailure::TooLarge, L"网络响应超过大小上限"); return result;
        }
        for (;;) {
            if (Interrupted(stop, deadline, result)) return result;
            state->Prepare(WINHTTP_CALLBACK_STATUS_READ_COMPLETE);
#ifdef STAGE_NATIVE_HTTP_TEST
            TestPendingOperation pending(pendingReads);
#endif
            if (!WinHttpReadData(request, state->buffer.data(), static_cast<DWORD>(state->buffer.size()), nullptr)) {
                WindowsFailure(result, GetLastError(), L"读取响应数据"); return result;
            }
            if (!Wait(state, stop, deadline, result)) return result;
            DWORD read;
            { std::lock_guard lock(state->mutex); read = state->read; }
            if (read > state->buffer.size()) { WindowsFailure(result, ERROR_INVALID_DATA, L"校验响应长度"); return result; }
            if (!read) break;
            if (result.bytes.size() + read > input.maxBytes) { Fail(result, NativeHttpFailure::TooLarge, L"网络响应超过大小上限"); return result; }
            result.bytes.insert(result.bytes.end(), state->buffer.begin(), state->buffer.begin() + read);
        }
        Interrupted(stop, deadline, result);
        return result;
    } catch (...) { Fail(result, NativeHttpFailure::Network, L"网络操作内存或状态异常"); return result; }
}
} // namespace
NativeHttpResponse NativeHttps(const NativeHttpRequest& request, std::stop_token stop) {
    return Perform(request, stop, true, INTERNET_DEFAULT_HTTPS_PORT, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY);
}
#ifdef STAGE_NATIVE_HTTP_TEST
NativeHttpResponse NativeHttpLoopbackCheck(const NativeHttpRequest& request, uint16_t port, std::stop_token stop) {
    auto local = request; local.host = L"127.0.0.1";
    if (!port) { NativeHttpResponse result; Fail(result, NativeHttpFailure::InvalidRequest, L"测试端口无效"); return result; }
    return Perform(local, stop, false, port, WINHTTP_ACCESS_TYPE_NO_PROXY);
}
size_t NativeHttpOutstandingContextsCheck() { return outstandingContexts.load(); }
size_t NativeHttpPendingReadsCheck() { return pendingReads.load(); }
size_t NativeHttpPendingHeadersCheck() { return pendingHeaders.load(); }
size_t NativeHttpPendingSendCheck() { return pendingSend.load(); }
#endif
} // namespace stage
