#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <syncstream>
#include <vector>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/program_options.hpp>
#include <boost/system/system_error.hpp>
#include <openssl/evp.h>
#include "librespot/audio/cdn_source.h"
#include "librespot/audio/paced_player.h"
#include "librespot/audio/wav_writer.h"
#include "librespot/cache/credentials.h"
#include "librespot/connect/receiver.h"
#include "librespot/core/spotify_uri.h"
#include "librespot/discovery/server.h"
#include "librespot/session.h"

namespace {

librespot::log_level log_level(std::string const &name) {
  for(auto level : {librespot::log_level::trace, librespot::log_level::debug, librespot::log_level::info,
    librespot::log_level::warning, librespot::log_level::error, librespot::log_level::off}) {
    auto text{std::string{librespot::to_string(level)}};
    for(auto &byte : text) if(byte >= 'A' && byte <= 'Z') byte = static_cast<char>(byte - 'A' + 'a');
    if(name == text) return level;
  }
  throw std::invalid_argument{"--log-level must be trace, debug, info, warning, error, or off"};
}

librespot::log_handler console_logger(librespot::log_level minimum) {
  if(minimum == librespot::log_level::off) return {};
  return [minimum, start{std::chrono::steady_clock::now()}](librespot::log_event const &event){
    if(event.level < minimum) return;
    auto const elapsed{std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)};
    std::osyncstream{std::cerr} << std::format("[{:>8}ms] [{:7}] [{}] {}", elapsed.count(), librespot::to_string(event.level), event.component, event.message) << std::endl;
  };
}

std::string stable_id(std::string const &name) {
  /// Derive a stable example identity from the machine ID and application device name
  std::ifstream input{"/etc/machine-id"};
  std::string machine;
  if(!std::getline(input, machine) || machine.empty()) throw std::runtime_error{"cannot read machine ID; supply --device-id"};
  auto const identity{machine + ':' + name};
  std::array<unsigned char, 20> digest{};
  unsigned int count{0};
  if(EVP_Digest(identity.data(), identity.size(), digest.data(), &count, EVP_sha1(), nullptr) != 1) throw std::runtime_error{"cannot derive device ID"};
  std::string result;
  for(auto const byte : digest) {
    result += "0123456789abcdef"[byte >> 4];
    result += "0123456789abcdef"[byte & 15];
  }
  return result;
}

struct recording {
  std::filesystem::path path;
  librespot::log_handler on_log;
  std::optional<librespot::audio::wav_writer> writer;

  void append(librespot::audio::pcm_frame const &frame) {
    if(!writer) {
      writer.emplace(path, frame.format());
      librespot::emit_log(on_log, librespot::log_level::info, "recording", "Writing PCM to " + path.string());
    }
    writer->append(frame);
  }

  boost::asio::awaitable<void> finish() {
    if(writer) {
      writer->finish();
      librespot::emit_log(on_log, librespot::log_level::info, "recording", "Finalized WAV " + path.string());
    }
    co_return;
  }
};

boost::asio::awaitable<void> play(librespot::audio::paced_player &player, std::shared_ptr<recording> output,
  boost::asio::any_io_executor worker) {
  std::exception_ptr failure;
  try {
    co_await player.run();
  } catch(...) {
    failure = std::current_exception();
  }
  if(output) co_await boost::asio::co_spawn(worker, output->finish(), boost::asio::use_awaitable);
  if(failure) std::rethrow_exception(failure);
}

struct application : std::enable_shared_from_this<application> {
  boost::asio::any_io_executor executor;
  boost::asio::any_io_executor worker;
  librespot::net::http_transport &http;
  librespot::connect::receiver_config config;
  std::filesystem::path credentials_path;
  std::optional<std::filesystem::path> output_path;
  unsigned int recordings{0};
  std::unique_ptr<librespot::session> session;
  std::unique_ptr<librespot::oauth::service_auth> auth;
  std::unique_ptr<librespot::connect::receiver> receiver;
  std::unique_ptr<librespot::audio::paced_player> player;
  boost::asio::steady_timer player_finished;
  std::string playing_uri;
  std::string playing_label;
  std::chrono::milliseconds playing_duration{0};
  bool player_running{false};
  std::string reported_error;
  bool stopped{false};

