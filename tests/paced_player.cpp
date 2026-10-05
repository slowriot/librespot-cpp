#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include "librespot/audio/paced_player.h"

namespace {
namespace asio = boost::asio;
using namespace std::chrono_literals;

std::shared_ptr<std::vector<std::byte> const> wav(unsigned int samples) {
  auto bytes{std::make_shared<std::vector<std::byte>>(44 + samples * 2)};
  auto write{[&](unsigned int offset, std::uint32_t value, unsigned int count){
    for(unsigned int index{0}; index < count; ++index) (*bytes)[offset + index] = static_cast<std::byte>((value >> (8 * index)) & 255u);
  }};
  std::memcpy(bytes->data(), "RIFF", 4);
  write(4, 36 + samples * 2, 4);
  std::memcpy(bytes->data() + 8, "WAVEfmt ", 8);
  write(16, 16, 4);
  write(20, 1, 2);
  write(22, 1, 2);
  write(24, 16'000, 4);
  write(28, 32'000, 4);
  write(32, 2, 2);
  write(34, 16, 2);
  std::memcpy(bytes->data() + 36, "data", 4);
  write(40, samples * 2, 4);
  return bytes;
}

asio::awaitable<void> delay(std::chrono::milliseconds interval) {
  asio::steady_timer timer{co_await asio::this_coro::executor};
  timer.expires_after(interval);
  co_await timer.async_wait(asio::use_awaitable);
}

} // anonymous namespace

TEST_CASE("Paced PCM pauses the sample clock, seeks while paused, and resumes at normal speed") {
  asio::io_context executor;
  asio::thread_pool worker{1};
  auto bytes{wav(32'000)};
  std::atomic<bool> cancelled{false};
  librespot::audio::paced_player player{worker.get_executor(), [bytes]{ return std::make_unique<librespot::audio::decoder>(bytes); },
    {.position{200ms}, .paused{true}, .cancel_source{[&]{ cancelled = true; }}}};
  auto running{asio::co_spawn(executor, player.run(), asio::use_future)};
  auto controls{asio::co_spawn(executor, [&]()->asio::awaitable<void> {
    struct cleanup {
      librespot::audio::paced_player &player;
      ~cleanup() { player.close(); }
    } guard{player};
    co_await delay(80ms);
    CHECK(player.snapshot().position == 200ms);
    CHECK(player.snapshot().frames == 0);
    CHECK_FALSE(player.snapshot().buffering);
    player.pause(false);
    co_await delay(120ms);
    auto const playing{player.snapshot()};
    CHECK(playing.position >= 290ms);
    CHECK(playing.position < 350ms);
    CHECK(playing.samples > 0);
    player.pause(true);
    auto const paused{player.snapshot().position};
    co_await delay(80ms);
    CHECK(player.snapshot().position == paused);
    player.seek(600ms);
    co_await delay(50ms);
    CHECK(player.snapshot().position == 600ms);
    CHECK_FALSE(player.snapshot().buffering);
    player.pause(false);
    co_await delay(80ms);
    CHECK(player.snapshot().position >= 650ms);
    CHECK(player.snapshot().position < 730ms);
  }, asio::use_future)};
  executor.run();
  controls.get();
  running.get();
  worker.join();
  CHECK(cancelled.load());
  CHECK(player.snapshot().error.empty());
}

TEST_CASE("Paced PCM consumes a complete stream in real time and reports EOF") {
  asio::io_context executor;
  asio::thread_pool worker{1};
  auto bytes{wav(3840)};
  std::atomic<std::uint64_t> received{0};
  librespot::audio::paced_player player{worker.get_executor(), [bytes]{ return std::make_unique<librespot::audio::decoder>(bytes); },
    {.paused{false}, .on_frame{[&](librespot::audio::pcm_frame const &frame){ received += frame.sample_count(); }}}};
  auto const start{std::chrono::steady_clock::now()};
  auto running{asio::co_spawn(executor, player.run(), asio::use_future)};
  executor.run();
  running.get();
  worker.join();
  CHECK(std::chrono::steady_clock::now() - start >= 220ms);
  CHECK(received.load() == 3840);
  CHECK(player.snapshot().samples == 3840);
  CHECK(player.snapshot().position == 240ms);
  CHECK(player.snapshot().ended);
  CHECK(player.snapshot().paused);
  CHECK(player.snapshot().error.empty());
}

TEST_CASE("Paced PCM exposes decoder failures without leaving a playback clock running") {
  asio::io_context executor;
  asio::thread_pool worker{1};
  librespot::audio::paced_player player{worker.get_executor(), []()->std::unique_ptr<librespot::audio::decoder> {
    throw std::runtime_error{"fixture decoder failure"};
  }, {.paused{false}}};
  auto running{asio::co_spawn(executor, player.run(), asio::use_future)};
  executor.run();
  running.get();
  worker.join();
  CHECK(player.snapshot().error == "fixture decoder failure");
  CHECK(player.snapshot().paused);
  CHECK_FALSE(player.snapshot().buffering);
  CHECK_THROWS(player.seek(-1ms));
}
