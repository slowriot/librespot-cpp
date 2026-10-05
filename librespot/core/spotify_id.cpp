#include "spotify_id.h"
#include <algorithm>
#include <cstdint>
#include <ranges>

namespace librespot {
namespace {

std::string_view constexpr base62_digits{"0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"};
std::string_view constexpr hex_digits{"0123456789abcdef"};

} // anonymous namespace

spotify_id::spotify_id(std::array<std::byte, 16> bytes) : value{bytes} {
  /// Store the identifier in protocol byte order
}

std::expected<spotify_id, id_error> spotify_id::from_base62(std::string_view text) {
  /// Parse with bounded arithmetic so inputs exceeding 128 bits cannot wrap
  if(text.size() != 22) return std::unexpected{id_error::invalid_length};
  std::array<std::byte, 16> result{};
  for(char character : text) {
    auto const digit{base62_digits.find(character)};
    if(digit == std::string_view::npos) return std::unexpected{id_error::invalid_character};
    auto carry{static_cast<unsigned int>(digit)};
    for(auto &byte : result | std::views::reverse) {
      carry += std::to_integer<unsigned int>(byte) * 62u;
      byte = static_cast<std::byte>(carry & 255u);
      carry >>= 8;
    }
    if(carry != 0) return std::unexpected{id_error::overflow};
  }
  return spotify_id{result};
}

std::expected<spotify_id, id_error> spotify_id::from_hex(std::string_view text) {
  /// Accept upper and lowercase hexadecimal, emitting lowercase canonically
  if(text.size() != 32) return std::unexpected{id_error::invalid_length};
  std::array<std::byte, 16> result{};
  for(std::size_t index{0}; index < text.size(); ++index) {
    char character{text[index]};
    if(character >= 'A' && character <= 'F') character = static_cast<char>(character - 'A' + 'a');
    auto const digit{hex_digits.find(character)};
    if(digit == std::string_view::npos) return std::unexpected{id_error::invalid_character};
    result[index / 2] |= static_cast<std::byte>(digit << (index % 2 == 0 ? 4 : 0));
  }
  return spotify_id{result};
}

std::expected<spotify_id, id_error> spotify_id::from_bytes(std::span<std::byte const> bytes) {
  /// Require the exact identifier size rather than silently padding or truncating
  if(bytes.size() != 16) return std::unexpected{id_error::invalid_length};
  std::array<std::byte, 16> result{};
  std::ranges::copy(bytes, result.begin());
  return spotify_id{result};
}

std::string spotify_id::to_base62() const {
  /// Divide bytewise using native arithmetic and retain leading zero digits
  auto remaining{value};
  std::string result(22, '0');
  for(auto &character : result | std::views::reverse) {
    unsigned int remainder{0};
    for(auto &byte : remaining) {
      auto const dividend{remainder * 256u + std::to_integer<unsigned int>(byte)};
      byte = static_cast<std::byte>(dividend / 62u);
      remainder = dividend % 62u;
    }
    character = base62_digits[remainder];
  }
  return result;
}

std::string spotify_id::to_hex() const {
  /// Emit exactly 32 hexadecimal characters
  std::string result(32, '0');
  for(std::size_t index{0}; index < value.size(); ++index) {
    auto const byte{std::to_integer<unsigned int>(value[index])};
    result[index * 2] = hex_digits[byte >> 4];
    result[index * 2 + 1] = hex_digits[byte & 15u];
  }
  return result;
}

std::span<std::byte const, 16> spotify_id::bytes() const noexcept {
  /// Return a view whose lifetime is bounded by this identifier
  return value;
}

} // namespace librespot
