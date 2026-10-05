#include <catch2/catch_test_macros.hpp>
#include <deque>
#include <future>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include "librespot/audio/fetch.h"

namespace {

class fake_cdn final : public librespot::net::http_transport {
public:
  std::deque<librespot::net::http_response> responses;
  std::vector<librespot::net::http_request> requests;

  boost::asio::awaitable<librespot::net::http_response> request(librespot::net::http_request request) override {
    requests.push_back(std::move(request));
    REQUIRE_FALSE(responses.empty());
    auto response{std::move(responses.front())};
    responses.pop_front();
    co_return response;
  }
};

} // anonymous namespace

TEST_CASE("Audio range fetching retries HTTP failures and validates the successful CDN range") {
  boost::asio::io_context executor;
  fake_cdn cdn;
  cdn.responses.push_back({500, {}, "error"});
  cdn.responses.push_back({206, {{"Content-Range", "bytes 8-10/11"}}, "abc"});
  auto result{boost::asio::co_spawn(executor, librespot::audio::fetch_range(cdn,
    {"https://bad.example/audio", "https://good.example/audio?signature=abc"}, 8, 10), boost::asio::use_future)};
  executor.run();
  auto const range{result.get()};
  CHECK(range.offset == 8);
  CHECK(range.total_size == 11);
  CHECK(range.bytes.size() == 3);
  REQUIRE(cdn.requests.size() == 2);
  CHECK(cdn.requests.back().headers.at("Range") == "bytes=8-17");
  CHECK(cdn.requests.back().target == "/audio?signature=abc");
  CHECK_FALSE(cdn.requests.back().headers.contains("Authorization"));
}

TEST_CASE("Audio range fetching rejects inconsistent bodies and unsafe endpoints") {
  boost::asio::io_context executor;
  fake_cdn cdn;
  cdn.responses.push_back({206, {{"content-range", "bytes 0-2/10"}}, "ab"});
  auto failed{boost::asio::co_spawn(executor, librespot::audio::fetch_range(cdn,
    {"http://unsafe.example/audio", "https://valid.example/audio"}, 0, 3), boost::asio::use_future)};
  executor.run();
  CHECK_THROWS(failed.get());
  CHECK(cdn.requests.size() == 1);
}
