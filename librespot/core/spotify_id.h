#pragma once
#include <array>
#include <compare>
#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace librespot {

enum class id_error {
  invalid_length,
  invalid_character,
  overflow,
};

class spotify_id {
private:
  std::array<std::byte, 16> value{};

public:
  spotify_id() = default;
  explicit spotify_id(std::array<std::byte, 16> bytes);
  [[nodiscard]] static std::expected<spotify_id, id_error> from_base62(std::string_view text);
  [[nodiscard]] static std::expected<spotify_id, id_error> from_hex(std::string_view text);
  [[nodiscard]] static std::expected<spotify_id, id_error> from_bytes(std::span<std::byte const> bytes);
  [[nodiscard]] std::string to_base62() const;
  [[nodiscard]] std::string to_hex() const;
  [[nodiscard]] std::span<std::byte const, 16> bytes() const noexcept;
  auto operator<=>(spotify_id const &) const = default;
};

} // namespace librespot
