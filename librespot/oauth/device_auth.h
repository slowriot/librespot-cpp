#pragma once
#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <boost/asio/awaitable.hpp>
#include "librespot/net/http_client.h"

namespace librespot::oauth {

struct token {
  std::string access_token;
  std::optional<std::string> refresh_token;
  std::string scope;
  std::chrono::seconds expires_in{0};
};

struct device_challenge {
  std::string device_code;
  std::string user_code;
  std::string verification_uri;
  std::optional<std::string> verification_uri_complete;
  std::chrono::seconds expires_in{0};
  std::chrono::seconds interval{5};
};

enum class poll_status {
  pending,
  slow_down,
};

using poll_result = std::variant<token, poll_status>;

class auth_error : public std::runtime_error {
private:
  std::string error_code;

public:
  auth_error(std::string code, std::string description);
  [[nodiscard]] std::string_view code() const noexcept;
};

/// Call poll no faster than the challenge interval; slow_down adds five seconds
class device_auth {
private:
  net::http_transport &transport;
  std::string client_id;

public:
  device_auth(net::http_transport &transport, std::string client_id);
  boost::asio::awaitable<device_challenge> start(std::string scope = "streaming");
  boost::asio::awaitable<poll_result> poll(std::string device_code);
  boost::asio::awaitable<token> refresh(std::string refresh_token);
};

} // namespace librespot::oauth