  application(boost::asio::any_io_executor executor, boost::asio::any_io_executor worker, librespot::net::http_transport &http,
    librespot::connect::receiver_config config, std::filesystem::path credentials_path, std::optional<std::filesystem::path> output_path)
    : executor{std::move(executor)}, worker{std::move(worker)}, http{http}, config{std::move(config)}, credentials_path{std::move(credentials_path)}, output_path{std::move(output_path)}, player_finished{this->executor} {
    player_finished.expires_at(std::chrono::steady_clock::time_point::max());
  }

  boost::asio::awaitable<void> stop_player() {
    if(player) player->close();
    while(player_running) {
      boost::system::error_code ignored;
      co_await player_finished.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
    }
    player.reset();
    playing_uri.clear();
    playing_label.clear();
    reported_error.clear();
  }

  void refresh(librespot::connect::player_state &state) {
    if(!player) {
      if(state.active) {
        state.paused = true;
        state.buffering = false;
      }
      return;
    }
    if(state.track.uri != playing_uri) return;
    auto const status{player->snapshot()};
    state.position = std::chrono::duration_cast<std::chrono::milliseconds>(status.position);
    state.duration = playing_duration;
    state.buffering = status.buffering;
    if(status.ended || !status.error.empty()) state.paused = true;
    if(!state.active) player->pause(true);
    if(!status.error.empty() && status.error != reported_error) {
      reported_error = status.error;
      librespot::emit_log(config.on_log, librespot::log_level::error, "playback", status.error);
    }
    librespot::emit_log(config.on_log, librespot::log_level::trace, "playback", std::format("PCM frames={}; samples/channel={}; position_ms={}; paused={}; buffering={}; ended={}",
      status.frames, status.samples, state.position.count(), status.paused, status.buffering, status.ended));
  }

