#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <boost/asio/awaitable.hpp>
#include "decrypt.h"
#include "librespot/net/http_client.h"

namespace librespot::audio {

struct audio_range {
  std::uint64_t offset{0};
  std::uint64_t total_size{0};
  std::vector<std::byte> bytes;
};

/// Fetch at most 16 MiB, retrying alternative CDN URLs on transport or invalid HTTP responses
boost::asio::awaitable<audio_range> fetch_range(net::http_transport &transport, std::vector<std::string> urls,
  std::uint64_t offset, std::size_t length, std::optional<audio_key> key = std::nullopt);

} // namespace librespot::audio
