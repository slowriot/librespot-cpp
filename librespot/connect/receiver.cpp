#include "receiver.h"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <google/protobuf/util/json_util.h>
#include <nlohmann/json.hpp>
#include "connect.pb.h"
#include "librespot/encoding/protocol.h"
#include "librespot/version.h"

namespace librespot::connect {
namespace asio = boost::asio;
namespace proto = spotify::connectstate;
namespace {

std::uint64_t timestamp() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string payload(nlohmann::json const &message, bool request) {
  std::string result;
  if(request) result = encoding::decode_base64(message.at("payload").at("compressed").get<std::string>());
  else {
    if(!message.contains("payloads") || message.at("payloads").empty()) return result;
    auto const &values{message.at("payloads")};
    if(!values.is_array() || values.size() != 1) throw std::runtime_error{"invalid Dealer payload count"};
    auto const &value{values.front()};
    if(value.is_string()) result = encoding::decode_base64(value.get<std::string>());
    else if(value.is_array()) {
      for(auto const &byte : value) {
        auto const number{byte.get<int>()};
        if(number < 0 || number > 255) throw std::runtime_error{"invalid Dealer byte payload"};
        result += static_cast<char>(number);
      }
    } else return value.dump();
  }
  if(message.contains("headers") && message.at("headers").value("Transfer-Encoding", "") == "gzip") result = encoding::inflate_gzip(result);
  return result;
}

void validate(player_state const &state) {
  if(state.position.count() < 0 || state.duration.count() < 0 || state.next_tracks.size() > 1024 || state.previous_tracks.size() > 1024
    || state.track.uri.size() > 4096 || state.context_uri.size() > 4096) throw std::invalid_argument{"invalid application player state"};
}

std::string diagnostic_message(google::protobuf::Message const &message) {
  std::string json;
  google::protobuf::util::JsonPrintOptions options;
  options.preserve_proto_field_names = true;
  if(!google::protobuf::util::MessageToJsonString(message, &json, options).ok()) return "<protobuf JSON unavailable>";
  return diagnostic_json(json);
}

struct inbox {
  asio::experimental::channel<asio::any_io_executor, void(boost::system::error_code, std::string)> messages;
  asio::steady_timer tick;
  asio::steady_timer finished;
  std::exception_ptr failure;
  bool stopped{false};
  unsigned int tasks{0};
  explicit inbox(asio::any_io_executor executor) : messages{executor, 16}, tick{executor}, finished{executor} {
    finished.expires_at(std::chrono::steady_clock::time_point::max());
  }
};

asio::awaitable<void> receive_messages(std::shared_ptr<net::dealer_transport> dealer, std::shared_ptr<inbox> input) {
  while(!input->stopped) {
    auto message{co_await dealer->receive()};
    co_await input->messages.async_send(boost::system::error_code{}, std::move(message), asio::use_awaitable);
  }
}

asio::awaitable<void> playback_ticks(std::shared_ptr<inbox> input, std::chrono::milliseconds interval) {
  while(!input->stopped) {
    input->tick.expires_after(interval);
    co_await input->tick.async_wait(asio::use_awaitable);
    input->messages.try_send(boost::system::error_code{}, std::string{});
  }
}

} // anonymous namespace

struct receiver::implementation {
  asio::strand<asio::any_io_executor> executor;
  net::http_transport &http;
  oauth::service_auth &auth;
  receiver_config config;
  command_handler handler;
  std::shared_ptr<net::dealer_transport> dealer;
  service::client service;
  player_state player;
  proto::PutStateRequest registration;
  std::string connection_id;
  std::uint64_t active_since{0};
  asio::steady_timer retry;
  asio::steady_timer finished;
  asio::cancellation_signal cancellation;
  bool stopped{false};
  bool started{false};
  bool done{false};
  bool registered{false};
  std::shared_ptr<inbox> input;

