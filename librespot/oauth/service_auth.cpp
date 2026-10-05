#include "service_auth.h"
#include <algorithm>
#include <optional>
#include <stdexcept>
#include <sys/utsname.h>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include "librespot/crypto/hashcash.h"
#include "spotify/clienttoken/v0/clienttoken_http.pb.h"
#include "spotify/login5/v3/login5.pb.h"

namespace librespot::oauth {
namespace asio = boost::asio;
namespace client_protocol = spotify::clienttoken::http::v0;
namespace login_protocol = spotify::login5::v3;
namespace {

std::span<std::byte const> bytes(std::string const &value) {
  return std::as_bytes(std::span{value});
}

asio::awaitable<crypto::hashcash_solution> solve(std::string context, std::string prefix, int difficulty) {
  if(difficulty < 0) throw std::runtime_error{"invalid hashcash difficulty"};
  co_return crypto::solve_hashcash(bytes(context), bytes(prefix), static_cast<unsigned int>(difficulty));
}

std::string decode_hex(std::string const &hex) {
  if(hex.size() % 2 != 0 || hex.size() > 2048) throw std::runtime_error{"invalid hashcash prefix"};
  auto nibble{[](char digit)->unsigned int {
    if(digit >= '0' && digit <= '9') return static_cast<unsigned int>(digit - '0');
    if(digit >= 'a' && digit <= 'f') return static_cast<unsigned int>(digit - 'a' + 10);
    if(digit >= 'A' && digit <= 'F') return static_cast<unsigned int>(digit - 'A' + 10);
    throw std::runtime_error{"invalid hashcash prefix"};
  }};
  std::string result(hex.size() / 2, '\0');
  for(std::size_t index{0}; index < result.size(); ++index) result[index] = static_cast<char>(nibble(hex[index * 2]) * 16 + nibble(hex[index * 2 + 1]));
  return result;
}

std::string encode_hex(std::span<std::byte const> bytes) {
  std::string result;
  for(auto byte : bytes) {
    auto const value{std::to_integer<unsigned int>(byte)};
    result += "0123456789ABCDEF"[value >> 4];
    result += "0123456789ABCDEF"[value & 15];
  }
  return result;
}

void validate_token(std::string const &value) {
  if(value.empty() || std::ranges::any_of(value, [](unsigned char byte){
    return byte <= 32 || byte >= 127;
  })) throw std::runtime_error{"invalid service credential"};
}

std::chrono::steady_clock::time_point expiry(int seconds) {
  if(seconds <= 0 || seconds > 31'536'000) throw std::runtime_error{"invalid service token lifetime"};
  return std::chrono::steady_clock::now() + std::chrono::seconds{seconds};
}

} // anonymous namespace

struct service_auth::implementation {
  net::http_transport &transport;
  service_auth_config config;
  credentials stored;
  asio::any_io_executor worker;
  std::optional<access_token> client;
  std::optional<access_token> access;

  implementation(net::http_transport &transport, service_auth_config config, credentials stored, asio::any_io_executor worker)
    : transport{transport}, config{std::move(config)}, stored{std::move(stored)}, worker{std::move(worker)} {
    if(this->config.device_id.empty() || this->config.client_id.empty() || this->config.client_token_id.empty()
      || this->config.client_version.empty()) throw std::invalid_argument{"invalid service identity"};
    if(!this->stored.username || this->stored.username->empty() || this->stored.data.empty()
      || this->stored.type != authentication_type::stored_spotify) throw std::invalid_argument{"Login5 requires reusable Spotify credentials"};
  }

