#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

namespace chronicles::admin_console {

// Where the console connects. Only the URL and token file path are saved; a pasted token is
// kept in memory for the session.
struct ConsoleConnection {
  std::string server_url;
  std::filesystem::path token_file;
};

// The console's single route to the server: every request goes to /admin/v1 on the configured
// origin with the operator token. The web UI never sees the token.
class ConsoleClient {
public:
  // `use_defaults_only` ignores the saved connection, so explicit command-line options win.
  ConsoleClient(std::filesystem::path settings_file, ConsoleConnection defaults,
                bool use_defaults_only = false, std::filesystem::path profiles_file = {});

  // Saved connection (falls back to the defaults) plus the defaults, for the settings form.
  nlohmann::json describe() const;

  // Validates the URL and token (file or pasted), checks them against /admin/v1/overview and
  // saves the URL and token file on success. Returns the overview. Throws on any failure.
  nlohmann::json connect(const nlohmann::json &payload);

  // Proxies one admin API call. `path` must start with /admin/v1/. Throws HttpError for an
  // HTTP failure and std::invalid_argument for a rejected request.
  nlohmann::json request(const std::string &method, const std::string &path,
                         const nlohmann::json &body) const;

private:
  void save_settings() const;

  const std::filesystem::path settings_file_;
  const ConsoleConnection defaults_;

  mutable std::mutex mutex_;
  ConsoleConnection connection_;
  std::string token_;
  nlohmann::json profiles_;
  nlohmann::json ssh_configuration_;
  std::string server_id_{"custom"};
  std::uint64_t connection_generation_{};
};

// Reads a 64-lowercase-hex admin token from the first line of `file`.
std::string read_admin_token_file(const std::filesystem::path &file);

} // namespace chronicles::admin_console
