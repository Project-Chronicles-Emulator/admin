#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace chronicles::distribution {

inline constexpr std::size_t kMaximumManifestBytes = 8 * 1024 * 1024;
inline constexpr std::uint64_t kMaximumPatchFileBytes = 64ULL * 1024 * 1024 * 1024;
inline constexpr std::uint64_t kMaximumBootstrapBytes = 16 * 1024 * 1024;
inline constexpr std::string_view kBlobRoute = "/distribution/v1/blobs/";

struct ServiceOrigin {
  std::string url;
  std::string host;
  std::uint16_t port{};
  bool secure{};
  bool loopback{};
};

// Remote origins require HTTPS. Userinfo, paths, queries and fragments are rejected.
ServiceOrigin parse_service_origin(std::string_view value);

bool valid_channel(std::string_view value);

bool valid_sha256(std::string_view value);

bool valid_patch_path(std::string_view value);

std::string sha256_bytes(std::string_view bytes);

std::string sha256_file(const std::filesystem::path &path, std::stop_token stop = {},
                        const std::function<void(std::uint64_t)> &progress = {});

std::string read_bounded_file(const std::filesystem::path &path, std::size_t maximum_bytes);

struct PatchFile {
  std::string path;
  std::string sha256;
  std::uint64_t size{};
};

struct Release {
  std::string channel;
  std::string version;
  std::string api_origin;
  std::string payload_sha256;
  std::uint64_t sequence{};
  std::uint64_t total_bytes{};
  std::vector<PatchFile> files;
  std::optional<PatchFile> bootstrap;
};

// The signature covers the exact UTF-8 payload string, not reserialized JSON.
Release verify_release(const std::string &envelope, const std::string &public_key_hex,
                       const std::string &channel, const std::string &api_origin);

class PatchCatalog {
public:
  PatchCatalog(std::filesystem::path directory, std::string public_key_hex, std::string api_origin);

  std::string manifest(const std::string &channel) const;

  std::filesystem::path blob(const std::string &sha256) const;

private:
  std::filesystem::path directory_;
  std::string public_key_hex_;
  std::string api_origin_;
};

} // namespace chronicles::distribution
