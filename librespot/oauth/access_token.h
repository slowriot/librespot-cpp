#pragma once
#include <chrono>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace librespot::oauth {

struct access_token {
  std::string value;
  std::string type;
  std::vector<std::string> scopes;
  std::chrono::steady_clock::time_point expires_at;

  [[nodiscard]] bool expired(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const noexcept;
  [[nodiscard]] bool covers(std::span<std::string const> requested) const;
};

/// Parse Keymaster responses without including token contents in error messages
[[nodiscard]] access_token decode_access_token(std::string_view body,
  std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

} // namespace librespot::oauth
