#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace librespot::audio {

using audio_key = std::array<std::byte, 16>;

/// Decrypt a range in place; offset is relative to the original encrypted file
void decrypt(audio_key const &key, std::uint64_t offset, std::span<std::byte> data);

} // namespace librespot::audio
