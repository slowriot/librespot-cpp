#pragma once
#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace librespot::crypto {

class diffie_hellman {
private:
  struct implementation;
  std::unique_ptr<implementation> state;

public:
  diffie_hellman();
  ~diffie_hellman();
  diffie_hellman(diffie_hellman const &) = delete;
  diffie_hellman &operator=(diffie_hellman const &) = delete;
  [[nodiscard]] std::vector<std::byte> public_key() const;
  [[nodiscard]] std::vector<std::byte> shared_secret(std::span<std::byte const> remote) const;
};

struct handshake_keys {
  std::array<std::byte, 20> challenge{};
  std::array<std::byte, 32> send{};
  std::array<std::byte, 32> receive{};
};

[[nodiscard]] handshake_keys derive_keys(std::span<std::byte const> secret, std::span<std::byte const> transcript);
void verify_server_key(std::span<std::byte const> key, std::span<std::byte const> signature);

} // namespace librespot::crypto
