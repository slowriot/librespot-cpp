#include "resolve.h"
#include <boost/asio/deferred.hpp>
#include <boost/asio/experimental/parallel_group.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

namespace librespot::net::detail {

boost::asio::awaitable<boost::asio::ip::tcp::resolver::results_type> resolve(
  boost::asio::any_io_executor executor, std::string host, std::string port, std::chrono::seconds timeout) {
  /// Race completion rather than success so DNS errors propagate immediately
  boost::asio::ip::tcp::resolver resolver{executor};
  boost::asio::steady_timer deadline{executor, timeout};
  auto [order, resolve_error, addresses, timer_error]{co_await boost::asio::experimental::make_parallel_group(
    resolver.async_resolve(host, port, boost::asio::deferred),
    deadline.async_wait(boost::asio::deferred)
  ).async_wait(boost::asio::experimental::wait_for_one(), boost::asio::use_awaitable)};
  if(order[0] == 1) throw boost::system::system_error{timer_error ? timer_error : boost::asio::error::timed_out};
  if(resolve_error) throw boost::system::system_error{resolve_error};
  co_return addresses;
}

} // namespace librespot::net::detail
