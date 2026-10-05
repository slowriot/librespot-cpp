#include <catch2/catch_test_macros.hpp>
#include <array>
#include <cstdlib>
#include <deque>
#include <future>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <nlohmann/json.hpp>
#include <zlib.h>
#include "connect.pb.h"
#include "librespot/connect/receiver.h"
#include "librespot/encoding/base64.h"
#include "librespot/encoding/protocol.h"
#include "spotify/clienttoken/v0/clienttoken_http.pb.h"
#include "spotify/login5/v3/login5.pb.h"
#include "transfer_state.pb.h"

namespace {
namespace asio = boost::asio;
namespace proto = spotify::connectstate;

std::string gzip(std::string const &input) {
  z_stream stream{};
  REQUIRE(deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK);
  std::string result(compressBound(input.size()) + 32, '\0');
  stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.data()));
  stream.avail_in = static_cast<uInt>(input.size());
  stream.next_out = reinterpret_cast<Bytef *>(result.data());
  stream.avail_out = static_cast<uInt>(result.size());
  auto const status{deflate(&stream, Z_FINISH)};
  result.resize(stream.total_out);
  deflateEnd(&stream);
  REQUIRE(status == Z_STREAM_END);
  return result;
}

class fake_http : public librespot::net::http_transport {
public:
  std::vector<proto::PutStateRequest> registrations;
  std::vector<std::string> connection_ids;
  unsigned int deletes{0};
  unsigned int tokens{0};
  bool reject_registration{false};
  std::string registration_error{R"({"error":"bad device","access_token":"must-not-be-logged"})"};

  asio::awaitable<librespot::net::http_response> request(librespot::net::http_request request) override {
    if(request.host == "apresolve.spotify.com") {
      if(request.target == "/?type=dealer") co_return librespot::net::http_response{200, {}, R"({"dealer":["dealer.example:443"]})"};
      co_return librespot::net::http_response{200, {}, R"({"spclient":["spclient.example:443"]})"};
    }
    if(request.host == "clienttoken.spotify.com") {
      spotify::clienttoken::http::v0::ClientTokenResponse response;
      response.set_response_type(spotify::clienttoken::http::v0::RESPONSE_GRANTED_TOKEN_RESPONSE);
      response.mutable_granted_token()->set_token("client-token");
      response.mutable_granted_token()->set_expires_after_seconds(3600);
      response.mutable_granted_token()->set_refresh_after_seconds(1800);
      co_return librespot::net::http_response{200, {}, response.SerializeAsString()};
    }
    if(request.host == "login5.spotify.com") {
      ++tokens;
      spotify::login5::v3::LoginResponse response;
      response.mutable_ok()->set_access_token("access-" + std::to_string(tokens));
      response.mutable_ok()->set_access_token_expires_in(3600);
      co_return librespot::net::http_response{200, {}, response.SerializeAsString()};
    }
    REQUIRE(request.host == "spclient.example");
    CHECK(request.target == "/connect-state/v1/devices/device");
    CHECK(request.headers.at("client-token") == "client-token");
    if(request.method == "DELETE") {
      ++deletes;
      co_return librespot::net::http_response{204, {}, {}};
    }
    CHECK(request.method == "PUT");
    if(reject_registration) co_return librespot::net::http_response{400, {{"Content-Type", "application/json"}}, registration_error};
    proto::PutStateRequest state;
    REQUIRE(state.ParseFromString(request.body));
    CHECK_FALSE(request.headers.contains("Spotify-Connection-Id"));
    connection_ids.push_back(request.headers.at("X-Spotify-Connection-Id"));
    registrations.push_back(std::move(state));
    proto::Cluster cluster;
    auto const &published{registrations.back()};
    cluster.set_active_device_id(published.is_active() ? "device" : "controller");
    *cluster.mutable_player_state() = published.device().player_state();
    co_return librespot::net::http_response{200, {}, cluster.SerializeAsString()};
  }
};

