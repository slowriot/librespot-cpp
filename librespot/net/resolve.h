#pragma once
#include <chrono>
#include <string>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>

namespace librespot::net::detail {

boost::asio::awaitable<boost::asio::ip::tcp::resolver::results_type> resolve(
  boost::asio::any_io_executor executor, std::string host, std::string port, std::chrono::seconds timeout);

} // namespace librespot::net::detail
