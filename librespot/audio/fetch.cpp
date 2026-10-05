#include "fetch.h"
#include <algorithm>
#include <charconv>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <boost/asio/error.hpp>
#include <boost/system/system_error.hpp>

namespace librespot::audio {
namespace {

net::http_request request_for(std::string_view url, std::uint64_t offset, std::uint64_t end) {
  /// Preserve signed query strings while restricting requests to HTTPS DNS endpoints
  if(!url.starts_with("https://") || url.find_first_of("\r\n\t ") != std::string_view::npos) throw std::invalid_argument{"invalid HTTPS audio URL"};
  url.remove_prefix(8);
  auto const separator{url.find_first_of("/?#")};
  auto const host{url.substr(0, separator)};
  if(host.empty() || host.find_first_of(":@") != std::string_view::npos || url.find('#') != std::string_view::npos) {
    throw std::invalid_argument{"invalid audio CDN authority"};
  }
  auto target{separator == std::string_view::npos ? std::string{"/"} : std::string{url.substr(separator)}};
  if(target.front() == '?') target.insert(target.begin(), '/');
  return {
    .host{host},
    .port{"443"},
    .target{std::move(target)},
    .method{"GET"},
    .headers{
      {"Range", "bytes=" + std::to_string(offset) + "-" + std::to_string(end)},
      {"Accept-Encoding", "identity"},
    },
    .body{},
  };
}

std::optional<std::string_view> header(net::http_response const &response, std::string_view wanted) {
  /// HTTP field names are case-insensitive even in injected transports
  for(auto const &[name, value] : response.headers) {
    if(std::ranges::equal(name, wanted, [](char first, char second){
      auto const lower{[](char character){
        return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
      }};
      return lower(first) == lower(second);
    })) return value;
  }
  return std::nullopt;
}

std::uint64_t integer(std::string_view text) {
  std::uint64_t value{0};
  auto const [end, error]{std::from_chars(text.data(), text.data() + text.size(), value)};
  if(error != std::errc{} || end != text.data() + text.size()) throw std::runtime_error{"invalid CDN content-range integer"};
  return value;
}

audio_range decode_range(net::http_response const &response, std::uint64_t offset, std::uint64_t requested_end) {
  /// Verify offsets and body length before allowing the caller to decrypt any bytes
  auto const range{header(response, "content-range")};
  if(!range || !range->starts_with("bytes ")) throw std::runtime_error{"missing CDN content-range"};
  auto const value{range->substr(6)};
  auto const dash{value.find('-')};
  auto const slash{value.find('/')};
  if(dash == std::string_view::npos || slash == std::string_view::npos || slash <= dash) throw std::runtime_error{"malformed CDN content-range"};
  auto const first{integer(value.substr(0, dash))};
  auto const last{integer(value.substr(dash + 1, slash - dash - 1))};
  auto const total{integer(value.substr(slash + 1))};
  if(first != offset || total <= first || last != std::min(requested_end, total - 1) || response.body.size() != last - first + 1) {
    throw std::runtime_error{"CDN returned an inconsistent audio range"};
  }
  if(auto const encoding{header(response, "content-encoding")}; encoding && *encoding != "identity") throw std::runtime_error{"CDN returned compressed audio range"};
  audio_range result{
    .offset{offset},
    .total_size{total},
    .bytes{std::vector<std::byte>(response.body.size())},
  };
  std::memcpy(result.bytes.data(), response.body.data(), response.body.size());
  return result;
}

} // anonymous namespace

boost::asio::awaitable<audio_range> fetch_range(net::http_transport &transport, std::vector<std::string> urls,
  std::uint64_t offset, std::size_t length, std::optional<audio_key> key) {
  /// Retry HTTP failures as well as transport errors, preserving coroutine cancellation
  if(length == 0 || length > 16 * 1024 * 1024 || offset > std::numeric_limits<std::uint64_t>::max() - (length - 1)) {
    throw std::invalid_argument{"invalid audio range length or offset"};
  }
  auto const end{offset + length - 1};
  for(auto const &url : urls) {
    try {
      auto const response{co_await transport.request(request_for(url, offset, end))};
      if(response.status != 206) continue;
      auto result{decode_range(response, offset, end)};
      if(key) decrypt(*key, offset, result.bytes);
      co_return result;
    } catch(boost::system::system_error const &error) {
      if(error.code() == boost::asio::error::operation_aborted) throw;
    } catch(std::runtime_error const &) {
    } catch(std::invalid_argument const &) {
    }
  }
  throw std::runtime_error{"no audio CDN returned a valid requested range"};
}

} // namespace librespot::audio
