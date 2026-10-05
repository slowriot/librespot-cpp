#include "pairing.h"
#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <nlohmann/json.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include "librespot/crypto/key_exchange.h"
#include "librespot/encoding/base64.h"
#include "librespot/encoding/protocol.h"
#include "librespot/version.h"

namespace librespot::discovery {
namespace {

std::array<unsigned char, 20> sha1(std::string_view data) {
  std::array<unsigned char, 20> result{};
  unsigned int size{0};
  if(EVP_Digest(data.data(), data.size(), result.data(), &size, EVP_sha1(), nullptr) != 1 || size != result.size()) throw std::runtime_error{"pairing digest failed"};
  return result;
}

std::array<unsigned char, 20> hmac(std::span<unsigned char const> key, std::string_view data) {
  std::array<unsigned char, 20> result{};
  unsigned int size{0};
  if(!HMAC(EVP_sha1(), key.data(), static_cast<int>(key.size()), reinterpret_cast<unsigned char const *>(data.data()), data.size(), result.data(), &size)
    || size != result.size()) throw std::runtime_error{"pairing MAC failed"};
  return result;
}

std::string decrypt(EVP_CIPHER const *cipher, unsigned char const *key, unsigned char const *iv, std::string_view input) {
  auto context{std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>{EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free}};
  if(!context || EVP_DecryptInit_ex(context.get(), cipher, nullptr, key, iv) != 1 || EVP_CIPHER_CTX_set_padding(context.get(), 0) != 1) throw std::runtime_error{"pairing cipher initialisation failed"};
  std::string result(input.size() + EVP_MAX_BLOCK_LENGTH, '\0');
  int count{0};
  int tail{0};
  if(EVP_DecryptUpdate(context.get(), reinterpret_cast<unsigned char *>(result.data()), &count,
    reinterpret_cast<unsigned char const *>(input.data()), static_cast<int>(input.size())) != 1
    || EVP_DecryptFinal_ex(context.get(), reinterpret_cast<unsigned char *>(result.data()) + count, &tail) != 1) throw std::runtime_error{"invalid pairing ciphertext"};
  result.resize(static_cast<std::size_t>(count + tail));
  return result;
}

class blob_reader {
private:
  std::string_view remaining;

public:
  explicit blob_reader(std::string_view bytes) : remaining{bytes} {
  }
  unsigned int byte() {
    if(remaining.empty()) throw std::runtime_error{"truncated pairing credentials"};
    auto const result{static_cast<unsigned char>(remaining.front())};
    remaining.remove_prefix(1);
    return result;
  }
  unsigned int integer() {
    auto const low{byte()};
    return (low & 128u) == 0 ? low : (low & 127u) | (byte() << 7u);
  }
  std::string bytes() {
    auto const size{integer()};
    if(size > remaining.size()) throw std::runtime_error{"truncated pairing credential field"};
    auto const result{std::string{remaining.substr(0, size)}};
    remaining.remove_prefix(size);
    return result;
  }
};

std::string_view name(connect::device_type type) {
  switch(type) {
  case connect::device_type::computer: return "Computer";
  case connect::device_type::tablet: return "Tablet";
  case connect::device_type::smartphone: return "Smartphone";
  case connect::device_type::speaker: return "Speaker";
  case connect::device_type::television: return "TV";
  case connect::device_type::audio_dongle: return "AudioDongle";
  }
  throw std::invalid_argument{"invalid Connect device type"};
}

} // anonymous namespace

struct pairing::implementation {
  connect::device_config config;
  crypto::diffie_hellman keys;

