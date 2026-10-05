#include "device_auth.h"
#include <cstdint>
#include <utility>
#include <nlohmann/json.hpp>

namespace librespot::oauth {
namespace {

nlohmann::json parse_response(net::http_response const &response) {
  /// Translate malformed server payloads into an OAuth-specific error
  auto result = nlohmann::json::parse(response.body, nullptr, false);
  if(result.is_discarded() || !result.is_object()) throw auth_error{"invalid_response", "expected a JSON object"};
  return result;
}

std::string required_string(nlohmann::json const &object, std::string_view name) {
  /// Require nonempty strings without including secrets in diagnostics
  auto const field{object.find(std::string{name})};
  if(field == object.end() || !field->is_string() || field->get_ref<std::string const&>().empty()) {
    throw auth_error{"invalid_response", "missing or invalid " + std::string{name}};
  }
  return field->get<std::string>();
}

std::optional<std::string> optional_string(nlohmann::json const &object, std::string_view name) {
  /// Distinguish omitted token fields from malformed fields
  if(!object.contains(std::string{name})) return std::nullopt;
  return required_string(object, name);
}

std::chrono::seconds required_seconds(nlohmann::json const &object, std::string_view name) {
  /// Bound durations so converting untrusted JSON integers cannot overflow
  auto const field{object.find(std::string{name})};
  if(field == object.end() || !field->is_number_integer() || *field <= 0 || *field > 31'536'000) {
    throw auth_error{"invalid_response", "missing or invalid " + std::string{name}};
  }
  return std::chrono::seconds{field->get<std::int64_t>()};
}

[[noreturn]] void throw_response_error(nlohmann::json const &object) {
  /// Propagate OAuth error codes without echoing arbitrary server descriptions
  throw auth_error{required_string(object, "error"), "authorisation request failed"};
}

token parse_token(nlohmann::json const &object) {
  /// Preserve optional refresh tokens and verify the supported authorisation scheme
  auto const type{required_string(object, "token_type")};
  if(type != "Bearer" && type != "bearer") throw auth_error{"invalid_response", "unsupported token type"};
  return {
    .access_token{required_string(object, "access_token")},
    .refresh_token{optional_string(object, "refresh_token")},
    .scope{optional_string(object, "scope").value_or("")},
    .expires_in{required_seconds(object, "expires_in")},
  };
}

net::http_request form_request(std::string target, std::string body) {
  /// Route account requests through the injected HTTPS transport
  return {
    .host{"accounts.spotify.com"},
    .port{"443"},
    .target{std::move(target)},
    .method{"POST"},
    .headers{{"Content-Type", "application/x-www-form-urlencoded"}},
    .body{std::move(body)},
  };
}

} // anonymous namespace

auth_error::auth_error(std::string code, std::string description)
  : std::runtime_error{"OAuth " + code + ": " + description},
    error_code{std::move(code)} {
  /// Keep the machine-readable error available to the application
}

std::string_view auth_error::code() const noexcept {
  return error_code;
}

device_auth::device_auth(net::http_transport &transport, std::string client_id)
  : transport{transport},
    client_id{std::move(client_id)} {
  /// Require a client ID enabled by Spotify for the device grant
  if(this->client_id.empty()) throw std::invalid_argument{"empty OAuth client ID"};
}

boost::asio::awaitable<device_challenge> device_auth::start(std::string scope) {
  /// Obtain pairing instructions without requiring a local HTTP listener
  auto const response{co_await transport.request(form_request("/oauth2/device/authorize",
    "client_id=" + net::form_encode(client_id) + "&scope=" + net::form_encode(scope)))};
  auto const object = parse_response(response);
  if(response.status != 200) throw_response_error(object);
  co_return device_challenge{
    .device_code{required_string(object, "device_code")},
    .user_code{required_string(object, "user_code")},
    .verification_uri{required_string(object, "verification_uri")},
    .verification_uri_complete{optional_string(object, "verification_uri_complete")},
    .expires_in{required_seconds(object, "expires_in")},
    .interval{object.contains("interval") ? required_seconds(object, "interval") : std::chrono::seconds{5}},
  };
}

boost::asio::awaitable<poll_result> device_auth::poll(std::string device_code) {
  /// Perform one poll; the application owns scheduling and expiry of the challenge
  if(device_code.empty()) throw std::invalid_argument{"empty OAuth device code"};
  auto const response{co_await transport.request(form_request("/api/token",
    "client_id=" + net::form_encode(client_id) + "&grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code&device_code=" + net::form_encode(device_code)))};
  auto const object = parse_response(response);
  if(response.status == 200) co_return parse_token(object);
  auto const error{required_string(object, "error")};
  if(response.status == 400 && error == "authorization_pending") co_return poll_status::pending;
  if(response.status == 400 && error == "slow_down") co_return poll_status::slow_down;
  throw_response_error(object);
}

boost::asio::awaitable<token> device_auth::refresh(std::string refresh_token) {
  /// Exchange a refresh token, retaining it when Spotify omits a replacement
  if(refresh_token.empty()) throw std::invalid_argument{"empty OAuth refresh token"};
  auto const response{co_await transport.request(form_request("/api/token",
    "client_id=" + net::form_encode(client_id) + "&grant_type=refresh_token&refresh_token=" + net::form_encode(refresh_token)))};
  auto const object = parse_response(response);
  if(response.status != 200) throw_response_error(object);
  auto result{parse_token(object)};
  if(!result.refresh_token) result.refresh_token = std::move(refresh_token);
  co_return result;
}

} // namespace librespot::oauth
