#pragma once
#include <string>
#include <string_view>

namespace librespot::encoding {

[[nodiscard]] std::string decode_base64(std::string const &text);
[[nodiscard]] std::string inflate_gzip(std::string_view bytes);

} // namespace librespot::encoding
