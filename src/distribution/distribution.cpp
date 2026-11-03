#include "distribution/distribution.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>

namespace chronicles::distribution {
namespace {

using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
using PublicKey = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
constexpr std::uint64_t kMaximumReleaseBytes = 1024ULL * 1024 * 1024 * 1024;
constexpr std::size_t kMaximumReleaseFiles = 100000;

bool ascii_alphanumeric(char character) {
  return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
         (character >= '0' && character <= '9');
}

std::string ascii_lower(std::string value) {
  for (auto &character : value) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character + ('a' - 'A'));
    }
  }
  return value;
}

std::string hex_encode(const unsigned char *bytes, std::size_t count) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(count * 2);
  for (std::size_t index = 0; index < count; ++index) {
    result.push_back(digits[bytes[index] >> 4]);
    result.push_back(digits[bytes[index] & 15]);
  }
  return result;
}

std::vector<unsigned char> decode_hex(std::string_view value, std::size_t count) {
  if (value.size() != count * 2) {
    throw std::invalid_argument("Invalid patch signing key or signature length");
  }
  std::vector<unsigned char> result;
  result.reserve(count);
  for (std::size_t index = 0; index < value.size(); index += 2) {
    unsigned byte{};
    const auto parsed = std::from_chars(value.data() + index, value.data() + index + 2, byte, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + index + 2) {
      throw std::invalid_argument("Patch signing key or signature is not hexadecimal");
    }
    result.push_back(static_cast<unsigned char>(byte));
  }
  return result;
}

DigestContext start_digest() {
  DigestContext context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
    throw std::runtime_error("Cannot initialize patch SHA-256");
  }
  return context;
}

std::string finish_digest(EVP_MD_CTX *context) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned digest_size{};
  if (EVP_DigestFinal_ex(context, digest.data(), &digest_size) != 1 || digest_size != 32) {
    throw std::runtime_error("Cannot finish patch SHA-256");
  }
  return hex_encode(digest.data(), digest_size);
}

PatchFile parse_file(const nlohmann::json &document, bool bootstrap) {
  PatchFile file;
  file.sha256 = document.at("sha256").get<std::string>();
  if (!document.at("size").is_number_unsigned()) {
    throw std::invalid_argument("Patch file size must be an unsigned integer");
  }
  file.size = document.at("size").get<std::uint64_t>();
  if (!valid_sha256(file.sha256) || file.size > kMaximumPatchFileBytes) {
    throw std::invalid_argument("Invalid patch file hash or size");
  }
  if (bootstrap) {
    if (file.size == 0 || file.size > kMaximumBootstrapBytes) {
      throw std::invalid_argument("Bootstrap template exceeds its size limit");
    }
  } else {
    file.path = document.at("path").get<std::string>();
    if (!valid_patch_path(file.path)) {
      throw std::invalid_argument("Unsafe patch file path");
    }
  }
  return file;
}

} // namespace

ServiceOrigin parse_service_origin(std::string_view value) {
  if (value.ends_with('/')) {
    value.remove_suffix(1);
  }
  ServiceOrigin origin;
  origin.url = std::string(value);
  origin.secure = value.starts_with("https://");
  const bool plain = value.starts_with("http://");
  if ((!origin.secure && !plain) || value.size() > 512) {
    throw std::invalid_argument("Service origin must use HTTPS (HTTP is loopback-only)");
  }
  auto authority = value.substr(origin.secure ? 8 : 7);
  if (authority.empty() || authority.find_first_of("/@?#\\% \r\n\t") != std::string_view::npos) {
    throw std::invalid_argument("Service origin must contain only a host and optional port");
  }
  std::string_view host;
  std::string_view port_text;
  if (authority.starts_with('[')) {
    const auto close = authority.find(']');
    if (close == std::string_view::npos || authority.substr(0, close + 1) != "[::1]") {
      throw std::invalid_argument("Only the IPv6 loopback literal is supported in origins");
    }
    host = authority.substr(0, close + 1);
    const auto remainder = authority.substr(close + 1);
    if (!remainder.empty()) {
      if (!remainder.starts_with(':')) {
        throw std::invalid_argument("Invalid service origin port");
      }
      port_text = remainder.substr(1);
    }
  } else {
    const auto colon = authority.find(':');
    host = authority.substr(0, colon);
    if (colon != std::string_view::npos) {
      port_text = authority.substr(colon + 1);
    }
    if (host.empty() || host.size() > 253 || host.front() == '.' || host.back() == '.' ||
        host.find("..") != std::string_view::npos || !std::ranges::all_of(host, [](char character) {
          return ascii_alphanumeric(character) || character == '.' || character == '-';
        })) {
      throw std::invalid_argument("Invalid service origin host");
    }
  }
  origin.host = ascii_lower(std::string(host));
  origin.loopback =
      origin.host == "127.0.0.1" || origin.host == "localhost" || origin.host == "[::1]";
  if (!origin.secure && !origin.loopback) {
    throw std::invalid_argument("Remote service origins require HTTPS");
  }
  unsigned port = origin.secure ? 443 : 80;
  if (!port_text.empty()) {
    const auto parsed =
        std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
    if (parsed.ec != std::errc{} || parsed.ptr != port_text.data() + port_text.size() ||
        port == 0 || port > 65535) {
      throw std::invalid_argument("Invalid service origin port");
    }
  } else if (authority.ends_with(':')) {
    throw std::invalid_argument("Service origin port is empty");
  }
  origin.port = static_cast<std::uint16_t>(port);
  return origin;
}

