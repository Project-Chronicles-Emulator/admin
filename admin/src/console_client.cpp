#include "console_client.hpp"

#include "distribution/distribution.hpp"
#include "encoding.hpp"
#include "http_client.hpp"
#include "ssh_transport.hpp"
#include "xorstr.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace chronicles::admin_console {
namespace {

using Json = nlohmann::json;

constexpr std::string_view kAdminPathPrefix = "/admin/v1/";
constexpr std::size_t kTokenHexLength = 64;
constexpr std::size_t kMaximumPathLength = 2048;
constexpr int kRequestTimeoutMs = 20000;
constexpr int kConnectTimeoutMs = 5000;

std::string trim(std::string text) {
  const auto not_space = [](unsigned char character) {
    return character != ' ' && character != '\t' && character != '\r' && character != '\n';
  };
  text.erase(text.begin(), std::ranges::find_if(text, not_space));
  text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
  return text;
}

std::string validated_token(std::string token) {
  token = trim(std::move(token));
  const bool hex = std::ranges::all_of(token, [](char character) {
    return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
  });
  if (token.size() != kTokenHexLength || !hex) {
    throw std::invalid_argument("The admin token must be 64 lowercase hexadecimal characters");
  }
  return token;
}

// Accepts an origin such as http://127.0.0.1:8080 (HTTP is loopback-only) or an HTTPS origin.
std::string validated_origin(const std::string &url) {
  auto origin = trim(url);
  while (!origin.empty() && origin.back() == '/') {
    origin.pop_back();
  }
  return distribution::parse_service_origin(origin).url;
}

void validate_path(const std::string &path) {
  const bool traversal = path.find("..") != std::string::npos;
  const bool fragment =
      path.find('#') != std::string::npos || path.find_first_of("\r\n") != std::string::npos;
  if (!path.starts_with(kAdminPathPrefix) || traversal || fragment ||
      path.size() > kMaximumPathLength) {
    throw std::invalid_argument("The console only calls /admin/v1/ routes");
  }
}

} // namespace

std::string read_admin_token_file(const std::filesystem::path &file) {
  std::ifstream input(file, std::ios::binary);
  if (!input) {
    throw std::invalid_argument(
        "The admin token file cannot be opened: " + launcher::narrow(file.wstring()) +
        ". Start the server with --admin-token-file first.");
  }

  std::string token;
  std::getline(input, token);
  return validated_token(std::move(token));
}

ConsoleClient::ConsoleClient(std::filesystem::path settings_file, ConsoleConnection defaults,
                             bool use_defaults_only, std::filesystem::path profiles_file)
    : settings_file_(std::move(settings_file)), defaults_(std::move(defaults)),
      connection_(defaults_), profiles_(Json::array()), ssh_configuration_(Json::object()) {
  if (!profiles_file.empty() && std::filesystem::is_regular_file(profiles_file)) {
    profiles_ = Json::parse(distribution::read_bounded_file(profiles_file, 65536));
    if (!profiles_.is_array() || profiles_.size() > 16) {
      throw std::invalid_argument("Invalid admin server catalog");
    }
  }
  if (profiles_.empty()) {
    profiles_.push_back({{"id", "local"},
                         {"name", "Local testing"},
                         {"server_url", defaults_.server_url},
                         {"token_file", launcher::narrow(defaults_.token_file.wstring())}});
  }
  if (use_defaults_only) {
    return;
  }

  server_id_ = profiles_.front().at("id").get<std::string>();
  std::ifstream input(settings_file_, std::ios::binary);
  if (!input) {
    return;
  }

  // A damaged settings file only loses the saved connection; the defaults remain usable.
  const auto saved = Json::parse(input, nullptr, false);
  if (!saved.is_object()) {
    return;
  }
  server_id_ = saved.value("server_id", server_id_);
  if (saved.contains("server_url") && saved.at("server_url").is_string()) {
    connection_.server_url = saved.at("server_url").get<std::string>();
  }
  if (saved.contains("token_file") && saved.at("token_file").is_string()) {
    connection_.token_file = launcher::widen(saved.at("token_file").get<std::string>());
  }
  // Earlier builds saved the default local connection as "custom". Migrate it
  // to the matching preset without requiring the operator to enter it again.
  if (server_id_ == "custom") {
    for (const auto &profile : profiles_) {
      if (profile.value("server_url", std::string{}) == connection_.server_url &&
          profile.value("token_file", std::string{}) ==
              launcher::narrow(connection_.token_file.wstring())) {
        server_id_ = profile.at("id").get<std::string>();
        break;
      }
    }
  } else if (std::ranges::none_of(
                 profiles_, [&](const auto &profile) { return profile.at("id") == server_id_; })) {
    server_id_ = profiles_.front().at("id").get<std::string>();
  }
}

