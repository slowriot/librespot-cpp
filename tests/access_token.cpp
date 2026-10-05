#include <catch2/catch_test_macros.hpp>
#include <array>
#include <stdexcept>
#include "librespot/oauth/access_token.h"

TEST_CASE("Service tokens use monotonic expiry and match all requested scopes") {
  auto const now{std::chrono::steady_clock::now()};
  auto const token{librespot::oauth::decode_access_token(
    R"({"accessToken":"secret","tokenType":"Bearer","expiresIn":3600,"scope":["streaming","user-read-private"]})", now)};
  CHECK(token.expires_at == now + std::chrono::seconds{3600});
  CHECK_FALSE(token.expired(now));
  CHECK(token.expired(now + std::chrono::seconds{3590}));
  CHECK(token.covers(std::array<std::string, 1>{"streaming"}));
  CHECK_FALSE(token.covers(std::array<std::string, 2>{"streaming", "user-read-email"}));
}

TEST_CASE("Service token errors reject malformed lifetimes and never echo credentials") {
  for(auto const lifetime : {"-1", "1.5", "18446744073709551615", "\"3600\""}) {
    auto const body{std::string{"{\"accessToken\":\"secret\",\"tokenType\":\"Bearer\",\"scope\":[],\"expiresIn\":"} + lifetime + '}'};
    CHECK_THROWS_AS(librespot::oauth::decode_access_token(body), std::runtime_error);
  }
  try {
    static_cast<void>(librespot::oauth::decode_access_token(R"({"accessToken":"private credential","tokenType":42,"expiresIn":3600,"scope":[]})"));
    FAIL("invalid token response accepted");
  } catch(std::runtime_error const &error) {
    CHECK(std::string_view{error.what()}.find("private credential") == std::string_view::npos);
  }
}
