#include "http_client.hpp"
#include "xorstr.hpp"

#include "distribution/distribution.hpp"
#include "encoding.hpp"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <memory>
#include <string_view>
#include <thread>

namespace chronicles::launcher {
namespace {

using WinHttpHandle =
    std::unique_ptr<std::remove_pointer_t<HINTERNET>, decltype(&WinHttpCloseHandle)>;

WinHttpHandle make_handle(HINTERNET handle) {
  return WinHttpHandle(handle, &WinHttpCloseHandle);
}

void throw_http_error(const char *operation) {
  throw HttpError(std::string(operation) + " failed (Windows error " +
                  std::to_string(GetLastError()) + ")");
}

struct HttpRequest {
  WinHttpHandle session{nullptr, WinHttpCloseHandle};
  WinHttpHandle connection{nullptr, WinHttpCloseHandle};
  WinHttpHandle request{nullptr, WinHttpCloseHandle};
};

// Best effort: an unreadable error body leaves only the status for the caller.
std::string read_error_body(HINTERNET request) {
  constexpr std::size_t kMaximumErrorBodyBytes = 4096;

  std::string body;
  std::array<char, 1024> buffer{};
  while (body.size() < kMaximumErrorBodyBytes) {
    DWORD received{};
    if (!WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &received) ||
        received == 0) {
      break;
    }
    const auto kept = std::min<std::size_t>(received, kMaximumErrorBodyBytes - body.size());
    body.append(buffer.data(), kept);
  }
  return body;
}

HttpRequest open_request(const std::string &url, const wchar_t *method,
                         const std::string &request_body, const std::string &bearer_token,
                         int timeout_ms) {
  if (request_body.size() > 65536 || url.find('#') != std::string::npos) {
    throw HttpError("Invalid HTTP request size or URL fragment");
  }
  if (!bearer_token.empty() && !distribution::valid_sha256(bearer_token)) {
    throw HttpError("Invalid bearer token");
  }
  const auto separator = url.find("://");
  if (separator == std::string::npos) {
    throw HttpError("Invalid service URL");
  }
  const auto path_start = url.find('/', separator + 3);
  const auto origin = distribution::parse_service_origin(url.substr(0, path_start));
  auto wide_url = widen(url);
  URL_COMPONENTS components{};
  components.dwStructSize = sizeof(components);
  components.dwHostNameLength = static_cast<DWORD>(-1);
  components.dwUrlPathLength = static_cast<DWORD>(-1);
  components.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &components)) {
    throw_http_error("WinHttpCrackUrl");
  }
  const std::wstring host(components.lpszHostName, components.dwHostNameLength);
  std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
  if (components.dwExtraInfoLength > 0) {
    path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
  }
  if (path.empty()) {
    path = L"/";
  }

  HttpRequest handles;
  handles.session =
      make_handle(WinHttpOpen(xorstr_(L"ChroniclesLauncher/0.2.6"), WINHTTP_ACCESS_TYPE_NO_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
  if (!handles.session ||
      !WinHttpSetTimeouts(handles.session.get(), timeout_ms, timeout_ms, timeout_ms, timeout_ms)) {
    throw_http_error("WinHttpOpen/SetTimeouts");
  }
  handles.connection =
      make_handle(WinHttpConnect(handles.session.get(), host.c_str(), components.nPort, 0));
  if (!handles.connection) {
    throw_http_error("WinHttpConnect");
  }
  handles.request = make_handle(WinHttpOpenRequest(
      handles.connection.get(), method, path.c_str(), nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, origin.secure ? WINHTTP_FLAG_SECURE : 0));
  if (!handles.request) {
    throw_http_error("WinHttpOpenRequest");
  }
  // Never follow a redirect carrying a password/grant, retain cookies, or send Windows identity.
  DWORD disabled =
      WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
  if (!WinHttpSetOption(handles.request.get(), WINHTTP_OPTION_DISABLE_FEATURE, &disabled,
                        sizeof(disabled))) {
    throw_http_error("WinHttpSetOption");
  }
  std::wstring headers = L"Accept: application/json, application/octet-stream\r\n";
  if (method[0] == L'P') {
    headers += L"Content-Type: application/json\r\n";
  }
  if (!bearer_token.empty()) {
    headers += xorstr_(L"Authorization: Bearer ") + widen(bearer_token) + L"\r\n";
  }
  auto *body =
      request_body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char *>(request_body.data());
  const auto body_size = static_cast<DWORD>(request_body.size());
  if (!WinHttpSendRequest(handles.request.get(), headers.c_str(),
                          static_cast<DWORD>(headers.size()), body, body_size, body_size, 0) ||
      !WinHttpReceiveResponse(handles.request.get(), nullptr)) {
    throw_http_error("HTTP send/receive");
  }
  DWORD status{};
  DWORD status_size = sizeof(status);
  if (!WinHttpQueryHeaders(
          handles.request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
          WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX)) {
    throw_http_error("WinHttpQueryHeaders");
  }
  const bool created = status == 201 && std::wstring_view(method) == L"POST";
  // The admin API answers some POSTs with 202 Accepted (for example a scheduled shutdown).
  const bool accepted = status == 202 && std::wstring_view(method) == L"POST";
  if (status != 200 && status != 204 && !created && !accepted) {
    throw HttpError("HTTP request failed with status " + std::to_string(status), status,
                    read_error_body(handles.request.get()));
  }
  return handles;
}

// The live API occasionally leaves a connection attempt unanswered. A GET can be repeated
// safely, so connection and response failures (which carry no HTTP status) are retried a few
// times; an HTTP error status is a real answer and is returned at once.
constexpr int kIdempotentGetAttempts = 4;
constexpr auto kRetryPause = std::chrono::milliseconds(300);

HttpRequest open_get_request_with_retry(const std::string &url, int timeout_ms,
                                        std::stop_token stop) {
  for (int attempt = 1;; ++attempt) {
    if (stop.stop_requested()) {
      throw HttpError("Update cancelled");
    }
    try {
      return open_request(url, L"GET", {}, {}, timeout_ms);
    } catch (const HttpError &error) {
      if (error.status() != 0 || attempt >= kIdempotentGetAttempts) {
        throw;
      }
    }
    std::this_thread::sleep_for(kRetryPause);
  }
}

std::string request_text(const std::string &url, const wchar_t *method,
                         const std::string &request_body, const std::string &bearer_token,
                         int timeout_ms, std::stop_token stop = {}) {
  if (stop.stop_requested()) {
    throw HttpError("Update cancelled");
  }
  const bool retryable_get = std::wstring_view(method) == L"GET" && request_body.empty() &&
                             bearer_token.empty();
  auto handles = retryable_get ? open_get_request_with_retry(url, timeout_ms, stop)
                               : open_request(url, method, request_body, bearer_token, timeout_ms);
  std::string body;
  std::array<char, 65536> buffer{};
  for (;;) {
    if (stop.stop_requested()) {
      throw HttpError("Update cancelled");
    }
    DWORD received{};
    if (!WinHttpReadData(handles.request.get(), buffer.data(), static_cast<DWORD>(buffer.size()),
                         &received)) {
      throw_http_error("WinHttpReadData");
    }
    if (received == 0) {
      return body;
    }
    if (received > distribution::kMaximumManifestBytes - body.size()) {
      throw HttpError("HTTP response exceeds its size limit");
    }
    body.append(buffer.data(), received);
  }
}

} // namespace

