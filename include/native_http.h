#pragma once
#include <cstddef>
#include <cstdint>
#include <stop_token>
#include <string>
#include <vector>

namespace stage {
enum class NativeHttpFailure { None, Cancelled, Timeout, Network, HttpStatus, TooLarge, InvalidRequest };
struct NativeHttpRequest {
    std::wstring host, path, headers;
    std::string body; // empty: GET; nonempty: POST, owned throughout async use
    size_t maxBytes = 2 * 1024 * 1024;
    uint32_t totalTimeoutMs = 15000; // includes send, headers and every read
};
struct NativeHttpResponse {
    std::vector<uint8_t> bytes;
    uint32_t status = 0;
    NativeHttpFailure failure = NativeHttpFailure::None;
    std::wstring diagnostic;
    bool ok() const { return failure == NativeHttpFailure::None; }
    bool cancelled() const { return failure == NativeHttpFailure::Cancelled; }
};
// Blocking facade for the existing application worker. WinHTTP I/O is async;
// stop interrupts the wait and closes the individual request exactly once.
// Default TLS, HTTPS:443 only, no cookies or redirects. Callers still enforce
// their fixed API/CDN host and path policies; no arbitrary URL is accepted.
NativeHttpResponse NativeHttps(const NativeHttpRequest& request, std::stop_token stop = {});
#ifdef STAGE_NATIVE_HTTP_TEST
// Compiled only into the isolated check executable. Fixed 127.0.0.1 HTTP,
// explicit ephemeral port and no proxy; never exposed in the application build.
NativeHttpResponse NativeHttpLoopbackCheck(const NativeHttpRequest& request, uint16_t port,
    std::stop_token stop = {});
size_t NativeHttpOutstandingContextsCheck(); // count only, no static state retention
size_t NativeHttpPendingReadsCheck();
size_t NativeHttpPendingHeadersCheck();
size_t NativeHttpPendingSendCheck();
#endif
}
