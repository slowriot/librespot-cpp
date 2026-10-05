#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <future>
#include <thread>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include "librespot/audio/cdn_source.h"
#include "librespot/audio/decoder.h"
#include "librespot/audio/wav_writer.h"

namespace {

std::vector<std::byte> wav() {
  std::vector<std::byte> bytes(44 + 48'000 * 20 * 2);
  auto write{[&](std::size_t offset, std::uint32_t value, unsigned int count){
    for(unsigned int index{0}; index < count; ++index) bytes[offset + index] = static_cast<std::byte>((value >> (index * 8)) & 255u);
  }};
  std::memcpy(bytes.data(), "RIFF", 4);
  write(4, static_cast<std::uint32_t>(bytes.size() - 8), 4);
  std::memcpy(bytes.data() + 8, "WAVEfmt ", 8);
  write(16, 16, 4);
  write(20, 1, 2);
  write(22, 1, 2);
  write(24, 48'000, 4);
  write(28, 96'000, 4);
  write(32, 2, 2);
  write(34, 16, 2);
  std::memcpy(bytes.data() + 36, "data", 4);
  write(40, static_cast<std::uint32_t>(bytes.size() - 44), 4);
  for(std::size_t sample{0}; sample < (bytes.size() - 44) / 2; ++sample) write(44 + sample * 2, static_cast<std::uint32_t>(sample & 32767u), 2);
  return bytes;
}

class fixture_cdn final : public librespot::net::http_transport {
public:
  std::vector<std::byte> encrypted;
  std::atomic<std::size_t> transferred{0};
  bool hang{false};
  std::promise<void> started;

  boost::asio::awaitable<librespot::net::http_response> request(librespot::net::http_request request) override {
    if(hang) {
      started.set_value();
      boost::asio::steady_timer timer{co_await boost::asio::this_coro::executor};
      timer.expires_at(std::chrono::steady_clock::time_point::max());
      co_await timer.async_wait(boost::asio::use_awaitable);
    }
    auto const &range{request.headers.at("Range")};
    auto const dash{range.find('-')};
    auto const begin{std::stoull(range.substr(6, dash - 6))};
    auto const end{std::min<std::uint64_t>(std::stoull(range.substr(dash + 1)), encrypted.size() - 1)};
    if(begin > end || begin >= encrypted.size()) throw std::runtime_error{"invalid fixture range"};
    std::string body{reinterpret_cast<char const *>(encrypted.data() + begin), static_cast<std::size_t>(end - begin + 1)};
    transferred += body.size();
    co_return librespot::net::http_response{206,
      {{"Content-Range", "bytes " + std::to_string(begin) + '-' + std::to_string(end) + '/' + std::to_string(encrypted.size())}}, std::move(body)};
  }
};

struct network {
  boost::asio::io_context executor;
  boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work{executor.get_executor()};
  std::jthread thread{[this]{
    executor.run();
  }};

  ~network() {
    work.reset();
  }
};

} // anonymous namespace

TEST_CASE("Encrypted CDN streaming produces exact PCM before downloading the full track") {
  auto const original{wav()};
  fixture_cdn transport;
  transport.encrypted.resize(167, std::byte{42});
  transport.encrypted.insert(transport.encrypted.end(), original.begin(), original.end());
  librespot::audio::audio_key key{};
  librespot::audio::decrypt(key, 0, transport.encrypted);
  network network;
  auto source{std::make_shared<librespot::audio::cdn_source>(network.executor.get_executor(), transport,
    std::vector<std::string>{"https://fixture.example/audio"}, key,
    librespot::audio::cdn_source_config{.chunk_size{64 * 1024}, .container_offset{167}})};
  librespot::audio::decoder decoder{source};
  auto frame{decoder.next()};
  REQUIRE(frame);
  CHECK(transport.transferred.load() < transport.encrypted.size());
  std::size_t samples{0};
  do {
    auto const bytes{frame->plane(0)};
    REQUIRE(bytes.size() == frame->sample_count() * 2);
    CHECK(std::ranges::equal(bytes, std::span{original}.subspan(44 + samples * 2, bytes.size())));
    samples += frame->sample_count();
    frame = decoder.next();
  } while(frame);
  CHECK(samples == 48'000 * 20);
  decoder.seek(std::chrono::seconds{10});
  auto sought{decoder.next()};
  REQUIRE(sought);
  CHECK(sought->position() == std::chrono::seconds{10});
  CHECK(std::ranges::equal(sought->plane(0), std::span{original}.subspan(44 + 48'000 * 10 * 2, sought->plane(0).size())));
  source->cancel();
}

TEST_CASE("CDN source caches one chunk and reads across encrypted block and container offsets") {
  fixture_cdn transport;
  transport.encrypted = wav();
  auto const original{transport.encrypted};
  librespot::audio::audio_key key{};
  librespot::audio::decrypt(key, 0, transport.encrypted);
  network network;
  librespot::audio::cdn_source source{network.executor.get_executor(), transport,
    {"https://fixture.example/audio"}, key, {.chunk_size{4096}, .container_offset{167}}};
  CHECK(source.size() == original.size() - 167);
  std::array<std::byte, 100> bytes;
  CHECK(source.read_at(123, bytes) == bytes.size());
  CHECK(std::ranges::equal(bytes, std::span{original}.subspan(290, bytes.size())));
  auto const transferred{transport.transferred.load()};
  source.read_at(123, bytes);
  CHECK(transport.transferred.load() == transferred);
  source.read_at(10'000, bytes);
  CHECK(transport.transferred.load() == transferred + 4096);
  source.read_at(123, bytes);
  CHECK(transport.transferred.load() == transferred + 8192);
  source.cancel();
  CHECK_THROWS_AS(source.size(), boost::system::system_error);
}

TEST_CASE("Cancelling a CDN source interrupts an in-flight request and wakes its decoder worker") {
  fixture_cdn transport;
  transport.hang = true;
  auto started{transport.started.get_future()};
  network network;
  librespot::audio::cdn_source source{network.executor.get_executor(), transport, {"https://fixture.example/audio"}, {}};
  auto reading{std::async(std::launch::async, [&]{ return source.size(); })};
  REQUIRE(started.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
  source.cancel();
  REQUIRE(reading.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
  CHECK_THROWS_AS(reading.get(), boost::system::system_error);
}

TEST_CASE("WAV output interleaves native decoded samples and round trips their precision") {
  auto const path{std::filesystem::temp_directory_path() / ("librespot-pcm-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".wav")};
  struct cleanup {
    std::filesystem::path path;
    ~cleanup() {
      std::error_code error;
      std::filesystem::remove(path, error);
    }
  } remove{path};
  for(auto const name : {"tone.flac", "tone.ogg", "tone_stereo.ogg"}) {
    librespot::audio::decoder input{std::filesystem::path{LIBRESPOT_TEST_FIXTURES} / name};
    auto frame{input.next()};
    REQUIRE(frame);
    auto const format{frame->format()};
    std::vector<std::byte> expected;
    librespot::audio::wav_writer output{path, format};
    do {
      output.append(*frame);
      if(!format.planar) {
        auto const bytes{frame->plane(0)};
        expected.insert(expected.end(), bytes.begin(), bytes.end());
      } else {
        auto const width{frame->plane(0).size() / frame->sample_count()};
        for(std::size_t sample{0}; sample < frame->sample_count(); ++sample) {
          for(unsigned int channel{0}; channel < format.channels; ++channel) {
            auto const bytes{frame->plane(channel).subspan(sample * width, width)};
            expected.insert(expected.end(), bytes.begin(), bytes.end());
          }
        }
      }
      frame = input.next();
    } while(frame);
    output.finish();
    librespot::audio::decoder restored{path};
    std::vector<std::byte> actual;
    while(auto decoded{restored.next()}) {
      CHECK(decoded->format().samples == format.samples);
      CHECK(decoded->format().sample_rate == format.sample_rate);
      CHECK(decoded->format().channels == format.channels);
      auto const bytes{decoded->plane(0)};
      actual.insert(actual.end(), bytes.begin(), bytes.end());
    }
    CHECK(actual == expected);
  }
}
