#include "control.h"
#include <algorithm>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>
#include <nlohmann/json.hpp>
#include "librespot/core/spotify_id.h"
#include "librespot/encoding/protocol.h"
#include "transfer_state.pb.h"

namespace librespot::connect {
namespace {

provided_track track(nlohmann::json const &object) {
  return {.uri{object.value("uri", "")}, .uid{object.value("uid", "")}};
}

provided_track track(spotify::player::proto::ContextTrack const &source) {
  auto uri{source.uri()};
  if(uri.empty() && !source.gid().empty()) {
    auto const id{spotify_id::from_bytes(std::as_bytes(std::span{source.gid().data(), source.gid().size()}))};
    if(!id) throw std::runtime_error{"invalid Connect transfer track GID"};
    uri = "spotify:track:" + id->to_base62();
  }
  if(uri.empty()) throw std::runtime_error{"Connect transfer track has neither URI nor GID"};
  return {.uri{std::move(uri)}, .uid{source.uid()}};
}

std::vector<provided_track> tracks(nlohmann::json const &object, char const *key) {
  std::vector<provided_track> result;
  if(!object.contains(key)) return result;
  auto const &items{object.at(key)};
  if(!items.is_array() || items.size() > 1024) throw std::runtime_error{"invalid Connect queue"};
  for(auto const &item : items) result.push_back(track(item));
  return result;
}

void boolean(nlohmann::json const &object, char const *key, std::optional<bool> &target) {
  if(object.contains(key) && !object.at(key).is_null()) target = object.at(key).get<bool>();
}

} // anonymous namespace

command decode_command(std::string const &json) {
  /// Preserve the raw command while extracting supported typed controls and transfer state
  if(json.size() > 16 * 1024 * 1024) throw std::runtime_error{"Connect command too large"};
  auto object = nlohmann::json::parse(json);
  auto const &body{object.at("command")};
  command result;
  auto const message_id{object.at("message_id").get<std::int64_t>()};
  if(message_id < 0 || message_id > std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error{"invalid Connect command ID"};
  result.message_id = static_cast<std::uint32_t>(message_id);
  result.sender = object.at("sent_by_device_id").get<std::string>();
  result.endpoint = body.at("endpoint").get<std::string>();
  result.payload = body.dump();
  for(auto const &[name, type] : {
    std::pair{"transfer", command_type::transfer}, {"play", command_type::play}, {"pause", command_type::pause},
    {"resume", command_type::resume}, {"seek_to", command_type::seek}, {"skip_next", command_type::next}, {"skip_prev", command_type::previous},
    {"set_shuffling_context", command_type::shuffle}, {"set_repeating_context", command_type::repeat_context},
    {"set_repeating_track", command_type::repeat_track}, {"set_options", command_type::options},
    {"add_to_queue", command_type::add_to_queue}, {"set_queue", command_type::set_queue}, {"update_context", command_type::update_context},
  }) {
    if(result.endpoint == name) result.type = type;
  }
  if(result.type == command_type::seek) {
    auto const position{body.at("value").get<std::int64_t>()};
    if(position < 0 || position > std::numeric_limits<std::int32_t>::max()) throw std::runtime_error{"invalid Connect seek position"};
    result.position = std::chrono::milliseconds{position};
  }
  if(result.type == command_type::shuffle) result.shuffle = body.at("value").get<bool>();
  if(result.type == command_type::repeat_context) result.repeat_context = body.at("value").get<bool>();
  if(result.type == command_type::repeat_track) result.repeat_track = body.at("value").get<bool>();
  if(result.type == command_type::options) {
    boolean(body, "shuffling_context", result.shuffle);
    boolean(body, "repeating_context", result.repeat_context);
    boolean(body, "repeating_track", result.repeat_track);
  }
  if(body.contains("context")) result.context_uri = body.at("context").value("uri", "");
  if(body.contains("track")) result.track = track(body.at("track"));
  result.next_tracks = tracks(body, "next_tracks");
  result.previous_tracks = tracks(body, "prev_tracks");
  if(result.type == command_type::play && body.contains("options")) {
    auto const &options{body.at("options")};
    boolean(options, "initially_paused", result.paused);
    if(options.contains("seek_to") && !options.at("seek_to").is_null()) result.position = std::chrono::milliseconds{options.at("seek_to").get<std::uint32_t>()};
    if(options.contains("skip_to") && options.at("skip_to").is_object()) result.track.uri = options.at("skip_to").value("track_uri", "");
    if(result.track.uri.empty() && result.context_uri.starts_with("spotify:track:")) result.track.uri = result.context_uri;
  }
  if(result.type == command_type::transfer && body.contains("data") && !body.at("data").is_null()) {
    auto const data{encoding::decode_base64(body.at("data").get<std::string>())};
    spotify::player::proto::transfer::TransferState transfer;
    if(!transfer.ParseFromString(data)) throw std::runtime_error{"malformed Connect transfer state"};
    player_state state;
    state.active = true;
    state.paused = transfer.playback().is_paused();
    state.position = std::chrono::milliseconds{std::max(0, transfer.playback().position_as_of_timestamp())};
    auto const &queue{transfer.queue()};
    if(queue.tracks_size() > 1024) throw std::runtime_error{"Connect transfer queue too large"};
    auto const playing_queue{queue.is_playing_queue()};
    if(playing_queue && queue.tracks().empty()) throw std::runtime_error{"Connect transfer playing queue is empty"};
    state.track = track(playing_queue ? queue.tracks(0) : transfer.playback().current_track());
    state.context_uri = transfer.current_session().context().uri();
    state.shuffle = transfer.options().shuffling_context();
    state.repeat_context = transfer.options().repeating_context();
    state.repeat_track = transfer.options().repeating_track();
    for(int index{playing_queue ? 1 : 0}; index < queue.tracks_size(); ++index) state.next_tracks.push_back(track(queue.tracks(index)));
    result.transferred_state = std::move(state);
  }
  return result;
}

bool apply_control(command const &request, player_state &state) {
  /// Model interoperability without advancing a playback clock or creating an audio device
  switch(request.type) {
  case command_type::transfer:
    if(!request.transferred_state) return false;
    {
      auto const volume{state.volume};
      state = *request.transferred_state;
      state.volume = volume;
    }
    break;
  case command_type::play:
    state.active = true;
    state.paused = request.paused.value_or(false);
    state.context_uri = request.context_uri;
    state.track = request.track;
    state.position = request.position.value_or(std::chrono::milliseconds{0});
    break;
  case command_type::pause: state.paused = true; break;
  case command_type::resume: state.active = true; state.paused = false; break;
  case command_type::seek:
    if(!request.position) return false;
    state.position = *request.position;
    break;
  case command_type::volume:
    if(!request.volume) return false;
    state.volume = *request.volume;
    break;
  case command_type::shuffle: case command_type::repeat_context: case command_type::repeat_track: case command_type::options:
    if(request.shuffle) state.shuffle = *request.shuffle;
    if(request.repeat_context) state.repeat_context = *request.repeat_context;
    if(request.repeat_track) state.repeat_track = *request.repeat_track;
    break;
  case command_type::add_to_queue:
    if(request.track.uri.empty() || state.next_tracks.size() >= 1024) return false;
    state.next_tracks.push_back(request.track);
    break;
  case command_type::set_queue:
    state.next_tracks = request.next_tracks;
    state.previous_tracks = request.previous_tracks;
    break;
  case command_type::next:
    if(state.next_tracks.empty() && request.track.uri.empty()) return false;
    if(state.previous_tracks.size() >= 1024) state.previous_tracks.erase(state.previous_tracks.begin());
    state.previous_tracks.push_back(state.track);
    if(!request.track.uri.empty()) state.track = request.track;
    else {
      state.track = state.next_tracks.front();
      state.next_tracks.erase(state.next_tracks.begin());
    }
    state.position = std::chrono::milliseconds{0};
    break;
  case command_type::previous:
    if(state.previous_tracks.empty() || state.next_tracks.size() >= 1024) return false;
    state.next_tracks.insert(state.next_tracks.begin(), state.track);
    state.track = state.previous_tracks.back();
    state.previous_tracks.pop_back();
    state.position = std::chrono::milliseconds{0};
    break;
  case command_type::update_context: state.context_uri = request.context_uri; break;
  case command_type::unknown: return false;
  }
  return true;
}

} // namespace librespot::connect
