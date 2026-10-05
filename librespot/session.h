#pragma once
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include "audio/decrypt.h"
#include "core/file_id.h"
#include "diagnostics.h"
#include "net/mercury.h"
#include "oauth/access_token.h"

namespace librespot {

struct session_config {
  std::string device_id;
  std::chrono::seconds io_timeout{90};
  std::chrono::seconds request_timeout{10};
  std::function<void(net::packet const &)> on_packet;
  log_handler on_log;
};

/// Owns dispatch and keepalive; the executor and HTTP transport must outlive the session
class session {
private:
  struct implementation;
  std::shared_ptr<implementation> state;

public:
  session(boost::asio::any_io_executor executor, net::http_transport &transport, session_config config,
    std::shared_ptr<net::access_point_transport> connection = {});
  ~session();
  session(session const &) = delete;
  session &operator=(session const &) = delete;
  boost::asio::awaitable<credentials> connect(credentials login);
  boost::asio::awaitable<audio::audio_key> request_audio_key(spotify_id track, file_id file);
  boost::asio::awaitable<net::mercury_response> request(net::mercury_request request);
  boost::asio::awaitable<oauth::access_token> request_token(std::string client_id, std::vector<std::string> scopes);
  void close();
};

} // namespace librespot