  template<typename Response, typename Request>
  asio::awaitable<Response> exchange(std::string host, std::string target, Request message, std::optional<std::string> client_token = {}) {
    emit_log(config.on_log, log_level::debug, "auth", "POST " + host + target + "; protobuf bytes=" + std::to_string(message.ByteSizeLong()));
    net::http_request request{
      .host{std::move(host)}, .port{"443"}, .target{std::move(target)}, .method{"POST"},
      .headers{{"Accept", "application/x-protobuf"}, {"Content-Type", "application/x-protobuf"}},
      .body{message.SerializeAsString()},
    };
    if(client_token) request.headers.emplace("client-token", *client_token);
    auto const response{co_await transport.request(std::move(request))};
    emit_log(config.on_log, log_level::debug, "auth", "Authentication response HTTP " + std::to_string(response.status) + "; protobuf bytes=" + std::to_string(response.body.size()));
    if(response.status != 200) throw std::runtime_error{"service authentication returned HTTP " + std::to_string(response.status)};
    Response decoded;
    if(!decoded.ParseFromString(response.body)) throw std::runtime_error{"malformed service authentication response"};
    co_return decoded;
  }
};

service_auth::service_auth(net::http_transport &transport, service_auth_config config, credentials stored, asio::any_io_executor worker)
  : state{std::make_unique<implementation>(transport, std::move(config), std::move(stored), std::move(worker))} {
}

service_auth::~service_auth() = default;

asio::awaitable<std::string> service_auth::client_token() {
  if(state->client && !state->client->expired()) {
    emit_log(state->config.on_log, log_level::trace, "auth", "Using cached client token");
    co_return state->client->value;
  }
  emit_log(state->config.on_log, log_level::debug, "auth", "Requesting client token");
  client_protocol::ClientTokenRequest request;
  request.set_request_type(client_protocol::REQUEST_CLIENT_DATA_REQUEST);
  auto &data{*request.mutable_client_data()};
  data.set_client_id(state->config.client_token_id);
  data.set_client_version(state->config.client_version);
  data.mutable_connectivity_sdk_data()->set_device_id(state->config.device_id);
  utsname platform{};
  if(uname(&platform) != 0) throw std::runtime_error{"cannot read Linux platform identity"};
  auto &linux{*data.mutable_connectivity_sdk_data()->mutable_platform_specific_data()->mutable_desktop_linux()};
  linux.set_system_name(platform.sysname);
  linux.set_system_release(platform.release);
  linux.set_system_version(platform.version);
  linux.set_hardware(platform.machine);
  for(unsigned int attempt{0}; attempt < 3; ++attempt) {
    auto const response{co_await state->exchange<client_protocol::ClientTokenResponse>("clienttoken.spotify.com", "/v1/clienttoken", request)};
    if(response.response_type() == client_protocol::RESPONSE_GRANTED_TOKEN_RESPONSE && response.has_granted_token()) {
      auto const &granted{response.granted_token()};
      validate_token(granted.token());
      auto lifetime{granted.expires_after_seconds()};
      if(granted.refresh_after_seconds() > 0) lifetime = std::min(lifetime, granted.refresh_after_seconds());
      state->client = access_token{.value{granted.token()}, .type{"Bearer"}, .scopes{}, .expires_at{expiry(lifetime)}};
      emit_log(state->config.on_log, log_level::debug, "auth", "Client token granted; refresh in " + std::to_string(lifetime) + " seconds");
      co_return state->client->value;
    }
    if(response.response_type() != client_protocol::RESPONSE_CHALLENGES_RESPONSE || !response.has_challenges()) throw std::runtime_error{"client token was not granted"};
    auto const &challenges{response.challenges()};
    if(challenges.challenges_size() == 0 || challenges.challenges_size() > 8) throw std::runtime_error{"invalid client token challenges"};
    request.Clear();
    request.set_request_type(client_protocol::REQUEST_CHALLENGE_ANSWERS_REQUEST);
    request.mutable_challenge_answers()->set_state(challenges.state());
    for(auto const &challenge : challenges.challenges()) {
      if(challenge.type() != client_protocol::CHALLENGE_HASH_CASH || !challenge.has_evaluate_hashcash_parameters()) throw std::runtime_error{"unsupported client token challenge"};
      auto const &parameters{challenge.evaluate_hashcash_parameters()};
      emit_log(state->config.on_log, log_level::debug, "auth", "Solving client-token hashcash challenge; difficulty=" + std::to_string(parameters.length()));
      auto solution{co_await asio::co_spawn(state->worker, solve({}, decode_hex(parameters.prefix()), parameters.length()), asio::use_awaitable)};
      auto &answer{*request.mutable_challenge_answers()->add_answers()};
      answer.set_challengetype(client_protocol::CHALLENGE_HASH_CASH);
      answer.mutable_hash_cash()->set_suffix(encode_hex(solution.suffix));
    }
  }
  throw std::runtime_error{"client token challenge attempts exhausted"};
}

asio::awaitable<access_token> service_auth::token() {
  if(state->access && !state->access->expired()) {
    emit_log(state->config.on_log, log_level::trace, "auth", "Using cached Login5 access token");
    co_return *state->access;
  }
  emit_log(state->config.on_log, log_level::debug, "auth", "Authenticating reusable credentials through Login5");
  auto client{co_await client_token()};
  login_protocol::LoginRequest request;
  request.mutable_client_info()->set_client_id(state->config.client_id);
  request.mutable_client_info()->set_device_id(state->config.device_id);
  request.mutable_stored_credential()->set_username(*state->stored.username);
  request.mutable_stored_credential()->set_data(state->stored.data);
  for(unsigned int attempt{0}; attempt < 3; ++attempt) {
    auto const response{co_await state->exchange<login_protocol::LoginResponse>("login5.spotify.com", "/v3/login", request, client)};
    if(response.has_ok()) {
      auto const &ok{response.ok()};
      validate_token(ok.access_token());
      state->access = access_token{.value{ok.access_token()}, .type{"Bearer"}, .scopes{}, .expires_at{expiry(ok.access_token_expires_in())}};
      emit_log(state->config.on_log, log_level::info, "auth", "Login5 authentication accepted; token expires in " + std::to_string(ok.access_token_expires_in()) + " seconds");
      if(!ok.stored_credential().empty()) state->stored.data = ok.stored_credential();
      co_return *state->access;
    }
    if(response.has_error()) {
      if(response.error() != login_protocol::TIMEOUT && response.error() != login_protocol::TOO_MANY_ATTEMPTS) throw std::runtime_error{"Login5 rejected credentials (code " + std::to_string(response.error()) + ')'};
      asio::steady_timer timer{co_await asio::this_coro::executor, std::chrono::seconds{3}};
      co_await timer.async_wait(asio::use_awaitable);
      continue;
    }
    if(!response.has_challenges() || response.challenges().challenges_size() == 0 || response.challenges().challenges_size() > 8) throw std::runtime_error{"invalid Login5 challenges"};
    request.mutable_challenge_solutions()->clear_solutions();
    for(auto const &challenge : response.challenges().challenges()) {
      if(!challenge.has_hashcash()) throw std::runtime_error{"unsupported Login5 challenge"};
      auto const &parameters{challenge.hashcash()};
      emit_log(state->config.on_log, log_level::debug, "auth", "Solving Login5 hashcash challenge; difficulty=" + std::to_string(parameters.length()));
      auto solution{co_await asio::co_spawn(state->worker, solve(response.login_context(), parameters.prefix(), parameters.length()), asio::use_awaitable)};
      auto &answer{*request.mutable_challenge_solutions()->add_solutions()->mutable_hashcash()};
      answer.set_suffix(solution.suffix.data(), solution.suffix.size());
      auto const seconds{std::chrono::duration_cast<std::chrono::seconds>(solution.elapsed)};
      answer.mutable_duration()->set_seconds(seconds.count());
      answer.mutable_duration()->set_nanos(static_cast<int>((solution.elapsed - seconds).count()));
    }
    request.set_login_context(response.login_context());
  }
  throw std::runtime_error{"Login5 attempts exhausted"};
}

void service_auth::invalidate_token() {
  state->access.reset();
}

} // namespace librespot::oauth
