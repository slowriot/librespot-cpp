#pragma once
#include <array>
#include <chrono>
#include <cstddef>
#include <span>
#include <stop_token>

namespace librespot::crypto {

struct hashcash_solution {
  std::array<std::byte, 16> suffix;
  std::chrono::nanoseconds elapsed;
};

[[nodiscard]] hashcash_solution solve_hashcash(std::span<std::byte const> context,
  std::span<std::byte const> prefix, unsigned int difficulty,
  std::chrono::seconds timeout = std::chrono::seconds{5}, std::stop_token stop = {});

} // namespace librespot::crypto
