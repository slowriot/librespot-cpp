#include <catch2/catch_test_macros.hpp>
#include <future>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include "librespot/session.h"

namespace {

class fake_resolver final : public librespot::net::http_transport {
public:
  boost::asio::awaitable<librespot::net::http_response> request(librespot::net::http_request request) override {
    CHECK(request.host == "apresolve.spotify.com");
    co_return librespot::net::http_response{200, {}, R"({"accesspoint":["test.example:443"]})"};
  }
};

class fake_access_point final : public librespot::net::access_point_transport {
private:
  boost::asio::experimental::channel<boost::asio::any_io_executor, void(boost::system::error_code, librespot::net::packet)> replies;

public:
  bool auto_reply{true};
  std::string token_response;
  std::vector<librespot::net::packet> sent;

  explicit fake_access_point(boost::asio::any_io_executor executor) : replies{executor, 16} {
  }

  boost::asio::awaitable<librespot::credentials> connect(librespot::net::endpoint address, librespot::credentials login, std::string device_id) override {
    CHECK(address.host == "test.example");
    CHECK((login.data == "token" || login.data == "reusable"));
    replies.reset();
    CHECK(device_id == "device");
    co_return librespot::credentials{.username{"test"}, .type{librespot::authentication_type::stored_spotify}, .data{"reusable"}};
  }

  boost::asio::awaitable<void> send(librespot::net::packet packet) override {
    sent.push_back(packet);
    if(!auto_reply) co_return;
    if(packet.command == 0x0c) {
      REQUIRE(packet.payload.size() == 42);
      librespot::net::packet response{.command{0x0d}, .payload{packet.payload.begin() + 36, packet.payload.begin() + 40}};
      response.payload.insert(response.payload.end(), 16, std::byte{42});
      REQUIRE(replies.try_send(boost::system::error_code{}, std::move(response)));
    } else if(packet.command == 0xb2) {
      auto const frame{librespot::net::decode_mercury(packet.payload)};
      librespot::net::packet response{.command{0xb2}, .payload{std::byte{0}, std::byte{8}}};
      response.payload.insert(response.payload.end(), frame.sequence.begin(), frame.sequence.end());
      std::vector<std::byte> rest{
        std::byte{1}, std::byte{0}, std::byte{2}, std::byte{0}, std::byte{11},
        std::byte{0x0a}, std::byte{6}, std::byte{'h'}, std::byte{'m'}, std::byte{':'}, std::byte{'/'}, std::byte{'/'}, std::byte{'x'}, std::byte{0x20}, std::byte{0x90}, std::byte{3},
        std::byte{0}, std::byte{1}, std::byte{42},
      };
      if(!token_response.empty()) {
        rest.resize(rest.size() - 3);
        rest.push_back(static_cast<std::byte>(token_response.size() >> 8));
        rest.push_back(static_cast<std::byte>(token_response.size() & 255));
        auto const bytes{std::as_bytes(std::span{token_response})};
        rest.insert(rest.end(), bytes.begin(), bytes.end());
      }
      response.payload.insert(response.payload.end(), rest.begin(), rest.end());
      REQUIRE(replies.try_send(boost::system::error_code{}, std::move(response)));
    }
  }

  boost::asio::awaitable<librespot::net::packet> receive() override {
    co_return co_await replies.async_receive(boost::asio::use_awaitable);
  }

  void close() override {
    replies.cancel();
    replies.close();
  }
};

boost::asio::awaitable<void> exercise(librespot::session &session, bool timeout) try {
  auto const reusable{co_await session.connect({.username{}, .type{librespot::authentication_type::spotify_token}, .data{"token"}})};
  CHECK(reusable.data == "reusable");
  if(timeout) {
    try {
      co_await session.request_audio_key({}, {});
      FAIL("missing audio key did not time out");
    } catch(boost::system::system_error const &error) {
      CHECK(error.code() == boost::asio::error::timed_out);
    }
  } else {
    using boost::asio::experimental::awaitable_operators::operator&&;
    auto [key, response]{co_await (session.request_audio_key({}, {})
      && session.request({.method{librespot::net::mercury_method::get}, .uri{"hm://x"}, .content_type{}, .payload{}}))};
    CHECK(key[0] == std::byte{42});
    CHECK(response.status == 200);
    REQUIRE(response.payload.size() == 1);
    CHECK(response.payload[0] == std::vector<std::byte>{std::byte{42}});
  }
  session.close();
} catch(...) {
  session.close();
  throw;
}

} // anonymous namespace

TEST_CASE("Session caches service tokens by client identity and scope coverage") {
  boost::asio::io_context executor;
  fake_resolver resolver;
  auto transport{std::make_shared<fake_access_point>(executor.get_executor())};
  transport->token_response = R"({"accessToken":"secret","tokenType":"Bearer","expiresIn":3600,"scope":["streaming","user-read-private"]})";
  librespot::session session{executor.get_executor(), resolver, {.device_id{"device"}}, transport};
  auto exercise_tokens{[&]()->boost::asio::awaitable<void> {
    try {
      co_await session.connect({.type{librespot::authentication_type::spotify_token}, .data{"token"}});
      std::vector<std::string> all_scopes{"streaming", "user-read-private", "streaming"};
      auto token{co_await session.request_token("first", std::move(all_scopes))};
      CHECK(token.value == "secret");
      std::vector<std::string> streaming{"streaming"};
      co_await session.request_token("first", streaming);
      CHECK(transport->sent.size() == 1);
      co_await session.request_token("second", streaming);
      CHECK(transport->sent.size() == 2);
      session.close();
    } catch(...) {
      session.close();
      throw;
    }
  }};
  auto done{boost::asio::co_spawn(executor, exercise_tokens(), boost::asio::use_future)};
  executor.run();
  CHECK_NOTHROW(done.get());
}

TEST_CASE("Session multiplexes Mercury and audio-key requests and shuts down its pumps") {
  boost::asio::io_context executor;
  fake_resolver resolver;
  auto transport{std::make_shared<fake_access_point>(executor.get_executor())};
  librespot::session session{executor.get_executor(), resolver, {.device_id{"device"}}, transport};
  auto done{boost::asio::co_spawn(executor, exercise(session, false), boost::asio::use_future)};
  executor.run();
  CHECK_NOTHROW(done.get());
  CHECK(transport->sent.size() == 2);
}

TEST_CASE("Session audio-key timeouts release pending operations") {
  boost::asio::io_context executor;
  fake_resolver resolver;
  auto transport{std::make_shared<fake_access_point>(executor.get_executor())};
  transport->auto_reply = false;
  librespot::session session{executor.get_executor(), resolver, {.device_id{"device"}, .request_timeout{std::chrono::seconds{1}}}, transport};
  auto done{boost::asio::co_spawn(executor, exercise(session, true), boost::asio::use_future)};
  executor.run();
  CHECK_NOTHROW(done.get());
}
