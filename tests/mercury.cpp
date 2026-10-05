#include <catch2/catch_test_macros.hpp>
#include <array>
#include <span>
#include "librespot/net/mercury.h"

namespace {

std::vector<std::byte> response_header() {
  /// An independently encoded protobuf Header with URI hm://x and status 200
  return {std::byte{0x0a}, std::byte{6}, std::byte{'h'}, std::byte{'m'}, std::byte{':'}, std::byte{'/'}, std::byte{'/'}, std::byte{'x'}, std::byte{0x20}, std::byte{0x90}, std::byte{3}};
}

} // anonymous namespace

TEST_CASE("Mercury requests encode network-order sequence numbers and method headers") {
  auto const packet{librespot::net::encode_mercury({.method{librespot::net::mercury_method::get}, .uri{"hm://x"}, .content_type{}, .payload{}}, 0x01'02'03'04'05'06'07'08u)};
  CHECK(packet.command == 0xb2);
  auto const frame{librespot::net::decode_mercury(packet.payload)};
  CHECK(frame.sequence == std::vector<std::byte>{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}});
  CHECK(frame.flags == 1);
  REQUIRE(frame.parts.size() == 1);
  CHECK(frame.parts[0] == std::vector<std::byte>{std::byte{0x0a}, std::byte{6}, std::byte{'h'}, std::byte{'m'}, std::byte{':'}, std::byte{'/'}, std::byte{'/'}, std::byte{'x'}, std::byte{0x1a}, std::byte{3}, std::byte{'G'}, std::byte{'E'}, std::byte{'T'}});
  for(std::size_t length{0}; length < packet.payload.size(); ++length) CHECK_THROWS(librespot::net::decode_mercury(std::span{packet.payload}.first(length)));
}

TEST_CASE("Mercury joins split headers and payloads and enforces sequence and size limits") {
  auto const header{response_header()};
  std::vector<std::byte> const sequence{std::byte{1}};
  librespot::net::mercury_assembler assembler;
  CHECK_FALSE(assembler.append({.sequence{sequence}, .flags{2}, .parts{{header.begin(), header.begin() + 5}}}));
  auto const result{assembler.append({.sequence{sequence}, .flags{1}, .parts{{header.begin() + 5, header.end()}, {std::byte{42}}}})};
  REQUIRE(result);
  CHECK(result->uri == "hm://x");
  CHECK(result->status == 200);
  REQUIRE(result->payload.size() == 1);
  CHECK(result->payload[0] == std::vector<std::byte>{std::byte{42}});
  CHECK_THROWS_AS(assembler.append({.sequence{sequence}, .flags{1}, .parts{}}), std::logic_error);
  librespot::net::mercury_assembler bounded{4};
  CHECK_THROWS(bounded.append({.sequence{sequence}, .flags{1}, .parts{header}}));
  librespot::net::mercury_assembler interleaved;
  CHECK_FALSE(interleaved.append({.sequence{sequence}, .flags{0}, .parts{header}}));
  CHECK_THROWS_AS(interleaved.append({.sequence{std::byte{2}}, .flags{1}, .parts{}}), std::invalid_argument);
}