  implementation(asio::any_io_executor executor, net::http_transport &http, oauth::service_auth &auth,
    receiver_config config, command_handler handler, std::shared_ptr<net::dealer_transport> dealer)
    : executor{asio::make_strand(std::move(executor))}, http{http}, auth{auth}, config{std::move(config)}, handler{std::move(handler)},
      dealer{std::move(dealer)}, service{http, auth, this->config.on_log}, retry{this->executor}, finished{this->executor} {
    if(!this->dealer) {
      auto transport_config{this->config.dealer};
      if(!transport_config.on_log) transport_config.on_log = this->config.on_log;
      this->dealer = std::make_shared<net::dealer>(this->executor, std::move(transport_config));
    }
    if(this->config.device.id.empty() || this->config.device.name.empty() || !this->handler) throw std::invalid_argument{"Connect identity and command handler are required"};
    finished.expires_at(std::chrono::steady_clock::time_point::max());
    player.volume = this->config.device.initial_volume;
    auto &device{*registration.mutable_device()->mutable_device_info()};
    if(this->config.state_interval < std::chrono::milliseconds{100}) throw std::invalid_argument{"Connect state interval must be at least 100 ms"};
    auto const &identity{this->config.device};
    device.set_can_play(true);
    device.set_name(identity.name);
    device.set_device_id(identity.id);
    device.set_device_type(static_cast<proto::devices::DeviceType>(identity.type));
    device.set_device_software_version(std::string{version});
    device.set_spirc_version("3.2.6");
    device.set_client_id(identity.client_id);
    device.set_brand(identity.brand);
    device.set_model(identity.model);
    auto &capabilities{*device.mutable_capabilities()};
    capabilities.set_can_be_player(true);
    capabilities.set_gaia_eq_connect_id(true);
    capabilities.set_is_observable(true);
    capabilities.set_is_controllable(true);
    capabilities.set_volume_steps(64);
    capabilities.set_command_acks(true);
    capabilities.set_supports_transfer_command(true);
    capabilities.set_supports_command_request(true);
    capabilities.set_needs_full_player_state(true);
    capabilities.set_supports_gzip_pushes(true);
    capabilities.set_supports_set_options_command(true);
    capabilities.add_supported_types("audio/track");
    registration.set_member_type(proto::CONNECT_STATE);
    auto session_id{boost::uuids::to_string(boost::uuids::random_generator{}())};
    std::erase(session_id, '-');
    registration.mutable_device()->mutable_player_state()->set_session_id(std::move(session_id));
  }

  void status(std::string message) {
    emit_log(config.on_log, log_level::info, "connect", message);
    if(config.on_status) {
      try {
        config.on_status(std::move(message));
      } catch(...) {
        // diagnostic callbacks must not interrupt transport cleanup
      }
    }
  }

  void stop() {
    if(!stopped) emit_log(config.on_log, log_level::debug, "connect", "Stopping receiver and cancelling Dealer operations");
    stopped = true;
    cancellation.emit(asio::cancellation_type::all);
    dealer->close();
    if(input) {
      input->stopped = true;
      input->tick.cancel();
      input->messages.cancel();
    }
    retry.cancel();
  }

  asio::awaitable<std::vector<net::endpoint>> resolve() {
    emit_log(config.on_log, log_level::debug, "connect", "Resolving Dealer endpoints");
    auto const response{co_await http.request({.host{"apresolve.spotify.com"}, .port{"443"}, .target{"/?type=dealer"}, .method{"GET"}, .headers{}, .body{}})};
    if(response.status != 200) throw std::runtime_error{"Dealer resolver failed"};
    auto object = nlohmann::json::parse(response.body);
    auto const values{object.at("dealer").get<std::vector<std::string>>()};
    if(values.empty() || values.size() > 32) throw std::runtime_error{"invalid Dealer address list"};
    std::vector<net::endpoint> result;
    for(auto const &value : values) {
      auto const colon{value.rfind(':')};
      if(colon == std::string::npos) throw std::runtime_error{"invalid Dealer address"};
      auto const host{value.substr(0, colon)};
      auto const port{value.substr(colon + 1)};
      unsigned int number{0};
      auto const [end, error]{std::from_chars(port.data(), port.data() + port.size(), number)};
      if(host.empty() || host.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") != std::string::npos
        || error != std::errc{} || end != port.data() + port.size() || number == 0 || number > 65535) throw std::runtime_error{"invalid Dealer address"};
      result.push_back({.host{host}, .port{port}});
      emit_log(config.on_log, log_level::debug, "connect", "Resolved Dealer endpoint " + host + ':' + port);
    }
    co_return result;
  }