bool valid_channel(std::string_view value) {
  return !value.empty() && value.size() <= 32 && std::ranges::all_of(value, [](char character) {
    return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
           character == '-';
  });
}

bool valid_sha256(std::string_view value) {
  return value.size() == 64 && std::ranges::all_of(value, [](char character) {
           return (character >= 'a' && character <= 'f') || (character >= '0' && character <= '9');
         });
}

bool valid_patch_path(std::string_view value) {
  if (value.empty() || value.size() > 220 || value.front() == '/' || value.back() == '/') {
    return false;
  }
  while (!value.empty()) {
    const auto separator = value.find('/');
    const auto segment = value.substr(0, separator);
    if (segment.empty() || segment == "." || segment == ".." || segment.back() == '.' ||
        segment.back() == ' ' || ascii_lower(std::string(segment)).starts_with(".chronicles") ||
        !std::ranges::all_of(segment, [](char character) {
          return ascii_alphanumeric(character) || character == '.' || character == '_' ||
                 character == '-' || character == ' ';
        })) {
      return false;
    }
    const auto device = ascii_lower(std::string(segment.substr(0, segment.find('.'))));
    if (device == "con" || device == "prn" || device == "aux" || device == "nul" ||
        device == "conin$" || device == "conout$" ||
        (device.size() == 4 && (device.starts_with("com") || device.starts_with("lpt")) &&
         device.back() >= '0' && device.back() <= '9')) {
      return false;
    }
    if (separator == std::string_view::npos) {
      break;
    }
    value.remove_prefix(separator + 1);
  }
  return true;
}

std::string sha256_bytes(std::string_view bytes) {
  auto context = start_digest();
  if (EVP_DigestUpdate(context.get(), bytes.data(), bytes.size()) != 1) {
    throw std::runtime_error("Cannot hash patch content");
  }
  return finish_digest(context.get());
}

std::string sha256_file(const std::filesystem::path &path, std::stop_token stop,
                        const std::function<void(std::uint64_t)> &progress) {
  if (stop.stop_requested()) {
    throw std::runtime_error("Update cancelled during file verification");
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("Cannot open file for patch verification: " + path.string());
  }
  auto context = start_digest();
  std::array<char, 65536> buffer{};
  std::uint64_t completed_bytes = 0;
  std::uint64_t next_progress_bytes = 16ULL * 1024 * 1024;
  while (input) {
    if (stop.stop_requested()) {
      throw std::runtime_error("Update cancelled during file verification");
    }
    input.read(buffer.data(), buffer.size());
    const auto read_bytes = static_cast<std::size_t>(input.gcount());
    if (EVP_DigestUpdate(context.get(), buffer.data(), read_bytes) !=
        1) {
      throw std::runtime_error("Cannot hash patch file");
    }
    completed_bytes += read_bytes;
    if (progress && completed_bytes >= next_progress_bytes) {
      progress(completed_bytes);
      next_progress_bytes = completed_bytes + 16ULL * 1024 * 1024;
    }
  }
  if (!input.eof()) {
    throw std::runtime_error("Cannot read file for patch verification: " + path.string());
  }
  if (progress) {
    progress(completed_bytes);
  }
  return finish_digest(context.get());
}

std::string read_bounded_file(const std::filesystem::path &path, std::size_t maximum_bytes) {
  const auto size = std::filesystem::file_size(path);
  if (size > maximum_bytes) {
    throw std::runtime_error("Distribution file exceeds its size limit");
  }
  std::ifstream input(path, std::ios::binary);
  std::string bytes(size, '\0');
  if (!input || !input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
    throw std::runtime_error("Cannot read distribution file: " + path.string());
  }
  return bytes;
}