nlohmann::json ConsoleClient::describe() const {
  std::lock_guard lock(mutex_);
  Json servers = Json::array();
  for (const auto &profile : profiles_) {
    servers.push_back({{"id", profile.at("id")},
                       {"name", profile.at("name")},
                       {"server_url", profile.at("server_url")}});
  }
  return {{"servers", servers},
          {"server_id", server_id_},
          {"server_url", connection_.server_url},
          {"token_file", launcher::narrow(connection_.token_file.wstring())},
          {"default_server_url", defaults_.server_url},
          {"default_token_file", launcher::narrow(defaults_.token_file.wstring())},
          {"connected", !token_.empty() || !ssh_configuration_.empty()}};
}

nlohmann::json ConsoleClient::connect(const nlohmann::json &payload) {
  std::uint64_t generation{};
  {
    std::lock_guard lock(mutex_);
    generation = ++connection_generation_;
  }
  const auto selected_id = payload.value("server_id", std::string("custom"));
  auto selected = payload;
  Json ssh = Json::object();
  if (selected_id != "custom") {
    const auto found = std::ranges::find_if(
        profiles_, [&](const auto &profile) { return profile.at("id") == selected_id; });
    if (found == profiles_.end()) {
      throw std::invalid_argument("Choose an available admin server.");
    }
    selected = *found;
    ssh = selected.value("ssh", Json::object());
  }
  const auto origin = validated_origin(selected.value("server_url", std::string{}));
  const auto pasted_token = selected.value("token", std::string{});
  const auto token_file_text = trim(selected.value("token_file", std::string{}));
  const auto token_file = std::filesystem::path(launcher::widen(token_file_text));

  std::string token;
  if (!ssh.empty()) {
    // The remote proxy reads the server token locally; it is never copied to this PC.
  } else if (!trim(pasted_token).empty()) {
    token = validated_token(pasted_token);
  } else if (!token_file_text.empty()) {
    token = read_admin_token_file(token_file);
  } else {
    throw std::invalid_argument("Choose the server's admin token file or paste the token");
  }

  // Verify before replacing the working connection, so a typo keeps the previous one.
  auto overview =
      ssh.empty() ? launcher::http_get_json(origin + xorstr_("/admin/v1/overview"),
                                            kConnectTimeoutMs, token)
                  : ssh_admin_request(ssh, "GET", xorstr_("/admin/v1/overview"), Json::object());

  {
    std::lock_guard lock(mutex_);
    if (generation != connection_generation_) {
      throw std::runtime_error("A newer server selection replaced this connection attempt.");
    }
    connection_.server_url = origin;
    connection_.token_file = token_file;
    token_ = std::move(token);
    ssh_configuration_ = std::move(ssh);
    server_id_ = selected_id;
  }
  save_settings();
  return overview;
}

nlohmann::json ConsoleClient::request(const std::string &method, const std::string &path,
                                      const nlohmann::json &body) const {
  validate_path(path);

  std::string origin;
  std::string token;
  Json ssh;
  {
    std::lock_guard lock(mutex_);
    origin = connection_.server_url;
    token = token_;
    ssh = ssh_configuration_;
  }
  if (!ssh.empty()) {
    return ssh_admin_request(ssh, method, path, body);
  }
  if (token.empty()) {
    throw std::invalid_argument("Not connected: open Server > Connection first");
  }

  if (method == "GET") {
    return launcher::http_get_json(origin + path, kRequestTimeoutMs, token);
  }
  if (method == "POST") {
    const auto payload = body.is_object() ? body : Json::object();
    return launcher::http_post_json(origin + path, payload, token, kRequestTimeoutMs);
  }
  throw std::invalid_argument("The console only sends GET and POST requests");
}

void ConsoleClient::save_settings() const {
  if (settings_file_.empty()) {
    return;
  }
  Json document;
  {
    std::lock_guard lock(mutex_);
    document = {{"server_id", server_id_},
                {"server_url", connection_.server_url},
                {"token_file", launcher::narrow(connection_.token_file.wstring())}};
  }

  std::filesystem::create_directories(settings_file_.parent_path());
  std::ofstream output(settings_file_, std::ios::binary | std::ios::trunc);
  output << document.dump(2) << '\n';
}

} // namespace chronicles::admin_console