  asio::awaitable<void> publish(proto::PutStateReason reason) {
    validate(player);
    auto const now{timestamp()};
    if(!player.active) active_since = 0;
    else if(active_since == 0) active_since = now;
    registration.set_put_state_reason(reason);
    registration.set_is_active(player.active);
    registration.set_started_playing_at(active_since);
    registration.set_client_side_timestamp(now);
    registration.mutable_device()->mutable_device_info()->set_volume(player.volume);
    auto &state{*registration.mutable_device()->mutable_player_state()};
    state.set_timestamp(static_cast<std::int64_t>(now));
    auto const context{player.active && player.context_uri.empty() ? std::string{"spotify:unknown"} : player.context_uri};
    state.set_context_uri(context);
    state.set_context_url(context.empty() ? "" : "context://" + context);
    state.set_position_as_of_timestamp(player.position.count());
    state.set_position(player.position.count());
    state.set_duration(player.duration.count());
    /// Spotify controllers require all three flags for a paused active player
    state.set_is_playing(player.active);
    state.set_is_paused(player.paused);
    state.set_is_buffering(player.buffering || (player.active && player.paused));
    state.set_is_system_initiated(true);
    state.set_playback_speed(player.paused ? 0.0 : 1.0);
    state.mutable_track()->set_uri(player.track.uri);
    state.mutable_track()->set_uid(player.track.uid);
    state.mutable_track()->set_provider("context");
    state.mutable_options()->set_shuffling_context(player.shuffle);
    state.mutable_options()->set_repeating_context(player.repeat_context);
    state.mutable_options()->set_repeating_track(player.repeat_track);
    state.mutable_play_origin();
    state.mutable_suppressions();
    state.clear_next_tracks();
    state.clear_prev_tracks();
    for(auto const &track : player.next_tracks) {
      auto &item{*state.add_next_tracks()};
      item.set_uri(track.uri);
      item.set_uid(track.uid);
      item.set_provider("queue");
    }
    for(auto const &track : player.previous_tracks) {
      auto &item{*state.add_prev_tracks()};
      item.set_uri(track.uri);
      item.set_uid(track.uid);
      item.set_provider("context");
    }
    emit_log(config.on_log, log_level::debug, "connect", "Publishing " + proto::PutStateReason_Name(reason)
      + "; device=" + config.device.id + "; active=" + (player.active ? "true" : "false")
      + "; paused=" + (player.paused ? "true" : "false") + "; position_ms=" + std::to_string(player.position.count()));
    if(config.on_log) emit_log(config.on_log, log_level::trace, "connect", "PutStateRequest " + diagnostic_message(registration));
    co_await service.put_connect_state(config.device.id, connection_id, registration.SerializeAsString());
    registered = true;
    emit_log(config.on_log, log_level::debug, "connect", "Connect state publication accepted");
  }

  asio::awaitable<bool> control(command request) {
    if(config.on_state) config.on_state(player);
    emit_log(config.on_log, log_level::debug, "connect", "Command endpoint=" + request.endpoint + "; message_id="
      + std::to_string(request.message_id) + "; sender=" + request.sender);
    if(config.on_log && !request.payload.empty()) emit_log(config.on_log, log_level::trace, "connect", "Decoded command " + diagnostic_json(request.payload));
    auto proposed{player};
    if(!co_await handler(request, proposed)) {
      emit_log(config.on_log, log_level::debug, "connect", "Application rejected command " + request.endpoint);
      co_return false;
    }
    validate(proposed);
    player = std::move(proposed);
    registration.set_last_command_message_id(request.message_id);
    registration.set_last_command_sent_by_device_id(request.sender);
    co_await publish(request.type == command_type::volume ? proto::VOLUME_CHANGED : proto::PLAYER_STATE_CHANGED);
    co_return true;
  }

