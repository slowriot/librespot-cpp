#include "server.h"
#include <charconv>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/ip/v6_only.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>
#include "advertisement.h"

namespace librespot::discovery {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace {

std::string decode_form(std::string_view text) {
  std::string result;
  for(std::size_t index{0}; index < text.size(); ++index) {
    if(text[index] == '+') result += ' ';
    else if(text[index] == '%') {
      if(text.size() - index < 3) throw std::invalid_argument{"truncated form escape"};
      unsigned int value{0};
      auto const [end, error]{std::from_chars(text.data() + index + 1, text.data() + index + 3, value, 16)};
      if(error != std::errc{} || end != text.data() + index + 3 || value == 0) throw std::invalid_argument{"invalid form escape"};
      result += static_cast<char>(value);
      index += 2;
    } else {
      if(text[index] == '\0') throw std::invalid_argument{"invalid form character"};
      result += text[index];
    }
  }
  return result;
}

void parse_form(std::map<std::string, std::string> &params, std::string_view text) {
  while(!text.empty()) {
    auto const separator{text.find('&')};
    auto const pair{text.substr(0, separator)};
    auto const equal{pair.find('=')};
    if(equal == std::string_view::npos) throw std::invalid_argument{"invalid pairing form"};
    if(params.size() >= 32 || !params.emplace(decode_form(pair.substr(0, equal)), decode_form(pair.substr(equal + 1))).second) throw std::invalid_argument{"duplicate or excessive pairing parameters"};
    if(separator == std::string_view::npos) break;
    text.remove_prefix(separator + 1);
  }
}

} // anonymous namespace

struct server::implementation : std::enable_shared_from_this<implementation> {
  asio::strand<asio::any_io_executor> executor;
  server_config config;
  pairing_handler handler;
  pairing protocol;
  asio::ip::tcp::acceptor acceptor;
  asio::steady_timer drained;
  std::unique_ptr<advertisement> publication;
  std::set<std::shared_ptr<beast::tcp_stream>> connections;
  bool stopped{false};
  bool running{false};
  bool pairing_busy{false};

  implementation(asio::any_io_executor executor, server_config config, pairing_handler handler)
    : executor{asio::make_strand(std::move(executor))}, config{std::move(config)}, handler{std::move(handler)},
      protocol{this->config.device}, acceptor{this->executor}, drained{this->executor} {
    if(!this->handler.add_user || !this->handler.reset_users) throw std::invalid_argument{"pairing handlers are required"};
    auto const address{asio::ip::make_address(this->config.address)};
    asio::ip::tcp::endpoint endpoint{address, this->config.port};
    acceptor.open(endpoint.protocol());
    acceptor.set_option(asio::ip::tcp::acceptor::reuse_address{true});
    if(address.is_v6()) acceptor.set_option(asio::ip::v6_only{false});
    acceptor.bind(endpoint);
    acceptor.listen(16);
    this->config.port = acceptor.local_endpoint().port();
    drained.expires_at(std::chrono::steady_clock::time_point::max());
  }

  void stop() {
    stopped = true;
    publication.reset();
    boost::system::error_code ignored;
    acceptor.close(ignored);
    for(auto const &connection : connections) connection->socket().close(ignored);
  }

