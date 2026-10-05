#pragma once
#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>

namespace librespot::net {

struct http_request {
  std::string host;
  std::string port{"443"};
  std::string target{"/"};
  std::string method{"GET"};
  std::map<std::string, std::string> headers;
  std::string body;
};

struct http_response {
  unsigned int status{0};
  std::map<std::string, std::string> headers;
  std::string body;
};

struct http_config {
  std::chrono::seconds timeout{30};
  std::size_t body_limit{16 * 1024 * 1024};
  /// Omitted by default; a request header may override the application's value
  std::optional<std::string> user_agent;
};

/// Implementations and their executors must outlive outstanding requests
class http_transport {
public:
  virtual ~http_transport() = default;
  virtual boost::asio::awaitable<http_response> request(http_request request) = 0;
};

class http_client final : public http_transport {
private:
  struct implementation;
  std::unique_ptr<implementation> state;

public:
  http_client(boost::asio::any_io_executor executor, http_config config = {});
  ~http_client() override;
  http_client(http_client const &) = delete;
  http_client &operator=(http_client const &) = delete;
  boost::asio::awaitable<http_response> request(http_request request) override;
};

[[nodiscard]] std::string form_encode(std::string_view text);

} // namespace librespot::net
