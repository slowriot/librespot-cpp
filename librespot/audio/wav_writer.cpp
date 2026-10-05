#include "wav_writer.h"
#include <array>
#include <bit>
#include <limits>
#include <stdexcept>
#include <vector>

namespace librespot::audio {

void wav_writer::integer(std::uint32_t value, unsigned int count) {
  std::array<char, 4> bytes;
  for(unsigned int index{0}; index < count; ++index) bytes[index] = static_cast<char>((value >> (index * 8)) & 255u);
  output.write(bytes.data(), count);
}

wav_writer::wav_writer(std::filesystem::path const &path, pcm_format format) : format{format} {
  switch(format.samples) {
  case sample_format::unsigned_8: bytes_per_sample = 1; break;
  case sample_format::signed_16: bytes_per_sample = 2; break;
  case sample_format::signed_32: case sample_format::float_32: bytes_per_sample = 4; break;
  case sample_format::signed_64: case sample_format::float_64: bytes_per_sample = 8; break;
  default: throw std::invalid_argument{"invalid WAV sample format"};
  }
  auto const float_samples{format.samples == sample_format::float_32 || format.samples == sample_format::float_64};
  header_size = float_samples ? 56u : 44u;
  if(format.channels == 0 || format.channels > 2 || format.sample_rate == 0
    || static_cast<std::uint64_t>(format.sample_rate) * format.channels * bytes_per_sample > std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument{"WAV demonstration supports valid mono or stereo PCM"};
  }
  output.exceptions(std::ios::failbit | std::ios::badbit);
  output.open(path, std::ios::binary | std::ios::trunc);
  output.write("RIFF", 4);
  integer(0, 4);
  output.write("WAVEfmt ", 8);
  integer(16, 4);
  integer(float_samples ? 3u : 1u, 2);
  integer(format.channels, 2);
  integer(format.sample_rate, 4);
  integer(format.sample_rate * format.channels * bytes_per_sample, 4);
  integer(format.channels * bytes_per_sample, 2);
  integer(bytes_per_sample * 8, 2);
  if(float_samples) {
    output.write("fact", 4);
    integer(4, 4);
    integer(0, 4);
  }
  output.write("data", 4);
  integer(0, 4);
}

void wav_writer::append(pcm_frame const &frame) {
  if(finished) throw std::logic_error{"WAV output already finished"};
  auto const incoming{frame.format()};
  if(incoming.samples != format.samples || incoming.sample_rate != format.sample_rate || incoming.channels != format.channels) {
    throw std::invalid_argument{"PCM format changed during WAV output"};
  }
  auto const count{frame.sample_count() * format.channels * bytes_per_sample};
  auto const new_size{data_size + count};
  if(new_size + (new_size & 1u) > std::numeric_limits<std::uint32_t>::max() - (header_size - 8u)) throw std::runtime_error{"WAV output exceeds RIFF size limit"};
  if(!incoming.planar && std::endian::native == std::endian::little) {
    auto const bytes{frame.plane(0)};
    output.write(reinterpret_cast<char const *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  } else {
    interleaved.resize(count);
    std::array<std::span<std::byte const>, 2> planes{frame.plane(0), {}};
    if(incoming.planar && format.channels == 2) planes[1] = frame.plane(1);
    for(std::size_t sample{0}; sample < frame.sample_count(); ++sample) {
      for(unsigned int channel{0}; channel < format.channels; ++channel) {
        auto const plane{planes[incoming.planar ? channel : 0]};
        auto const start{(incoming.planar ? sample : sample * format.channels + channel) * bytes_per_sample};
        for(unsigned int byte{0}; byte < bytes_per_sample; ++byte) {
          auto const source{std::endian::native == std::endian::little ? byte : bytes_per_sample - 1 - byte};
          interleaved[(sample * format.channels + channel) * bytes_per_sample + byte] = static_cast<char>(plane[start + source]);
        }
      }
    }
    output.write(interleaved.data(), static_cast<std::streamsize>(interleaved.size()));
  }
  data_size += count;
}

void wav_writer::finish() {
  if(finished) return;
  if((data_size & 1u) != 0) {
    output.seekp(0, std::ios::end);
    integer(0, 1);
  }
  output.seekp(4);
  integer(static_cast<std::uint32_t>(data_size + (data_size & 1u) + header_size - 8), 4);
  if(header_size == 56) {
    output.seekp(44);
    integer(static_cast<std::uint32_t>(data_size / (format.channels * bytes_per_sample)), 4);
  }
  output.seekp(header_size - 4);
  integer(static_cast<std::uint32_t>(data_size), 4);
  output.flush();
  output.close();
  finished = true;
}

} // namespace librespot::audio
