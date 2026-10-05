#include "session.h"
#include <algorithm>
#include <limits>
#include <map>
#include <stdexcept>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/experimental/parallel_group.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>

namespace librespot {
namespace asio = boost::asio;

namespace {

template<typename T>
using reply_channel = asio::experimental::channel<asio::any_io_executor, void(boost::system::error_code, T)>;

template<typename T>
asio::awaitable<T> timed_receive(reply_channel<T> &channel, std::chrono::seconds timeout) {
  /// Cancel the losing wait without hiding transport or cancellation errors
  asio::steady_timer deadline{co_await asio::this_coro::executor, timeout};
  auto [order, error, value, timer_error]{co_await asio::experimental::make_parallel_group(
    channel.async_receive(asio::deferred), deadline.async_wait(asio::deferred)
  ).async_wait(asio::experimental::wait_for_one(), asio::use_awaitable)};
  if(order[0] == 1) throw boost::system::system_error{timer_error ? timer_error : asio::error::timed_out};
  if(error) throw boost::system::system_error{error};
  co_return value;
}

std::uint64_t sequence(std::span<std::byte const> bytes) {
  std::uint64_t result{0};
  for(auto byte : bytes) result = (result << 8) | std::to_integer<std::uint64_t>(byte);
  return result;
}

} // anonymous namespace

struct session::implementation {
  asio::strand<asio::any_io_executor> executor;
  net::http_transport &transport;
  session_config config;
  std::shared_ptr<net::access_point_transport> connection;
  reply_channel<net::packet> outgoing;
  asio::steady_timer pong_timer;
  bool active{false};
  bool connecting{false};
  bool stopped{false};
  std::uint64_t next_mercury{0};
  std::uint64_t next_key{0};
  std::map<std::uint32_t, std::shared_ptr<reply_channel<audio::audio_key>>> keys;

  struct mercury_pending {
    net::mercury_assembler assembler;
    reply_channel<net::mercury_response> reply;
    explicit mercury_pending(asio::any_io_executor executor) : reply{executor, 1} {
    }
  };

  std::map<std::uint64_t, std::shared_ptr<mercury_pending>> mercury;
  std::map<std::string, std::vector<oauth::access_token>> tokens;

  implementation(asio::any_io_executor executor, net::http_transport &transport, session_config config,
    std::shared_ptr<net::access_point_transport> connection)
    : executor{asio::make_strand(executor)},
      transport{transport},
      config{std::move(config)},
      connection{connection ? std::move(connection) : std::make_shared<net::access_point>(this->executor, this->config.io_timeout, this->config.on_log)},
      outgoing{this->executor, 64},
      pong_timer{this->executor} {
    if(this->config.device_id.empty() || this->config.request_timeout.count() <= 0) throw std::invalid_argument{"invalid session configuration"};
    pong_timer.expires_at(std::chrono::steady_clock::time_point::max());
  }

  void stop() {
    /// Unblock all waiters and stop both packet pumps on the session strand
    active = false;
    stopped = true;
    tokens.clear();
    connection->close();
    outgoing.close();
    outgoing.cancel();
    pong_timer.cancel();
    for(auto const &[sequence, channel] : keys) channel->cancel();
    for(auto const &[sequence, pending] : mercury) pending->reply.cancel();
  }

  void enqueue(net::packet packet) {
    /// Expose bounded backpressure instead of growing an unlimited send queue
    if(!active) throw std::logic_error{"session is not connected"};
    if(!outgoing.try_send(boost::system::error_code{}, std::move(packet))) throw std::runtime_error{"session send queue is full"};
  }

  static asio::awaitable<void> write_packets(std::shared_ptr<implementation> self) {
    while(self->active) {
      auto packet{co_await self->outgoing.async_receive(asio::use_awaitable)};
      if(!self->active) co_return;
      co_await self->connection->send(std::move(packet));
    }
  }

  static asio::awaitable<void> keepalive(std::shared_ptr<implementation> self) {
    /// Match upstream's delayed four-zero-byte pong while keeping dispatch responsive
    while(self->active) {
      boost::system::error_code error;
      co_await self->pong_timer.async_wait(asio::redirect_error(asio::use_awaitable, error));
      if(!self->active) co_return;
      if(error == asio::error::operation_aborted) continue;
      if(error) throw boost::system::system_error{error};
      self->pong_timer.expires_at(std::chrono::steady_clock::time_point::max());
      self->enqueue({.command{0x49}, .payload{std::vector<std::byte>(4)}});
    }
  }