class fake_dealer : public librespot::net::dealer_transport {
public:
  asio::experimental::channel<asio::any_io_executor, void(boost::system::error_code, std::string)> incoming;
  std::vector<nlohmann::json> replies;
  std::vector<std::string> tokens;
  std::function<void()> completed;
  std::function<void(unsigned int)> on_connect;
  unsigned int connections{0};
  bool drop_once{false};

  explicit fake_dealer(asio::any_io_executor executor) : incoming{executor, 64} {
  }
  void push(nlohmann::json message) {
    REQUIRE(incoming.try_send(boost::system::error_code{}, message.dump()));
  }
  asio::awaitable<void> connect(librespot::net::endpoint address, std::string token) override {
    CHECK(address.host == "dealer.example");
    CHECK(address.port == "443");
    tokens.push_back(std::move(token));
    incoming.reset();
    ++connections;
    if(on_connect) on_connect(connections);
    co_return;
  }
  asio::awaitable<std::string> receive() override {
    if(drop_once && connections == 1) throw std::runtime_error{"simulated Dealer loss"};
    co_return co_await incoming.async_receive(asio::use_awaitable);
  }
  asio::awaitable<void> send(std::string message) override {
    replies.push_back(nlohmann::json::parse(message));
    if(completed) completed();
    co_return;
  }
  void close() override {
    incoming.cancel();
  }
};

nlohmann::json request(std::string const &key, nlohmann::json command, bool compressed = false) {
  auto const body{nlohmann::json{{"message_id", 42}, {"sent_by_device_id", "controller"}, {"command", command}}.dump()};
  return {{"type", "request"}, {"key", key}, {"message_ident", "hm://connect-state/v1/player/command"},
    {"headers", compressed ? nlohmann::json{{"Transfer-Encoding", "gzip"}} : nlohmann::json::object()},
    {"payload", {{"compressed", base64::encode(compressed ? gzip(body) : body)}}}};
}

nlohmann::json connection(std::string const &id) {
  return {{"type", "message"}, {"uri", "hm://pusher/v1/connections/device"}, {"headers", {{"Spotify-Connection-Id", id}}}};
}

} // anonymous namespace

