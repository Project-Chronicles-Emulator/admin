#pragma once

#include "distribution/distribution.hpp"

#include <filesystem>
#include <functional>
#include <stop_token>
#include <vector>

namespace chronicles::launcher {

struct PatchPlan {
  std::vector<distribution::PatchFile> changed;
  std::uint64_t download_bytes{};
};

using PatchProgress = std::function<void(std::uint64_t completed, std::uint64_t total,
                                         const std::string &path, const std::string &phase)>;
using PatchDownload = std::function<void(
    const distribution::PatchFile &file, const std::filesystem::path &destination,
    const std::function<void(std::uint64_t)> &progress, std::stop_token stop)>;

enum class ExistingDirectory {
  managed_only,
  game,
  launcher,
};

// Owns only signed manifest paths in an explicitly selected, managed installation directory.
// A journal and same-volume replacements make interrupted commits recoverable on the next run.
class PatchInstaller {
public:
  explicit PatchInstaller(std::filesystem::path root,
                          ExistingDirectory existing_directory = ExistingDirectory::managed_only);

  PatchPlan inspect(const distribution::Release &release, std::stop_token stop = {},
                    const PatchProgress &progress = {});

  // Recover an interrupted transaction before attempting another update.
  void recover_pending();

  // Fast startup check of a previously installed release, not a content checksum scan.
  bool is_current_release(const distribution::Release &release) const;

  // Record an equivalent signed file set and per-origin sequence without rehashing the game.
  void remember_current_release(const distribution::Release &release);

  void install(const distribution::Release &release, const PatchDownload &download,
               const PatchProgress &progress, std::stop_token stop,
               const std::function<void()> &require_game_closed);

private:
  std::filesystem::path root_;
  ExistingDirectory existing_directory_;
};

// The launcher and game maintain separate journals when their files share a directory.
std::filesystem::path launcher_update_journal_path(const std::filesystem::path &root);

// Validates every existing parent and the leaf against symlinks and Windows reparse points.
void require_regular_patch_path(const std::filesystem::path &path);

} // namespace chronicles::launcher
