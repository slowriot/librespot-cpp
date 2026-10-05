#include "access_point.h"
#include <array>
#include <format>
#include <limits>
#include <span>
#include <stdexcept>
#include <boost/asio/read.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <nlohmann/json.hpp>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include "librespot/crypto/key_exchange.h"
#include "librespot/crypto/shannon.h"
#include "librespot/version.h"
#include "authentication.pb.h"
#include "keyexchange.pb.h"
#include "resolve.h"

namespace librespot::net {
namespace asio = boost::asio;

namespace {

void append_u32(std::vector<std::byte> &bytes, std::uint32_t value) {
  for(unsigned int index{0}; index < 4; ++index) bytes.push_back(static_cast<std::byte>((value >> ((3 - index) * 8)) & 255u));
}

std::uint32_t read_u32(std::span<std::byte const, 4> bytes) {
  std::uint32_t result{0};
  for(auto byte : bytes) result = (result << 8) | std::to_integer<std::uint32_t>(byte);
  return result;
}

template<typename T>
T parse(std::span<std::byte const> bytes) {
  /// Require complete, initialised protocol messages within the bounded packet size
  T message;
  if(bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())
    || !message.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) throw std::runtime_error{"malformed access-point protobuf message"};
  return message;
}

std::vector<std::byte> serialise(google::protobuf::Message const &message) {
  /// Validate required fields before sending a protocol message
  std::string bytes;
  if(!message.SerializeToString(&bytes)) throw std::runtime_error{"incomplete access-point protobuf message"};
  auto const view{std::as_bytes(std::span{bytes})};
  return {view.begin(), view.end()};
}

struct active_operation {
  bool &active;
  explicit active_operation(bool &active) : active{active} {
    if(active) throw std::logic_error{"overlapping access-point operations"};
    active = true;
  }
  ~active_operation() {
    active = false;
  }
};

} // anonymous namespace

struct access_point::implementation {
  asio::any_io_executor executor;
  boost::beast::tcp_stream stream;
  std::chrono::seconds timeout;
  log_handler on_log;
  std::unique_ptr<crypto::shannon> encoder;
  std::unique_ptr<crypto::shannon> decoder;
  std::uint64_t send_nonce{0};
  std::uint64_t receive_nonce{0};
  bool reading{false};
  bool writing{false};
  bool connecting{false};

  implementation(asio::any_io_executor executor, std::chrono::seconds timeout, log_handler on_log)
    : executor{executor},
      stream{executor},
      timeout{timeout}, on_log{std::move(on_log)} {
    if(timeout.count() <= 0) throw std::invalid_argument{"invalid access-point timeout"};
  }
};

access_point::access_point(asio::any_io_executor executor, std::chrono::seconds timeout, log_handler on_log)
  : state{std::make_unique<implementation>(executor, timeout, std::move(on_log))} {
  /// Bind an access-point connection to the application's executor
}

access_point::~access_point() = default;