  asio::awaitable<http::response<http::string_body>> respond(http::request<http::string_body> request) {
    http::response<http::string_body> response{http::status::ok, 11};
    response.set(http::field::content_type, "application/json");
    auto status = [&](int code, std::string_view message, int error){
      response.body() = nlohmann::json{{"status", code}, {"statusString", message}, {"spotifyError", error}, {"responseSource", config.device.name}}.dump();
    };
    try {
      auto const target{std::string_view{request.target()}};
      auto const query{target.find('?')};
      if(target.substr(0, query) != "/") {
        response.result(http::status::not_found);
        co_return response;
      }
      std::map<std::string, std::string> params;
      if(query != std::string_view::npos) parse_form(params, target.substr(query + 1));
      parse_form(params, request.body());
      auto const action{params.find("action")};
      if(action == params.end()) throw std::invalid_argument{"missing pairing action"};
      if(request.method() == http::verb::get && action->second == "getInfo") response.body() = protocol.get_info(config.active_user);
      else if(request.method() == http::verb::post && (action->second == "addUser" || action->second == "resetUsers")) {
        if(pairing_busy) {
          status(102, "ERROR-BUSY", 1);
          co_return response;
        }
        pairing_busy = true;
        struct cleanup {
          bool &busy;
          ~cleanup() {
            busy = false;
          }
        } guard{pairing_busy};
        if(action->second == "resetUsers") {
          co_await handler.reset_users();
          config.active_user.clear();
        } else {
          auto login{protocol.decode(params.at("userName"), params.at("blob"), params.at("clientKey"))};
          config.active_user.clear();
          auto const accepted{co_await handler.add_user(std::move(login))};
          if(!accepted.username || accepted.data.empty()) throw std::runtime_error{"pairing authentication returned no credentials"};
          config.active_user = *accepted.username;
        }
        status(101, "OK", 0);
      } else {
        response.result(http::status::not_found);
      }
    } catch(std::exception const &) {
      status(102, "ERROR-LOGIN", 1);
    }
    co_return response;
  }

  asio::awaitable<void> serve(std::shared_ptr<beast::tcp_stream> stream) {
    beast::flat_buffer buffer;
    http::request_parser<http::string_body> parser;
    parser.body_limit(65536);
    parser.header_limit(8192);
    stream->expires_after(std::chrono::seconds{30});
    co_await http::async_read(*stream, buffer, parser, asio::use_awaitable);
    auto response{co_await respond(parser.release())};
    response.keep_alive(false);
    response.prepare_payload();
    stream->expires_after(std::chrono::seconds{10});
    co_await http::async_write(*stream, response, asio::use_awaitable);
    boost::system::error_code ignored;
    stream->socket().shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    stream->socket().close(ignored);
  }

  asio::awaitable<void> run() {
    if(running || stopped) throw std::logic_error{"pairing server cannot be restarted"};
    running = true;
    if(config.advertise) publication = std::make_unique<advertisement>(executor, config.device.name, config.port, config.on_error);
    while(!stopped) {
      boost::system::error_code error;
      auto socket{co_await acceptor.async_accept(asio::redirect_error(asio::use_awaitable, error))};
      if(error) {
        if(stopped) break;
        throw boost::system::system_error{error};
      }
      if(connections.size() >= 16) continue;
      auto stream{std::make_shared<beast::tcp_stream>(std::move(socket))};
      connections.insert(stream);
      auto self{shared_from_this()};
      asio::co_spawn(executor, serve(stream), [self, stream](std::exception_ptr){
        self->connections.erase(stream);
        if(self->connections.empty()) self->drained.cancel();
      });
    }
  }

  static asio::awaitable<void> shutdown(std::shared_ptr<implementation> self) {
    self->stop();
    while(!self->connections.empty()) {
      boost::system::error_code ignored;
      co_await self->drained.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
    }
  }
};

server::server(asio::any_io_executor executor, server_config config, pairing_handler handler)
  : state{std::make_shared<implementation>(std::move(executor), std::move(config), std::move(handler))} {
}

server::~server() {
  close();
}

std::uint16_t server::port() const {
  return state->config.port;
}

asio::awaitable<void> server::run() {
  auto shared{state};
  return asio::co_spawn(shared->executor, [shared]()->asio::awaitable<void> {
    co_await shared->run();
  }, asio::use_awaitable);
}

void server::close() {
  if(state) asio::dispatch(state->executor, [shared{state}]{
    shared->stop();
  });
}

asio::awaitable<void> server::shutdown() {
  return asio::co_spawn(state->executor, implementation::shutdown(state), asio::use_awaitable);
}

void server::set_active_user(std::string username) {
  asio::dispatch(state->executor, [shared{state}, username{std::move(username)}]() mutable {
    shared->config.active_user = std::move(username);
  });
}

} // namespace librespot::discovery
