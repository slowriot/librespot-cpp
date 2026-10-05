#include "decrypt.h"
#include <algorithm>
#include <climits>
#include <memory>
#include <stdexcept>
#include <openssl/evp.h>

namespace librespot::audio {

void decrypt(audio_key const &key, std::uint64_t offset, std::span<std::byte> data) {
  /// Reconstruct the big-endian CTR counter and discard the partial-block prefix
  std::array<unsigned char, 16> counter{
    0x72, 0xe0, 0x67, 0xfb, 0xdd, 0xcb, 0xcf, 0x77,
    0xeb, 0xe8, 0xbc, 0x64, 0x3f, 0x63, 0x0d, 0x93,
  };
  auto carry{offset / 16};
  for(std::size_t index{counter.size()}; index > 0; --index) {
    carry += counter[index - 1];
    counter[index - 1] = static_cast<unsigned char>(carry & 255u);
    carry >>= 8;
  }
  std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> context{EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free};
  if(!context || EVP_EncryptInit_ex(context.get(), EVP_aes_128_ctr(), nullptr,
    reinterpret_cast<unsigned char const *>(key.data()), counter.data()) != 1) {
    throw std::runtime_error{"cannot initialise audio AES-CTR cipher"};
  }
  std::array<unsigned char, 16> skipped{};
  int written{0};
  if(EVP_EncryptUpdate(context.get(), skipped.data(), &written, skipped.data(), static_cast<int>(offset % 16)) != 1) {
    throw std::runtime_error{"cannot seek audio AES-CTR cipher"};
  }
  while(!data.empty()) {
    auto const length{std::min(data.size(), static_cast<std::size_t>(INT_MAX))};
    auto const buffer{reinterpret_cast<unsigned char *>(data.data())};
    if(EVP_EncryptUpdate(context.get(), buffer, &written, buffer, static_cast<int>(length)) != 1 || written != static_cast<int>(length)) {
      throw std::runtime_error{"cannot decrypt audio AES-CTR range"};
    }
    data = data.subspan(length);
  }
}

} // namespace librespot::audio
