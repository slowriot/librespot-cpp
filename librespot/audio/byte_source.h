#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace librespot::audio {

/// Blocking random-access input for a decoder worker; implementations return zero only at EOF
class byte_source {
public:
  virtual ~byte_source() = default;
  virtual std::uint64_t size() = 0;
  virtual std::size_t read_at(std::uint64_t offset, std::span<std::byte> destination) = 0;
};

} // namespace librespot::audio
