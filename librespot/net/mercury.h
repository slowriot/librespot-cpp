#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "access_point.h"

namespace librespot::net {

enum class mercury_method {
  get,
  send,
  subscribe,
  unsubscribe,
};

struct mercury_request {
  mercury_method method{mercury_method::get};
  std::string uri;
  std::optional<std::string> content_type;
  std::vector<std::vector<std::byte>> payload;
};

struct mercury_frame {
  std::vector<std::byte> sequence;
  unsigned int flags{0};
  std::vector<std::vector<std::byte>> parts;
};

struct mercury_response {
  std::string uri;
  int status{0};
  std::optional<std::string> content_type;
  std::vector<std::vector<std::byte>> payload;
};

[[nodiscard]] packet encode_mercury(mercury_request const &request, std::uint64_t sequence);
[[nodiscard]] mercury_frame decode_mercury(std::span<std::byte const> payload);

/// Assemble one sequence; interleaved requests need separate instances
class mercury_assembler {
private:
  std::vector<std::byte> sequence;
  std::vector<std::vector<std::byte>> parts;
  std::optional<std::vector<std::byte>> partial;
  std::size_t received{0};
  std::size_t size_limit;
  bool complete{false};

public:
  explicit mercury_assembler(std::size_t size_limit = 16 * 1024 * 1024);
  [[nodiscard]] std::optional<mercury_response> append(mercury_frame frame);
};

} // namespace librespot::net
