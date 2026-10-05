#include <catch2/catch_test_macros.hpp>
#include <array>
#include <span>
#include <string>
#include "librespot/crypto/key_exchange.h"
#include "librespot/crypto/shannon.h"

namespace {

std::string hex(std::span<std::byte const> bytes) {
  std::string result;
  std::string_view constexpr digits{"0123456789abcdef"};
  for(auto byte : bytes) {
    auto const value{std::to_integer<unsigned int>(byte)};
    result += digits[value >> 4];
    result += digits[value & 15u];
  }
  return result;
}

} // anonymous namespace

TEST_CASE("Shannon matches independent upstream Rust cipher vectors") {
  std::array<std::byte, 32> key;
  for(unsigned int index{0}; index < key.size(); ++index) key[index] = static_cast<std::byte>(index);
  std::array<std::byte, 16> plain;
  for(unsigned int index{0}; index < plain.size(); ++index) plain[index] = static_cast<std::byte>(index);
  librespot::crypto::shannon encoder{key};
  encoder.nonce(42);
  auto encrypted{plain};
  encoder.encrypt(encrypted);
  CHECK(hex(encrypted) == "25d4ea23addd5b5194d1a86ab3d2b4fc");
  CHECK(hex(encoder.finish()) == "d2f6a9e4");
  CHECK_THROWS_AS(encoder.finish(), std::logic_error);
  for(unsigned int split{0}; split <= plain.size(); ++split) {
    librespot::crypto::shannon decoder{key};
    decoder.nonce(42);
    auto decrypted{encrypted};
    decoder.decrypt(std::span{decrypted}.first(split));
    decoder.decrypt(std::span{decrypted}.subspan(split));
    CHECK(decrypted == plain);
    CHECK(hex(decoder.finish()) == "d2f6a9e4");
  }
  encoder.nonce(42);
  CHECK(hex(encoder.finish()) == "5a78d66c");
  encoder.nonce(42);
  std::array<std::byte, 3> partial{std::byte{0}, std::byte{1}, std::byte{2}};
  encoder.encrypt(partial);
  CHECK(hex(partial) == "25d4ea");
  CHECK(hex(encoder.finish()) == "3288cb0e");
}

TEST_CASE("Handshake derivation matches Python HMAC-SHA1 vectors") {
  std::string const secret{"shared secret"};
  std::string const transcript{"client hello\0server response", 28};
  auto const keys{librespot::crypto::derive_keys(std::as_bytes(std::span{secret}), std::as_bytes(std::span{transcript}))};
  CHECK(hex(keys.challenge) == "4fbe25c30873be1b87dd3543f6962b8767f1871d");
  CHECK(hex(keys.send) == "a51a7c3de8ed9411ea03f957856860a44c2cffa6c23c533bc9c8679e49ca552f");
  CHECK(hex(keys.receive) == "1a128bbee568055bbad534e0cc5aaf440e733c4d20cf6759182996c944bb1238");
}

TEST_CASE("Diffie-Hellman peers agree and reject degenerate public keys") {
  librespot::crypto::diffie_hellman first;
  librespot::crypto::diffie_hellman second;
  CHECK(first.shared_secret(second.public_key()) == second.shared_secret(first.public_key()));
  CHECK(first.public_key() != second.public_key());
  CHECK_THROWS_AS(first.shared_secret(std::array<std::byte, 1>{std::byte{0}}), std::invalid_argument);
  CHECK_THROWS_AS(first.shared_secret(std::array<std::byte, 1>{std::byte{1}}), std::invalid_argument);
  CHECK_THROWS_AS(first.shared_secret(std::array<std::byte, 97>{}), std::invalid_argument);
  CHECK_THROWS(librespot::crypto::verify_server_key(second.public_key(), std::array<std::byte, 256>{}));
}