asio::awaitable<void> access_point::handshake() {
  /// Verify the server signature before installing traffic keys
  emit_log(state->on_log, log_level::debug, "access_point", "Sending Diffie-Hellman ClientHello");
  crypto::diffie_hellman local;
  auto const public_key{local.public_key()};
  protocol::ClientHello hello;
  hello.mutable_build_info()->set_product(protocol::PRODUCT_CLIENT);
  hello.mutable_build_info()->add_product_flags(protocol::PRODUCT_FLAG_NONE);
#if defined(__aarch64__) || defined(__arm__)
  hello.mutable_build_info()->set_platform(protocol::PLATFORM_LINUX_ARM);
#else
  hello.mutable_build_info()->set_platform(protocol::PLATFORM_LINUX_X86_64);
#endif // architecture selection
  hello.mutable_build_info()->set_version(124200290);
  hello.add_cryptosuites_supported(protocol::CRYPTO_SUITE_SHANNON);
  auto const dh{hello.mutable_login_crypto_hello()->mutable_diffie_hellman()};
  dh->set_gc(public_key.data(), public_key.size());
  dh->set_server_keys_known(1);
  std::array<unsigned char, 16> nonce;
  if(RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) throw std::runtime_error{"cannot generate access-point nonce"};
  hello.set_client_nonce(nonce.data(), nonce.size());
  hello.set_padding("\x1e");
  auto const payload{serialise(hello)};
  std::vector<std::byte> transcript{std::byte{0}, std::byte{4}};
  append_u32(transcript, static_cast<std::uint32_t>(payload.size() + 6));
  transcript.insert(transcript.end(), payload.begin(), payload.end());
  co_await asio::async_write(state->stream, asio::buffer(transcript), asio::use_awaitable);
  std::array<std::byte, 4> header;
  co_await asio::async_read(state->stream, asio::buffer(header), asio::use_awaitable);
  auto const length{read_u32(header)};
  if(length < 4 || length > 1024 * 1024) throw std::runtime_error{"invalid access-point handshake length"};
  std::vector<std::byte> response(length - 4);
  co_await asio::async_read(state->stream, asio::buffer(response), asio::use_awaitable);
  transcript.insert(transcript.end(), header.begin(), header.end());
  transcript.insert(transcript.end(), response.begin(), response.end());
  auto const message{parse<protocol::APResponseMessage>(response)};
  if(!message.has_challenge() || !message.challenge().has_login_crypto_challenge()
    || !message.challenge().login_crypto_challenge().has_diffie_hellman()) throw std::runtime_error{"access point did not send a Diffie-Hellman challenge"};
  auto const &challenge{message.challenge().login_crypto_challenge().diffie_hellman()};
  auto const remote{std::as_bytes(std::span{challenge.gs()})};
  crypto::verify_server_key(remote, std::as_bytes(std::span{challenge.gs_signature()}));
  emit_log(state->on_log, log_level::debug, "access_point", "Server signature verified; deriving Shannon traffic keys");
  auto secret{local.shared_secret(remote)};
  auto keys{crypto::derive_keys(secret, transcript)};
  OPENSSL_cleanse(secret.data(), secret.size());
  protocol::ClientResponsePlaintext reply;
  reply.mutable_login_crypto_response()->mutable_diffie_hellman()->set_hmac(keys.challenge.data(), keys.challenge.size());
  reply.mutable_pow_response();
  reply.mutable_crypto_response();
  auto const reply_data{serialise(reply)};
  std::vector<std::byte> encoded;
  append_u32(encoded, static_cast<std::uint32_t>(reply_data.size() + 4));
  encoded.insert(encoded.end(), reply_data.begin(), reply_data.end());
  state->encoder = std::make_unique<crypto::shannon>(keys.send);
  state->decoder = std::make_unique<crypto::shannon>(keys.receive);
  OPENSSL_cleanse(&keys, sizeof(keys));
  co_await asio::async_write(state->stream, asio::buffer(encoded), asio::use_awaitable);
}

asio::awaitable<credentials> access_point::connect(endpoint address, credentials login, std::string device_id) try {
  /// Establish and authenticate a new connection, returning reusable credentials
  active_operation connection{state->connecting};
  if(state->stream.socket().is_open()) throw std::logic_error{"access point is already connected"};
  if(device_id.empty() || login.data.empty()) throw std::invalid_argument{"empty access-point login data or device ID"};
  emit_log(state->on_log, log_level::debug, "access_point", "Resolving " + address.host + ':' + address.port);
  auto const addresses{co_await detail::resolve(state->executor, address.host, address.port, state->timeout)};
  state->stream.expires_after(state->timeout);
  co_await state->stream.async_connect(addresses, asio::use_awaitable);
  emit_log(state->on_log, log_level::debug, "access_point", "TCP connected to " + address.host + ':' + address.port);
  state->send_nonce = state->receive_nonce = 0;
  co_await handshake();
  emit_log(state->on_log, log_level::debug, "access_point", "Handshake complete; sending login (type " + std::to_string(static_cast<unsigned int>(login.type)) + ')');
  protocol::ClientResponseEncrypted request;
  auto const credentials{request.mutable_login_credentials()};
  auto const type{static_cast<int>(login.type)};
  if(!protocol::AuthenticationType_IsValid(type)) throw std::invalid_argument{"invalid authentication type"};
  if(login.username) credentials->set_username(*login.username);
  credentials->set_typ(static_cast<protocol::AuthenticationType>(type));
  credentials->set_auth_data(login.data);
  request.mutable_system_info()->set_os(protocol::OS_LINUX);
#if defined(__aarch64__) || defined(__arm__)
  request.mutable_system_info()->set_cpu_family(protocol::CPU_ARM);
#else
  request.mutable_system_info()->set_cpu_family(protocol::CPU_X86_64);
#endif // architecture selection
  request.mutable_system_info()->set_device_id(std::move(device_id));
  request.mutable_system_info()->set_system_information_string("librespot-cpp");
  request.set_version_string("librespot-cpp " + std::string{librespot::version});
  co_await send(packet{.command{0xab}, .payload{serialise(request)}});
  auto const reply{co_await receive()};
  if(reply.command == 0xad) {
    auto const failure{parse<protocol::APLoginFailed>(reply.payload)};
    throw std::runtime_error{"access-point login failed: " + protocol::ErrorCode_Name(failure.error_code())};
  }
  if(reply.command != 0xac) throw std::runtime_error{"unexpected access-point login packet"};
  auto const welcome{parse<protocol::APWelcome>(reply.payload)};
  emit_log(state->on_log, log_level::info, "access_point", "APWelcome accepted; reusable credentials received");
  co_return librespot::credentials{
    .username{welcome.canonical_username()},
    .type{static_cast<authentication_type>(welcome.reusable_auth_credentials_type())},
    .data{welcome.reusable_auth_credentials()},
  };
} catch(...) {
  close();
  throw;
}

