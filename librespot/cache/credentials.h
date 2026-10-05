#pragma once
#include <filesystem>
#include <optional>
#include "librespot/net/access_point.h"

namespace librespot::cache {

/// Linux credential files are owner-only, written atomically, and never opened through symlinks
void save_credentials(std::filesystem::path const &path, librespot::credentials const &credentials);
[[nodiscard]] std::optional<librespot::credentials> load_credentials(std::filesystem::path const &path);

} // namespace librespot::cache