TEST_CASE("Connect registers, accepts compressed controls, rejects unsupported commands, and withdraws") {
  asio::io_context executor;
  asio::thread_pool worker{1};
  fake_http http;
  librespot::oauth::service_auth auth{http, {.device_id{"device"}},
    {.username{"username"}, .type{librespot::authentication_type::stored_spotify}, .data{"stored"}}, worker.get_executor()};
  auto dealer{std::make_shared<fake_dealer>(executor.get_executor())};
  unsigned int commands{0};
  librespot::connect::receiver receiver{executor.get_executor(), http, auth,
    {.device{.id{"device"}, .name{"owning-program"}, .brand{"brand"}, .model{"model"}}, .reconnect{false}},
    [&](librespot::connect::command const &command, librespot::connect::player_state &state)->asio::awaitable<bool> {
      ++commands;
      if(command.type == librespot::connect::command_type::unknown) {
        state.volume = 1;
        co_return false;
      }
      co_return librespot::connect::apply_control(command, state);
    }, dealer};
  dealer->on_connect = [&](unsigned int){
    dealer->push(connection("connection-one"));
    dealer->push(request("play", {{"endpoint", "play"}, {"context", {{"uri", "spotify:track:0123456789012345678901"}}}, {"options", nlohmann::json::object()}}));
    dealer->push(request("seek", {{"endpoint", "seek_to"}, {"value", 1234}}, true));
    dealer->push(request("unsupported", {{"endpoint", "future_command"}}));
    proto::SetVolumeCommand volume;
    volume.set_volume(12345);
    dealer->push({{"type", "message"}, {"uri", "hm://connect-state/v1/connect/volume"}, {"payloads", {base64::encode(volume.SerializeAsString())}}});
    dealer->push(request("pause", {{"endpoint", "pause"}}));
    dealer->push(request("bad", {{"endpoint", "seek_to"}, {"value", -1}}));
  };
  dealer->completed = [&]{
    if(dealer->replies.size() == 5) receiver.close();
  };
  auto running{asio::co_spawn(executor, receiver.run(), asio::use_future)};
  executor.run();
  running.get();
  worker.join();
  REQUIRE(dealer->replies.size() == 5);
  CHECK(dealer->replies[0].at("payload").at("success") == true);
  CHECK(dealer->replies[1].at("payload").at("success") == true);
  CHECK(dealer->replies[2].at("payload").at("success") == false);
  CHECK(dealer->replies[3].at("payload").at("success") == true);
  CHECK(dealer->replies[4].at("payload").at("success") == false);
  CHECK(commands == 5);
  CHECK(http.deletes == 1);
  REQUIRE(http.registrations.size() == 5);
  auto const &initial{http.registrations.front()};
  CHECK(initial.member_type() == proto::CONNECT_STATE);
  CHECK(initial.put_state_reason() == proto::NEW_DEVICE);
  CHECK(initial.device().device_info().name() == "owning-program");
  CHECK(initial.device().device_info().brand() == "brand");
  CHECK(initial.device().player_state().session_id().size() == 32);
  CHECK(initial.device().player_state().session_id() != "device");
  CHECK(initial.device().device_info().capabilities().supports_command_request());
  CHECK_FALSE(initial.device().device_info().capabilities().supports_logout());
  auto const &last{http.registrations.back()};
  CHECK(last.device().device_info().volume() == 12345);
  CHECK(last.device().player_state().position_as_of_timestamp() == 1234);
  CHECK(last.device().player_state().is_paused());
  CHECK(last.last_command_message_id() == 42);
  CHECK(last.last_command_sent_by_device_id() == "controller");
  CHECK(last.is_active());
  CHECK(initial.started_playing_at() == 0);
  CHECK(http.registrations.at(1).started_playing_at() > 0);
  CHECK(last.started_playing_at() == http.registrations.at(1).started_playing_at());
  CHECK(last.device().player_state().context_url() == "context://spotify:track:0123456789012345678901");
  CHECK(last.device().player_state().is_playing());
  for(auto const &id : http.connection_ids) CHECK(id == "connection-one");
}

TEST_CASE("Connect reconnects with a refreshed token and accepts a new Dealer connection ID") {
  asio::io_context executor;
  asio::thread_pool worker{1};
  fake_http http;
  librespot::oauth::service_auth auth{http, {.device_id{"device"}},
    {.username{"username"}, .type{librespot::authentication_type::stored_spotify}, .data{"stored"}}, worker.get_executor()};
  auto dealer{std::make_shared<fake_dealer>(executor.get_executor())};
  dealer->drop_once = true;
  librespot::connect::receiver receiver{executor.get_executor(), http, auth, {.device{.id{"device"}, .name{"test"}}},
    [](librespot::connect::command const &request, librespot::connect::player_state &state)->asio::awaitable<bool> {
      co_return librespot::connect::apply_control(request, state);
    }, dealer};
  dealer->on_connect = [&](unsigned int attempt){
    if(attempt == 2) {
      dealer->push(connection("connection-two"));
      dealer->push(request("pause", {{"endpoint", "pause"}}));
    }
  };
  dealer->completed = [&]{
    receiver.close();
  };
  auto running{asio::co_spawn(executor, receiver.run(), asio::use_future)};
  executor.run();
  running.get();
  worker.join();
  CHECK(dealer->connections == 2);
  CHECK(dealer->tokens == std::vector<std::string>{"access-1", "access-2"});
  REQUIRE(http.connection_ids.size() == 2);
  CHECK(http.connection_ids.front() == "connection-two");
  CHECK(http.deletes == 1);
}

