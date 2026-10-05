#include <catch2/catch_test_macros.hpp>
#include <array>
#include <random>
#include "librespot/core/spotify_uri.h"

TEST_CASE("Spotify IDs match upstream conversion vectors") {
  auto const id{librespot::spotify_id::from_base62("5sWHDYs0csV6RS48xBl0tH")};
  REQUIRE(id);
  CHECK(id->to_hex() == "b39fe8081e1f4c54be38e8d6f9f12bb9");
  CHECK(id->to_base62() == "5sWHDYs0csV6RS48xBl0tH");
  CHECK(librespot::spotify_id::from_hex("B39FE8081E1F4C54BE38E8D6F9F12BB9") == id);
  CHECK(librespot::spotify_id{}.to_base62() == "0000000000000000000000");
  CHECK(librespot::spotify_id::from_hex("ffffffffffffffffffffffffffffffff")->to_base62() == "7N42dgm5tFLK9N8MT7fHC7");
}

TEST_CASE("Spotify IDs reject malformed and overflowing values") {
  CHECK_FALSE(librespot::spotify_id::from_base62("ZZZZZZZZZZZZZZZZZZZZZZ"));
  CHECK_FALSE(librespot::spotify_id::from_base62("!!!!!!!!!!!!!!!!!!!!!!"));
  CHECK_FALSE(librespot::spotify_id::from_base62("short"));
  CHECK_FALSE(librespot::spotify_id::from_hex("--------------------------------"));
  CHECK_FALSE(librespot::spotify_id::from_bytes(std::array<std::byte, 17>{}));
}

TEST_CASE("Spotify IDs round trip across random 128-bit values") {
  std::mt19937 random{42};
  for(unsigned int iteration{0}; iteration < 1000; ++iteration) {
    std::array<std::byte, 16> bytes;
    for(auto &byte : bytes) byte = static_cast<std::byte>(random() & 255u);
    librespot::spotify_id const id{bytes};
    CHECK(librespot::spotify_id::from_base62(id.to_base62()) == id);
    CHECK(librespot::spotify_id::from_hex(id.to_hex()) == id);
  }
}

TEST_CASE("Spotify URIs preserve catalog, named, local and unknown resources") {
  for(auto const text : {
    "spotify:track:5sWHDYs0csV6RS48xBl0tH",
    "spotify:user:spotify:playlist:37i9dQZF1DWSw8liJZcPOI",
    "spotify:local:David+Wise:Donkey%3AKong:Snomads:127",
    "spotify:local:::Title:0",
    "spotify:future-kind:resource",
  }) {
    auto const parsed{librespot::parse_uri(text)};
    REQUIRE(parsed);
    CHECK(librespot::to_string(*parsed) == text);
  }
  CHECK(librespot::is_playable(*librespot::parse_uri("spotify:track:5sWHDYs0csV6RS48xBl0tH")));
  CHECK_FALSE(librespot::is_playable(*librespot::parse_uri("spotify:show:5sWHDYs0csV6RS48xBl0tH")));
}

TEST_CASE("Spotify URIs reject trailing fields and invalid durations") {
  for(auto const text : {
    "spotify:track:5sWHDYs0csV6RS48xBl0tH:extra",
    "spotify:local:::Title:-1",
    "spotify:local:::Title:1second",
    "spotify:local:::Title:99999999999999999999999",
    "spotify:user:user:track:5sWHDYs0csV6RS48xBl0tH",
    "other:track:5sWHDYs0csV6RS48xBl0tH",
  }) CHECK_FALSE(librespot::parse_uri(text));
}
