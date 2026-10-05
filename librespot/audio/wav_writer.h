#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>
#include "decoder.h"

namespace librespot::audio {

/// Writes native-precision PCM, interleaving planar samples without changing their values
class wav_writer {
private:
  std::ofstream output;
  pcm_format format;
  unsigned int bytes_per_sample;
  unsigned int header_size;
  std::uint64_t data_size{0};
  std::vector<char> interleaved;
  bool finished{false};
  void integer(std::uint32_t value, unsigned int count);

public:
  wav_writer(std::filesystem::path const &path, pcm_format format);
  wav_writer(wav_writer const &) = delete;
  wav_writer &operator=(wav_writer const &) = delete;
  void append(pcm_frame const &frame);
  /// Finalise the RIFF sizes and report any output error
  void finish();
};

} // namespace librespot::audio
