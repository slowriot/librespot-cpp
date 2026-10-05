#include "hashcash.h"
#include <bit>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <openssl/evp.h>

namespace librespot::crypto {
namespace {

using digest_context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

std::array<unsigned char, 20> digest(std::span<std::byte const> bytes) {
  std::array<unsigned char, 20> result;
  std::size_t length{0};
  if(EVP_Q_digest(nullptr, "SHA1", nullptr, bytes.data(), bytes.size(), result.data(), &length) != 1 || length != result.size()) {
    throw std::runtime_error{"cannot calculate hashcash digest"};
  }
  return result;
}

} // anonymous namespace

hashcash_solution solve_hashcash(std::span<std::byte const> context, std::span<std::byte const> prefix,
  unsigned int difficulty, std::chrono::seconds timeout, std::stop_token stop) {
  if(difficulty > 64 || timeout.count() <= 0 || prefix.size() > 1024) throw std::invalid_argument{"invalid hashcash challenge"};
  auto const start{std::chrono::steady_clock::now()};
  auto const seed{digest(context)};
  std::uint64_t target{0};
  for(std::size_t index{12}; index < seed.size(); ++index) target = (target << 8) | seed[index];
  digest_context base{EVP_MD_CTX_new(), EVP_MD_CTX_free};
  digest_context work{EVP_MD_CTX_new(), EVP_MD_CTX_free};
  if(!base || !work) throw std::bad_alloc{};
  if(EVP_DigestInit_ex(base.get(), EVP_sha1(), nullptr) != 1 || EVP_DigestUpdate(base.get(), prefix.data(), prefix.size()) != 1) {
    throw std::runtime_error{"cannot initialise hashcash"};
  }
  for(std::uint64_t counter{0};; ++counter) {
    if((counter & 255u) == 0) {
      if(stop.stop_requested()) throw std::runtime_error{"hashcash cancelled"};
      if(std::chrono::steady_clock::now() - start >= timeout) throw std::runtime_error{"hashcash deadline exceeded"};
    }
    std::array<std::byte, 16> suffix;
    for(unsigned int index{0}; index < 8; ++index) {
      suffix[index] = static_cast<std::byte>(((target + counter) >> ((7 - index) * 8)) & 255u);
      suffix[index + 8] = static_cast<std::byte>((counter >> ((7 - index) * 8)) & 255u);
    }
    std::array<unsigned char, 20> result;
    unsigned int length{0};
    if(EVP_MD_CTX_copy_ex(work.get(), base.get()) != 1 || EVP_DigestUpdate(work.get(), suffix.data(), suffix.size()) != 1
      || EVP_DigestFinal_ex(work.get(), result.data(), &length) != 1 || length != result.size()) throw std::runtime_error{"cannot solve hashcash"};
    std::uint64_t tail{0};
    for(std::size_t index{12}; index < result.size(); ++index) tail = (tail << 8) | result[index];
    if(static_cast<unsigned int>(std::countr_zero(tail)) >= difficulty) return {.suffix{suffix}, .elapsed{std::chrono::steady_clock::now() - start}};
  }
}

} // namespace librespot::crypto
