#include "diagnostics.h"
#include <utility>
#include <boost/system/system_error.hpp>
#include <nlohmann/json.hpp>

namespace librespot {
namespace {

std::string normalise(std::string_view key) {
  std::string result;
  for(char character : key) {
    auto const byte{static_cast<unsigned char>(character)};
    if(byte >= 'A' && byte <= 'Z') result += static_cast<char>(byte - 'A' + 'a');
    else if((byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9')) result += static_cast<char>(byte);
  }
  return result;
}

bool secret(std::string const &key) {
  return (key.find("token") != std::string::npos && key != "tokentype") || key.find("password") != std::string::npos
    || key.find("credential") != std::string::npos || key.find("secret") != std::string::npos
    || key == "authorization" || key == "proxyauthorization" || key == "authdata" || key == "cookie" || key == "setcookie"
    || key == "blob" || key == "compressed" || key == "clientkey" || key == "logincontext" || key == "transferdata";
}

nlohmann::json redact(nlohmann::json const &input, unsigned int depth) {
  if(depth >= 32) return "<depth limit>";
  if(input.is_object()) {
    auto result = nlohmann::json::object();
    unsigned int count{0};
    for(auto const &[key, value] : input.items()) {
      if(count++ >= 64) {
        result["<truncated>"] = "additional fields omitted";
        break;
      }
      auto const field{normalise(key)};
      if(secret(field)) result[key] = "<redacted>";
      else if(field == "data" && value.is_string() && input.contains("endpoint") && input.at("endpoint") == "transfer") result[key] = "<encoded transfer state>";
      else if(field == "payloads" && value.is_array()) {
        auto descriptions = nlohmann::json::array();
        for(auto const &payload : value) {
          if(descriptions.size() >= 64) break;
          if(payload.is_string()) descriptions.push_back("<encoded payload: " + std::to_string(payload.get_ref<std::string const &>().size()) + " bytes>");
          else if(payload.is_array()) descriptions.push_back("<binary payload: " + std::to_string(payload.size()) + " bytes>");
          else descriptions.push_back(redact(payload, depth + 1));
        }
        result[key] = std::move(descriptions);
      }
      else result[key] = redact(value, depth + 1);
    }
    return result;
  }
  if(input.is_array()) {
    auto result = nlohmann::json::array();
    for(auto const &value : input) {
      if(result.size() >= 64) {
        result.push_back("<additional items omitted>");
        break;
      }
      result.push_back(redact(value, depth + 1));
    }
    return result;
  }
  if(input.is_string()) {
    auto text{diagnostic_url(input.get_ref<std::string const &>())};
    if(text.size() > 1024) text = text.substr(0, 1024) + "<truncated>";
    return text;
  }
  return input;
}

} // anonymous namespace

void emit_log(log_handler const &handler, log_level level, std::string_view component, std::string message) noexcept {
  if(!handler) return;
  try {
    handler({.level{level}, .component{component}, .message{std::move(message)}});
  } catch(...) {
    // diagnostic consumers cannot change protocol behaviour
  }
}

std::string_view to_string(log_level level) noexcept {
  switch(level) {
  case log_level::trace: return "TRACE";
  case log_level::debug: return "DEBUG";
  case log_level::info: return "INFO";
  case log_level::warning: return "WARNING";
  case log_level::error: return "ERROR";
  case log_level::off: return "OFF";
  }
  return "UNKNOWN";
}

std::string diagnostic_url(std::string_view url) {
  if(url.find("://") == std::string_view::npos && !url.starts_with('/')) return std::string{url};
  auto const query{url.find_first_of("?#")};
  return std::string{url.substr(0, query)} + (query == std::string_view::npos ? "" : "?<redacted>");
}

std::string diagnostic_json(std::string const &json) {
  /// Avoid echoing malformed input, which may contain partially decoded credentials
  if(json.size() > 16 * 1024 * 1024) return "<JSON exceeds diagnostic input limit>";
  try {
    auto result{redact(nlohmann::json::parse(json), 0).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace)};
    if(result.size() > 8192) result = result.substr(0, 8192) + "<truncated>";
    return result;
  } catch(nlohmann::json::parse_error const &error) {
    return "<invalid JSON at byte " + std::to_string(error.byte) + "; " + std::to_string(json.size()) + " bytes>";
  } catch(nlohmann::json::exception const &) {
    return "<JSON diagnostic unavailable>";
  }
}

std::string diagnostic_error(std::exception const &error) {
  if(auto const *parse{dynamic_cast<nlohmann::json::parse_error const *>(&error)}) return "JSON parse error at byte " + std::to_string(parse->byte);
  if(auto const *system{dynamic_cast<boost::system::system_error const *>(&error)}) {
    return std::string{system->code().category().name()} + ':' + std::to_string(system->code().value()) + " (" + system->code().message() + ')';
  }
  auto message{std::string{error.what()}.substr(0, 1024)};
  for(auto &byte : message) if(static_cast<unsigned char>(byte) < 32) byte = ' ';
  return message;
}

} // namespace librespot
