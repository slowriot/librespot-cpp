#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include "http_client.h"

namespace librespot {

enum class authentication_type : std::uint32_t {
  password = 0,
  stored_spotify = 1,
  stored_facebook = 2,
  spotify_token = 3,
  facebook_token = 4,
};

struct credentials {
  std::optional<std::string> username;
  authentication_type type{authentication_type::spotify_token};
  std::string data;
};

namespace net {

struct endpoint {
  std::string host;
  std::string port;
};

struct packet {
  std::uint8_t command{0};
  std::vector<std::byte> payload;
};

/// One reader and one writer may run concurrently on the same executor strand
class access_point_transport {
public:
  virtual ~access_point_transport() = default;
  virtual boost::asio::awaitable<credentials> connect(endpoint address, credentials login, std::string device_id) = 0;
  virtual boost::asio::awaitable<void> send(packet message) = 0;
  virtual boost::asio::awaitable<packet> receive() = 0;
  virtual void close() = 0;
};

class access_point final : public access_point_transport {
private:
  struct implementation;
  std::unique_ptr<implementation> state;
  boost::asio::awaitable<void> handshake();

public:
  explicit access_point(boost::asio::any_io_executor executor, std::chrono::seconds timeout = std::chrono::seconds{30}, log_handler on_log = {});
  ~access_point() override;
  access_point(access_point const &) = delete;
  access_point &operator=(access_point const &) = delete;
  boost::asio::awaitable<credentials> connect(endpoint address, credentials login, std::string device_id) override;
  boost::asio::awaitable<void> send(packet message) override;
  boost::asio::awaitable<packet> receive() override;
  void close() override;
};

[[nodiscard]] boost::asio::awaitable<std::vector<endpoint>> resolve_access_points(http_transport &transport);

} // namespace net
} // namespace librespot
