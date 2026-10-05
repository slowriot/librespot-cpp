#include "mercury.h"
#include <limits>
#include <stdexcept>
#include "mercury.pb.h"

namespace librespot::net {
namespace {

void append_u16(std::vector<std::byte> &bytes, std::size_t value) {
  if(value > 65535) throw std::invalid_argument{"Mercury field exceeds sixteen-bit length"};
  bytes.push_back(static_cast<std::byte>((value >> 8) & 255u));
  bytes.push_back(static_cast<std::byte>(value & 255u));
}

void append_part(std::vector<std::byte> &bytes, std::span<std::byte const> part) {
  append_u16(bytes, part.size());
  bytes.insert(bytes.end(), part.begin(), part.end());
}

std::size_t read_u16(std::span<std::byte const> &bytes) {
  if(bytes.size() < 2) throw std::runtime_error{"truncated Mercury length"};
  auto const result{std::to_integer<unsigned int>(bytes[0]) * 256u + std::to_integer<unsigned int>(bytes[1])};
  bytes = bytes.subspan(2);
  return result;
}

std::vector<std::byte> read_part(std::span<std::byte const> &bytes) {
  auto const length{read_u16(bytes)};
  if(length > bytes.size()) throw std::runtime_error{"truncated Mercury part"};
  auto const view{bytes.first(length)};
  std::vector<std::byte> result(view.begin(), view.end());
  bytes = bytes.subspan(length);
  return result;
}

} // anonymous namespace

packet encode_mercury(mercury_request const &request, std::uint64_t sequence) {
  /// Encode a complete request with the protocol's eight-byte sequence number
  if(request.uri.empty()) throw std::invalid_argument{"empty Mercury URI"};
  protocol::Header header;
  header.set_uri(request.uri);
  if(request.content_type) header.set_content_type(*request.content_type);
  std::uint8_t command{0xb2};
  switch(request.method) {
  case mercury_method::get: header.set_method("GET"); break;
  case mercury_method::send: header.set_method("SEND"); break;
  case mercury_method::subscribe: header.set_method("SUB"); command = 0xb3; break;
  case mercury_method::unsubscribe: header.set_method("UNSUB"); command = 0xb4; break;
  default: throw std::invalid_argument{"invalid Mercury method"};
  }
  if(request.payload.size() >= 65535) throw std::invalid_argument{"too many Mercury parts"};
  packet result{.command{command}, .payload{}};
  append_u16(result.payload, 8);
  for(unsigned int index{0}; index < 8; ++index) result.payload.push_back(static_cast<std::byte>((sequence >> ((7 - index) * 8)) & 255u));
  result.payload.push_back(std::byte{1});
  append_u16(result.payload, request.payload.size() + 1);
  auto const encoded_header{header.SerializeAsString()};
  append_part(result.payload, std::as_bytes(std::span{encoded_header}));
  for(auto const &part : request.payload) append_part(result.payload, part);
  if(result.payload.size() > 65535) throw std::invalid_argument{"Mercury request exceeds access-point packet length"};
  return result;
}

mercury_frame decode_mercury(std::span<std::byte const> payload) {
  /// Validate all lengths before reading an untrusted frame
  mercury_frame result{.sequence{read_part(payload)}, .flags{0}, .parts{}};
  if(result.sequence.empty() || payload.empty()) throw std::runtime_error{"missing Mercury sequence or flags"};
  result.flags = std::to_integer<unsigned int>(payload.front());
  if(result.flags > 2) throw std::runtime_error{"invalid Mercury flags"};
  payload = payload.subspan(1);
  auto const count{read_u16(payload)};
  if(count > payload.size() / 2) throw std::runtime_error{"truncated Mercury part list"};
  result.parts.reserve(count);
  for(std::size_t index{0}; index < count; ++index) result.parts.push_back(read_part(payload));
  if(!payload.empty()) throw std::runtime_error{"trailing bytes in Mercury frame"};
  return result;
}

mercury_assembler::mercury_assembler(std::size_t size_limit) : size_limit{size_limit} {
  if(size_limit == 0 || size_limit > static_cast<std::size_t>(std::numeric_limits<int>::max())) throw std::invalid_argument{"invalid Mercury response size limit"};
}

std::optional<mercury_response> mercury_assembler::append(mercury_frame frame) {
  /// Join fragmented parts without allowing unrelated sequences or unbounded growth
  if(complete) throw std::logic_error{"Mercury response already complete"};
  if(frame.sequence.empty() || frame.flags > 2) throw std::invalid_argument{"invalid Mercury frame"};
  if(sequence.empty()) sequence = frame.sequence;
  if(sequence != frame.sequence) throw std::invalid_argument{"interleaved Mercury sequence"};
  if(frame.flags == 2 && frame.parts.empty()) throw std::invalid_argument{"partial Mercury frame has no part"};
  for(std::size_t index{0}; index < frame.parts.size(); ++index) {
    auto &part{frame.parts[index]};
    if(part.size() > size_limit - received) throw std::runtime_error{"Mercury response exceeds size limit"};
    received += part.size();
    if(partial) {
      partial->insert(partial->end(), part.begin(), part.end());
      part = std::move(*partial);
      partial.reset();
    }
    if(frame.flags == 2 && index + 1 == frame.parts.size()) partial = std::move(part);
    else parts.push_back(std::move(part));
    if(parts.size() > 65535) throw std::runtime_error{"too many assembled Mercury parts"};
  }
  if(frame.flags != 1) return std::nullopt;
  if(partial || parts.empty()) throw std::runtime_error{"incomplete Mercury response"};
  complete = true;
  protocol::Header header;
  if(!header.ParseFromArray(parts.front().data(), static_cast<int>(parts.front().size())) || !header.has_status_code() || !header.has_uri()) {
    throw std::runtime_error{"invalid Mercury response header"};
  }
  parts.erase(parts.begin());
  return mercury_response{
    .uri{header.uri()},
    .status{header.status_code()},
    .content_type{header.has_content_type() ? std::optional<std::string>{header.content_type()} : std::nullopt},
    .payload{std::move(parts)},
  };
}

} // namespace librespot::net
