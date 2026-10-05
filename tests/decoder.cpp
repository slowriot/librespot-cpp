#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <cmath>
#include <memory>
#include "librespot/audio/decoder.h"

namespace {

std::shared_ptr<std::vector<std::byte> const> wav() {
  /// Generate a deterministic 48 kHz mono PCM fixture without external tools
  auto bytes{std::make_shared<std::vector<std::byte>>(44 + 4800 * 2)};
  auto write{[&bytes](std::size_t offset, std::uint32_t value, unsigned int count){
    for(unsigned int index{0}; index < count; ++index) (*bytes)[offset + index] = static_cast<std::byte>((value >> (index * 8)) & 255u);
  }};
  std::memcpy(bytes->data(), "RIFF", 4);
  write(4, static_cast<std::uint32_t>(bytes->size() - 8), 4);
  std::memcpy(bytes->data() + 8, "WAVEfmt ", 8);
  write(16, 16, 4);
  write(20, 1, 2);
  write(22, 1, 2);
  write(24, 48000, 4);
  write(28, 96000, 4);
  write(32, 2, 2);
  write(34, 16, 2);
  std::memcpy(bytes->data() + 36, "data", 4);
  write(40, 9600, 4);
  for(std::size_t index{0}; index < 4800; ++index) write(44 + index * 2, static_cast<std::uint32_t>(index), 2);
  return bytes;
}

} // anonymous namespace

TEST_CASE("Decoder preserves PCM samples and frame ownership across seek and EOF") {
  auto const bytes{wav()};
  librespot::audio::decoder decoder{bytes};
  auto first{decoder.next()};
  REQUIRE(first);
  CHECK(first->format().sample_rate == 48000);
  CHECK(first->format().channels == 1);
  CHECK(first->format().samples == librespot::audio::sample_format::signed_16);
  auto const first_samples{first->plane(0)};
  CHECK(first_samples[2] == std::byte{1});
  CHECK_THROWS_AS(first->plane(1), std::out_of_range);
  std::size_t samples{first->sample_count()};
  while(auto frame{decoder.next()}) samples += frame->sample_count();
  CHECK(samples == 4800);
  CHECK_FALSE(decoder.next());
  decoder.seek(std::chrono::milliseconds{50});
  auto sought{decoder.next()};
  REQUIRE(sought);
  REQUIRE(sought->position());
  CHECK(*sought->position() == std::chrono::milliseconds{50});
  CHECK(sought->plane(0)[0] == std::byte{0x60});
  CHECK(sought->plane(0)[1] == std::byte{0x09});
  CHECK(first_samples[2] == std::byte{1});
  CHECK_THROWS_AS(decoder.seek(std::chrono::microseconds{-1}), std::invalid_argument);
}

TEST_CASE("Decoder rejects empty and malformed encoded input") {
  CHECK_THROWS_AS(librespot::audio::decoder{std::make_shared<std::vector<std::byte> const>()}, std::invalid_argument);
  CHECK_THROWS(librespot::audio::decoder{std::make_shared<std::vector<std::byte> const>(128, std::byte{0})});
}

TEST_CASE("Decoder handles FLAC, Vorbis and MP3 without altering the native sample rate") {
  for(auto const extension : {"flac", "ogg", "mp3"}) {
    CAPTURE(extension);
    librespot::audio::decoder decoder{std::filesystem::path{LIBRESPOT_TEST_FIXTURES} / (std::string{"tone."} + extension)};
    std::size_t total{0};
    double energy{0};
    while(auto frame{decoder.next()}) {
      CHECK(frame->format().sample_rate == 44100);
      CHECK(frame->format().channels == 1);
      auto const bytes{frame->plane(0)};
      for(std::size_t index{0}; index < frame->sample_count(); ++index) {
        double sample{0};
        if(frame->format().samples == librespot::audio::sample_format::signed_16) {
          std::int16_t value{0};
          std::memcpy(&value, bytes.data() + index * sizeof(value), sizeof(value));
          sample = static_cast<double>(value) / 32768.0;
        } else if(frame->format().samples == librespot::audio::sample_format::float_32) {
          float value{0};
          std::memcpy(&value, bytes.data() + index * sizeof(value), sizeof(value));
          sample = value;
        } else {
          FAIL("unexpected fixture sample representation");
        }
        REQUIRE(std::isfinite(sample));
        energy += sample * sample;
        if(std::string_view{extension} == "flac") {
          auto const expected{std::lround(16000 * std::sin(static_cast<double>(total + index) * 2 * std::acos(-1.0) * 440 / 44100))};
          CHECK(sample * 32768.0 == static_cast<double>(expected));
        }
      }
      total += frame->sample_count();
    }
    CHECK(total >= 4000);
    CHECK(total <= 5000);
    auto const rms{std::sqrt(energy / static_cast<double>(total))};
    CHECK(rms > 0.30);
    CHECK(rms < 0.38);
    decoder.seek(std::chrono::milliseconds{50});
    auto frame{decoder.next()};
    REQUIRE(frame);
    REQUIRE(frame->position());
    CHECK(*frame->position() >= std::chrono::milliseconds{50});
  }
}