TEST_CASE("Connect handoff publishes active ownership before acknowledging the transfer") {
  bool paused{false};
  SECTION("Paused playback remains paused after handoff") {
    paused = true;
  }
  SECTION("Playing playback remains playing after handoff") {
    paused = false;
  }
  asio::io_context executor;
  asio::thread_pool worker{1};
  fake_http http;
  librespot::oauth::service_auth auth{http, {.device_id{"device"}},
    {.username{"username"}, .type{librespot::authentication_type::stored_spotify}, .data{"stored"}}, worker.get_executor()};
  auto dealer{std::make_shared<fake_dealer>(executor.get_executor())};
  std::string diagnostics;
  librespot::connect::receiver receiver{executor.get_executor(), http, auth,
    {.device{.id{"device"}, .name{"test"}}, .reconnect{false}, .on_log{[&](librespot::log_event const &event){
      diagnostics += event.message + '\n';
    }}},
    [](librespot::connect::command const &command, librespot::connect::player_state &state)->asio::awaitable<bool> {
      co_return librespot::connect::apply_control(command, state);
    }, dealer};
  spotify::player::proto::transfer::TransferState transfer;
  transfer.mutable_playback()->set_is_paused(paused);
  transfer.mutable_playback()->mutable_current_track()->set_gid(std::string{"\xb3\x9f\xe8\x08\x1e\x1f\x4c\x54\xbe\x38\xe8\xd6\xf9\xf1\x2b\xb9", 16});
  transfer.mutable_playback()->set_position_as_of_timestamp(1234);
  transfer.mutable_current_session()->mutable_context()->set_uri("spotify:playlist:context");
  dealer->on_connect = [&](unsigned int){
    dealer->push(connection("connection-id"));
    dealer->push(request("handoff", {{"endpoint", "transfer"}, {"data", base64::encode(transfer.SerializeAsString())}}, true));
  };
  dealer->completed = [&]{
    REQUIRE(http.registrations.size() == 2);
    auto const &published{http.registrations.back()};
    CHECK(published.is_active());
    CHECK(published.started_playing_at() > 0);
    CHECK(published.started_playing_at() <= published.client_side_timestamp());
    CHECK(published.last_command_message_id() == 42);
    CHECK(published.last_command_sent_by_device_id() == "controller");
    CHECK(published.device().player_state().context_url() == "context://spotify:playlist:context");
    CHECK(published.device().player_state().track().uri() == "spotify:track:5sWHDYs0csV6RS48xBl0tH");
    CHECK(published.device().player_state().position_as_of_timestamp() == 1234);
    CHECK(published.device().player_state().is_playing());
    CHECK(published.device().player_state().is_paused() == paused);
    CHECK(published.device().player_state().is_buffering() == paused);
    CHECK(published.device().player_state().playback_speed() == (paused ? 0.0 : 1.0));
    CHECK(dealer->replies.back().at("payload").at("success") == true);
    receiver.close();
  };
  auto running{asio::co_spawn(executor, receiver.run(), asio::use_future)};
  executor.run();
  running.get();
  worker.join();
  CHECK(dealer->replies.size() == 1);
  CHECK(http.deletes == 1);
  CHECK(diagnostics.find("Connect response active_device=device; this_device_active=true") != std::string::npos);
}

