#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace librespot::connect {

struct provided_track {
  std::string uri;
  std::string uid;
};

struct player_state {
  bool active{false};
  bool paused{true};
  bool buffering{false};
  std::chrono::milliseconds position{0};
  std::chrono::milliseconds duration{0};
  std::string context_uri;
  provided_track track;
  std::vector<provided_track> next_tracks;
  std::vector<provided_track> previous_tracks;
  std::uint16_t volume{32768};
  bool shuffle{false};
  bool repeat_context{false};
  bool repeat_track{false};
};

enum class command_type {
  transfer, play, pause, resume, seek, next, previous, volume,
  shuffle, repeat_context, repeat_track, options, add_to_queue, set_queue, update_context, unknown,
};

struct command {
  command_type type{command_type::unknown};
  std::string endpoint;
  std::uint32_t message_id{0};
  std::string sender;
  std::optional<std::chrono::milliseconds> position;
  std::optional<std::uint16_t> volume;
  std::optional<bool> paused;
  std::optional<bool> shuffle;
  std::optional<bool> repeat_context;
  std::optional<bool> repeat_track;
  std::optional<player_state> transferred_state;
  std::string context_uri;
  provided_track track;
  std::vector<provided_track> next_tracks;
  std::vector<provided_track> previous_tracks;
  /// Original command JSON for application-specific context resolution and options
  std::string payload;
};

[[nodiscard]] command decode_command(std::string const &json);
/// A control-only state model for examples; it does not fetch context pages or decode audio
[[nodiscard]] bool apply_control(command const &request, player_state &state);

} // namespace librespot::connect