  boost::asio::awaitable<bool> control(librespot::connect::command const &command, librespot::connect::player_state &state) {
    if(stopped || !librespot::connect::apply_control(command, state)) co_return false;
    auto const playback{player ? std::optional{player->snapshot()} : std::nullopt};
    if(playback && playback->ended
      && (command.type == librespot::connect::command_type::resume || command.type == librespot::connect::command_type::play)) state.position = std::chrono::milliseconds{0};
    if(state.active && (state.track.uri != playing_uri || !player || !player_running || playback->ended || !playback->error.empty())) {
      auto const parsed{librespot::parse_uri(state.track.uri)};
      auto const *uri{parsed ? std::get_if<librespot::catalog_uri>(&*parsed) : nullptr};
      if(!uri || uri->type != librespot::item_type::track) co_return false;
      co_await stop_player();
      librespot::service::client service{http, *auth, config.on_log};
      auto track{co_await service.get_track(uri->id)};
      auto file{librespot::service::select_audio(track)};
      std::vector<std::string> urls;
      for(;;) {
        try {
          urls = co_await service.resolve_audio(file.id);
          break;
        } catch(librespot::service::storage_unavailable const &) {
          std::erase_if(track.files, [&file](librespot::service::audio_file const &candidate){ return candidate.id == file.id; });
          if(track.files.empty()) throw;
          file = librespot::service::select_audio(track);
        }
      }
      std::optional<librespot::audio::audio_key> key;
      try {
        key = co_await session->request_audio_key(track.id, file.id);
      } catch(boost::system::system_error const &error) {
        if(error.code() != boost::asio::error::access_denied) throw;
        librespot::emit_log(config.on_log, librespot::log_level::warning, "playback", "No audio key granted; trying an unencrypted container");
      }
      if(stopped) co_return false;
      auto source{std::make_shared<librespot::audio::cdn_source>(executor, http, std::move(urls), key,
        librespot::audio::cdn_source_config{.container_offset{librespot::service::is_vorbis(file.format) ? 167u : 0u}})};
      playing_uri = state.track.uri;
      playing_duration = track.duration;
      for(auto const &artist : track.artists) {
        if(artist.empty()) continue;
        if(!playing_label.empty()) playing_label += ", ";
        playing_label += artist;
      }
      if(!track.name.empty()) {
        if(!playing_label.empty()) playing_label += " - ";
        playing_label += track.name;
      }
      std::shared_ptr<recording> output;
      std::function<void(librespot::audio::pcm_frame const &)> sink;
      if(output_path) {
        auto path{*output_path};
        if(++recordings > 1) path = path.parent_path() / (path.stem().string() + '-' + std::to_string(recordings) + path.extension().string());
        output = std::make_shared<recording>(std::move(path), config.on_log, std::nullopt);
        sink = [output](librespot::audio::pcm_frame const &frame){ output->append(frame); };
      }
      player = std::make_unique<librespot::audio::paced_player>(worker,
        [source]{ return std::make_unique<librespot::audio::decoder>(source); },
        librespot::audio::paced_player_config{.position{state.position}, .paused{state.paused}, .on_frame{std::move(sink)},
          .cancel_source{[source]{ source->cancel(); }}, .on_log{config.on_log}});
      librespot::emit_log(config.on_log, librespot::log_level::info, "playback", "Opening " + (playing_label.empty() ? playing_uri : playing_label) + "; "
        + std::string{librespot::service::to_string(file.format)} + "; duration_ms=" + std::to_string(track.duration.count()));
      player_running = true;
      auto self{shared_from_this()};
      boost::asio::co_spawn(executor, play(*player, std::move(output), worker), [self](std::exception_ptr failure){
        if(failure) {
          try { std::rethrow_exception(failure); }
          catch(std::exception const &error) { librespot::emit_log(self->config.on_log, librespot::log_level::error, "playback", librespot::diagnostic_error(error)); }
        }
        self->player_running = false;
        self->player_finished.cancel();
      });
    } else if(player) {
      if(command.type == librespot::connect::command_type::seek || command.type == librespot::connect::command_type::transfer) player->seek(state.position);
      player->pause(state.paused || !state.active);
    }
    state.duration = playing_duration;
    state.buffering = player && player->snapshot().buffering;
    auto const &label{state.track.uri == playing_uri && !playing_label.empty() ? playing_label : state.track.uri};
    librespot::emit_log(config.on_log, librespot::log_level::info, "example", "Command " + command.endpoint + "; track=" + label);
    co_return true;
  }

  boost::asio::awaitable<void> disconnect() {
    librespot::emit_log(config.on_log, librespot::log_level::debug, "example", "Disconnecting account services");
    if(receiver) co_await receiver->shutdown();
    receiver.reset();
    co_await stop_player();
    auth.reset();
    if(session) session->close();
    session.reset();
  }

