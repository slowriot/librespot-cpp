#include <catch2/catch_test_macros.hpp>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include "librespot/diagnostics.h"

TEST_CASE("Protocol diagnostics redact credentials, signed URLs and opaque binary payloads") {
  auto const input{nlohmann::json{
    {"headers", {{"Authorization", "Bearer access-secret"}, {"client-token", "client-secret"}, {"Spotify-Connection-Id", "connection-id"}}},
    {"command", {{"endpoint", "play"}, {"context", {{"uri", "spotify:track:visible"}, {"url", "https://example/context?access_token=query-secret"}}}}},
    {"payload", {{"compressed", "opaque-secret"}}}, {"payloads", {"encoded-secret"}},
    {"credentials", {{"username", "user"}, {"data", "stored-secret"}}}, {"password", "password-secret"},
    {"userName", "visible-user"}, {"blob", "blob-secret"}, {"clientKey", "key-secret"}, {"tokenType", "default"},
  }.dump()};
  auto const output{librespot::diagnostic_json(input)};
  CHECK(output.find("secret") == std::string::npos);
  auto parsed = nlohmann::json::parse(output);
  CHECK(parsed.at("headers").at("Authorization") == "<redacted>");
  CHECK(parsed.at("headers").at("Spotify-Connection-Id") == "connection-id");
  CHECK(parsed.at("command").at("endpoint") == "play");
  CHECK(parsed.at("command").at("context").at("uri") == "spotify:track:visible");
  CHECK(parsed.at("userName") == "visible-user");
  CHECK(parsed.at("tokenType") == "default");
  CHECK(librespot::diagnostic_url("wss://dealer.example/?access_token=secret") == "wss://dealer.example/?<redacted>");
}

TEST_CASE("Diagnostics bound output, avoid malformed input echoes, and contain callback failures") {
  CHECK(librespot::diagnostic_json("{\"access_token\":\"secret\"").find("secret") == std::string::npos);
  try {
    auto invalid = nlohmann::json::parse("{\"token\":\"secret\"");
    FAIL("expected malformed JSON");
  } catch(nlohmann::json::parse_error const &error) {
    CHECK(librespot::diagnostic_error(error).find("secret") == std::string::npos);
    CHECK(librespot::diagnostic_error(error).find("byte") != std::string::npos);
  }
  CHECK(librespot::diagnostic_json(nlohmann::json{{"message", std::string(20000, 'x')}}.dump()).size() < 8192);
  auto object = nlohmann::json::object();
  for(unsigned int index{0}; index < 100; ++index) object[std::to_string(index)] = std::string(1024, 'x');
  CHECK(librespot::diagnostic_json(object.dump()).size() <= 8203);
  CHECK_NOTHROW(librespot::emit_log([](librespot::log_event const &){
    throw std::runtime_error{"failed logger"};
  }, librespot::log_level::error, "test", "protocol error"));
}
