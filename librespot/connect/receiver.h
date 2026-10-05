#pragma once
#include <functional>
#include <memory>
#include <string>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include "control.h"
#include "device.h"
#include "librespot/diagnostics.h"
#include "librespot/net/dealer.h"
#include "librespot/service/client.h"

namespace librespot::connect {

struct receiver_config {
  device_config device;
  net::dealer_config dealer;
  bool reconnect{true};
  std::function<void(std::string)> on_status;
  log_handler on_log;
};

/// Mutate the proposed state and return true only when the application accepts the command
using command_handler = std::function<boost::asio::awaitable<bool>(command const &, player_state &)>;

/// Runs Dealer and Connect state independently of decoding/output; dependencies must outlive run/shutdown
class receiver {
private:
  struct implementation;
  std::shared_ptr<implementation> state;

public:
  receiver(boost::asio::any_io_executor executor, net::http_transport &http, oauth::service_auth &auth,
    receiver_config config, command_handler handler, std::shared_ptr<net::dealer_transport> dealer = {});
  ~receiver();
  receiver(receiver const &) = delete;
  receiver &operator=(receiver const &) = delete;
  boost::asio::awaitable<void> run();
  /// Stop, await in-flight callbacks, and withdraw the device registration
  boost::asio::awaitable<void> shutdown();
  /// Cancel transport operations; await run or shutdown before releasing dependencies
  void close();
};

} // namespace librespot::connect