  boost::asio::awaitable<librespot::credentials> login(librespot::credentials credentials, bool replacing_account = true) {
    if(stopped) throw std::runtime_error{"receiver is stopping"};
    co_await disconnect();
    librespot::emit_log(config.on_log, librespot::log_level::info, "example", replacing_account ? "Authenticating newly paired account" : "Authenticating saved reusable credentials");
    if(replacing_account) {
      std::error_code removed;
      std::filesystem::remove(credentials_path, removed);
      if(removed) throw std::system_error{removed, "cannot clear previous account credentials"};
    }
    session = std::make_unique<librespot::session>(executor, http, librespot::session_config{.device_id{config.device.id}, .on_log{config.on_log}});
    auto reusable{co_await session->connect(std::move(credentials))};
    if(stopped) {
      session->close();
      throw std::runtime_error{"receiver is stopping"};
    }
    auth = std::make_unique<librespot::oauth::service_auth>(http,
      librespot::oauth::service_auth_config{.device_id{config.device.id}, .client_id{config.device.client_id}, .on_log{config.on_log}}, reusable, worker);
    co_await auth->token();
    if(stopped) throw std::runtime_error{"receiver is stopping"};
    librespot::cache::save_credentials(credentials_path, reusable);
    librespot::emit_log(config.on_log, librespot::log_level::debug, "example", "Reusable credentials saved; starting Connect receiver");
    config.on_state = [this](librespot::connect::player_state &state){ refresh(state); };
    receiver = std::make_unique<librespot::connect::receiver>(executor, http, *auth, config,
      [this](librespot::connect::command const &command, librespot::connect::player_state &state)->boost::asio::awaitable<bool> {
        co_return co_await control(command, state);
      });
    auto self{shared_from_this()};
    boost::asio::co_spawn(executor, receiver->run(), [self](std::exception_ptr failure){
      if(failure && !self->stopped) {
        try {
          std::rethrow_exception(failure);
        } catch(std::exception const &error) {
          librespot::emit_log(self->config.on_log, librespot::log_level::error, "example", "Connect receiver stopped: " + librespot::diagnostic_error(error));
        }
      }
    });
    co_return reusable;
  }

  boost::asio::awaitable<void> reset() {
    librespot::emit_log(config.on_log, librespot::log_level::info, "example", "Resetting paired account and removing credentials");
    co_await disconnect();
    std::error_code error;
    std::filesystem::remove(credentials_path, error);
    if(error) throw std::system_error{error, "cannot remove credential file"};
  }

  void stop() {
    if(!stopped) librespot::emit_log(config.on_log, librespot::log_level::info, "example", "Shutdown requested");
    stopped = true;
    if(receiver) receiver->close();
    if(player) player->close();
    if(session) session->close();
  }
};

boost::asio::awaitable<void> run(std::shared_ptr<application> app, librespot::discovery::server &discovery) {
  std::exception_ptr failure;
  try {
    if(auto stored{librespot::cache::load_credentials(app->credentials_path)}) {
      bool authenticated{false};
      try {
        auto const accepted{co_await app->login(std::move(*stored), false)};
        discovery.set_active_user(accepted.username.value_or(""));
        authenticated = true;
      } catch(std::exception const &error) {
        if(!app->stopped) librespot::emit_log(app->config.on_log, librespot::log_level::warning, "example", "Saved account could not be authenticated: "
          + librespot::diagnostic_error(error) + "; waiting for local pairing");
      }
      if(!authenticated) co_await app->disconnect();
    }
    if(!app->stopped) {
      std::cout << "Discovery listening on port " << discovery.port() << "; select \"" << app->config.device.name
        << "\" in Spotify on the same network. This example produces no audio." << std::endl;
      co_await discovery.run();
    }
  } catch(...) {
    if(!app->stopped) failure = std::current_exception();
  }
  app->stop();
  co_await discovery.shutdown();
  co_await app->disconnect();
  if(failure) std::rethrow_exception(failure);
}

} // anonymous namespace