TEST_CASE("Connect publishes playback progress while the Dealer connection is idle") {
  asio::io_context executor;
  asio::thread_pool worker{1};
  fake_http http;
  librespot::oauth::service_auth auth{http, {.device_id{"device"}},
    {.username{"username"}, .type{librespot::authentication_type::stored_spotify}, .data{"stored"}}, worker.get_executor()};
  auto dealer{std::make_shared<fake_dealer>(executor.get_executor())};
  librespot::connect::receiver receiver{executor.get_executor(), http, auth,
    {.device{.id{"device"}, .name{"test"}}, .reconnect{false}, .on_state{[](librespot::connect::player_state &state){
      if(state.active && !state.paused) state.position += std::chrono::milliseconds{100};
    }}, .state_interval{std::chrono::milliseconds{100}}},
    [](librespot::connect::command const &command, librespot::connect::player_state &state)->asio::awaitable<bool> {
      co_return librespot::connect::apply_control(command, state);
    }, dealer};
  dealer->on_connect = [&](unsigned int){
    dealer->push(connection("connection-id"));
    dealer->push(request("play", {{"endpoint", "play"}, {"track", {{"uri", "spotify:track:current"}}}}));
    proto::ClusterUpdate old_cluster;
    old_cluster.mutable_cluster()->set_changed_timestamp_ms(1);
    old_cluster.mutable_cluster()->set_active_device_id("previous-device");
    dealer->push({{"type", "message"}, {"uri", "hm://connect-state/v1/cluster"}, {"payloads", {base64::encode(old_cluster.SerializeAsString())}}});
  };
  auto running{asio::co_spawn(executor, receiver.run(), asio::use_future)};
  auto finish{asio::co_spawn(executor, [&]()->asio::awaitable<void> {
    asio::steady_timer timer{executor};
    timer.expires_after(std::chrono::milliseconds{450});
    co_await timer.async_wait(asio::use_awaitable);
    receiver.close();
  }, asio::use_future)};
  executor.run();
  finish.get();
  running.get();
  worker.join();
  REQUIRE(http.registrations.size() >= 4);
  CHECK(http.registrations.back().device().player_state().position_as_of_timestamp() >= 200);
  CHECK(dealer->replies.size() == 1);
  CHECK(http.deletes == 1);
}

TEST_CASE("Connect transfer resolves binary track IDs and selects the current queued track") {
  spotify::player::proto::transfer::TransferState transfer;
  auto *current{transfer.mutable_playback()->mutable_current_track()};
  current->set_gid(std::string{"\xb3\x9f\xe8\x08\x1e\x1f\x4c\x54\xbe\x38\xe8\xd6\xf9\xf1\x2b\xb9", 16});
  current->set_uid("current-uid");
  auto *queued{transfer.mutable_queue()->add_tracks()};
  queued->set_gid(std::string(16, '\0'));
  queued->set_uid("queued-uid");
  transfer.mutable_queue()->add_tracks()->set_uri("spotify:track:next");
  auto decode{[&]{
    return librespot::connect::decode_command(nlohmann::json{{"message_id", 7}, {"sent_by_device_id", "controller"},
      {"command", {{"endpoint", "transfer"}, {"data", base64::encode(transfer.SerializeAsString())}}}}.dump());
  }};
  SECTION("Binary GIDs supply URIs for the current track and queue") {
    auto const command{decode()};
    REQUIRE(command.transferred_state);
    CHECK(command.transferred_state->track.uri == "spotify:track:5sWHDYs0csV6RS48xBl0tH");
    CHECK(command.transferred_state->track.uid == "current-uid");
    REQUIRE(command.transferred_state->next_tracks.size() == 2);
    CHECK(command.transferred_state->next_tracks.front().uri == "spotify:track:0000000000000000000000");
  }
  SECTION("A playing queue supplies the current track without duplicating it") {
    transfer.mutable_queue()->set_is_playing_queue(true);
    auto const command{decode()};
    REQUIRE(command.transferred_state);
    CHECK(command.transferred_state->track.uri == "spotify:track:0000000000000000000000");
    CHECK(command.transferred_state->track.uid == "queued-uid");
    REQUIRE(command.transferred_state->next_tracks.size() == 1);
    CHECK(command.transferred_state->next_tracks.front().uri == "spotify:track:next");
  }
  SECTION("An explicit URI takes precedence over a binary GID") {
    current->set_uri("spotify:track:explicit");
    CHECK(decode().transferred_state->track.uri == "spotify:track:explicit");
  }
  SECTION("Invalid GID length is rejected") {
    current->set_gid("short");
    CHECK_THROWS(decode());
  }
  SECTION("A track without either identifier is rejected") {
    current->clear_gid();
    CHECK_THROWS(decode());
  }
  SECTION("A playing queue without a current track is rejected") {
    transfer.mutable_queue()->set_is_playing_queue(true);
    transfer.mutable_queue()->clear_tracks();
    CHECK_THROWS(decode());
  }
}

