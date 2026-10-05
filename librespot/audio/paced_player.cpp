#include "paced_player.h"
#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>

namespace librespot::audio {
namespace asio = boost::asio;

struct paced_player::implementation {
  using clock = std::chrono::steady_clock;
  asio::strand<asio::any_io_executor> worker;
  decoder_factory open;
  paced_player_config config;
  asio::steady_timer wake;
  mutable std::mutex mutex;
  playback_snapshot status;
  clock::time_point anchor{clock::now()};
  std::chrono::microseconds ceiling{0};
  std::uint64_t revision{0};
  bool stopped{false};
  bool started{false};
  bool decoding{true};

  implementation(asio::any_io_executor worker, decoder_factory open, paced_player_config config)
    : worker{asio::make_strand(std::move(worker))}, open{std::move(open)}, config{std::move(config)}, wake{this->worker} {
    status.position = this->config.position;
    status.paused = this->config.paused;
    ceiling = status.position;
  }

  playback_snapshot current() const {
    auto result{status};
    if(!result.paused && !result.buffering && !result.ended) {
      result.position = std::min(ceiling, result.position + std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - anchor));
      if(decoding && clock::now() > anchor + (ceiling - status.position) + std::chrono::milliseconds{50}) result.buffering = true;
    }
    return result;
  }

  void interrupt(std::shared_ptr<implementation> const &self) {
    asio::post(worker, [self]{ self->wake.cancel(); });
  }

  static asio::awaitable<void> run(std::shared_ptr<implementation> self) {
    {
      std::lock_guard lock{self->mutex};
      if(self->started) throw std::logic_error{"paced player may only run once"};
      self->started = true;
      if(self->stopped) co_return;
    }
    try {
      auto decoder{self->open()};
      if(!decoder) throw std::runtime_error{"decoder factory returned no decoder"};
      std::uint64_t revision{0};
      std::chrono::microseconds decoded_position;
      {
        std::lock_guard lock{self->mutex};
        decoded_position = self->status.position;
        revision = self->revision;
      }
      if(decoded_position.count() > 0) decoder->seek(decoded_position);
      for(;;) {
        bool seeking{false};
        {
          std::lock_guard lock{self->mutex};
          if(self->stopped) break;
          seeking = revision != self->revision;
          if(seeking) {
            revision = self->revision;
            decoded_position = self->status.position;
          }
          self->decoding = true;
        }
        if(seeking) decoder->seek(decoded_position);
        auto frame{decoder->next()};
        {
          std::lock_guard lock{self->mutex};
          self->decoding = false;
        }
        if(!frame) {
          std::lock_guard lock{self->mutex};
          if(revision != self->revision) continue;
          self->status.position = decoded_position;
          self->status.buffering = false;
          self->status.ended = true;
          self->status.paused = true;
          break;
        }
        auto const format{frame->format()};
        if(format.sample_rate == 0) throw std::runtime_error{"PCM sample rate is zero"};
        auto const duration{std::chrono::microseconds{static_cast<std::int64_t>(frame->sample_count() * 1'000'000ull / format.sample_rate)}};
        auto const begin{frame->position().value_or(decoded_position)};
        auto const end{begin + duration};
        decoded_position = end;
        if(self->status.frames == 0) emit_log(self->config.on_log, log_level::info, "playback", "PCM ready: "
          + std::to_string(format.sample_rate) + " Hz; channels=" + std::to_string(format.channels));
        bool consumed{false};
        bool prepared{false};
        while(!consumed) {
          clock::time_point deadline;
          {
            std::lock_guard lock{self->mutex};
            if(self->stopped) co_return;
            if(revision != self->revision) break;
            auto const now{clock::now()};
            if(!prepared) {
              if(self->status.buffering) {
                self->status.buffering = false;
                self->anchor = now;
              } else if(now > self->anchor + (begin - self->status.position) + std::chrono::milliseconds{5}) {
                self->status.position = std::max(self->status.position, begin);
                self->anchor = now;
              }
              prepared = true;
            }
            self->ceiling = end;
            auto const position{self->current().position};
            if(end <= position) consumed = true;
            else if(self->status.paused) deadline = clock::time_point::max();
            else deadline = self->anchor + (end - self->status.position);
          }
          if(consumed) break;
          self->wake.expires_at(deadline);
          boost::system::error_code ignored;
          co_await self->wake.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
        }
        {
          std::lock_guard lock{self->mutex};
          if(self->stopped) break;
          if(revision != self->revision) continue;
          auto const deadline{self->anchor + (end - self->status.position)};
          self->status.position = end;
          /// Preserve sample cadence; limit catch-up after a real decoder/network stall
          self->anchor = std::max(deadline, clock::now() - std::chrono::milliseconds{5});
          ++self->status.frames;
          self->status.samples += frame->sample_count();
        }
        if(self->config.on_frame) self->config.on_frame(*frame);
      }
    } catch(std::exception const &error) {
      std::lock_guard lock{self->mutex};
      if(!self->stopped) {
        self->status.position = self->current().position;
        self->status.error = diagnostic_error(error);
        self->status.paused = true;
        self->status.buffering = false;
      }
    }
  }
};

paced_player::paced_player(asio::any_io_executor worker, decoder_factory open, paced_player_config config)
  : state{std::make_shared<implementation>(std::move(worker), std::move(open), std::move(config))} {
  if(!state->open || state->status.position.count() < 0) throw std::invalid_argument{"invalid paced player configuration"};
}

paced_player::~paced_player() { close(); }

asio::awaitable<void> paced_player::run() {
  return asio::co_spawn(state->worker, implementation::run(state), asio::use_awaitable);
}

void paced_player::pause(bool paused) {
  {
    std::lock_guard lock{state->mutex};
    if(state->stopped || state->status.ended || !state->status.error.empty()) return;
    state->status.position = state->current().position;
    state->anchor = implementation::clock::now();
    state->status.paused = paused;
  }
  state->interrupt(state);
}

void paced_player::seek(std::chrono::microseconds position) {
  if(position.count() < 0) throw std::invalid_argument{"negative playback position"};
  {
    std::lock_guard lock{state->mutex};
    if(state->stopped || state->status.ended || !state->status.error.empty()) throw std::logic_error{"cannot seek a finished stream"};
    state->status.position = position;
    state->ceiling = position;
    state->status.buffering = true;
    state->status.ended = false;
    state->anchor = implementation::clock::now();
    ++state->revision;
  }
  state->interrupt(state);
}

playback_snapshot paced_player::snapshot() const {
  std::lock_guard lock{state->mutex};
  return state->current();
}

void paced_player::close() {
  bool cancel{false};
  {
    std::lock_guard lock{state->mutex};
    state->status.position = state->current().position;
    state->status.paused = true;
    cancel = !std::exchange(state->stopped, true);
  }
  if(cancel && state->config.cancel_source) {
    try {
      state->config.cancel_source();
    } catch(std::exception const &error) {
      emit_log(state->config.on_log, log_level::error, "playback", "Input cancellation failed: " + diagnostic_error(error));
    } catch(...) {
      emit_log(state->config.on_log, log_level::error, "playback", "Input cancellation failed");
    }
  }
  state->interrupt(state);
}

} // namespace librespot::audio
