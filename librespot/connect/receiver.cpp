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
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
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
  asio::steady_timer retry;
  asio::steady_timer finished;
  asio::cancellation_signal cancellation;
  bool stopped{false};
  bool started{false};
  bool done{false};
  bool registered{false};

  implementation(asio::any_io_executor executor, net::http_transport &http, oauth::service_auth &auth,
    receiver_config config, command_handler handler, std::shared_ptr<net::dealer_transport> dealer)
    : executor{asio::make_strand(std::move(executor))}, http{http}, auth{auth}, config{std::move(config)}, handler{std::move(handler)},
      dealer{dealer ? std::move(dealer) : std::make_shared<net::dealer>(this->executor, this->config.dealer)}, service{http, auth}, retry{this->executor}, finished{this->executor} {
    if(this->config.device.id.empty() || this->config.device.name.empty() || !this->handler) throw std::invalid_argument{"Connect identity and command handler are required"};
    finished.expires_at(std::chrono::steady_clock::time_point::max());
    player.volume = this->config.device.initial_volume;
    auto &device{*registration.mutable_device()->mutable_device_info()};
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
    registration.mutable_device()->mutable_player_state()->set_session_id(identity.id);
  }

  void status(std::string message) {
    if(config.on_status) {
      try {
        config.on_status(std::move(message));
      } catch(...) {
        // diagnostic callbacks must not interrupt transport cleanup
      }
    }
  }

  void stop() {
    stopped = true;
    cancellation.emit(asio::cancellation_type::all);
    dealer->close();
    retry.cancel();
  }

  asio::awaitable<std::vector<net::endpoint>> resolve() {
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
    }
    co_return result;
  }

  asio::awaitable<void> publish(proto::PutStateReason reason) {
    validate(player);
    auto const now{timestamp()};
    registration.set_put_state_reason(reason);
    registration.set_is_active(player.active);
    registration.set_client_side_timestamp(now);
    registration.mutable_device()->mutable_device_info()->set_volume(player.volume);
    auto &state{*registration.mutable_device()->mutable_player_state()};
    state.set_timestamp(static_cast<std::int64_t>(now));
    state.set_context_uri(player.context_uri);
    state.set_position_as_of_timestamp(player.position.count());
    state.set_position(player.position.count());
    state.set_duration(player.duration.count());
    state.set_is_playing(player.active);
    state.set_is_paused(player.paused);
    state.set_is_buffering(player.buffering);
    state.set_is_system_initiated(true);
    state.set_playback_speed(1.0);
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
    co_await service.put_connect_state(config.device.id, connection_id, registration.SerializeAsString());
    registered = true;
  }

  asio::awaitable<bool> control(command request) {
    auto proposed{player};
    if(!co_await handler(request, proposed)) co_return false;
    validate(proposed);
    player = std::move(proposed);
    registration.set_last_command_message_id(request.message_id);
    registration.set_last_command_sent_by_device_id(request.sender);
    co_await publish(request.type == command_type::volume ? proto::VOLUME_CHANGED : proto::PLAYER_STATE_CHANGED);
    co_return true;
  }

  asio::awaitable<void> handle(std::string bytes) {
    auto message = nlohmann::json::parse(bytes);
    auto const type{message.at("type").get<std::string>()};
    if(type == "pong") co_return;
    if(type == "ping") {
      co_await dealer->send("{\"type\":\"pong\"}");
      co_return;
    }
    if(type == "request") {
      auto const key{message.at("key").get<std::string>()};
      bool accepted{false};
      try {
        if(!connection_id.empty() && message.at("message_ident").get<std::string>().starts_with("hm://connect-state/v1/player/command")) accepted = co_await control(decode_command(payload(message, true)));
      } catch(std::exception const &) {
        status("Connect command rejected");
      }
      co_await dealer->send(nlohmann::json{{"type", "reply"}, {"key", key}, {"payload", {{"success", accepted}}}}.dump());
      co_return;
    }
    if(type != "message") throw std::runtime_error{"unknown Dealer envelope type"};
    auto const uri{message.at("uri").get<std::string>()};
    if(uri.starts_with("hm://pusher/v1/connections/")) {
      connection_id = message.at("headers").at("Spotify-Connection-Id").get<std::string>();
      if(connection_id.empty() || connection_id.size() > 4096) throw std::runtime_error{"invalid Dealer connection ID"};
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
      if(player.active && update.cluster().active_device_id() != config.device.id) {
        player.active = false;
        player.paused = true;
        status("Playback transferred to another device");
      }
    }
  }

  static asio::awaitable<void> run(std::shared_ptr<implementation> self) {
    if(self->started || self->stopped) throw std::logic_error{"Connect receiver cannot be restarted"};
    self->started = true;
    std::exception_ptr failure;
    unsigned int attempts{0};
    while(!self->stopped) {
      try {
        auto const addresses{co_await self->resolve()};
        bool connected{false};
        for(auto const &address : addresses) {
          if(self->stopped) break;
          try {
            auto const token{co_await self->auth.token()};
            if(self->stopped) break;
            co_await self->dealer->connect(address, token.value);
            connected = true;
            break;
          } catch(std::exception const &) {
            self->dealer->close();
          }
        }
        if(!connected) throw std::runtime_error{"Dealer endpoints unavailable"};
        self->connection_id.clear();
        self->status("Dealer connected");
        while(!self->stopped) {
          co_await self->handle(co_await self->dealer->receive());
          attempts = 0;
        }
      } catch(std::exception const &) {
        if(!self->stopped) failure = std::current_exception();
      }
      self->dealer->close();
      if(self->stopped || !self->config.reconnect) break;
      self->auth.invalidate_token();
      self->status("Dealer disconnected; reconnecting");
      self->retry.expires_after(std::chrono::seconds{std::min(30u, 1u << std::min(attempts++, 5u))});
      boost::system::error_code ignored;
      co_await self->retry.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
    }
    co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
    if(self->registered) {
      try {
        co_await self->service.delete_connect_state(self->config.device.id);
      } catch(std::exception const &) {
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
