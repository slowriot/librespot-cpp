#pragma once
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <boost/asio/awaitable.hpp>
#include "librespot/core/file_id.h"
#include "librespot/oauth/service_auth.h"

namespace librespot::service {

enum class audio_format {
  vorbis_96 = 0,
  vorbis_160 = 1,
  vorbis_320 = 2,
  mp3_256 = 3,
  mp3_320 = 4,
  mp3_160 = 5,
  mp3_96 = 6,
  flac_16 = 16,
  flac_24 = 22,
};

struct audio_file {
  file_id id;
  audio_format format;
};

struct track {
  spotify_id id;
  std::string name;
  std::string album;
  std::vector<std::string> artists;
  std::chrono::milliseconds duration{0};
  bool explicit_content{false};
  std::vector<audio_file> files;
};

class storage_unavailable : public std::runtime_error {
public:
  storage_unavailable();
};

/// Service calls run sequentially on one executor; authentication and HTTP transport must outlive them
class client {
private:
  struct implementation;
  std::unique_ptr<implementation> state;

public:
  client(net::http_transport &transport, oauth::service_auth &auth);
  ~client();
  client(client const &) = delete;
  client &operator=(client const &) = delete;
  boost::asio::awaitable<track> get_track(spotify_id id);
  boost::asio::awaitable<std::vector<std::string>> resolve_audio(file_id id);
  /// Low-level Connect operations; receiver normally supplies the private protobuf encoding
  boost::asio::awaitable<void> put_connect_state(std::string device_id, std::string connection_id, std::string protobuf);
  boost::asio::awaitable<void> delete_connect_state(std::string device_id);
};

/// Prefer native lossless quality when available; only return supported decoder formats
[[nodiscard]] audio_file select_audio(track const &track);
[[nodiscard]] bool is_vorbis(audio_format format) noexcept;
[[nodiscard]] std::string_view to_string(audio_format format) noexcept;

} // namespace librespot::service
