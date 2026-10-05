#pragma once
#include <chrono>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include "spotify_id.h"

namespace librespot {

enum class item_type {
  album,
  artist,
  episode,
  playlist,
  show,
  track,
};

struct catalog_uri {
  item_type type{item_type::track};
  spotify_id id;
  std::optional<std::string> user;
  bool operator==(catalog_uri const &) const = default;
};

struct local_uri {
  std::string artist;
  std::string album;
  std::string title;
  std::chrono::seconds duration{0};
  bool operator==(local_uri const &) const = default;
};

struct unknown_uri {
  std::string kind;
  std::string resource;
  bool operator==(unknown_uri const &) const = default;
};

using spotify_uri = std::variant<catalog_uri, local_uri, unknown_uri>;

enum class uri_error {
  invalid_scheme,
  invalid_format,
  invalid_id,
  invalid_duration,
};

[[nodiscard]] std::expected<spotify_uri, uri_error> parse_uri(std::string_view text);
[[nodiscard]] std::string to_string(spotify_uri const &uri);
[[nodiscard]] bool is_playable(spotify_uri const &uri) noexcept;
[[nodiscard]] std::string_view to_string(item_type type);

} // namespace librespot