auto main(int argc, char const *argv[])->int try {
  boost::program_options::options_description description{"Spotify Connect receiver with paced PCM decoding (no audio output)"};
  description.add_options()
    ("help,h", "Show usage")
    ("name", boost::program_options::value<std::string>()->default_value("C++ Connect test"), "Device name in Spotify")
    ("device-id", boost::program_options::value<std::string>(), "Stable device identity; otherwise derived from machine ID and name")
    ("credentials", boost::program_options::value<std::string>()->default_value("connect_credentials.json"), "Owner-only reusable credential file")
    ("output", boost::program_options::value<std::string>(), "Record consumed PCM to WAV; later streams use -2, -3 suffixes")
    ("brand", boost::program_options::value<std::string>()->default_value(""), "Application device brand")
    ("model", boost::program_options::value<std::string>()->default_value(""), "Application device model")
    ("client-id", boost::program_options::value<std::string>()->default_value(librespot::connect::device_config{}.client_id), "Spotify protocol client identity")
    ("user-agent", boost::program_options::value<std::string>()->default_value("connect_receiver"), "Application HTTP/websocket user agent")
    ("log-level", boost::program_options::value<std::string>()->default_value("debug"), "Diagnostics: trace, debug, info, warning, error, off")
    ("bind", boost::program_options::value<std::string>()->default_value("::"), "Pairing listener address")
    ("port", boost::program_options::value<std::uint16_t>()->default_value(0), "Pairing port; zero chooses an available port")
    ("no-discovery", boost::program_options::bool_switch(), "Disable mDNS publication for local protocol testing");
  boost::program_options::variables_map arguments;
  boost::program_options::store(boost::program_options::parse_command_line(argc, argv, description), arguments);
  if(arguments.contains("help")) {
    std::cout << description << std::endl;
    return EXIT_SUCCESS;
  }
  boost::program_options::notify(arguments);
  auto const logger{console_logger(log_level(arguments.at("log-level").as<std::string>()))};
  auto const name{arguments.at("name").as<std::string>()};
  librespot::connect::device_config identity{
    .id{arguments.contains("device-id") ? arguments.at("device-id").as<std::string>() : stable_id(name)},
    .name{name}, .brand{arguments.at("brand").as<std::string>()}, .model{arguments.at("model").as<std::string>()},
    .client_id{arguments.at("client-id").as<std::string>()},
  };
  boost::asio::io_context executor;
  boost::asio::thread_pool worker{2};
  auto const agent{arguments.at("user-agent").as<std::string>()};
  librespot::net::http_client http{executor.get_executor(), {.user_agent{agent}, .on_log{logger}}};
  std::optional<std::filesystem::path> output;
  if(arguments.contains("output")) output = arguments.at("output").as<std::string>();
  auto app{std::make_shared<application>(executor.get_executor(), worker.get_executor(), http,
    librespot::connect::receiver_config{.device{identity}, .dealer{.user_agent{agent}}, .on_log{logger}}, arguments.at("credentials").as<std::string>(), std::move(output))};
  librespot::discovery::server discovery{executor.get_executor(), {
    .device{identity}, .address{arguments.at("bind").as<std::string>()}, .port{arguments.at("port").as<std::uint16_t>()},
    .advertise{!arguments.at("no-discovery").as<bool>()}, .active_user{}, .on_error{[logger](std::string message){
      librespot::emit_log(logger, librespot::log_level::error, "discovery", std::move(message));
    }}, .on_log{logger},
  }, {.add_user{[app](librespot::credentials credentials)->boost::asio::awaitable<librespot::credentials> {
    co_return co_await app->login(std::move(credentials));
  }}, .reset_users{[app]()->boost::asio::awaitable<void> {
    co_await app->reset();
  }}}};
  boost::asio::signal_set signals{executor, SIGINT, SIGTERM};
  signals.async_wait([&](boost::system::error_code error, int){
    if(error) return;
    app->stop();
    discovery.close();
  });
  int result{EXIT_SUCCESS};
  boost::asio::co_spawn(executor, run(app, discovery), [&](std::exception_ptr failure){
    signals.cancel();
    if(!failure) return;
    result = EXIT_FAILURE;
    try {
      std::rethrow_exception(failure);
    } catch(std::exception const &error) {
      std::cerr << error.what() << std::endl;
    }
  });
  executor.run();
  worker.join();
  return result;
} catch(std::exception const &error) {
  std::cerr << error.what() << std::endl;
  return EXIT_FAILURE;
}