asio::awaitable<void> access_point::send(packet message) try {
  /// Authenticate command, length and payload with a unique packet nonce
  active_operation operation{state->writing};
  if(state->on_log) emit_log(state->on_log, log_level::trace, "access_point", std::format("Sending packet command=0x{:02x}; bytes={}", message.command, message.payload.size()));
  if(!state->encoder || !state->stream.socket().is_open()) throw std::logic_error{"access point is not connected"};
  if(message.payload.size() > 65535) throw std::invalid_argument{"access-point payload exceeds 65535 bytes"};
  if(state->send_nonce > std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error{"access-point send nonce exhausted"};
  std::vector<std::byte> bytes{
    static_cast<std::byte>(message.command),
    static_cast<std::byte>((message.payload.size() >> 8) & 255u),
    static_cast<std::byte>(message.payload.size() & 255u),
  };
  bytes.insert(bytes.end(), message.payload.begin(), message.payload.end());
  state->encoder->nonce(static_cast<std::uint32_t>(state->send_nonce++));
  state->encoder->encrypt(bytes);
  auto const mac{state->encoder->finish()};
  bytes.insert(bytes.end(), mac.begin(), mac.end());
  state->stream.expires_after(state->timeout);
  co_await asio::async_write(state->stream, asio::buffer(bytes), asio::use_awaitable);
} catch(...) {
  close();
  throw;
}

asio::awaitable<packet> access_point::receive() try {
  /// Release plaintext only after constant-time verification of the packet authenticator
  active_operation operation{state->reading};
  if(!state->decoder || !state->stream.socket().is_open()) throw std::logic_error{"access point is not connected"};
  if(state->receive_nonce > std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error{"access-point receive nonce exhausted"};
  std::array<std::byte, 3> header;
  state->stream.expires_after(state->timeout);
  co_await asio::async_read(state->stream, asio::buffer(header), asio::use_awaitable);
  state->decoder->nonce(static_cast<std::uint32_t>(state->receive_nonce++));
  state->decoder->decrypt(header);
  auto const length{std::to_integer<unsigned int>(header[1]) * 256u + std::to_integer<unsigned int>(header[2])};
  packet result{.command{std::to_integer<std::uint8_t>(header[0])}, .payload{std::vector<std::byte>(length)}};
  co_await asio::async_read(state->stream, asio::buffer(result.payload), asio::use_awaitable);
  state->decoder->decrypt(result.payload);
  std::array<std::byte, 4> expected;
  co_await asio::async_read(state->stream, asio::buffer(expected), asio::use_awaitable);
  auto const actual{state->decoder->finish()};
  if(CRYPTO_memcmp(actual.data(), expected.data(), actual.size()) != 0) throw std::runtime_error{"access-point packet MAC mismatch"};
  if(state->on_log) emit_log(state->on_log, log_level::trace, "access_point", std::format("Received packet command=0x{:02x}; bytes={}", result.command, result.payload.size()));
  co_return result;
} catch(...) {
  close();
  throw;
}

void access_point::close() {
  /// Cancel outstanding socket operations; destroy this object only once they finish
  boost::system::error_code error;
  state->stream.socket().close(error);
}

asio::awaitable<std::vector<endpoint>> resolve_access_points(http_transport &transport) {
  /// Resolve the service's current access-point list rather than hardcoding hosts
  auto const response{co_await transport.request(http_request{
    .host{"apresolve.spotify.com"},
    .port{"443"},
    .target{"/?type=accesspoint"},
    .method{"GET"},
    .headers{},
    .body{},
  })};
  if(response.status != 200) throw std::runtime_error{"access-point resolver returned HTTP " + std::to_string(response.status)};
  auto const object = nlohmann::json::parse(response.body);
  auto const &addresses{object.at("accesspoint")};
  if(!addresses.is_array()) throw std::runtime_error{"malformed access-point resolver response"};
  std::vector<endpoint> result;
  for(auto const &entry : addresses) {
    auto const address{entry.get<std::string>()};
    auto const separator{address.rfind(':')};
    if(separator == std::string::npos || separator == 0 || separator + 1 == address.size()) throw std::runtime_error{"invalid resolved access-point endpoint"};
    result.push_back({.host{address.substr(0, separator)}, .port{address.substr(separator + 1)}});
  }
  if(result.empty()) throw std::runtime_error{"access-point resolver returned no endpoints"};
  co_return result;
}

} // namespace librespot::net
