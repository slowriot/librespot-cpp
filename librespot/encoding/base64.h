#pragma once

#include <string>

namespace base64 {

std::string encode(std::string const &input);
std::string decode(std::string const &input);

} // namespace base64