  static asio::awaitable<void> read_packets(std::shared_ptr<implementation> self) {
    /// Match request sequences and process service events independently of application calls
    while(self->active) {
      auto packet{co_await self->connection->receive()};
      if(!self->active) co_return;
      if(packet.command == 0x04) {
        if(packet.payload.size() != 4) throw std::runtime_error{"invalid access-point ping"};
        self->pong_timer.expires_after(std::chrono::seconds{60});
      } else if(packet.command == 0x0d || packet.command == 0x0e) {
        if(packet.payload.size() < 4) throw std::runtime_error{"truncated audio key response"};
        auto const id{static_cast<std::uint32_t>(sequence(std::span{packet.payload}.first<4>()))};
        if(auto const found{self->keys.find(id)}; found != self->keys.end()) {
          if(packet.command == 0x0d) {
            if(packet.payload.size() != 20) throw std::runtime_error{"invalid audio key response length"};
            audio::audio_key key;
            std::ranges::copy(std::span{packet.payload}.subspan<4, 16>(), key.begin());
            found->second->try_send(boost::system::error_code{}, key);
          } else {
            found->second->try_send(asio::error::access_denied, audio::audio_key{});
          }
          self->keys.erase(found);
        }
      } else if(packet.command >= 0xb2 && packet.command <= 0xb5) {
        auto frame{net::decode_mercury(packet.payload)};
        if(frame.sequence.size() == 8 && packet.command != 0xb5) {
          if(auto const found{self->mercury.find(sequence(frame.sequence))}; found != self->mercury.end()) {
            if(auto response{found->second->assembler.append(std::move(frame))}) {
              found->second->reply.try_send(boost::system::error_code{}, std::move(*response));
              self->mercury.erase(found);
            }
          }
        }
      }
      if(self->config.on_packet) self->config.on_packet(packet);
    }
  }

  static asio::awaitable<credentials> connect(std::shared_ptr<implementation> self, credentials login) {
    /// Try each resolved access point and start the pumps after successful authentication
    if(self->connecting || self->active) throw std::logic_error{"session already connecting or connected"};
    if(self->stopped) throw std::logic_error{"closed sessions must be recreated"};
    self->connecting = true;
    struct reset_flag {
      bool &flag;
      ~reset_flag() {
        flag = false;
      }
    } reset{self->connecting};
    emit_log(self->config.on_log, log_level::debug, "session", "Resolving access-point endpoints");
    auto const addresses{co_await net::resolve_access_points(self->transport)};
    if(self->stopped) throw boost::system::system_error{asio::error::operation_aborted};
    std::exception_ptr failure;
    for(auto const &address : addresses) {
      try {
        emit_log(self->config.on_log, log_level::debug, "session", "Connecting to access point " + address.host + ':' + address.port);
        auto reusable{co_await self->connection->connect(address, login, self->config.device_id)};
        if(login.type == authentication_type::spotify_token) {
          /// Match upstream: token login supplies credentials for a reusable session
          if(self->stopped) throw boost::system::system_error{asio::error::operation_aborted};
          self->connection->close();
          reusable = co_await self->connection->connect(address, reusable, self->config.device_id);
        }
        if(self->stopped) {
          self->connection->close();
          throw boost::system::system_error{asio::error::operation_aborted};
        }
        self->outgoing.reset();
        self->active = true;
        auto finish{[self](std::exception_ptr failure){
          if(failure) {
            if(!self->stopped) {
              try {
                std::rethrow_exception(failure);
              } catch(std::exception const &error) {
                emit_log(self->config.on_log, log_level::error, "session", "Access-point session task failed: " + diagnostic_error(error));
              }
            }
            self->stop();
          }
        }};
        asio::co_spawn(self->executor, write_packets(self), finish);
        asio::co_spawn(self->executor, read_packets(self), finish);
        asio::co_spawn(self->executor, keepalive(self), finish);
        co_return reusable;
      } catch(boost::system::system_error const &error) {
        if(self->active) {
          self->stop();
          throw;
        }
        if(error.code() == asio::error::operation_aborted) throw;
        emit_log(self->config.on_log, log_level::warning, "session", "Access point " + address.host + ':' + address.port + " failed: " + diagnostic_error(error));
        failure = std::current_exception();
      } catch(std::exception const &error) {
        if(self->active) {
          self->stop();
          throw;
        }
        emit_log(self->config.on_log, log_level::warning, "session", "Access point " + address.host + ':' + address.port + " failed: " + diagnostic_error(error));
        failure = std::current_exception();
      } catch(...) {
        if(self->active) {
          self->stop();
          throw;
        }
        failure = std::current_exception();
      }
      if(self->stopped) throw boost::system::system_error{asio::error::operation_aborted};
    }
    std::rethrow_exception(failure);
  }

