#include <catch2/catch_test_macros.hpp>
#include <array>
#include <vector>
#include <openssl/evp.h>
#include "librespot/audio/decrypt.h"

TEST_CASE("Audio decryption agrees with independent OpenSSL one-shot encryption") {
  librespot::audio::audio_key key{};
  for(std::size_t index{0}; index < key.size(); ++index) key[index] = static_cast<std::byte>(index);
  std::vector<std::byte> plain(4097);
  for(std::size_t index{0}; index < plain.size(); ++index) plain[index] = static_cast<std::byte>(index & 255u);
  std::array<unsigned char, 16> const iv{0x72, 0xe0, 0x67, 0xfb, 0xdd, 0xcb, 0xcf, 0x77, 0xeb, 0xe8, 0xbc, 0x64, 0x3f, 0x63, 0x0d, 0x93};
  auto const context{EVP_CIPHER_CTX_new()};
  REQUIRE(context);
  REQUIRE(EVP_EncryptInit_ex(context, EVP_aes_128_ctr(), nullptr, reinterpret_cast<unsigned char const *>(key.data()), iv.data()) == 1);
  auto encrypted{plain};
  int written{0};
  REQUIRE(EVP_EncryptUpdate(context, reinterpret_cast<unsigned char *>(encrypted.data()), &written,
    reinterpret_cast<unsigned char const *>(plain.data()), static_cast<int>(plain.size())) == 1);
  EVP_CIPHER_CTX_free(context);
  for(std::size_t const offset : {0u, 1u, 15u, 16u, 17u, 255u, 4096u}) {
    std::vector<std::byte> range(encrypted.begin() + static_cast<std::ptrdiff_t>(offset), encrypted.end());
    librespot::audio::decrypt(key, offset, range);
    CHECK(std::equal(range.begin(), range.end(), plain.begin() + static_cast<std::ptrdiff_t>(offset)));
  }
}
