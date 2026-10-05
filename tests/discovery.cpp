#include <catch2/catch_test_macros.hpp>
#include <array>
#include <future>
#include <memory>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include "librespot/discovery/pairing.h"
#include "librespot/discovery/server.h"
#include "librespot/encoding/base64.h"
#include "librespot/encoding/protocol.h"

namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

std::string envelope(std::string const &public_key, std::string const &credential_blob = "6tT3s+AE/ZIsHL6jZWeOrCSon284aKYG5I9aPc3uPsc=") {
  /// Use an independent client with fixed DH exponent 2, plus a Python/OpenSSL-generated inner fixture
  auto const peer{base64::decode(public_key)};
  auto server{std::unique_ptr<BIGNUM, decltype(&BN_free)>{BN_bin2bn(reinterpret_cast<unsigned char const *>(peer.data()), static_cast<int>(peer.size()), nullptr), BN_free}};
  auto prime{std::unique_ptr<BIGNUM, decltype(&BN_free)>{BN_get_rfc2409_prime_768(nullptr), BN_free}};
  auto shared{std::unique_ptr<BIGNUM, decltype(&BN_free)>{BN_new(), BN_free}};
  auto context{std::unique_ptr<BN_CTX, decltype(&BN_CTX_free)>{BN_CTX_new(), BN_CTX_free}};
  REQUIRE(BN_mod_sqr(shared.get(), server.get(), prime.get(), context.get()) == 1);
  std::string secret(static_cast<std::size_t>(BN_num_bytes(shared.get())), '\0');
  BN_bn2bin(shared.get(), reinterpret_cast<unsigned char *>(secret.data()));
  std::array<unsigned char, 20> base{};
  REQUIRE(EVP_Digest(secret.data(), secret.size(), base.data(), nullptr, EVP_sha1(), nullptr) == 1);
  std::array<unsigned char, 20> encryption{};
  std::array<unsigned char, 20> checksum{};
  REQUIRE(EVP_Q_mac(nullptr, "HMAC", nullptr, "SHA1", nullptr, base.data(), 16,
    reinterpret_cast<unsigned char const *>("encryption"), 10, encryption.data(), 20, nullptr));
  REQUIRE(EVP_Q_mac(nullptr, "HMAC", nullptr, "SHA1", nullptr, base.data(), 16,
    reinterpret_cast<unsigned char const *>("checksum"), 8, checksum.data(), 20, nullptr));
  std::string result(16, '\x42');
  auto cipher{std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>{EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free}};
  REQUIRE(EVP_EncryptInit_ex(cipher.get(), EVP_aes_128_ctr(), nullptr, encryption.data(), reinterpret_cast<unsigned char const *>(result.data())) == 1);
  std::string encrypted(credential_blob.size() + 16, '\0');
  int size{0};
  REQUIRE(EVP_EncryptUpdate(cipher.get(), reinterpret_cast<unsigned char *>(encrypted.data()), &size,
    reinterpret_cast<unsigned char const *>(credential_blob.data()), static_cast<int>(credential_blob.size())) == 1);
  encrypted.resize(static_cast<std::size_t>(size));
  std::array<unsigned char, 20> mac{};
  REQUIRE(EVP_Q_mac(nullptr, "HMAC", nullptr, "SHA1", nullptr, checksum.data(), checksum.size(),
    reinterpret_cast<unsigned char const *>(encrypted.data()), encrypted.size(), mac.data(), mac.size(), nullptr));
  result += encrypted;
  result.append(reinterpret_cast<char const *>(mac.data()), mac.size());
  return base64::encode(result);
}

asio::awaitable<std::string> local_request(std::uint16_t port, std::string method, std::string target, std::string body = {}) {
  beast::tcp_stream stream{co_await asio::this_coro::executor};
  stream.expires_after(std::chrono::seconds{3});
  co_await stream.socket().async_connect({asio::ip::make_address("127.0.0.1"), port}, asio::use_awaitable);
  http::request<http::string_body> request{http::string_to_verb(method), target, 11};
  request.set(http::field::host, "localhost");
  request.set(http::field::content_type, "application/x-www-form-urlencoded");
  request.body() = std::move(body);
  request.prepare_payload();
  co_await http::async_write(stream, request, asio::use_awaitable);
  beast::flat_buffer buffer;
  http::response<http::string_body> response;
  co_await http::async_read(stream, buffer, response, asio::use_awaitable);
  co_return response.body();
}

} // anonymous namespace

