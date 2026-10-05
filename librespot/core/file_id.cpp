#include "file_id.h"
#include <algorithm>

namespace librespot {

file_id::file_id(std::array<std::byte, 20> bytes) : value{bytes} {
  /// Keep the canonical twenty-byte identifier used for storage and audio keys
}

std::expected<file_id, id_error> file_id::from_hex(std::string_view text) {
  if(text.size() != 40) return std::unexpected{id_error::invalid_length};
  std::array<std::byte, 20> result{};
  std::string_view constexpr digits{"0123456789abcdef"};
  for(std::size_t index{0}; index < text.size(); ++index) {
    auto character{text[index]};
    if(character >= 'A' && character <= 'F') character = static_cast<char>(character - 'A' + 'a');
    auto const digit{digits.find(character)};
    if(digit == std::string_view::npos) return std::unexpected{id_error::invalid_character};
    result[index / 2] |= static_cast<std::byte>(digit << (index % 2 == 0 ? 4 : 0));
  }
  return file_id{result};
}

std::expected<file_id, id_error> file_id::from_bytes(std::span<std::byte const> bytes) {
  /// Preserve compatibility with upstream's zero-padded sixteen-byte file IDs
  if(bytes.size() != 16 && bytes.size() != 20) return std::unexpected{id_error::invalid_length};
  std::array<std::byte, 20> result{};
  std::ranges::copy(bytes, result.begin());
  return file_id{result};
}

std::string file_id::to_hex() const {
  std::string_view constexpr digits{"0123456789abcdef"};
  std::string result(40, '0');
  for(std::size_t index{0}; index < value.size(); ++index) {
    auto const byte{std::to_integer<unsigned int>(value[index])};
    result[index * 2] = digits[byte >> 4];
    result[index * 2 + 1] = digits[byte & 15u];
  }
  return result;
}

std::span<std::byte const, 20> file_id::bytes() const noexcept {
  return value;
}

} // namespace librespot
