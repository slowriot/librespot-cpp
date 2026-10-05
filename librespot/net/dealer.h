#pragma once
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include "access_point.h"
#include "librespot/diagnostics.h"

namespace librespot::net {

struct dealer_config {
  std::optional<std::string> user_agent;
  std::chrono::seconds timeout{30};
  log_handler on_log;
};

/// Sequential operations on one executor; close cancels pending reads and writes
class dealer_transport {
public:
  virtual ~dealer_transport() = default;
  virtual boost::asio::awaitable<void> connect(endpoint address, std::string token) = 0;
  virtual boost::asio::awaitable<std::string> receive() = 0;
  virtual boost::asio::awaitable<void> send(std::string message) = 0;
  virtual void close() = 0;
};

class dealer final : public dealer_transport {
private:
  struct implementation;
  std::unique_ptr<implementation> state;

public:
  dealer(boost::asio::any_io_executor executor, dealer_config config = {});
  ~dealer() override;
  boost::asio::awaitable<void> connect(endpoint address, std::string token) override;
  boost::asio::awaitable<std::string> receive() override;
  boost::asio::awaitable<void> send(std::string message) override;
  void close() override;
};

} // namespace librespot::net
