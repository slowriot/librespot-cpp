#include <array>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/program_options.hpp>
#include <openssl/evp.h>
#include "librespot/cache/credentials.h"
#include "librespot/connect/receiver.h"
#include "librespot/discovery/server.h"
#include "librespot/session.h"

namespace {

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

struct application : std::enable_shared_from_this<application> {
  boost::asio::any_io_executor executor;
  boost::asio::any_io_executor worker;
  librespot::net::http_transport &http;
  librespot::connect::receiver_config config;
  std::filesystem::path credentials_path;
  std::unique_ptr<librespot::session> session;
  std::unique_ptr<librespot::oauth::service_auth> auth;
  std::unique_ptr<librespot::connect::receiver> receiver;
  bool stopped{false};

  application(boost::asio::any_io_executor executor, boost::asio::any_io_executor worker, librespot::net::http_transport &http,
    librespot::connect::receiver_config config, std::filesystem::path credentials_path)
    : executor{std::move(executor)}, worker{std::move(worker)}, http{http}, config{std::move(config)}, credentials_path{std::move(credentials_path)} {
  }

  boost::asio::awaitable<void> disconnect() {
    if(receiver) co_await receiver->shutdown();
    receiver.reset();
    auth.reset();
    if(session) session->close();
    session.reset();
  }

  boost::asio::awaitable<librespot::credentials> login(librespot::credentials credentials, bool replacing_account = true) {
    if(stopped) throw std::runtime_error{"receiver is stopping"};
    co_await disconnect();
    if(replacing_account) {
      std::error_code removed;
      std::filesystem::remove(credentials_path, removed);
      if(removed) throw std::system_error{removed, "cannot clear previous account credentials"};
    }
    session = std::make_unique<librespot::session>(executor, http, librespot::session_config{.device_id{config.device.id}});
    auto reusable{co_await session->connect(std::move(credentials))};
    if(stopped) {
      session->close();
      throw std::runtime_error{"receiver is stopping"};
    }
    auth = std::make_unique<librespot::oauth::service_auth>(http,
      librespot::oauth::service_auth_config{.device_id{config.device.id}, .client_id{config.device.client_id}}, reusable, worker);
    co_await auth->token();
    if(stopped) throw std::runtime_error{"receiver is stopping"};
    librespot::cache::save_credentials(credentials_path, reusable);
    receiver = std::make_unique<librespot::connect::receiver>(executor, http, *auth, config,
      [](librespot::connect::command const &command, librespot::connect::player_state &state)->boost::asio::awaitable<bool> {
        auto const accepted{librespot::connect::apply_control(command, state)};
        std::cout << "Command " << command.endpoint << ": " << (accepted ? "accepted" : "unsupported")
          << "; track " << state.track.uri << "; position " << state.position.count() << " ms; volume " << state.volume << std::endl;
        co_return accepted;
      });
    auto self{shared_from_this()};
    boost::asio::co_spawn(executor, receiver->run(), [self](std::exception_ptr failure){
      if(failure && !self->stopped) std::cerr << "Connect receiver stopped" << std::endl;
    });
    co_return reusable;
  }

  boost::asio::awaitable<void> reset() {
    co_await disconnect();
    std::error_code error;
    std::filesystem::remove(credentials_path, error);
    if(error) throw std::system_error{error, "cannot remove credential file"};
  }

  void stop() {
    stopped = true;
    if(receiver) receiver->close();
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
      } catch(std::exception const &) {
        if(!app->stopped) std::cerr << "Saved account could not be authenticated; waiting for local pairing" << std::endl;
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
  boost::program_options::options_description description{"Control-only Spotify Connect receiver"};
  description.add_options()
    ("help,h", "Show usage")
    ("name", boost::program_options::value<std::string>()->default_value("C++ Connect test"), "Device name in Spotify")
    ("device-id", boost::program_options::value<std::string>(), "Stable device identity; otherwise derived from machine ID and name")
    ("credentials", boost::program_options::value<std::string>()->default_value("connect_credentials.json"), "Owner-only reusable credential file")
    ("brand", boost::program_options::value<std::string>()->default_value(""), "Application device brand")
    ("model", boost::program_options::value<std::string>()->default_value(""), "Application device model")
    ("client-id", boost::program_options::value<std::string>()->default_value(librespot::connect::device_config{}.client_id), "Spotify protocol client identity")
    ("user-agent", boost::program_options::value<std::string>()->default_value("connect_receiver"), "Application HTTP/websocket user agent")
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
  auto const name{arguments.at("name").as<std::string>()};
  librespot::connect::device_config identity{
    .id{arguments.contains("device-id") ? arguments.at("device-id").as<std::string>() : stable_id(name)},
    .name{name}, .brand{arguments.at("brand").as<std::string>()}, .model{arguments.at("model").as<std::string>()},
    .client_id{arguments.at("client-id").as<std::string>()},
  };
  boost::asio::io_context executor;
  boost::asio::thread_pool worker{1};
  auto const agent{arguments.at("user-agent").as<std::string>()};
  librespot::net::http_client http{executor.get_executor(), {.user_agent{agent}}};
  auto app{std::make_shared<application>(executor.get_executor(), worker.get_executor(), http,
    librespot::connect::receiver_config{.device{identity}, .dealer{.user_agent{agent}}, .on_status{[](std::string message){
      std::cout << message << std::endl;
    }}}, arguments.at("credentials").as<std::string>())};
  librespot::discovery::server discovery{executor.get_executor(), {
    .device{identity}, .address{arguments.at("bind").as<std::string>()}, .port{arguments.at("port").as<std::uint16_t>()},
    .advertise{!arguments.at("no-discovery").as<bool>()}, .active_user{}, .on_error{[](std::string message){
      std::cerr << message << std::endl;
    }},
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
