#include "access_token.h"
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace librespot::oauth {

bool access_token::expired(std::chrono::steady_clock::time_point now) const noexcept {
  /// Reserve ten seconds for in-flight requests without relying on the wall clock
  return now >= expires_at || expires_at - now <= std::chrono::seconds{10};
}

bool access_token::covers(std::span<std::string const> requested) const {
  return std::ranges::all_of(requested, [this](std::string const &scope){
    return std::ranges::find(scopes, scope) != scopes.end();
  });
}

access_token decode_access_token(std::string_view body, std::chrono::steady_clock::time_point now) {
  try {
    auto data = nlohmann::json::parse(body);
    auto const &duration{data.at("expiresIn")};
    if(!duration.is_number_integer() || duration < 0 || duration > 31'536'000) {
      throw std::runtime_error{"invalid service token lifetime"};
    }
    auto const lifetime{std::chrono::seconds{duration.get<std::int64_t>()}};
    if(now > std::chrono::steady_clock::time_point::max() - lifetime) throw std::runtime_error{"service token expiry overflow"};
    access_token result{
      .value{data.at("accessToken").get<std::string>()},
      .type{data.at("tokenType").get<std::string>()},
      .scopes{data.at("scope").get<std::vector<std::string>>()},
      .expires_at{now + lifetime},
    };
    if(result.value.empty() || result.type != "Bearer"
      || result.value.find_first_of("\r\n") != std::string::npos) throw std::runtime_error{"invalid service token"};
    return result;
  } catch(nlohmann::json::exception const &) {
    throw std::runtime_error{"invalid service token response"};
  }
}

} // namespace librespot::oauth
