#include "patch_installer.hpp"
#include "game_process.hpp"

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <set>
#include <stdexcept>

namespace chronicles::launcher {
namespace {

using json = nlohmann::json;
using namespace distribution;
constexpr wchar_t kMarkerName[] = L".chronicles-install.json";
constexpr wchar_t kLauncherMarkerName[] = L".chronicles-launcher-install.json";
constexpr wchar_t kGameUpdateDirectory[] = L".chronicles-update";
constexpr wchar_t kLauncherUpdateDirectory[] = L".chronicles-launcher-update";

std::filesystem::path marker_path(const std::filesystem::path &root,
                                  ExistingDirectory existing_directory) {
  return root / (existing_directory == ExistingDirectory::launcher ? kLauncherMarkerName
                                                                     : kMarkerName);
}

std::filesystem::path update_directory(const std::filesystem::path &root,
                                       ExistingDirectory existing_directory) {
  return root / (existing_directory == ExistingDirectory::launcher ? kLauncherUpdateDirectory
                                                                     : kGameUpdateDirectory);
}

std::filesystem::path journal_path(const std::filesystem::path &root,
                                   ExistingDirectory existing_directory) {
  return update_directory(root, existing_directory) / L"journal.json";
}
// Recovery records include old hashes and backup metadata in addition to manifest entries.
constexpr std::size_t kMaximumJournalBytes = 64 * 1024 * 1024;

struct HandleCloser {
  using pointer = HANDLE;