  asio::awaitable<void> handle(std::string bytes) {
    if(config.on_log) emit_log(config.on_log, log_level::trace, "connect", "Dealer envelope " + diagnostic_json(bytes));
    auto message = nlohmann::json::parse(bytes);
    auto const type{message.at("type").get<std::string>()};
    emit_log(config.on_log, log_level::debug, "connect", "Received Dealer " + type + "; bytes=" + std::to_string(bytes.size()));
    if(type == "pong") co_return;
    if(type == "ping") {
      co_await dealer->send("{\"type\":\"pong\"}");
      co_return;
    }
    if(type == "request") {
      auto const key{message.at("key").get<std::string>()};
      emit_log(config.on_log, log_level::debug, "connect", "Request " + diagnostic_url(message.at("message_ident").get<std::string>()));
      bool accepted{false};
      try {
        if(!connection_id.empty() && message.at("message_ident").get<std::string>().starts_with("hm://connect-state/v1/player/command")) {
          auto const decoded{payload(message, true)};
          if(config.on_log) emit_log(config.on_log, log_level::trace, "connect", "Decoded request " + diagnostic_json(decoded));
          accepted = co_await control(decode_command(decoded));
        }
      } catch(std::exception const &error) {
        emit_log(config.on_log, log_level::warning, "connect", "Command failed: " + diagnostic_error(error));
        status("Connect command rejected");
      }
      emit_log(config.on_log, log_level::debug, "connect", "Reply success=" + std::string{accepted ? "true" : "false"});
      co_await dealer->send(nlohmann::json{{"type", "reply"}, {"key", key}, {"payload", {{"success", accepted}}}}.dump());
      co_return;
    }
    if(type != "message") throw std::runtime_error{"unknown Dealer envelope type"};
    auto const uri{message.at("uri").get<std::string>()};
    emit_log(config.on_log, log_level::debug, "connect", "Message " + diagnostic_url(uri));
    if(uri.starts_with("hm://pusher/v1/connections/")) {
      connection_id = message.at("headers").at("Spotify-Connection-Id").get<std::string>();
      if(connection_id.empty() || connection_id.size() > 4096) throw std::runtime_error{"invalid Dealer connection ID"};
      emit_log(config.on_log, log_level::debug, "connect", "Received Spotify connection ID; registering device");
      co_await publish(proto::NEW_DEVICE);
      status("Connect device registered");
    } else if(uri.starts_with("hm://connect-state/v1/connect/volume")) {
      proto::SetVolumeCommand volume;
      if(!volume.ParseFromString(payload(message, false)) || volume.volume() < 0 || volume.volume() > 65535) throw std::runtime_error{"invalid Connect volume command"};
      command request;
      request.type = command_type::volume;
      request.endpoint = "set_volume";
      request.volume = static_cast<std::uint16_t>(volume.volume());
      request.message_id = static_cast<std::uint32_t>(volume.command_options().message_id());
      co_await control(std::move(request));
    } else if(uri.starts_with("hm://connect-state/v1/cluster")) {
      proto::ClusterUpdate update;
      if(!update.ParseFromString(payload(message, false))) throw std::runtime_error{"invalid Connect cluster update"};
      emit_log(config.on_log, log_level::debug, "connect", "Cluster update " + proto::ClusterUpdateReason_Name(update.update_reason())
        + "; active_device=" + update.cluster().active_device_id());
      if(config.on_log) emit_log(config.on_log, log_level::trace, "connect", "ClusterUpdate " + diagnostic_message(update));
      auto const changed{update.cluster().changed_timestamp_ms()};
      auto const stale{changed > 0 && static_cast<std::uint64_t>(changed) < active_since};
      if(stale) emit_log(config.on_log, log_level::debug, "connect", "Ignoring cluster state from before this device acquired playback");
      if(player.active && !stale && update.cluster().active_device_id() != config.device.id) {
        player.active = false;
        active_since = 0;
        player.paused = true;
        if(config.on_state) config.on_state(player);
        status("Playback transferred to another device");
      }
    } else {
      emit_log(config.on_log, log_level::debug, "connect", "Ignoring unsubscribed Dealer message " + diagnostic_url(uri));
    }
  }

