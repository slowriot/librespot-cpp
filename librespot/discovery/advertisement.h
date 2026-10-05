#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <boost/asio/any_io_executor.hpp>

namespace librespot::discovery {

/// Publishes through the system Avahi daemon; errors are delivered on the supplied executor
class advertisement {
private:
  struct implementation;
  std::unique_ptr<implementation> state;

public:
  advertisement(boost::asio::any_io_executor executor, std::string name, std::uint16_t port,
    std::function<void(std::string)> on_error);
  ~advertisement();
  advertisement(advertisement const &) = delete;
  advertisement &operator=(advertisement const &) = delete;
};

} // namespace librespot::discovery
