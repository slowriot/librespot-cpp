#pragma once
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <boost/asio/any_io_executor.hpp>
#include "byte_source.h"
#include "fetch.h"

namespace librespot::audio {

struct cdn_source_config {
  std::size_t chunk_size{256 * 1024};
  /// Original encrypted-file offset of the audio container (167 for Spotify Vorbis)
  std::uint64_t container_offset{0};
};

/// Blocking reads belong on a worker; the HTTP executor must run independently
class cdn_source final : public byte_source {
private:
  struct implementation;
  std::shared_ptr<implementation> state;

public:
  cdn_source(boost::asio::any_io_executor executor, net::http_transport &transport,
    std::vector<std::string> urls, std::optional<audio_key> key, cdn_source_config config = {});
  ~cdn_source() override;
  cdn_source(cdn_source const &) = delete;
  cdn_source &operator=(cdn_source const &) = delete;
  std::uint64_t size() override;
  std::size_t read_at(std::uint64_t offset, std::span<std::byte> destination) override;
  /// Thread-safe; cancels in-flight HTTP and makes subsequent reads fail
  void cancel();
};

} // namespace librespot::audio