  static asio::awaitable<void> run(std::shared_ptr<implementation> self) {
    if(self->started || self->stopped) throw std::logic_error{"Connect receiver cannot be restarted"};
    self->started = true;
    std::exception_ptr failure;
    unsigned int attempts{0};
    while(!self->stopped) {
      std::string stage{"Dealer endpoint resolution"};
      try {
        auto const addresses{co_await self->resolve()};
        bool connected{false};
        for(auto const &address : addresses) {
          if(self->stopped) break;
          try {
            stage = "service authentication";
            emit_log(self->config.on_log, log_level::debug, "connect", "Obtaining access token for Dealer");
            auto const token{co_await self->auth.token()};
            if(self->stopped) break;
            stage = "Dealer connection to " + address.host + ':' + address.port;
            emit_log(self->config.on_log, log_level::debug, "connect", "Connecting to " + address.host + ':' + address.port);
            co_await self->dealer->connect(address, token.value);
            connected = true;
            break;
          } catch(std::exception const &error) {
            if(!self->stopped) emit_log(self->config.on_log, log_level::warning, "connect", stage + " failed: " + diagnostic_error(error));
            self->dealer->close();
          }
        }
        if(!connected) throw std::runtime_error{"Dealer endpoints unavailable"};
        self->connection_id.clear();
        self->status("Dealer connected");
        self->input = std::make_shared<inbox>(self->executor);
        auto const input{self->input};
        auto completed{[input](std::exception_ptr failure){
          if(failure && !input->stopped) {
            input->failure = failure;
            input->messages.try_send(boost::system::error_code{}, std::string{});
          }
          --input->tasks;
          input->finished.cancel();
        }};
        ++input->tasks;
        asio::co_spawn(self->executor, receive_messages(self->dealer, input), completed);
        if(self->config.on_state) {
          ++input->tasks;
          asio::co_spawn(self->executor, playback_ticks(input, self->config.state_interval), completed);
        }
        while(!self->stopped) {
          stage = "Dealer receive";
          auto bytes{co_await input->messages.async_receive(asio::use_awaitable)};
          if(input->failure) std::rethrow_exception(input->failure);
          if(bytes.empty()) {
            if(self->config.on_state && self->player.active && self->registered) {
              stage = "playback state publication";
              self->config.on_state(self->player);
              co_await self->publish(proto::PLAYER_STATE_CHANGED);
            }
            continue;
          }
          stage = "Dealer message handling / Connect registration";
          co_await self->handle(std::move(bytes));
          attempts = 0;
        }
      } catch(std::exception const &error) {
        if(!self->stopped) {
          failure = std::current_exception();
          emit_log(self->config.on_log, log_level::error, "connect", stage + " failed: " + diagnostic_error(error));
        }
      }
      self->dealer->close();
      if(self->input) {
        auto const input{self->input};
        input->stopped = true;
        input->tick.cancel();
        input->messages.cancel();
        co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
        while(input->tasks != 0) {
          boost::system::error_code ignored;
          co_await input->finished.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
        }
        self->input.reset();
      }
      if(self->stopped || !self->config.reconnect) break;
      self->auth.invalidate_token();
      auto const delay{std::chrono::seconds{std::min(30u, 1u << std::min(attempts++, 5u))}};
      self->status("Dealer disconnected; reconnecting");
      emit_log(self->config.on_log, log_level::debug, "connect", "Retrying in " + std::to_string(delay.count()) + " seconds");
      self->retry.expires_after(delay);
      boost::system::error_code ignored;
      co_await self->retry.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
    }
    co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
    if(self->registered) {
      try {
        emit_log(self->config.on_log, log_level::debug, "connect", "Withdrawing Connect device registration");
        co_await self->service.delete_connect_state(self->config.device.id);
      } catch(std::exception const &error) {
        emit_log(self->config.on_log, log_level::warning, "connect", "Withdrawal failed: " + diagnostic_error(error));
        self->status("Connect registration could not be withdrawn");
      }
    }
    self->done = true;
    self->finished.cancel();
    if(failure && !self->stopped) std::rethrow_exception(failure);
  }

  static asio::awaitable<void> shutdown(std::shared_ptr<implementation> self) {
    self->stop();
    while(self->started && !self->done) {
      boost::system::error_code ignored;
      co_await self->finished.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
    }
  }
};

receiver::receiver(asio::any_io_executor executor, net::http_transport &http, oauth::service_auth &auth,
  receiver_config config, command_handler handler, std::shared_ptr<net::dealer_transport> dealer)
  : state{std::make_shared<implementation>(std::move(executor), http, auth, std::move(config), std::move(handler), std::move(dealer))} {
}

receiver::~receiver() {
  close();
}

asio::awaitable<void> receiver::run() {
  return asio::co_spawn(state->executor, implementation::run(state), asio::bind_cancellation_slot(state->cancellation.slot(), asio::use_awaitable));
}

asio::awaitable<void> receiver::shutdown() {
  return asio::co_spawn(state->executor, implementation::shutdown(state), asio::use_awaitable);
}

void receiver::close() {
  if(state) asio::dispatch(state->executor, [shared{state}]{
    shared->stop();
  });
}

} // namespace librespot::connect
