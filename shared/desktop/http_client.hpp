#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>

namespace chronicles::launcher {

class HttpError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;

  HttpError(const std::string &message, unsigned status)
      : std::runtime_error(message), status_(status) {}

  // `body` is at most the first 4 KiB of the error response, for JSON error codes.
  HttpError(const std::string &message, unsigned status, std::string body)
      : std::runtime_error(message), status_(status), body_(std::move(body)) {}

  unsigned status() const {
    return status_;
  }

  const std::string &body() const {
    return body_;
  }

private:
  unsigned status_{};
  std::string body_;
};

// HTTPS uses Windows certificate and hostname validation. HTTP is loopback-only.
nlohmann::json http_get_json(const std::string &url, int timeout_ms,
                             const std::string &bearer_token = {});

std::string http_get_text(const std::string &url, int timeout_ms, std::stop_token stop = {});

// Sends JSON only to the selected server. Redirects are disabled so credentials cannot be
// forwarded to another origin by an HTTP response.
nlohmann::json http_post_json(const std::string &url, const nlohmann::json &body,
                              const std::string &bearer_token, int timeout_ms);

void http_download_file(const std::string &url, const std::filesystem::path &destination,
                        std::uint64_t expected_bytes,
                        const std::function<void(std::uint64_t)> &progress,
                        std::stop_token stop = {});

} // namespace chronicles::launcher
