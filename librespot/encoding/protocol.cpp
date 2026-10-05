#include "protocol.h"
#include <array>
#include <limits>
#include <stdexcept>
#include <zlib.h>
#include "base64.h"

namespace librespot::encoding {

std::string decode_base64(std::string const &text) {
  /// Accept only canonical base64 within the protocol message budget
  if(text.size() > 16 * 1024 * 1024 || text.size() % 4 != 0) throw std::runtime_error{"invalid base64 length"};
  auto const padding{text.ends_with("==") ? 2u : text.ends_with('=') ? 1u : 0u};
  for(std::size_t index{0}; index < text.size() - padding; ++index) {
    if(std::string_view{"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"}.find(text[index]) == std::string_view::npos) throw std::runtime_error{"invalid base64 alphabet"};
  }
  auto const result{base64::decode(text)};
  if(base64::encode(result) != text) throw std::runtime_error{"noncanonical base64 padding"};
  return result;
}

std::string inflate_gzip(std::string_view bytes) {
  /// Bound both compressed input and decompressed output to prevent oversized pushes
  if(bytes.size() > 16 * 1024 * 1024) throw std::runtime_error{"compressed message too large"};
  z_stream stream{};
  stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(bytes.data()));
  stream.avail_in = static_cast<uInt>(bytes.size());
  if(inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) throw std::runtime_error{"gzip initialisation failed"};
  struct cleanup {
    z_stream &stream;
    ~cleanup() {
      inflateEnd(&stream);
    }
  } guard{stream};
  std::string result;
  std::array<char, 32768> buffer{};
  int status{Z_OK};
  do {
    stream.next_out = reinterpret_cast<Bytef *>(buffer.data());
    stream.avail_out = static_cast<uInt>(buffer.size());
    status = inflate(&stream, Z_NO_FLUSH);
    if(status != Z_OK && status != Z_STREAM_END) throw std::runtime_error{"malformed gzip message"};
    auto const count{buffer.size() - stream.avail_out};
    if(count > 16 * 1024 * 1024 - result.size()) throw std::runtime_error{"decompressed message too large"};
    result.append(buffer.data(), count);
  } while(status != Z_STREAM_END);
  if(stream.avail_in != 0) throw std::runtime_error{"trailing gzip data"};
  return result;
}

} // namespace librespot::encoding