  static asio::awaitable<audio::audio_key> request_key(std::shared_ptr<implementation> self, spotify_id track, file_id file) {
    /// Register the reply before enqueueing the request to avoid a fast-response race
    if(self->next_key > std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error{"audio key sequence exhausted"};
    if(self->keys.size() >= 128) throw std::runtime_error{"too many pending audio key requests"};
    auto const id{static_cast<std::uint32_t>(self->next_key++)};
    auto channel{std::make_shared<reply_channel<audio::audio_key>>(self->executor, 1)};
    self->keys.emplace(id, channel);
    try {
      net::packet packet{.command{0x0c}, .payload{}};
      packet.payload.insert(packet.payload.end(), file.bytes().begin(), file.bytes().end());
      packet.payload.insert(packet.payload.end(), track.bytes().begin(), track.bytes().end());
      for(unsigned int index{0}; index < 4; ++index) packet.payload.push_back(static_cast<std::byte>((id >> ((3 - index) * 8)) & 255u));
      packet.payload.insert(packet.payload.end(), 2, std::byte{0});
      self->enqueue(std::move(packet));
      auto key{co_await timed_receive<audio::audio_key>(*channel, self->config.request_timeout)};
      self->keys.erase(id);
      co_return key;
    } catch(...) {
      self->keys.erase(id);
      throw;
    }
  }

  static asio::awaitable<net::mercury_response> request(std::shared_ptr<implementation> self, net::mercury_request request) {
    if(self->next_mercury == std::numeric_limits<std::uint64_t>::max()) throw std::runtime_error{"Mercury sequence exhausted"};
    if(self->mercury.size() >= 128) throw std::runtime_error{"too many pending Mercury requests"};
    auto const id{self->next_mercury++};
    auto pending{std::make_shared<mercury_pending>(self->executor)};
    self->mercury.emplace(id, pending);
    try {
      self->enqueue(net::encode_mercury(request, id));
      auto response{co_await timed_receive<net::mercury_response>(pending->reply, self->config.request_timeout)};
      self->mercury.erase(id);
      co_return response;
    } catch(...) {
      self->mercury.erase(id);
      throw;
    }
  }

  static asio::awaitable<oauth::access_token> request_token(std::shared_ptr<implementation> self,
    std::string client_id, std::vector<std::string> scopes) {
    if(!self->active) throw std::logic_error{"session is not connected"};
    if(client_id.empty() || scopes.empty()) throw std::invalid_argument{"client ID and token scopes are required"};
    std::ranges::sort(scopes);
    scopes.erase(std::unique(scopes.begin(), scopes.end()), scopes.end());
    std::string joined;
    for(auto const &scope : scopes) {
      if(scope.empty() || scope.find_first_of(", \t\r\n") != std::string::npos) throw std::invalid_argument{"invalid token scope"};
      if(!joined.empty()) joined += ',';
      joined += scope;
    }
    /// Keep client identities separate even when their requested scopes overlap
    auto found{self->tokens.find(client_id)};
    if(found != self->tokens.end()) {
      std::erase_if(found->second, [](oauth::access_token const &token){
        return token.expired();
      });
      for(auto const &token : found->second) if(token.covers(scopes)) co_return token;
    }
    auto response{co_await request(self, {
      .method{net::mercury_method::get},
      .uri{"hm://keymaster/token/authenticated?scope=" + net::form_encode(joined)
        + "&client_id=" + net::form_encode(client_id) + "&device_id=" + net::form_encode(self->config.device_id)},
      .content_type{},
      .payload{},
    })};
    if(response.status != 200 || response.payload.size() != 1) throw std::runtime_error{"service token request failed"};
    auto const &payload{response.payload.front()};
    auto token{oauth::decode_access_token({reinterpret_cast<char const *>(payload.data()), payload.size()})};
    if(!token.covers(scopes)) throw std::runtime_error{"service token is missing requested scopes"};
    /// Bound cached credentials across clients and scope combinations
    if(self->tokens.size() >= 32 && !self->tokens.contains(client_id)) self->tokens.erase(self->tokens.begin());
    auto &cached{self->tokens[client_id]};
    if(cached.size() >= 32) cached.erase(cached.begin());
    cached.push_back(token);
    co_return token;
  }
};

session::session(asio::any_io_executor executor, net::http_transport &transport, session_config config,
  std::shared_ptr<net::access_point_transport> connection)
  : state{std::make_shared<implementation>(std::move(executor), transport, std::move(config), std::move(connection))} {
  /// Own an internal strand so independent requests can share a connection
}

session::~session() {
  close();
}

asio::awaitable<credentials> session::connect(credentials login) {
  return asio::co_spawn(state->executor, implementation::connect(state, std::move(login)), asio::use_awaitable);
}

asio::awaitable<audio::audio_key> session::request_audio_key(spotify_id track, file_id file) {
  return asio::co_spawn(state->executor, implementation::request_key(state, track, file), asio::use_awaitable);
}

asio::awaitable<net::mercury_response> session::request(net::mercury_request request) {
  return asio::co_spawn(state->executor, implementation::request(state, std::move(request)), asio::use_awaitable);
}

asio::awaitable<oauth::access_token> session::request_token(std::string client_id, std::vector<std::string> scopes) {
  return asio::co_spawn(state->executor, implementation::request_token(state, std::move(client_id), std::move(scopes)), asio::use_awaitable);
}

void session::close() {
  /// Dispatch cancellation to the strand rather than racing a packet handler
  asio::dispatch(state->executor, [state{state}]{
    state->stop();
  });
}

} // namespace librespot
