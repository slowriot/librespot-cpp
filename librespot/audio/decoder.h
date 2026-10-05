#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace librespot::audio {

enum class sample_format {
  unsigned_8,
  signed_16,
  signed_32,
  signed_64,
  float_32,
  float_64,
};

struct pcm_format {
  sample_format samples{sample_format::float_32};
  unsigned int sample_rate{0};
  unsigned int channels{0};
  unsigned int significant_bits{0};
  bool planar{false};
  bool operator==(pcm_format const &) const = default;
};

/// Owns a decoded frame; plane views remain valid until this object is destroyed
class pcm_frame {
private:
  struct implementation;
  std::unique_ptr<implementation> state;
  explicit pcm_frame(std::unique_ptr<implementation> state);

public:
  ~pcm_frame();
  pcm_frame(pcm_frame &&) noexcept;
  pcm_frame &operator=(pcm_frame &&) noexcept;
  pcm_frame(pcm_frame const &) = delete;
  pcm_frame &operator=(pcm_frame const &) = delete;
  [[nodiscard]] pcm_format format() const;
  [[nodiscard]] std::size_t sample_count() const;
  [[nodiscard]] std::optional<std::chrono::microseconds> position() const;
  [[nodiscard]] std::size_t plane_count() const;
  [[nodiscard]] std::span<std::byte const> plane(std::size_t index) const;
  friend class decoder;
};

/// Synchronous decoding belongs on a worker thread, outside an audio device callback
class decoder {
private:
  struct implementation;
  std::unique_ptr<implementation> state;

public:
  explicit decoder(std::filesystem::path const &path);
  explicit decoder(std::shared_ptr<std::vector<std::byte> const> bytes);
  ~decoder();
  decoder(decoder &&) noexcept;
  decoder &operator=(decoder &&) noexcept;
  decoder(decoder const &) = delete;
  decoder &operator=(decoder const &) = delete;
  [[nodiscard]] std::optional<pcm_frame> next();
  void seek(std::chrono::microseconds position);
};

} // namespace librespot::audio
