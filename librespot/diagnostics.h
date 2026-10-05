#pragma once
#include <exception>
#include <functional>
#include <string>
#include <string_view>

namespace librespot {

enum class log_level {
  trace, debug, info, warning, error, off,
};

struct log_event {
  log_level level;
  std::string component;
  std::string message;
};

using log_handler = std::function<void(log_event const &)>;

/// Synchronous, application-owned diagnostics; exceptions from the callback are contained
void emit_log(log_handler const &handler, log_level level, std::string_view component, std::string message) noexcept;
[[nodiscard]] std::string_view to_string(log_level level) noexcept;
/// Bounded protocol descriptions omit secrets, opaque credential blobs and URL queries
[[nodiscard]] std::string diagnostic_json(std::string const &json);
[[nodiscard]] std::string diagnostic_error(std::exception const &error);
[[nodiscard]] std::string diagnostic_url(std::string_view url);

} // namespace librespot
