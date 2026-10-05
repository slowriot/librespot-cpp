#include "base64.h"
#include <stdexcept>
#include <boost/archive/iterators/base64_from_binary.hpp>
#include <boost/archive/iterators/binary_from_base64.hpp>
#include <boost/archive/iterators/transform_width.hpp>

namespace base64 {

std::string encode(std::string const &input) {
  /// Encode a string as base64 - adapted from https://stackoverflow.com/a/28471421/1678468
  using it = boost::archive::iterators::base64_from_binary<boost::archive::iterators::transform_width<std::string::const_iterator, 6, 8>>;
  std::string tmp{it(std::begin(input)), it(std::end(input))};
  return tmp.append((3 - input.size() % 3) % 3, '=');
};

std::string decode(std::string const &input) {
  /// Decode binary data, removing only bytes introduced by Base64 padding
  using it = boost::archive::iterators::transform_width<boost::archive::iterators::binary_from_base64<std::string::const_iterator>, 8, 6>;
  std::string decoded{it(std::begin(input)), it(std::end(input))};
  size_t const padding{input.ends_with("==") ? 2u : input.ends_with('=') ? 1u : 0u};
  if(padding > decoded.size()) throw std::invalid_argument{"Base64 padding exceeds decoded size"};
  decoded.resize(decoded.size() - padding);
  return decoded;
}

} // namespace base64
