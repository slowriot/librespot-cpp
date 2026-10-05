#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace librespot::crypto {

/// Spotify access-point cipher; use only with the protocol's authenticated framing
class shannon {
private:
  std::array<std::uint32_t, 16> registers{};
  std::array<std::uint32_t, 16> crc{};
  std::array<std::uint32_t, 16> initial{};
  std::uint32_t constant{0x69'96'c5'3au};
  std::uint32_t stream_word{0};
  std::uint32_t mac_word{0};
  unsigned int buffered_bits{0};
  bool finished{false};
  void cycle();
  void load_key(std::span<std::byte const> key);
  void mac(std::uint32_t word);
  void process(std::span<std::byte> bytes, bool decrypt);

public:
  explicit shannon(std::span<std::byte const> key);
  ~shannon();
  shannon(shannon const &) = delete;
  shannon &operator=(shannon const &) = delete;
  void nonce(std::uint32_t value);
  void encrypt(std::span<std::byte> bytes);
  void decrypt(std::span<std::byte> bytes);
  [[nodiscard]] std::array<std::byte, 4> finish();
};

} // namespace librespot::crypto