TEST_CASE("Zeroconf decodes authenticated device-bound credentials and rejects malformed envelopes") {
  librespot::discovery::pairing pairing{{.id{"device"}, .name{"owning application"}, .brand{"brand"}, .model{"model"}}};
  auto info = nlohmann::json::parse(pairing.get_info());
  CHECK(info.at("deviceID") == "device");
  CHECK(info.at("brandDisplayName") == "brand");
  CHECK(info.at("remoteName") == "owning application");
  CHECK(info.at("activeUser") == "");
  auto const blob{envelope(info.at("publicKey").get<std::string>())};
  auto const client_key{base64::encode(std::string(1, '\x04'))};
  auto const credentials{pairing.decode("username", blob, client_key)};
  CHECK(credentials.username == "username");
  CHECK(credentials.type == librespot::authentication_type::stored_spotify);
  CHECK(credentials.data == std::string{"stored\0credential", 17});
  for(unsigned int length{0}; length < 36; ++length) CHECK_THROWS(pairing.decode("username", base64::encode(std::string(length, '\0')), client_key));
  auto bad{base64::decode(blob)};
  bad[16] ^= 1;
  CHECK_THROWS(pairing.decode("username", base64::encode(bad), client_key));
  CHECK_THROWS(pairing.decode("username", blob, "AA=="));
  CHECK_THROWS(pairing.decode("wrong-user", blob, client_key));
  CHECK_THROWS(pairing.decode("username", envelope(info.at("publicKey").get<std::string>(), "AA=="), client_key));
  CHECK_THROWS(librespot::encoding::decode_base64("!!!!"));
  CHECK_THROWS(librespot::encoding::decode_base64("AB=="));
}

TEST_CASE("Pairing HTTP confirms authentication before advertising an active user and resets accounts") {
  asio::io_context executor;
  int logins{0};
  int resets{0};
  bool accept{true};
  librespot::discovery::server server{executor.get_executor(), {
    .device{.id{"device"}, .name{"test"}}, .address{"127.0.0.1"}, .advertise{false},
  }, {.add_user{[&](librespot::credentials login)->asio::awaitable<librespot::credentials> {
    CHECK(login.data == std::string{"stored\0credential", 17});
    ++logins;
    auto info = nlohmann::json::parse(co_await local_request(server.port(), "GET", "/?action=getInfo"));
    CHECK(info.at("activeUser") == "");
    asio::steady_timer timer{co_await asio::this_coro::executor, std::chrono::milliseconds{10}};
    co_await timer.async_wait(asio::use_awaitable);
    if(!accept) throw std::runtime_error{"simulated authentication failure"};
    co_return librespot::credentials{.username{"accepted-user"}, .type{librespot::authentication_type::stored_spotify}, .data{"accepted-credential"}};
  }}, .reset_users{[&]()->asio::awaitable<void> {
    ++resets;
    co_return;
  }}}};
  auto running{asio::co_spawn(executor, server.run(), asio::use_future)};
  auto client{asio::co_spawn(executor, [&]()->asio::awaitable<void> {
    struct cleanup {
      librespot::discovery::server &server;
      ~cleanup() {
        server.close();
      }
    } guard{server};
    auto info = nlohmann::json::parse(co_await local_request(server.port(), "GET", "/?action=getInfo"));
    auto const blob{envelope(info.at("publicKey").get<std::string>())};
    auto const form{"action=addUser&userName=username&blob=" + librespot::net::form_encode(blob)
      + "&clientKey=" + librespot::net::form_encode(base64::encode(std::string(1, '\x04')))};
    auto response = nlohmann::json::parse(co_await local_request(server.port(), "POST", "/", form));
    REQUIRE(response.at("status") == 101);
    CHECK(logins == 1);
    info = nlohmann::json::parse(co_await local_request(server.port(), "GET", "/?action=getInfo"));
    CHECK(info.at("activeUser") == "accepted-user");
    response = nlohmann::json::parse(co_await local_request(server.port(), "POST", "/", "action=addUser&blob=bad"));
    CHECK(response.at("status") == 102);
    CHECK(logins == 1);
    accept = false;
    response = nlohmann::json::parse(co_await local_request(server.port(), "POST", "/", form));
    CHECK(response.at("status") == 102);
    CHECK(logins == 2);
    info = nlohmann::json::parse(co_await local_request(server.port(), "GET", "/?action=getInfo"));
    CHECK(info.at("activeUser") == "");
    response = nlohmann::json::parse(co_await local_request(server.port(), "POST", "/", "action=resetUsers"));
    CHECK(response.at("status") == 101);
    CHECK(resets == 1);
    info = nlohmann::json::parse(co_await local_request(server.port(), "GET", "/?action=getInfo"));
    CHECK(info.at("activeUser") == "");
    response = nlohmann::json::parse(co_await local_request(server.port(), "POST", "/", "action=resetUsers&action=resetUsers"));
    CHECK(response.at("status") == 102);
    response = nlohmann::json::parse(co_await local_request(server.port(), "POST", "/", "action=%zz"));
    CHECK(response.at("status") == 102);
    co_await server.shutdown();
  }, asio::use_future)};
  executor.run();
  client.get();
  running.get();
}
