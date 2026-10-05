#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <future>
#include <optional>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include "librespot/net/dealer.h"
#include "librespot/net/http_client.h"

namespace {

boost::asio::awaitable<boost::beast::http::request<boost::beast::http::string_body>> serve(
  boost::asio::ip::tcp::acceptor &acceptor, boost::asio::ssl::context &tls) {
  auto socket{co_await acceptor.async_accept(boost::asio::use_awaitable)};
  boost::asio::ssl::stream<boost::asio::ip::tcp::socket> stream{std::move(socket), tls};
  co_await stream.async_handshake(boost::asio::ssl::stream_base::server, boost::asio::use_awaitable);
  boost::beast::flat_buffer buffer;
  boost::beast::http::request<boost::beast::http::string_body> request;
  co_await boost::beast::http::async_read(stream, buffer, request, boost::asio::use_awaitable);
  boost::beast::http::response<boost::beast::http::string_body> response{boost::beast::http::status::ok, 11};
  response.body() = "response";
  response.prepare_payload();
  co_await boost::beast::http::async_write(stream, response, boost::asio::use_awaitable);
  co_return request;
}

struct trust_environment {
  std::optional<std::string> previous;
  trust_environment() {
    if(auto const value{std::getenv("SSL_CERT_FILE")}) previous = value;
    if(::setenv("SSL_CERT_FILE", LIBRESPOT_TEST_FIXTURES "/localhost.crt", 1) != 0) throw std::runtime_error{"cannot set test TLS trust"};
  }
  ~trust_environment() {
    if(previous) ::setenv("SSL_CERT_FILE", previous->c_str(), 1);
    else ::unsetenv("SSL_CERT_FILE");
  }
};

} // anonymous namespace

TEST_CASE("HTTPS omits default user agent and honours application and request overrides") {
  trust_environment trust;
  for(unsigned int test{0}; test < 3; ++test) {
    boost::asio::io_context executor;
    boost::asio::ip::tcp::acceptor acceptor{executor, {boost::asio::ip::make_address("127.0.0.1"), 0}};
    boost::asio::ssl::context tls{boost::asio::ssl::context::tls_server};
    tls.use_certificate_chain_file(LIBRESPOT_TEST_FIXTURES "/localhost.crt");
    tls.use_private_key_file(LIBRESPOT_TEST_FIXTURES "/localhost.key", boost::asio::ssl::context::pem);
    librespot::net::http_config config;
    if(test > 0) config.user_agent = "owning-application";
    librespot::net::http_client client{executor.get_executor(), config};
    librespot::net::http_request request{
      .host{"localhost"},
      .port{std::to_string(acceptor.local_endpoint().port())},
      .target{"/test"},
      .method{"GET"},
      .headers{},
      .body{},
    };
    if(test == 2) request.headers["user-agent"] = "request-override";
    auto captured{boost::asio::co_spawn(executor, serve(acceptor, tls), boost::asio::use_future)};
    auto response{boost::asio::co_spawn(executor, client.request(std::move(request)), boost::asio::use_future)};
    executor.run();
    REQUIRE(response.get().body == "response");
    auto const received{captured.get()};
    if(test == 0) CHECK(received.find(boost::beast::http::field::user_agent) == received.end());
    else CHECK(received[boost::beast::http::field::user_agent] == (test == 1 ? "owning-application" : "request-override"));
  }
}

TEST_CASE("Dealer websocket verifies TLS and preserves application-owned user agents") {
  trust_environment trust;
  for(bool configured : {false, true}) {
    boost::asio::io_context executor;
    boost::asio::ip::tcp::acceptor acceptor{executor, {boost::asio::ip::make_address("127.0.0.1"), 0}};
    boost::asio::ssl::context tls{boost::asio::ssl::context::tls_server};
    tls.use_certificate_chain_file(LIBRESPOT_TEST_FIXTURES "/localhost.crt");
    tls.use_private_key_file(LIBRESPOT_TEST_FIXTURES "/localhost.key", boost::asio::ssl::context::pem);
    librespot::net::dealer_config config;
    if(configured) config.user_agent = "owning-program";
    librespot::net::dealer dealer{executor.get_executor(), config};
    auto captured{boost::asio::co_spawn(executor, [&]()->boost::asio::awaitable<boost::beast::http::request<boost::beast::http::string_body>> {
      auto socket{co_await acceptor.async_accept(boost::asio::use_awaitable)};
      boost::beast::websocket::stream<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> stream{std::move(socket), tls};
      co_await stream.next_layer().async_handshake(boost::asio::ssl::stream_base::server, boost::asio::use_awaitable);
      boost::beast::flat_buffer buffer;
      boost::beast::http::request<boost::beast::http::string_body> request;
      co_await boost::beast::http::async_read(stream.next_layer(), buffer, request, boost::asio::use_awaitable);
      co_await stream.async_accept(request, boost::asio::use_awaitable);
      stream.text(true);
      co_await stream.async_write(boost::asio::buffer(std::string{"server-message"}), boost::asio::use_awaitable);
      co_await stream.async_read(buffer, boost::asio::use_awaitable);
      CHECK(boost::beast::buffers_to_string(buffer.data()) == "client-reply");
      co_return request;
    }, boost::asio::use_future)};
    auto client{boost::asio::co_spawn(executor, [&]()->boost::asio::awaitable<void> {
      co_await dealer.connect({"localhost", std::to_string(acceptor.local_endpoint().port())}, "private token+&");
      auto const message{co_await dealer.receive()};
      CHECK(message == "server-message");
      co_await dealer.send("client-reply");
      dealer.close();
    }, boost::asio::use_future)};
    executor.run();
    client.get();
    auto const request{captured.get()};
    CHECK(request.target() == "/?access_token=private+token%2B%26");
    if(configured) CHECK(request[boost::beast::http::field::user_agent] == "owning-program");
    else CHECK(request.find(boost::beast::http::field::user_agent) == request.end());
  }
}