  explicit implementation(connect::device_config config) : config{std::move(config)} {
    if(this->config.id.empty() || this->config.name.empty() || this->config.id.size() > 128 || this->config.name.size() > 255) throw std::invalid_argument{"invalid Connect device identity"};
    name(this->config.type);
  }
};

pairing::pairing(connect::device_config config) : state{std::make_unique<implementation>(std::move(config))} {
}

pairing::~pairing() = default;

std::string pairing::get_info(std::string const &active_user) const {
  auto const key{state->keys.public_key()};
  auto const &config{state->config};
  return nlohmann::json{
    {"status", 101}, {"statusString", "OK"}, {"spotifyError", 0}, {"responseSource", config.name},
    {"version", "2.9.0"}, {"deviceID", config.id}, {"deviceType", name(config.type)}, {"remoteName", config.name},
    {"publicKey", base64::encode(std::string{reinterpret_cast<char const *>(key.data()), key.size()})},
    {"brandDisplayName", config.brand}, {"modelDisplayName", config.model}, {"libraryVersion", version},
    {"resolverVersion", "1"}, {"groupStatus", "NONE"}, {"tokenType", "default"}, {"clientID", config.client_id},
    {"productID", 0}, {"scope", "streaming"}, {"availability", ""}, {"supported_drm_media_formats", nlohmann::json::array()},
    {"supported_capabilities", 1}, {"accountReq", "PREMIUM"}, {"activeUser", active_user}, {"aliases", nlohmann::json::array()},
  }.dump();
}

credentials pairing::decode(std::string const &username, std::string const &blob, std::string const &client_key) const {
  /// Authenticate the outer DH envelope before decrypting the device-bound credential blob
  if(username.empty() || username.size() > 1024 || blob.size() > 65536 || client_key.size() > 256) throw std::invalid_argument{"invalid pairing parameters"};
  auto const peer{encoding::decode_base64(client_key)};
  auto const secret{state->keys.shared_secret(std::as_bytes(std::span{peer}))};
  auto const hashed{sha1({reinterpret_cast<char const *>(secret.data()), secret.size()})};
  auto const base_key{std::span{hashed}.first<16>()};
  auto const checksum_key{hmac(base_key, "checksum")};
  auto const encryption_key{hmac(base_key, "encryption")};
  auto const envelope{encoding::decode_base64(blob)};
  if(envelope.size() < 36) throw std::runtime_error{"truncated pairing envelope"};
  auto const ciphertext{std::string_view{envelope}.substr(16, envelope.size() - 36)};
  auto const checksum{hmac(checksum_key, ciphertext)};
  if(CRYPTO_memcmp(checksum.data(), envelope.data() + envelope.size() - 20, 20) != 0) throw std::runtime_error{"pairing MAC mismatch"};
  auto const encoded{decrypt(EVP_aes_128_ctr(), encryption_key.data(), reinterpret_cast<unsigned char const *>(envelope.data()), ciphertext)};
  auto const encrypted{encoding::decode_base64(encoded)};
  if(encrypted.size() < 16 || encrypted.size() % 16 != 0) throw std::runtime_error{"invalid device-bound credential length"};
  auto const device_secret{sha1(state->config.id)};
  std::array<unsigned char, 24> key{};
  if(PKCS5_PBKDF2_HMAC(reinterpret_cast<char const *>(device_secret.data()), static_cast<int>(device_secret.size()),
    reinterpret_cast<unsigned char const *>(username.data()), static_cast<int>(username.size()), 256, EVP_sha1(), 20, key.data()) != 1) throw std::runtime_error{"pairing key derivation failed"};
  auto const derived{sha1({reinterpret_cast<char const *>(key.data()), 20})};
  std::copy(derived.begin(), derived.end(), key.begin());
  key[23] = 20;
  auto plaintext{decrypt(EVP_aes_192_ecb(), key.data(), nullptr, encrypted)};
  for(std::size_t index{plaintext.size()}; index > 16; --index) plaintext[index - 1] ^= plaintext[index - 17];
  blob_reader reader{plaintext};
  reader.byte();
  reader.bytes();
  reader.byte();
  auto const type{reader.integer()};
  if(type > 4) throw std::runtime_error{"unsupported pairing authentication type"};
  reader.byte();
  auto data{reader.bytes()};
  if(data.empty()) throw std::runtime_error{"empty pairing credential"};
  return {.username{username}, .type{static_cast<authentication_type>(type)}, .data{std::move(data)}};
}

} // namespace librespot::discovery