  void operator()(HANDLE handle) const {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
    }
  }
};

using ScopedHandle = std::unique_ptr<void, HandleCloser>;

void replace_file(const std::filesystem::path &source, const std::filesystem::path &destination) {
  require_regular_patch_path(source);
  require_regular_patch_path(destination);
  if (!MoveFileExW(source.c_str(), destination.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    throw std::runtime_error("Cannot replace patch file (Windows error " +
                             std::to_string(GetLastError()) + ")");
  }
}

void write_document(const std::filesystem::path &path, const json &document) {
  require_regular_patch_path(path);
  const auto temporary = std::filesystem::path(path.wstring() + L".tmp");
  require_regular_patch_path(temporary);
  const auto bytes = document.dump(2);
  ScopedHandle output(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
  if (output.get() == INVALID_HANDLE_VALUE) {
    throw std::runtime_error("Cannot create patch journal or install metadata");
  }
  DWORD written{};
  if (bytes.size() > MAXDWORD ||
      !WriteFile(output.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) ||
      written != bytes.size() || !FlushFileBuffers(output.get())) {
    throw std::runtime_error("Cannot persist patch journal or install metadata");
  }
  output.reset();
  replace_file(temporary, path);
}

std::filesystem::path target_path(const std::filesystem::path &root, const std::string &relative) {
  if (!valid_patch_path(relative)) {
    throw std::runtime_error("Unsafe path in patch install metadata");
  }
  const auto path = root / std::filesystem::path(relative);
  require_regular_patch_path(path);
  return path;
}

json empty_marker(const Release &release) {
  return {{"application", "chronicles"},
          {"channel", release.channel},
          {"sequence", 0},
          {"version", ""},
          {"payload_sha256", ""}};
}

json load_marker(const std::filesystem::path &root, const Release &release,
                 ExistingDirectory existing_directory) {
  const auto path = marker_path(root, existing_directory);
  require_regular_patch_path(path);
  if (!std::filesystem::exists(path)) {
    if (existing_directory == ExistingDirectory::game) {
      const auto client = expected_client_executable(root);
      require_regular_patch_path(client);
      if (!std::filesystem::is_regular_file(client)) {
        throw std::runtime_error("The selected existing game executable is missing");
      }
    }
    if (std::filesystem::exists(root) && existing_directory == ExistingDirectory::managed_only) {
      for (const auto &entry : std::filesystem::directory_iterator(root)) {
        if (entry.path().filename() != L".chronicles-update.lock") {
          throw std::runtime_error(
              "Automatic patching requires an empty game folder or a managed Project Chronicles install");
        }
      }
    }
    return empty_marker(release);
  }
  const auto marker = json::parse(read_bounded_file(path, 65536));
  if (marker.at("application") != "chronicles" || marker.at("channel") != release.channel ||
      !marker.at("sequence").is_number_unsigned()) {
    throw std::runtime_error("Game folder belongs to another application or patch channel");
  }
  return marker;
}

bool file_matches(const std::filesystem::path &path, const PatchFile &file,
                  std::stop_token stop = {},
                  const std::function<void(std::uint64_t)> &progress = {}) {
  require_regular_patch_path(path);
  return std::filesystem::is_regular_file(path) && std::filesystem::file_size(path) == file.size &&
         sha256_file(path, stop, progress) == file.sha256;
}

bool is_steam_compatibility_file(std::string_view path) {
  constexpr std::string_view directory =
      "Engine/Binaries/ThirdParty/Steamworks/Steamv161/Win64/";
  return path.starts_with(directory);
}

bool permits_release(const Release &release, const json &marker) {
  // Realm catalogs may have different bootstrap origins while sharing identical game
  // assets. Retain rollback history independently for each authenticated origin.
  auto installed_sequence = marker.at("sequence").get<std::uint64_t>();
  auto installed_payload = marker.at("payload_sha256").get<std::string>();
  if (marker.contains("origin") && marker.at("origin") != release.api_origin) {
    installed_sequence = 0;
    installed_payload.clear();
    const auto history = marker.value("origins", json::object());
    const auto previous_origin = history.find(release.api_origin);
    if (previous_origin != history.end()) {
      installed_sequence = previous_origin->at("sequence").get<std::uint64_t>();
      installed_payload = previous_origin->at("payload_sha256").get<std::string>();
    }
  }
  return release.sequence > installed_sequence ||
         (release.sequence == installed_sequence && installed_payload == release.payload_sha256);
}

std::string file_set_fingerprint(const Release &release) {
  auto files = release.files;
  std::ranges::sort(files, {}, &PatchFile::path);
  auto entries = json::array();
  for (const auto &file : files) {
    entries.push_back({{"path", file.path}, {"sha256", file.sha256}, {"size", file.size}});
  }
  return sha256_bytes(entries.dump());
}

PatchPlan make_plan(const std::filesystem::path &root, const Release &release, const json &marker,
                    std::stop_token stop, const PatchProgress &progress = {}) {
  if (!valid_sha256(release.payload_sha256) || !valid_channel(release.channel) ||
      release.sequence == 0 || release.files.empty()) {
    throw std::invalid_argument("Invalid verified release metadata");
  }
  if (!permits_release(release, marker)) {
    throw std::runtime_error("Refusing a rollback or conflicting patch release sequence");
  }
  PatchPlan plan;
  std::uint64_t verified_bytes{};
  for (const auto &file : release.files) {
    if (progress) {
      progress(verified_bytes, release.total_bytes, file.path, "checking");
    }
    if (stop.stop_requested()) {
      throw std::runtime_error("Update cancelled during file verification");
    }
    if (!valid_sha256(file.sha256) || file.size > kMaximumPatchFileBytes) {
      throw std::invalid_argument("Invalid verified patch file");
    }
    const auto report_file_progress = [&](std::uint64_t completed_in_file) {
      if (progress) {
        progress(verified_bytes + completed_in_file, release.total_bytes, file.path,
                 "checking");
      }
    };
    if (!file_matches(target_path(root, file.path), file, stop, report_file_progress)) {
      plan.changed.push_back(file);
      plan.download_bytes += file.size;
    }
    verified_bytes += file.size;
  }
  return plan;
}

ScopedHandle lock_installation(const std::filesystem::path &root) {
  require_regular_patch_path(root);
  std::filesystem::create_directories(root);
  const auto lock_path = root / L".chronicles-update.lock";
  require_regular_patch_path(lock_path);
  ScopedHandle lock(CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (lock.get() == INVALID_HANDLE_VALUE) {
    throw std::runtime_error(
        "Another launcher is updating this installation, or it is not writable");
  }
  return lock;
}

void copy_verified(const std::filesystem::path &source, const std::filesystem::path &destination,
                   const std::string &expected_sha256) {
  require_regular_patch_path(source);
  require_regular_patch_path(destination);
  std::filesystem::create_directories(destination.parent_path());
  std::filesystem::copy_file(source, destination,
                             std::filesystem::copy_options::overwrite_existing);
  if (sha256_file(destination) != expected_sha256) {
    throw std::runtime_error("Copied patch file did not pass SHA-256 verification");
  }
  ScopedHandle output(CreateFileW(destination.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (output.get() == INVALID_HANDLE_VALUE || !FlushFileBuffers(output.get())) {
    throw std::runtime_error("Cannot flush a verified patch copy to disk");
  }
}

void recover(const std::filesystem::path &root, ExistingDirectory existing_directory) {
  const auto journal_path = chronicles::launcher::journal_path(root, existing_directory);
  require_regular_patch_path(journal_path);
  if (!std::filesystem::exists(journal_path)) {
    return;
  }
  const auto journal = json::parse(read_bounded_file(journal_path, kMaximumJournalBytes));
  const auto transaction_hash = journal.at("payload_sha256").get<std::string>();
  if (!valid_sha256(transaction_hash) || !journal.at("files").is_array() ||
      journal.at("files").size() > 100000 || journal.at("application") != "chronicles") {
    throw std::runtime_error("Invalid patch recovery journal; manual repair is required");
  }
  const auto transaction = update_directory(root, existing_directory) / L"transactions" / transaction_hash;
  require_regular_patch_path(transaction);
  const auto installed = json::parse(read_bounded_file(marker_path(root, existing_directory), 65536));
  if (installed == journal.at("next")) {
    // Marker replacement is the commit point. Verify the completed transaction before clearing it.
    bool complete = true;
    for (const auto &entry : journal.at("files")) {
      const auto relative = entry.at("path").get<std::string>();
      PatchFile file{relative, entry.at("sha256").get<std::string>(),
                     entry.at("size").get<std::uint64_t>()};
      complete = complete && file_matches(target_path(root, relative), file);
    }
    if (complete) {
      std::filesystem::remove(journal_path);
      return;
    }
  }
  for (const auto &entry : journal.at("files")) {
    const auto relative = entry.at("path").get<std::string>();
    const auto target = target_path(root, relative);
    const auto old_hash = entry.at("old_sha256").get<std::string>();
    const auto new_hash = entry.at("sha256").get<std::string>();
    if ((!old_hash.empty() && !valid_sha256(old_hash)) || !valid_sha256(new_hash)) {
      throw std::runtime_error("Invalid recovery file fingerprint");
    }
    const auto current_hash = std::filesystem::exists(target) ? sha256_file(target) : "";
    if (current_hash == old_hash) {
      continue;
    }
    if (!current_hash.empty() && current_hash != new_hash) {
      throw std::runtime_error("A patched file was changed outside the launcher; recovery stopped");
    }
    if (old_hash.empty()) {
      if (std::filesystem::exists(target)) {
        std::filesystem::remove(target);
      }
    } else {
      const auto backup = target_path(transaction / "backup", relative);
      const auto restore = transaction / "restore.tmp";
      copy_verified(backup, restore, old_hash);
      replace_file(restore, target);
    }
  }
  write_document(marker_path(root, existing_directory), journal.at("previous"));
  std::filesystem::remove(journal_path);
}

void clean_committed_staging(const std::filesystem::path &update_root,
                             const std::filesystem::path &transaction, const json &journal) {
  // Delete only recorded backup files, never the player's directory or an inferred tree.
  for (const auto &entry : journal.at("files")) {
    const auto relative = entry.at("path").get<std::string>();
    const auto backup = target_path(transaction / "backup", relative);
    std::filesystem::remove(backup);
  }
  for (const auto *name : {"install.tmp", "restore.tmp"}) {
    const auto path = transaction / name;
    require_regular_patch_path(path);
    std::filesystem::remove(path);
  }
  constexpr std::uint64_t kMaximumCacheBytes = 1024ULL * 1024 * 1024;
  std::uint64_t cache_bytes{};
  std::vector<std::filesystem::directory_entry> cached_files;
  for (const auto &entry : std::filesystem::directory_iterator(update_root / "blobs")) {
    require_regular_patch_path(entry.path());
    const auto name = entry.path().filename().string();
    if (name.ends_with(".part") && valid_sha256(name.substr(0, name.size() - 5))) {
      std::filesystem::remove(entry.path());
    } else if (valid_sha256(name) && entry.is_regular_file()) {
      cache_bytes += entry.file_size();
      cached_files.push_back(entry);
    }
  }
  std::ranges::sort(cached_files, [](const auto &left, const auto &right) {
    return left.last_write_time() < right.last_write_time();
  });
  for (const auto &entry : cached_files) {
    if (cache_bytes <= kMaximumCacheBytes) {
      break;
    }
    cache_bytes -= entry.file_size();
    std::filesystem::remove(entry.path());
  }
}

} // namespace

std::filesystem::path launcher_update_journal_path(const std::filesystem::path &root) {
  return journal_path(root, ExistingDirectory::launcher);
}

void require_regular_patch_path(const std::filesystem::path &path) {
  const auto absolute = std::filesystem::absolute(path).lexically_normal();
  std::filesystem::path current;
  for (const auto &component : absolute) {
    current /= component;
    const auto attributes = GetFileAttributesW(current.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      throw std::runtime_error("Patch paths may not contain symlinks, junctions or reparse points");
    }
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      const auto error = GetLastError();
      if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND &&
          error != ERROR_INVALID_NAME) {
        throw std::runtime_error("Cannot inspect patch path (Windows error " +
                                 std::to_string(error) + ")");
      }
    }
  }
}

PatchInstaller::PatchInstaller(std::filesystem::path root, ExistingDirectory existing_directory)
    : existing_directory_(existing_directory) {
  if (root.empty()) {
    throw std::invalid_argument("Choose a game installation directory first");
  }
  root_ = std::filesystem::absolute(root).lexically_normal();
  if (root_ == root_.root_path() || root_.wstring().starts_with(L"\\\\")) {
    throw std::invalid_argument("Choose a dedicated local game folder");
  }
  require_regular_patch_path(root_);
}

PatchPlan PatchInstaller::inspect(const Release &release, std::stop_token stop,
                                  const PatchProgress &progress) {
  if (std::filesystem::exists(journal_path(root_, existing_directory_))) {
    auto lock = lock_installation(root_);
    recover(root_, existing_directory_);
  }
  return make_plan(root_, release, load_marker(root_, release, existing_directory_), stop, progress);
}

void PatchInstaller::recover_pending() {
  if (!std::filesystem::exists(journal_path(root_, existing_directory_))) {
    return;
  }

  auto lock = lock_installation(root_);
  recover(root_, existing_directory_);
}

bool PatchInstaller::is_current_release(const Release &release) const {
  if (std::filesystem::exists(journal_path(root_, existing_directory_))) {
    return false;
  }
  const auto marker = load_marker(root_, release, existing_directory_);
  if (!permits_release(release, marker)) {
    return false;
  }
  const bool same_release = marker.value("origin", std::string{}) == release.api_origin &&
                            marker.at("sequence") == release.sequence &&
                            marker.at("payload_sha256") == release.payload_sha256;
  const bool same_file_set = marker.value("files_sha256", std::string{}) ==
                             file_set_fingerprint(release);
  if (!same_release && !same_file_set) {
    return false;
  }
  // The signed payload must match the completed installation marker. Explicit
  // verification still hashes every byte to detect same-size local modifications.
  for (const auto &file : release.files) {
    const auto path = target_path(root_, file.path);
    if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) != file.size) {
      return false;
    }
    // A same-size replacement of the Steam compatibility payload breaks the game before
    // account login. Hash only this small launch-critical subtree on the fast path.
    if (is_steam_compatibility_file(file.path) && sha256_file(path) != file.sha256) {
      return false;
    }
  }
  return true;
}

void PatchInstaller::remember_current_release(const Release &release) {
  auto lock = lock_installation(root_);
  if (!is_current_release(release)) {
    throw std::runtime_error("Game files changed before the verified release could be recorded");
  }

  auto marker = load_marker(root_, release, existing_directory_);
  const auto fingerprint = file_set_fingerprint(release);
  const auto origin_record = json{{"sequence", release.sequence},
                                  {"payload_sha256", release.payload_sha256}};
  if (marker.value("origin", std::string{}) == release.api_origin &&
      marker.value("files_sha256", std::string{}) == fingerprint &&
      marker.value("origins", json::object()).value(release.api_origin, json::object()) ==
          origin_record) {
    return;
  }

  marker["origin"] = release.api_origin;
  marker["origins"][release.api_origin] = origin_record;
  marker["sequence"] = release.sequence;
  marker["version"] = release.version;
  marker["payload_sha256"] = release.payload_sha256;
  marker["files_sha256"] = fingerprint;
  write_document(marker_path(root_, existing_directory_), marker);
}

void PatchInstaller::install(const Release &release, const PatchDownload &download,
                             const PatchProgress &progress, std::stop_token stop,
                             const std::function<void()> &require_game_closed) {
  auto lock = lock_installation(root_);
  require_game_closed();
  recover(root_, existing_directory_);
  const auto previous = load_marker(root_, release, existing_directory_);
  const auto plan = make_plan(root_, release, previous, stop, progress);
  if (!std::filesystem::exists(marker_path(root_, existing_directory_))) {
    write_document(marker_path(root_, existing_directory_), previous);
  }
  const auto cache = update_directory(root_, existing_directory_) / L"blobs";
  const auto transaction = update_directory(root_, existing_directory_) / L"transactions" / release.payload_sha256;
  require_regular_patch_path(cache);
  require_regular_patch_path(transaction);
  std::filesystem::create_directories(cache);
  std::filesystem::create_directories(transaction);

  std::uint64_t required_space = plan.download_bytes;
  for (const auto &file : plan.changed) {
    const auto target = target_path(root_, file.path);
    if (std::filesystem::exists(target)) {
      required_space += std::filesystem::file_size(target);
    }
    required_space += file.size;
  }
  if (std::filesystem::space(root_).available < required_space) {
    throw std::runtime_error("Not enough free disk space for staged patches and rollback backups");
  }
  std::uint64_t completed{};
  json journal{{"application", "chronicles"},
               {"payload_sha256", release.payload_sha256},
               {"previous", previous},
               {"files", json::array()}};
  for (const auto &file : plan.changed) {
    if (stop.stop_requested()) {
      throw std::runtime_error("Update cancelled before installation");
    }
    const auto staged = cache / file.sha256;
    require_regular_patch_path(staged);
    if (!file_matches(staged, file, stop)) {
      const auto partial = std::filesystem::path(staged.wstring() + L".part");
      require_regular_patch_path(partial);
      download(
          file, partial,
          [&](std::uint64_t received) {
            progress(completed + received, plan.download_bytes, file.path, "downloading");
          },
          stop);
      if (!file_matches(partial, file, stop)) {
        throw std::runtime_error("Downloaded patch failed size or SHA-256 verification");
      }
      replace_file(partial, staged);
    }
    completed += file.size;
    progress(completed, plan.download_bytes, file.path, "verified");
    const auto target = target_path(root_, file.path);
    const auto old_hash = std::filesystem::exists(target) ? sha256_file(target) : "";
    if (!old_hash.empty()) {
      copy_verified(target, target_path(transaction / "backup", file.path), old_hash);
    }
    journal["files"].push_back({{"path", file.path},
                                {"sha256", file.sha256},
                                {"size", file.size},
                                {"old_sha256", old_hash}});
  }
  auto next = previous;
  next["origin"] = release.api_origin;
  next["origins"][release.api_origin] = {{"sequence", release.sequence},
                                         {"payload_sha256", release.payload_sha256}};
  next["sequence"] = release.sequence;
  next["version"] = release.version;
  next["payload_sha256"] = release.payload_sha256;
  next["files_sha256"] = file_set_fingerprint(release);
  journal["next"] = next;
  if (stop.stop_requested()) {
    throw std::runtime_error("Update cancelled before installation");
  }
  require_game_closed();
  write_document(journal_path(root_, existing_directory_), journal);
  try {
    for (const auto &entry : journal.at("files")) {
      require_game_closed();
      const auto relative = entry.at("path").get<std::string>();
      const auto target = target_path(root_, relative);
      const auto current_hash = std::filesystem::exists(target) ? sha256_file(target) : "";
      if (current_hash != entry.at("old_sha256").get<std::string>()) {
        throw std::runtime_error("A game file changed while the patch was downloading");
      }
      const auto temporary = transaction / "install.tmp";
      copy_verified(cache / entry.at("sha256").get<std::string>(), temporary,
                    entry.at("sha256").get<std::string>());
      std::filesystem::create_directories(target.parent_path());
      replace_file(temporary, target);
      progress(completed, plan.download_bytes, relative, "installing");
    }
    write_document(marker_path(root_, existing_directory_), next);
    std::filesystem::remove(journal_path(root_, existing_directory_));
  } catch (...) {
    recover(root_, existing_directory_);
    throw;
  }
  try {
    clean_committed_staging(update_directory(root_, existing_directory_), transaction, journal);
  } catch (const std::exception &error) {
    // Installation is already committed. A cache cleanup failure must not undo verified files.
    progress(completed, plan.download_bytes, error.what(), "cache_warning");
  }
  progress(completed, plan.download_bytes, release.version, "ready");
}

} // namespace chronicles::launcher