Release verify_release(const std::string &envelope, const std::string &public_key_hex,
                       const std::string &channel, const std::string &api_origin) {
  if (envelope.size() > kMaximumManifestBytes) {
    throw std::invalid_argument("Patch manifest exceeds its size limit");
  }
  const auto document = nlohmann::json::parse(envelope);
  const auto payload = document.at("payload").get<std::string>();
  const auto signature = decode_hex(document.at("signature").get<std::string>(), 64);
  const auto key_bytes = decode_hex(public_key_hex, 32);
  PublicKey key(
      EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, key_bytes.data(), key_bytes.size()),
      EVP_PKEY_free);
  DigestContext context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!key || !context ||
      EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, key.get()) != 1 ||
      EVP_DigestVerify(context.get(), signature.data(), signature.size(),
                       reinterpret_cast<const unsigned char *>(payload.data()),
                       payload.size()) != 1) {
    throw std::invalid_argument("Patch manifest signature verification failed");
  }
  const auto manifest = nlohmann::json::parse(payload);
  Release release;
  release.channel = manifest.at("channel").get<std::string>();
  release.version = manifest.at("version").get<std::string>();
  release.api_origin = manifest.at("api_origin").get<std::string>();
  if (manifest.at("format") != 1 || manifest.at("application") != "chronicles" ||
      !valid_channel(release.channel) || release.channel != channel ||
      release.api_origin != parse_service_origin(api_origin).url || release.version.empty() ||
      release.version.size() > 64 || !manifest.at("sequence").is_number_unsigned()) {
    throw std::invalid_argument("Patch manifest does not match this launcher configuration");
  }
  release.sequence = manifest.at("sequence").get<std::uint64_t>();
  if (release.sequence == 0 || release.sequence > static_cast<std::uint64_t>(INT64_MAX)) {
    throw std::invalid_argument("Invalid patch release sequence");
  }
  const auto &files = manifest.at("files");
  if (!files.is_array() || files.empty() || files.size() > kMaximumReleaseFiles) {
    throw std::invalid_argument("Invalid patch manifest file count");
  }
  std::set<std::string> names;
  for (const auto &entry : files) {
    auto file = parse_file(entry, false);
    const auto normalized = ascii_lower(file.path);
    if (!names.insert(normalized).second ||
        file.size > kMaximumReleaseBytes - release.total_bytes) {
      throw std::invalid_argument("Duplicate patch path or excessive release size");
    }
    release.total_bytes += file.size;
    release.files.push_back(std::move(file));
  }
  // A file may not also be a parent directory of another manifest entry.
  for (const auto &name : names) {
    for (auto slash = name.find('/'); slash != std::string::npos;
         slash = name.find('/', slash + 1)) {
      if (names.contains(name.substr(0, slash))) {
        throw std::invalid_argument("Patch file conflicts with a directory");
      }
    }
  }
  if (manifest.contains("bootstrap")) {
    release.bootstrap = parse_file(manifest.at("bootstrap"), true);
  }
  release.payload_sha256 = sha256_bytes(payload);
  return release;
}

PatchCatalog::PatchCatalog(std::filesystem::path directory, std::string public_key_hex,
                           std::string api_origin)
    : directory_(std::filesystem::canonical(directory)), public_key_hex_(std::move(public_key_hex)),
      api_origin_(parse_service_origin(api_origin).url) {
  decode_hex(public_key_hex_, 32);
  if (!std::filesystem::is_directory(directory_ / "channels") ||
      !std::filesystem::is_directory(directory_ / "blobs")) {
    throw std::invalid_argument("Patch directory must contain channels/ and blobs/");
  }
}

std::string PatchCatalog::manifest(const std::string &channel) const {
  if (!valid_channel(channel)) {
    throw std::invalid_argument("Invalid patch channel");
  }
  const auto envelope =
      read_bounded_file(directory_ / "channels" / (channel + ".json"), kMaximumManifestBytes);
  verify_release(envelope, public_key_hex_, channel, api_origin_);
  return envelope;
}

std::filesystem::path PatchCatalog::blob(const std::string &sha256) const {
  if (!valid_sha256(sha256)) {
    throw std::invalid_argument("Invalid patch blob identifier");
  }
  const auto path = directory_ / "blobs" / sha256;
  if (std::filesystem::is_symlink(path) || !std::filesystem::is_regular_file(path) ||
      std::filesystem::file_size(path) > kMaximumPatchFileBytes) {
    throw std::invalid_argument("Patch blob unavailable");
  }
  return path;
}

} // namespace chronicles::distribution
