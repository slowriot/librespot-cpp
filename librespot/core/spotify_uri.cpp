#include "spotify_uri.h"
#include <array>
#include <charconv>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace librespot {
namespace {

std::array<std::string_view, 6> constexpr item_names{"album", "artist", "episode", "playlist", "show", "track"};

std::vector<std::string_view> split(std::string_view text) {
  /// Preserve empty local-file fields and reject surplus fields in the parser
  std::vector<std::string_view> parts;
  for(;;) {
    auto const separator{text.find(':')};
    parts.push_back(text.substr(0, separator));
    if(separator == std::string_view::npos) break;
    text.remove_prefix(separator + 1);
  }
  return parts;
}

} // anonymous namespace

std::string_view to_string(item_type type) {
  /// Map catalog resource types to their wire spelling
  auto const index{static_cast<std::size_t>(type)};
  if(index >= item_names.size()) throw std::invalid_argument{"invalid catalog item type"};
  return item_names[index];
}

std::expected<spotify_uri, uri_error> parse_uri(std::string_view text) {
  /// Keep local fields encoded, as on the wire, and preserve unknown resource kinds
  auto const parts{split(text)};
  if(parts.front() != "spotify") return std::unexpected{uri_error::invalid_scheme};
  if(parts.size() < 3 || parts[1].empty()) return std::unexpected{uri_error::invalid_format};
  if(parts[1] == "local") {
    if(parts.size() != 6) return std::unexpected{uri_error::invalid_format};
    std::chrono::seconds::rep duration{0};
    auto const [end, error]{std::from_chars(parts[5].data(), parts[5].data() + parts[5].size(), duration)};
    if(error != std::errc{} || end != parts[5].data() + parts[5].size() || duration < 0) {
      return std::unexpected{uri_error::invalid_duration};
    }
    return local_uri{
      .artist{parts[2]},
      .album{parts[3]},
      .title{parts[4]},
      .duration{duration},
    };
  }
  bool const named{parts[1] == "user"};
  if(parts.size() != (named ? 5u : 3u)) return std::unexpected{uri_error::invalid_format};
  auto const kind{parts[named ? 3u : 1u]};
  auto const resource{parts[named ? 4u : 2u]};
  if(named && (kind != "playlist" || parts[2].empty())) return std::unexpected{uri_error::invalid_format};
  for(std::size_t index{0}; index < item_names.size(); ++index) {
    if(kind != item_names[index]) continue;
    auto const id{spotify_id::from_base62(resource)};
    if(!id) return std::unexpected{uri_error::invalid_id};
    return catalog_uri{
      .type{static_cast<item_type>(index)},
      .id{*id},
      .user{named ? std::optional<std::string>{parts[2]} : std::nullopt},
    };
  }
  if(kind.empty() || resource.empty()) return std::unexpected{uri_error::invalid_format};
  return unknown_uri{.kind{kind}, .resource{resource}};
}

std::string to_string(spotify_uri const &uri) {
  /// Serialise all variants without discarding unknown resource kinds
  return std::visit([](auto const &resource)->std::string {
    using T = std::remove_cvref_t<decltype(resource)>;
    if constexpr(std::is_same_v<T, catalog_uri>) {
      if(resource.user && resource.type != item_type::playlist) throw std::invalid_argument{"only playlist URIs may have a user"};
      auto prefix{resource.user ? "spotify:user:" + *resource.user + ":" : "spotify:"};
      return prefix + std::string{to_string(resource.type)} + ":" + resource.id.to_base62();
    } else if constexpr(std::is_same_v<T, local_uri>) {
      if(resource.duration.count() < 0) throw std::invalid_argument{"negative local file duration"};
      return "spotify:local:" + resource.artist + ":" + resource.album + ":" + resource.title + ":" + std::to_string(resource.duration.count());
    } else {
      return "spotify:" + resource.kind + ":" + resource.resource;
    }
  }, uri);
}

bool is_playable(spotify_uri const &uri) noexcept {
  /// Local files, tracks and episodes can provide audio
  if(std::holds_alternative<local_uri>(uri)) return true;
  auto const resource{std::get_if<catalog_uri>(&uri)};
  return resource && (resource->type == item_type::track || resource->type == item_type::episode);
}

} // namespace librespot
