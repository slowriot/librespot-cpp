#pragma once
#include <memory>
#include <string>
#include <boost/asio/awaitable.hpp>
#include "access_token.h"
#include "librespot/net/access_point.h"

namespace librespot::oauth {

struct service_auth_config {
  std::string device_id;
  std::string client_id{"65b708073fc0480ea92a077233ca87bd"};
  std::string client_token_id{"65b708073fc0480ea92a077233ca87bd"};
  /// Spotify protocol identity, independent of the application's HTTP user agent
  std::string client_version{"1.2.52.442"};
};

/// Use on one network executor; supply a separate worker executor for CPU challenges
class service_auth {
private:
  struct implementation;
  std::unique_ptr<implementation> state;

public:
  service_auth(net::http_transport &transport, service_auth_config config, credentials stored,
    boost::asio::any_io_executor worker);
  ~service_auth();
  service_auth(service_auth const &) = delete;
  service_auth &operator=(service_auth const &) = delete;
  boost::asio::awaitable<std::string> client_token();
  boost::asio::awaitable<access_token> token();
  void invalidate_token();
};

} // namespace librespot::oauth
