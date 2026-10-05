#include <catch2/catch_test_macros.hpp>
#include <deque>
#include <future>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include "librespot/oauth/device_auth.h"

namespace {

class fake_transport final : public librespot::net::http_transport {
public:
  std::deque<librespot::net::http_response> responses;
  std::vector<librespot::net::http_request> requests;

  boost::asio::awaitable<librespot::net::http_response> request(librespot::net::http_request request) override {
    requests.push_back(std::move(request));
    REQUIRE_FALSE(responses.empty());
    auto result{std::move(responses.front())};
    responses.pop_front();
    co_return result;
  }
};

template<typename T>
T run(boost::asio::io_context &executor, boost::asio::awaitable<T> operation) {
  auto future{boost::asio::co_spawn(executor, std::move(operation), boost::asio::use_future)};
  executor.restart();
  executor.run();
  return future.get();
}

} // anonymous namespace

TEST_CASE("Device OAuth encodes requests and handles polling states") {
  boost::asio::io_context executor;
  fake_transport transport;
  librespot::oauth::device_auth auth{transport, "client"};
  transport.responses.push_back({200, {}, R"({"device_code":"secret+code","user_code":"ABCDEF","verification_uri":"https://spotify.com/pair","expires_in":3600})"});
  auto const challenge{run(executor, auth.start("streaming user-read-email"))};
  CHECK(challenge.interval == std::chrono::seconds{5});
  CHECK(transport.requests.back().body == "client_id=client&scope=streaming+user-read-email");
  transport.responses.push_back({400, {}, R"({"error":"authorization_pending"})"});
  CHECK(std::get<librespot::oauth::poll_status>(run(executor, auth.poll(challenge.device_code))) == librespot::oauth::poll_status::pending);
  CHECK(transport.requests.back().body.ends_with("device_code=secret%2Bcode"));
  transport.responses.push_back({400, {}, R"({"error":"slow_down"})"});
  CHECK(std::get<librespot::oauth::poll_status>(run(executor, auth.poll(challenge.device_code))) == librespot::oauth::poll_status::slow_down);
  transport.responses.push_back({200, {}, R"({"access_token":"access","token_type":"Bearer","expires_in":3600,"refresh_token":"refresh"})"});
  CHECK(std::get<librespot::oauth::token>(run(executor, auth.poll(challenge.device_code))).access_token == "access");
}

TEST_CASE("Device OAuth preserves refresh tokens and rejects malformed replies") {
  boost::asio::io_context executor;
  fake_transport transport;
  librespot::oauth::device_auth auth{transport, "client"};
  transport.responses.push_back({200, {}, R"({"access_token":"new","token_type":"Bearer","expires_in":3600})"});
  CHECK(run(executor, auth.refresh("original")).refresh_token == "original");
  for(auto const body : {"not JSON", "[]", R"({"expires_in":-1})", R"({"expires_in":18446744073709551615})"}) {
    transport.responses.push_back({200, {}, body});
    CHECK_THROWS_AS(run(executor, auth.start()), librespot::oauth::auth_error);
  }
  transport.responses.push_back({400, {}, R"({"error":"expired_token"})"});
  try {
    run(executor, auth.poll("device"));
    FAIL("expired token was accepted");
  } catch(librespot::oauth::auth_error const &error) {
    CHECK(error.code() == "expired_token");
  }
}

TEST_CASE("Form encoding preserves byte values") {
  CHECK(librespot::net::form_encode("a +&=/:\xc3\xa9") == "a+%2B%26%3D%2F%3A%C3%A9");
}
