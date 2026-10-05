#include "cdn_source.h"
#include <algorithm>
#include <atomic>
#include <future>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>

namespace librespot::audio {
namespace asio = boost::asio;

struct cdn_source::implementation {
  asio::strand<asio::any_io_executor> executor;
  net::http_transport &transport;
  std::vector<std::string> urls;
  std::optional<audio_key> key;
  cdn_source_config config;
  std::mutex reader_mutex;
  std::atomic<bool> cancelled{false};
  asio::cancellation_signal cancellation;
  std::optional<std::uint64_t> total;
  std::optional<audio_range> cached;

  implementation(asio::any_io_executor executor, net::http_transport &transport,
    std::vector<std::string> urls, std::optional<audio_key> key, cdn_source_config config)
    : executor{asio::make_strand(executor)}, transport{transport}, urls{std::move(urls)}, key{key}, config{config} {
    if(this->urls.empty() || config.chunk_size == 0 || config.chunk_size > 16 * 1024 * 1024) throw std::invalid_argument{"invalid CDN source configuration"};
  }

  static asio::awaitable<audio_range> fetch(std::shared_ptr<implementation> self, std::uint64_t offset) {
    if(self->cancelled.load()) throw boost::system::system_error{asio::error::operation_aborted};
    co_return co_await fetch_range(self->transport, self->urls, offset, self->config.chunk_size, self->key);
  }

  static void load(std::shared_ptr<implementation> const &self, std::uint64_t offset) {
    if(self->cancelled.load()) throw boost::system::system_error{asio::error::operation_aborted};
    /// The promise wakes the decoder worker; network work remains on the HTTP executor
    auto promise{std::make_shared<std::promise<audio_range>>()};
    auto future{promise->get_future()};
    asio::post(self->executor, [self, offset, promise]{
      try {
        asio::co_spawn(self->executor, fetch(self, offset),
          asio::bind_cancellation_slot(self->cancellation.slot(), [promise](std::exception_ptr failure, audio_range range){
            if(failure) promise->set_exception(failure);
            else promise->set_value(std::move(range));
          }));
      } catch(...) {
        promise->set_exception(std::current_exception());
      }
    });
    auto range{future.get()};
    if(self->total && *self->total != range.total_size) throw std::runtime_error{"CDN file size changed while streaming"};
    if(range.total_size <= self->config.container_offset || range.total_size > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) throw std::runtime_error{"invalid streamed audio size"};
    self->total = range.total_size;
    self->cached = std::move(range);
  }
};

cdn_source::cdn_source(asio::any_io_executor executor, net::http_transport &transport,
  std::vector<std::string> urls, std::optional<audio_key> key, cdn_source_config config)
  : state{std::make_shared<implementation>(std::move(executor), transport, std::move(urls), key, config)} {
}

cdn_source::~cdn_source() {
  cancel();
}

std::uint64_t cdn_source::size() {
  std::lock_guard lock{state->reader_mutex};
  if(state->cancelled.load()) throw boost::system::system_error{asio::error::operation_aborted};
  if(!state->total) implementation::load(state, 0);
  return *state->total - state->config.container_offset;
}

std::size_t cdn_source::read_at(std::uint64_t offset, std::span<std::byte> destination) {
  std::lock_guard lock{state->reader_mutex};
  if(state->cancelled.load()) throw boost::system::system_error{asio::error::operation_aborted};
  if(destination.empty()) return 0;
  if(!state->total) implementation::load(state, 0);
  auto const size{*state->total - state->config.container_offset};
  if(offset >= size) return 0;
  auto const original_offset{offset + state->config.container_offset};
  if(!state->cached || original_offset < state->cached->offset || original_offset - state->cached->offset >= state->cached->bytes.size()) {
    auto const chunk_offset{original_offset / state->config.chunk_size * state->config.chunk_size};
    implementation::load(state, chunk_offset);
  }
  auto const begin{static_cast<std::size_t>(original_offset - state->cached->offset)};
  auto const count{std::min(destination.size(), state->cached->bytes.size() - begin)};
  std::ranges::copy(std::span{state->cached->bytes}.subspan(begin, count), destination.begin());
  return count;
}

void cdn_source::cancel() {
  if(state->cancelled.exchange(true)) return;
  asio::post(state->executor, [self{state}]{
    self->cancellation.emit(asio::cancellation_type::all);
  });
}

} // namespace librespot::audio
