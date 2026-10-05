#include "http_client.h"
#include <chrono>
#include <stdexcept>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <openssl/ssl.h>
#include "resolve.h"

namespace librespot::net {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

struct http_client::implementation {
  asio::any_io_executor executor;
  http_config config;
  asio::ssl::context tls{asio::ssl::context::tls_client};

  implementation(asio::any_io_executor executor, http_config config)
    : executor{std::move(executor)},
      config{std::move(config)} {
    /// Require peer verification against the system trust store
    if(this->config.timeout.count() <= 0 || this->config.body_limit == 0) throw std::invalid_argument{"invalid HTTP limits"};
    tls.set_default_verify_paths();
    tls.set_verify_mode(asio::ssl::verify_peer);
    if(SSL_CTX_set_min_proto_version(tls.native_handle(), TLS1_2_VERSION) != 1) {
      throw std::runtime_error{"cannot set minimum TLS version"};
    }
  }
};

http_client::http_client(asio::any_io_executor executor, http_config config)
  : state{std::make_unique<implementation>(std::move(executor), std::move(config))} {
  /// Bind requests to the application's executor
}

http_client::~http_client() = default;

asio::awaitable<http_response> http_client::request(http_request request) try {
  /// Perform a verified HTTPS exchange; redirects are returned to the caller
  if(request.host.empty() || request.target.empty() || request.target.front() != '/') throw std::invalid_argument{"invalid HTTPS endpoint"};
  auto const method{http::string_to_verb(request.method)};
  if(method == http::verb::unknown) throw std::invalid_argument{"unknown HTTP method"};
  auto const started{std::chrono::steady_clock::now()};
  emit_log(state->config.on_log, log_level::debug, "http", request.method + " https://" + request.host + ':' + request.port
    + diagnostic_url(request.target) + "; request bytes=" + std::to_string(request.body.size()));
  beast::ssl_stream<beast::tcp_stream> stream{state->executor, state->tls};
  if(SSL_set_tlsext_host_name(stream.native_handle(), request.host.c_str()) != 1) throw std::runtime_error{"cannot configure TLS server name"};
  stream.set_verify_callback(asio::ssl::host_name_verification{request.host});
  auto const addresses{co_await detail::resolve(state->executor, request.host, request.port, state->config.timeout)};
  beast::get_lowest_layer(stream).expires_after(state->config.timeout);
  co_await beast::get_lowest_layer(stream).async_connect(addresses, asio::use_awaitable);
  co_await stream.async_handshake(asio::ssl::stream_base::client, asio::use_awaitable);
  http::request<http::string_body> message{method, request.target, 11};
  message.set(http::field::host, request.host + (request.port == "443" ? "" : ":" + request.port));
  if(state->config.user_agent) message.set(http::field::user_agent, *state->config.user_agent);
  for(auto const &[name, value] : request.headers) message.set(name, value);
  message.body() = std::move(request.body);
  message.prepare_payload();
  co_await http::async_write(stream, message, asio::use_awaitable);
  beast::flat_buffer buffer;
  http::response_parser<http::string_body> parser;
  parser.body_limit(state->config.body_limit);
  co_await http::async_read(stream, buffer, parser, asio::use_awaitable);
  auto response{parser.release()};
  http_response result{
    .status{response.result_int()},
    .headers{},
    .body{std::move(response.body())},
  };
  for(auto const &field : response) result.headers.emplace(std::string{field.name_string()}, std::string{field.value()});
  auto const elapsed{std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)};
  emit_log(state->config.on_log, log_level::debug, "http", request.host + diagnostic_url(request.target) + " -> HTTP "
    + std::to_string(result.status) + "; response bytes=" + std::to_string(result.body.size()) + "; elapsed=" + std::to_string(elapsed.count()) + "ms");
  auto const content_type{response[http::field::content_type]};
  if(state->config.on_log && content_type.find("json") != beast::string_view::npos) emit_log(state->config.on_log, log_level::trace, "http", "Response JSON " + diagnostic_json(result.body));
  boost::system::error_code error;
  beast::get_lowest_layer(stream).socket().close(error);
  co_return result;
} catch(std::exception const &error) {
  emit_log(state->config.on_log, log_level::error, "http", request.method + " " + request.host + diagnostic_url(request.target) + " failed: " + diagnostic_error(error));
  throw;
}

std::string form_encode(std::string_view text) {
  /// Encode UTF-8 bytes as application/x-www-form-urlencoded
  std::string_view constexpr hex{"0123456789ABCDEF"};
  std::string result;
  for(char character : text) {
    auto const byte{static_cast<unsigned char>(character)};
    if((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.' || byte == '*') {
      result += character;
    } else if(byte == ' ') {
      result += '+';
    } else {
      result += '%';
      result += hex[byte >> 4];
      result += hex[byte & 15u];
    }
  }
  return result;
}

} // namespace librespot::net