TEST_CASE("Connect transfer decodes protobuf state, options and queues without an audio backend") {
  spotify::player::proto::transfer::TransferState transfer;
  transfer.mutable_playback()->set_is_paused(true);
  transfer.mutable_playback()->set_position_as_of_timestamp(4321);
  transfer.mutable_playback()->mutable_current_track()->set_uri("spotify:track:current");
  transfer.mutable_current_session()->mutable_context()->set_uri("spotify:playlist:list");
  transfer.mutable_options()->set_shuffling_context(true);
  transfer.mutable_queue()->add_tracks()->set_uri("spotify:track:next");
  auto const json{nlohmann::json{{"message_id", 7}, {"sent_by_device_id", "controller"},
    {"command", {{"endpoint", "transfer"}, {"data", base64::encode(transfer.SerializeAsString())}}}}.dump()};
  auto const command{librespot::connect::decode_command(json)};
  librespot::connect::player_state player;
  player.volume = 123;
  CHECK(librespot::connect::apply_control(command, player));
  CHECK(player.active);
  CHECK(player.paused);
  CHECK(player.position.count() == 4321);
  CHECK(player.context_uri == "spotify:playlist:list");
  CHECK(player.shuffle);
  CHECK(player.volume == 123);
  REQUIRE(player.next_tracks.size() == 1);
  librespot::connect::command next;
  next.type = librespot::connect::command_type::next;
  CHECK(librespot::connect::apply_control(next, player));
  CHECK(player.track.uri == "spotify:track:next");
  CHECK(player.position.count() == 0);
  CHECK(player.previous_tracks.front().uri == "spotify:track:current");
}

TEST_CASE("Dealer gzip decoding rejects truncation, trailing data, and expansion beyond the limit") {
  auto const compressed{gzip("protocol payload")};
  CHECK(librespot::encoding::inflate_gzip(compressed) == "protocol payload");
  CHECK_THROWS(librespot::encoding::inflate_gzip(compressed.substr(0, compressed.size() - 1)));
  CHECK_THROWS(librespot::encoding::inflate_gzip(compressed + 'x'));
  CHECK_THROWS(librespot::encoding::inflate_gzip(gzip(std::string(16 * 1024 * 1024 + 1, 'x'))));
}

TEST_CASE("Connect shutdown cancels an idle read and withdraws registration before returning") {
  asio::io_context executor;
  asio::thread_pool worker{1};
  fake_http http;
  librespot::oauth::service_auth auth{http, {.device_id{"device"}},
    {.username{"username"}, .type{librespot::authentication_type::stored_spotify}, .data{"stored"}}, worker.get_executor()};
  auto dealer{std::make_shared<fake_dealer>(executor.get_executor())};
  std::unique_ptr<librespot::connect::receiver> receiver;
  bool shutdown_finished{false};
  std::exception_ptr shutdown_error;
  receiver = std::make_unique<librespot::connect::receiver>(executor.get_executor(), http, auth,
    librespot::connect::receiver_config{.device{.id{"device"}, .name{"test"}}, .on_status{[&](std::string status){
      if(status == "Connect device registered") asio::co_spawn(executor, receiver->shutdown(), [&](std::exception_ptr failure){
        shutdown_finished = true;
        shutdown_error = failure;
        CHECK(http.deletes == 1);
      });
    }}}, [](librespot::connect::command const &, librespot::connect::player_state &)->asio::awaitable<bool> {
      co_return false;
    }, dealer);
  dealer->on_connect = [&](unsigned int){
    dealer->push(connection("connection-id"));
  };
  auto running{asio::co_spawn(executor, receiver->run(), asio::use_future)};
  executor.run();
  running.get();
  if(shutdown_error) std::rethrow_exception(shutdown_error);
  worker.join();
  CHECK(shutdown_finished);
  CHECK(http.registrations.size() == 1);
  CHECK(http.deletes == 1);
}

