#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include "pairing.h"

namespace librespot::discovery {

struct server_config {
  connect::device_config device;
  std::string address{"::"};
  std::uint16_t port{0};
  bool advertise{true};
  std::string active_user;
  std::function<void(std::string)> on_error;
};

struct pairing_handler {
  /// Return credentials accepted by Spotify, after authentication and any account replacement
  std::function<boost::asio::awaitable<credentials>(credentials)> add_user;
  std::function<boost::asio::awaitable<void>()> reset_users;
};

/// Owns local discovery and HTTP pairing; all callbacks run on its strand
class server {
private:
  struct implementation;
  std::shared_ptr<implementation> state;

public:
  server(boost::asio::any_io_executor executor, server_config config, pairing_handler handler);
  ~server();
  server(server const &) = delete;
  server &operator=(server const &) = delete;
  [[nodiscard]] std::uint16_t port() const;
  boost::asio::awaitable<void> run();
  boost::asio::awaitable<void> shutdown();
  void set_active_user(std::string username);
  void close();
};

} // namespace librespot::discovery
