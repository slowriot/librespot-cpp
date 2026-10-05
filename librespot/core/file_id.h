#pragma once
#include <array>
#include <compare>
#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include "spotify_id.h"

namespace librespot {

class file_id {
private:
  std::array<std::byte, 20> value{};

public:
  file_id() = default;
  explicit file_id(std::array<std::byte, 20> bytes);
  [[nodiscard]] static std::expected<file_id, id_error> from_hex(std::string_view text);
  [[nodiscard]] static std::expected<file_id, id_error> from_bytes(std::span<std::byte const> bytes);
  [[nodiscard]] std::string to_hex() const;
  [[nodiscard]] std::span<std::byte const, 20> bytes() const noexcept;
  auto operator<=>(file_id const &) const = default;
};

} // namespace librespot