TEST_CASE("Dealer rejects a trusted certificate for the wrong hostname and releases handshake resources") {
  struct trust {
    std::optional<std::string> previous;
    trust() {
      if(auto const value{std::getenv("SSL_CERT_FILE")}) previous = value;
      REQUIRE(::setenv("SSL_CERT_FILE", LIBRESPOT_TEST_FIXTURES "/localhost.crt", 1) == 0);
    }
    ~trust() {
      if(previous) ::setenv("SSL_CERT_FILE", previous->c_str(), 1);
      else ::unsetenv("SSL_CERT_FILE");
    }
  } environment;
  asio::io_context executor;
  asio::ip::tcp::acceptor acceptor{executor, {asio::ip::make_address("127.0.0.1"), 0}};
  asio::ssl::context tls{asio::ssl::context::tls_server};
  tls.use_certificate_chain_file(LIBRESPOT_TEST_FIXTURES "/localhost.crt");
  tls.use_private_key_file(LIBRESPOT_TEST_FIXTURES "/localhost.key", asio::ssl::context::pem);
  librespot::net::dealer dealer{executor.get_executor()};
  auto server{asio::co_spawn(executor, [&]()->asio::awaitable<void> {
    auto socket{co_await acceptor.async_accept(asio::use_awaitable)};
    asio::ssl::stream<asio::ip::tcp::socket> stream{std::move(socket), tls};
    boost::system::error_code error;
    co_await stream.async_handshake(asio::ssl::stream_base::server, asio::redirect_error(asio::use_awaitable, error));
    CHECK(error);
  }, asio::use_future)};
  auto client{asio::co_spawn(executor, dealer.connect({"127.0.0.1", std::to_string(acceptor.local_endpoint().port())}, "token"), asio::use_future)};
  executor.run();
  server.get();
  CHECK_THROWS(client.get());
}

TEST_CASE("Registration failure diagnostics preserve HTTP status, protocol stage and redacted server error") {
  asio::io_context executor;
  asio::thread_pool worker{1};
  fake_http http;
  http.reject_registration = true;
  SECTION("JSON server error") {
  }
  SECTION("Plain server error echoing request credentials") {
    http.registration_error = "bad device: access-1 client-token";
  }
  librespot::oauth::service_auth auth{http, {.device_id{"device"}},
    {.username{"username"}, .type{librespot::authentication_type::stored_spotify}, .data{"stored"}}, worker.get_executor()};
  auto dealer{std::make_shared<fake_dealer>(executor.get_executor())};
  std::string output;
  librespot::connect::receiver receiver{executor.get_executor(), http, auth,
    {.device{.id{"device"}, .name{"test"}}, .reconnect{false}, .on_log{[&](librespot::log_event const &event){
      output += event.message + '\n';
    }}}, [](librespot::connect::command const &, librespot::connect::player_state &)->asio::awaitable<bool> {
      co_return false;
    }, dealer};
  dealer->on_connect = [&](unsigned int){
    dealer->push(connection("connection-id"));
  };
  auto running{asio::co_spawn(executor, receiver.run(), asio::use_future)};
  executor.run();
  CHECK_THROWS(running.get());
  worker.join();
  CHECK(output.find("Dealer message handling / Connect registration failed") != std::string::npos);
  CHECK(output.find("returned HTTP 400") != std::string::npos);
  CHECK(output.find("bad device") != std::string::npos);
  CHECK(output.find("must-not-be-logged") == std::string::npos);
  CHECK(output.find("access-1") == std::string::npos);
  CHECK(output.find("client-token") == std::string::npos);
}
