#include "dealer.h"
#include <stdexcept>
#include <boost/asio/ssl.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <openssl/ssl.h>
#include "http_client.h"
#include "resolve.h"

namespace librespot::net {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;

struct dealer::implementation {
  using stream_type = websocket::stream<beast::ssl_stream<beast::tcp_stream>>;
  asio::any_io_executor executor;
  dealer_config config;
  asio::ssl::context tls{asio::ssl::context::tls_client};
  std::shared_ptr<stream_type> stream;

  implementation(asio::any_io_executor executor, dealer_config config) : executor{std::move(executor)}, config{std::move(config)} {
    if(this->config.timeout.count() <= 0) throw std::invalid_argument{"invalid Dealer timeout"};
    tls.set_default_verify_paths();
    tls.set_verify_mode(asio::ssl::verify_peer);
    if(SSL_CTX_set_min_proto_version(tls.native_handle(), TLS1_2_VERSION) != 1) throw std::runtime_error{"cannot set Dealer TLS version"};
  }
};

dealer::dealer(asio::any_io_executor executor, dealer_config config) : state{std::make_unique<implementation>(std::move(executor), std::move(config))} {
}

dealer::~dealer() {
  close();
}

asio::awaitable<void> dealer::connect(endpoint address, std::string token) {
  /// Authenticate a verified websocket without exposing the signed URL in diagnostics
  if(token.empty() || address.host.empty()) throw std::invalid_argument{"invalid Dealer connection parameters"};
  close();
  struct cleanup {
    dealer &connection;
    bool connected{false};
    ~cleanup() {
      if(!connected) connection.close();
    }
  } guard{*this};
  auto connection{std::make_shared<implementation::stream_type>(state->executor, state->tls)};
  state->stream = connection;
  auto &stream{*connection};
  if(SSL_set_tlsext_host_name(stream.next_layer().native_handle(), address.host.c_str()) != 1) throw std::runtime_error{"cannot set Dealer TLS server name"};
  stream.next_layer().set_verify_callback(asio::ssl::host_name_verification{address.host});
  auto const addresses{co_await detail::resolve(state->executor, address.host, address.port, state->config.timeout)};
  beast::get_lowest_layer(stream).expires_after(state->config.timeout);
  co_await beast::get_lowest_layer(stream).async_connect(addresses, asio::use_awaitable);
  co_await stream.next_layer().async_handshake(asio::ssl::stream_base::client, asio::use_awaitable);
  beast::get_lowest_layer(stream).expires_never();
  stream.set_option(websocket::stream_base::timeout{state->config.timeout, std::chrono::seconds{60}, true});
  stream.read_message_max(16 * 1024 * 1024);
  stream.set_option(websocket::stream_base::decorator{[agent{state->config.user_agent}](websocket::request_type &request){
    request.erase(beast::http::field::user_agent);
    if(agent) request.set(beast::http::field::user_agent, *agent);
  }});
  co_await stream.async_handshake(address.host + ':' + address.port, "/?access_token=" + form_encode(token), asio::use_awaitable);
  stream.text(true);
  guard.connected = true;
}

asio::awaitable<std::string> dealer::receive() {
  auto stream{state->stream};
  if(!stream || !stream->is_open()) throw std::logic_error{"Dealer is not connected"};
  beast::flat_buffer buffer;
  co_await stream->async_read(buffer, asio::use_awaitable);
  if(!stream->got_text()) throw std::runtime_error{"unexpected binary Dealer envelope"};
  co_return beast::buffers_to_string(buffer.data());
}

asio::awaitable<void> dealer::send(std::string message) {
  auto stream{state->stream};
  if(!stream || !stream->is_open()) throw std::logic_error{"Dealer is not connected"};
  if(message.size() > 16 * 1024 * 1024) throw std::invalid_argument{"Dealer message too large"};
  co_await stream->async_write(asio::buffer(message), asio::use_awaitable);
}

void dealer::close() {
  if(state->stream) {
    boost::system::error_code ignored;
    beast::get_lowest_layer(*state->stream).socket().close(ignored);
    state->stream.reset();
  }
}

} // namespace librespot::net
