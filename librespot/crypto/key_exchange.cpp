#include "key_exchange.h"
#include <algorithm>
#include <climits>
#include <stdexcept>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include "server_key.h"

namespace librespot::crypto {
namespace {

using big_number = std::unique_ptr<BIGNUM, decltype(&BN_clear_free)>;
using number_context = std::unique_ptr<BN_CTX, decltype(&BN_CTX_free)>;

big_number number() {
  big_number result{BN_new(), BN_clear_free};
  if(!result) throw std::bad_alloc{};
  return result;
}

std::vector<std::byte> export_number(BIGNUM const *number) {
  /// Use minimal big-endian encoding as required by the access-point protocol
  std::vector<std::byte> result(static_cast<std::size_t>(BN_num_bytes(number)));
  if(BN_bn2bin(number, reinterpret_cast<unsigned char *>(result.data())) != static_cast<int>(result.size())) {
    throw std::runtime_error{"cannot serialise Diffie-Hellman number"};
  }
  return result;
}

std::array<std::byte, 20> hmac(std::span<std::byte const> key, std::span<std::byte const> message) {
  /// Use the provider API for the protocol's HMAC-SHA1 construction
  std::array<std::byte, 20> result{};
  std::size_t written{0};
  if(!EVP_Q_mac(nullptr, "HMAC", nullptr, "SHA1", nullptr, key.data(), key.size(),
    reinterpret_cast<unsigned char const *>(message.data()), message.size(),
    reinterpret_cast<unsigned char *>(result.data()), result.size(), &written) || written != result.size()) {
    throw std::runtime_error{"cannot derive access-point handshake HMAC"};
  }
  return result;
}

} // anonymous namespace

struct diffie_hellman::implementation {
  big_number prime{BN_get_rfc2409_prime_768(nullptr), BN_clear_free};
  big_number generator{number()};
  big_number private_key{number()};
  big_number public_key{number()};

  implementation() {
    /// Match Spotify's legacy group while using cryptographic randomness and constant-time exponentiation
    number_context context{BN_CTX_secure_new(), BN_CTX_free};
    if(!prime || !context) throw std::bad_alloc{};
    if(BN_set_word(generator.get(), 2) != 1 || BN_priv_rand(private_key.get(), 760, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY) != 1
      || BN_is_zero(private_key.get()) || BN_mod_exp_mont_consttime(public_key.get(), generator.get(), private_key.get(), prime.get(), context.get(), nullptr) != 1) {
      throw std::runtime_error{"cannot generate access-point Diffie-Hellman keys"};
    }
  }
};

diffie_hellman::diffie_hellman() : state{std::make_unique<implementation>()} {
  /// Create fresh ephemeral keys for one handshake
}

diffie_hellman::~diffie_hellman() = default;

std::vector<std::byte> diffie_hellman::public_key() const {
  return export_number(state->public_key.get());
}

std::vector<std::byte> diffie_hellman::shared_secret(std::span<std::byte const> remote) const {
  /// Reject degenerate peer values rather than deriving a predictable secret
  if(remote.empty() || remote.size() > 96) throw std::invalid_argument{"invalid remote Diffie-Hellman key length"};
  big_number peer{BN_bin2bn(reinterpret_cast<unsigned char const *>(remote.data()), static_cast<int>(remote.size()), nullptr), BN_clear_free};
  auto upper{number()};
  if(!peer || !BN_copy(upper.get(), state->prime.get()) || BN_sub_word(upper.get(), 1) != 1) throw std::bad_alloc{};
  if(BN_cmp(peer.get(), BN_value_one()) <= 0 || BN_cmp(peer.get(), upper.get()) >= 0) throw std::invalid_argument{"invalid remote Diffie-Hellman key value"};
  auto secret{number()};
  number_context context{BN_CTX_secure_new(), BN_CTX_free};
  if(!context) throw std::bad_alloc{};
  if(BN_mod_exp_mont_consttime(secret.get(), peer.get(), state->private_key.get(), state->prime.get(), context.get(), nullptr) != 1) {
    throw std::runtime_error{"cannot derive access-point shared secret"};
  }
  return export_number(secret.get());
}

handshake_keys derive_keys(std::span<std::byte const> secret, std::span<std::byte const> transcript) {
  /// Derive distinct send and receive keys and authenticate the complete handshake transcript
  if(secret.empty()) throw std::invalid_argument{"empty access-point shared secret"};
  std::vector<std::byte> message(transcript.begin(), transcript.end());
  message.push_back(std::byte{0});
  std::array<std::byte, 100> data{};
  for(unsigned int index{1}; index <= 5; ++index) {
    message.back() = static_cast<std::byte>(index);
    auto const digest{hmac(secret, message)};
    std::ranges::copy(digest, data.begin() + static_cast<std::ptrdiff_t>((index - 1) * digest.size()));
  }
  handshake_keys keys;
  keys.challenge = hmac(std::span{data}.first<20>(), transcript);
  std::ranges::copy(std::span{data}.subspan<20, 32>(), keys.send.begin());
  std::ranges::copy(std::span{data}.subspan<52, 32>(), keys.receive.begin());
  OPENSSL_cleanse(data.data(), data.size());
  return keys;
}

void verify_server_key(std::span<std::byte const> key, std::span<std::byte const> signature) {
  /// Verify Spotify's signed ephemeral key before using it for the handshake
  if(key.empty() || key.size() > 96 || signature.size() != 256) throw std::runtime_error{"invalid access-point server signature length"};
  big_number modulus{BN_bin2bn(server_key.data(), static_cast<int>(server_key.size()), nullptr), BN_clear_free};
  auto exponent{number()};
  if(!modulus || BN_set_word(exponent.get(), 65537) != 1) throw std::bad_alloc{};
  std::unique_ptr<OSSL_PARAM_BLD, decltype(&OSSL_PARAM_BLD_free)> builder{OSSL_PARAM_BLD_new(), OSSL_PARAM_BLD_free};
  if(!builder || OSSL_PARAM_BLD_push_BN(builder.get(), OSSL_PKEY_PARAM_RSA_N, modulus.get()) != 1
    || OSSL_PARAM_BLD_push_BN(builder.get(), OSSL_PKEY_PARAM_RSA_E, exponent.get()) != 1) throw std::runtime_error{"cannot construct access-point signature key"};
  std::unique_ptr<OSSL_PARAM, decltype(&OSSL_PARAM_free)> parameters{OSSL_PARAM_BLD_to_param(builder.get()), OSSL_PARAM_free};
  std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context{EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr), EVP_PKEY_CTX_free};
  if(!parameters || !context || EVP_PKEY_fromdata_init(context.get()) != 1) throw std::runtime_error{"cannot initialise server signature verification"};
  EVP_PKEY *raw_key{nullptr};
  auto const imported{EVP_PKEY_fromdata(context.get(), &raw_key, EVP_PKEY_PUBLIC_KEY, parameters.get())};
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> public_key{raw_key, EVP_PKEY_free};
  if(imported != 1 || !public_key) throw std::runtime_error{"cannot import access-point signature key"};
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> verifier{EVP_MD_CTX_new(), EVP_MD_CTX_free};
  if(!verifier || EVP_DigestVerifyInit(verifier.get(), nullptr, EVP_sha1(), nullptr, public_key.get()) != 1
    || EVP_DigestVerify(verifier.get(), reinterpret_cast<unsigned char const *>(signature.data()), signature.size(),
      reinterpret_cast<unsigned char const *>(key.data()), key.size()) != 1) {
    throw std::runtime_error{"access-point server signature verification failed"};
  }
}

} // namespace librespot::crypto
