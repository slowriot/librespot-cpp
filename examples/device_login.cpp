#include <chrono>
#include <cstdlib>
#include <iostream>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/program_options.hpp>
#include "librespot/oauth/device_auth.h"

namespace {

boost::asio::awaitable<void> login(librespot::oauth::device_auth &auth) {
  /// Demonstrate pairing without printing access tokens or storing credentials
  auto const challenge{co_await auth.start()};
  std::cout << "Open " << challenge.verification_uri << " and enter " << challenge.user_code << std::endl;
  auto const deadline{std::chrono::steady_clock::now() + challenge.expires_in};
  auto interval{challenge.interval};
  boost::asio::steady_timer timer{co_await boost::asio::this_coro::executor};
  while(std::chrono::steady_clock::now() + interval < deadline) {
    timer.expires_after(interval);
    co_await timer.async_wait(boost::asio::use_awaitable);
    auto const result{co_await auth.poll(challenge.device_code)};
    if(std::holds_alternative<librespot::oauth::token>(result)) {
      std::cout << "Authorisation succeeded" << std::endl;
      co_return;
    }
    if(std::get<librespot::oauth::poll_status>(result) == librespot::oauth::poll_status::slow_down) interval += std::chrono::seconds{5};
  }
  throw librespot::oauth::auth_error{"expired_token", "pairing deadline reached"};
}

} // anonymous namespace

auto main(int argc, char const *argv[])->int try {
  boost::program_options::options_description options{"Device login options"};
  options.add_options()
    ("help,h", "Show usage")
    ("client-id", boost::program_options::value<std::string>()->required(), "Spotify client ID enabled for device authorisation")
    ("user-agent", boost::program_options::value<std::string>()->default_value("device_login"), "HTTP user agent identifying this application");
  boost::program_options::variables_map arguments;
  boost::program_options::store(boost::program_options::parse_command_line(argc, argv, options), arguments);
  if(arguments.contains("help")) {
    std::cout << options << std::endl;
    return EXIT_SUCCESS;
  }
  boost::program_options::notify(arguments);
  boost::asio::io_context executor;
  librespot::net::http_client transport{executor.get_executor(), {.user_agent{arguments.at("user-agent").as<std::string>()}}};
  librespot::oauth::device_auth auth{transport, arguments.at("client-id").as<std::string>()};
  int status{EXIT_SUCCESS};
  boost::asio::co_spawn(executor, login(auth), [&status](std::exception_ptr failure){
    if(!failure) return;
    status = EXIT_FAILURE;
    try {
      std::rethrow_exception(failure);
    } catch(std::exception const &error) {
      std::cerr << error.what() << std::endl;
    }
  });
  executor.run();
  return status;
} catch(std::exception const &error) {
  std::cerr << error.what() << std::endl;
  return EXIT_FAILURE;
}
