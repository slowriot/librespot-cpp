#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include "decoder.h"
#include "librespot/diagnostics.h"

namespace librespot::audio {

struct playback_snapshot {
  std::chrono::microseconds position{0};
  bool paused{true};
  bool buffering{true};
  bool ended{false};
  std::uint64_t frames{0};
  std::uint64_t samples{0};
  std::string error;
};

struct paced_player_config {
  std::chrono::microseconds position{0};
  bool paused{true};
  /// Called on the decoder worker at playback speed; an empty sink discards PCM
  std::function<void(pcm_frame const &)> on_frame;
  /// Must unblock a decoder read; called when close is requested
  std::function<void()> cancel_source;
  log_handler on_log;
};

/// Owns one stream. Commands and snapshots are thread-safe; await run before releasing input dependencies
class paced_player {
private:
  struct implementation;
  std::shared_ptr<implementation> state;

public:
  using decoder_factory = std::function<std::unique_ptr<decoder>()>;
  paced_player(boost::asio::any_io_executor worker, decoder_factory open, paced_player_config config = {});
  ~paced_player();
  paced_player(paced_player const &) = delete;
  paced_player &operator=(paced_player const &) = delete;
  /// Call once; opens and decodes on the supplied worker, never on the caller's executor
  boost::asio::awaitable<void> run();
  void pause(bool paused);
  /// Seek a running stream; create a new player after EOF or a decoder failure
  void seek(std::chrono::microseconds position);
  [[nodiscard]] playback_snapshot snapshot() const;
  void close();
};

} // namespace librespot::audio