std::string http_get_text(const std::string &url, int timeout_ms, std::stop_token stop) {
  return request_text(url, L"GET", {}, {}, timeout_ms, stop);
}

nlohmann::json http_get_json(const std::string &url, int timeout_ms,
                             const std::string &bearer_token) {
  return nlohmann::json::parse(request_text(url, L"GET", {}, bearer_token, timeout_ms));
}

nlohmann::json http_post_json(const std::string &url, const nlohmann::json &body,
                              const std::string &bearer_token, int timeout_ms) {
  const auto response = request_text(url, L"POST", body.dump(), bearer_token, timeout_ms);
  return response.empty() ? nlohmann::json::object() : nlohmann::json::parse(response);
}

void http_download_file(const std::string &url, const std::filesystem::path &destination,
                        std::uint64_t expected_bytes,
                        const std::function<void(std::uint64_t)> &progress, std::stop_token stop) {
  if (expected_bytes > distribution::kMaximumPatchFileBytes) {
    throw HttpError("Patch download exceeds its size limit");
  }
  if (stop.stop_requested()) {
    throw HttpError("Patch download cancelled");
  }
  // Synchronous WinHTTP calls cannot safely be closed from another thread. Keep
  // each network wait bounded, then observe cancellation before the next read.
  auto handles = open_get_request_with_retry(url, 3000, stop);
  std::ofstream output(destination, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw HttpError("Cannot create staged patch download");
  }
  std::array<char, 65536> buffer{};
  std::uint64_t completed{};
  for (;;) {
    if (stop.stop_requested()) {
      throw HttpError("Patch download cancelled");
    }
    DWORD received{};
    if (!WinHttpReadData(handles.request.get(), buffer.data(), static_cast<DWORD>(buffer.size()),
                         &received)) {
      throw_http_error("WinHttpReadData");
    }
    if (received == 0) {
      break;
    }
    if (received > expected_bytes - completed) {
      throw HttpError("Patch download is larger than its signed manifest entry");
    }
    output.write(buffer.data(), received);
    if (!output) {
      throw HttpError("Cannot write staged patch download");
    }
    completed += received;
    if (progress) {
      progress(completed);
    }
  }
  output.flush();
  if (!output || completed != expected_bytes) {
    throw HttpError("Patch download is truncated or could not be flushed");
  }
}

} // namespace chronicles::launcher
