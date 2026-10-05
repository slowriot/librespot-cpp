#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <array>
#include <fstream>
#include <future>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <openssl/evp.h>
#include "librespot/audio/cdn_source.h"
#include "librespot/audio/decoder.h"
#include "librespot/crypto/hashcash.h"
#include "librespot/service/client.h"
#include "librespot/session.h"
#include "extended_metadata.pb.h"
#include "metadata.pb.h"
#include "spotify/clienttoken/v0/clienttoken_http.pb.h"
#include "spotify/login5/v3/login5.pb.h"
#include "storage-resolve.pb.h"

namespace {
namespace client_protocol = spotify::clienttoken::http::v0;
namespace login_protocol = spotify::login5::v3;

bool valid_solution(std::string const &prefix, std::string const &suffix, unsigned int difficulty) {
  auto const input{prefix + suffix};
  std::array<unsigned char, 20> digest;
  std::size_t size{0};
  REQUIRE(EVP_Q_digest(nullptr, "SHA1", nullptr, input.data(), input.size(), digest.data(), &size) == 1);
  for(unsigned int bit{0}; bit < difficulty; ++bit) if((digest[19 - bit / 8] & (1u << (bit % 8))) != 0) return false;
  return true;
}

class fake_service final : public librespot::net::http_transport {
public:
  boost::asio::thread_pool worker{1};
  unsigned int client_requests{0};
  unsigned int login_requests{0};
  unsigned int metadata_requests{0};
  bool challenge{false};
  bool unauthorised_once{false};
  bool wrong_entity{false};
  bool restricted{false};
  bool unsupported_challenge{false};
  int token_lifetime{3600};
  std::string encrypted_audio;
  std::vector<std::string> authorised;

  boost::asio::awaitable<librespot::net::http_response> request(librespot::net::http_request request) override {
    if(request.host == "cdn.example") {
      auto const &range{request.headers.at("Range")};
      auto const dash{range.find('-')};
      auto const first{std::stoull(range.substr(6, dash - 6))};
      auto const last{std::min<std::size_t>(std::stoull(range.substr(dash + 1)), encrypted_audio.size() - 1)};
      co_return librespot::net::http_response{206,
        {{"Content-Range", "bytes " + std::to_string(first) + '-' + std::to_string(last) + '/' + std::to_string(encrypted_audio.size())}},
        encrypted_audio.substr(first, last - first + 1)};
    }
    if(request.host == "clienttoken.spotify.com") {
      ++client_requests;
      client_protocol::ClientTokenRequest message;
      REQUIRE(message.ParseFromString(request.body));
      client_protocol::ClientTokenResponse reply;
      if(challenge && client_requests == 1) {
        REQUIRE(message.has_client_data());
        CHECK(message.client_data().connectivity_sdk_data().device_id() == "device");
        reply.set_response_type(client_protocol::RESPONSE_CHALLENGES_RESPONSE);
        reply.mutable_challenges()->set_state("challenge-state");
        auto &parameters{*reply.mutable_challenges()->add_challenges()};
        parameters.set_type(client_protocol::CHALLENGE_HASH_CASH);
        parameters.mutable_evaluate_hashcash_parameters()->set_prefix("abcd");
        parameters.mutable_evaluate_hashcash_parameters()->set_length(8);
      } else {
        if(challenge) {
          REQUIRE(message.has_challenge_answers());
          CHECK(message.challenge_answers().state() == "challenge-state");
          auto const &hex{message.challenge_answers().answers(0).hash_cash().suffix()};
          REQUIRE(hex.size() == 32);
          std::string suffix;
          for(std::size_t index{0}; index < hex.size(); index += 2) suffix += static_cast<char>(std::stoul(hex.substr(index, 2), nullptr, 16));
          CHECK(valid_solution(std::string{"\xab\xcd", 2}, suffix, 8));
        }
        reply.set_response_type(client_protocol::RESPONSE_GRANTED_TOKEN_RESPONSE);
        reply.mutable_granted_token()->set_token("client-token");
        reply.mutable_granted_token()->set_expires_after_seconds(3600);
        reply.mutable_granted_token()->set_refresh_after_seconds(1800);
      }
      co_return librespot::net::http_response{200, {}, reply.SerializeAsString()};
    }
    if(request.host == "login5.spotify.com") {
      ++login_requests;
      CHECK(request.headers.at("client-token") == "client-token");
      login_protocol::LoginRequest message;
      REQUIRE(message.ParseFromString(request.body));
      CHECK(message.stored_credential().username() == "username");
      login_protocol::LoginResponse reply;
      if(unsupported_challenge) {
        reply.mutable_challenges()->add_challenges()->mutable_code();
      } else if(challenge && login_requests == 1) {
        reply.set_login_context("login-context");
        auto &parameters{*reply.mutable_challenges()->add_challenges()->mutable_hashcash()};
        parameters.set_prefix("prefix");
        parameters.set_length(8);
      } else {
        if(challenge) {
          CHECK(message.login_context() == "login-context");
          REQUIRE(message.challenge_solutions().solutions_size() == 1);
          CHECK(valid_solution("prefix", message.challenge_solutions().solutions(0).hashcash().suffix(), 8));
        }
        reply.mutable_ok()->set_access_token("access-" + std::to_string(login_requests));
        reply.mutable_ok()->set_access_token_expires_in(token_lifetime);
      }
      co_return librespot::net::http_response{200, {}, reply.SerializeAsString()};
    }
    if(request.host == "apresolve.spotify.com") {
      if(request.target == "/?type=accesspoint") {
        co_return librespot::net::http_response{200, {}, R"({"accesspoint":["accesspoint.example:443"]})"};
      }
      CHECK(request.target == "/?type=spclient");
      co_return librespot::net::http_response{200, {}, R"({"spclient":["service.example:443"]})"};
    }
    CHECK(request.host == "service.example");
    authorised.push_back(request.headers.at("Authorization"));
    if(request.target == "/extended-metadata/v0/extended-metadata") {
      ++metadata_requests;
      if(unauthorised_once && metadata_requests == 1) co_return librespot::net::http_response{401, {}, {}};
      spotify::extendedmetadata::BatchedEntityRequest query;
      REQUIRE(query.ParseFromString(request.body));
      REQUIRE(query.entity_request_size() == 1);
      CHECK(query.entity_request(0).entity_uri() == "spotify:track:0000000000000000000000");
      librespot::protocol::metadata::Track track;
      track.set_gid(std::string(16, '\0'));
      track.set_name("Test track");
      track.set_duration(20'000);
      track.mutable_album()->set_name("Test album");
      track.add_artist()->set_name("Test artist");
      auto &vorbis{*track.add_file()};
      vorbis.set_file_id(std::string(20, '\0'));
      vorbis.set_format(librespot::protocol::metadata::AudioFile::OGG_VORBIS_320);
      auto &flac{*track.add_file()};
      flac.set_file_id(std::string(20, '\1'));
      flac.set_format(librespot::protocol::metadata::AudioFile::FLAC_FLAC_24BIT);
      if(!encrypted_audio.empty()) flac.set_format(librespot::protocol::metadata::AudioFile::FLAC_FLAC);
      spotify::extendedmetadata::BatchedExtensionResponse reply;
      auto &extension{*reply.add_extended_metadata()};
      extension.set_extension_kind(spotify::extendedmetadata::TRACK_V4);
      auto &entity{*extension.add_extension_data()};
      entity.set_entity_uri(wrong_entity ? "spotify:track:another" : query.entity_request(0).entity_uri());
      entity.mutable_header()->set_status_code(200);
      entity.mutable_extension_data()->set_value(track.SerializeAsString());
      co_return librespot::net::http_response{200, {}, reply.SerializeAsString()};
    }
    CHECK(request.target.starts_with("/storage-resolve/files/audio/interactive/"));
    spotify::download::proto::StorageResolveResponse reply;
    reply.set_result(restricted ? spotify::download::proto::StorageResolveResponse::RESTRICTED : spotify::download::proto::StorageResolveResponse::CDN);
    reply.add_cdnurl("https://cdn.example/file?signature=secret");
    reply.set_fileid(std::string(20, '\1'));
    co_return librespot::net::http_response{200, {}, reply.SerializeAsString()};
  }
};

librespot::credentials stored() {
  return {.username{"username"}, .type{librespot::authentication_type::stored_spotify}, .data{"credential"}};
}

class fake_access_point final : public librespot::net::access_point_transport {
private:
  boost::asio::experimental::channel<boost::asio::any_io_executor, void(boost::system::error_code, librespot::net::packet)> replies;

public:
  explicit fake_access_point(boost::asio::any_io_executor executor) : replies{executor, 1} {
  }

  boost::asio::awaitable<librespot::credentials> connect(librespot::net::endpoint address, librespot::credentials, std::string) override {
    CHECK(address.host == "accesspoint.example");
    co_return stored();
  }

  boost::asio::awaitable<void> send(librespot::net::packet packet) override {
    REQUIRE(packet.command == 0x0c);
    REQUIRE(packet.payload.size() == 42);
    for(std::size_t index{0}; index < 20; ++index) CHECK(packet.payload[index] == std::byte{1});
    librespot::net::packet response{.command{0x0d}, .payload{packet.payload.begin() + 36, packet.payload.begin() + 40}};
    response.payload.insert(response.payload.end(), 16, std::byte{0});
    REQUIRE(replies.try_send(boost::system::error_code{}, std::move(response)));
    co_return;
  }

  boost::asio::awaitable<librespot::net::packet> receive() override {
    co_return co_await replies.async_receive(boost::asio::use_awaitable);
  }

  void close() override {
    replies.cancel();
    replies.close();
  }
};

boost::asio::awaitable<std::vector<std::byte>> consume(std::shared_ptr<librespot::audio::byte_source> source) {
  librespot::audio::decoder decoder{source};
  std::vector<std::byte> result;
  while(auto frame{decoder.next()}) {
    auto const bytes{frame->plane(0)};
    result.insert(result.end(), bytes.begin(), bytes.end());
  }
  co_return result;
}

} // anonymous namespace

TEST_CASE("Service authentication solves client-token and Login5 challenges and caches credentials") {
  boost::asio::io_context executor;
  fake_service transport;
  transport.challenge = true;
  librespot::oauth::service_auth auth{transport, {.device_id{"device"}}, stored(), transport.worker.get_executor()};
  auto exercise{[&]()->boost::asio::awaitable<void> {
    auto const first{co_await auth.token()};
    auto const second{co_await auth.token()};
    CHECK(first.value == "access-2");
    CHECK(second.value == first.value);
  }};
  auto done{boost::asio::co_spawn(executor, exercise(), boost::asio::use_future)};
  executor.run();
  CHECK_NOTHROW(done.get());
  CHECK(transport.client_requests == 2);
  CHECK(transport.login_requests == 2);
}

TEST_CASE("Track resolution refreshes rejected tokens and selects native lossless audio") {
  boost::asio::io_context executor;
  fake_service transport;
  transport.unauthorised_once = true;
  librespot::oauth::service_auth auth{transport, {.device_id{"device"}}, stored(), transport.worker.get_executor()};
  librespot::service::client service{transport, auth};
  auto exercise{[&]()->boost::asio::awaitable<void> {
    auto const track{co_await service.get_track({})};
    CHECK(track.name == "Test track");
    CHECK(track.duration == std::chrono::seconds{20});
    CHECK(track.artists == std::vector<std::string>{"Test artist"});
    auto const file{librespot::service::select_audio(track)};
    CHECK(file.format == librespot::service::audio_format::flac_24);
    auto const urls{co_await service.resolve_audio(file.id)};
    CHECK(urls == std::vector<std::string>{"https://cdn.example/file?signature=secret"});
  }};
  auto done{boost::asio::co_spawn(executor, exercise(), boost::asio::use_future)};
  executor.run();
  CHECK_NOTHROW(done.get());
  CHECK(transport.login_requests == 2);
  REQUIRE(transport.authorised.size() == 3);
  CHECK(transport.authorised[0] == "Bearer access-1");
  CHECK(transport.authorised[1] == "Bearer access-2");
}

TEST_CASE("Service rejects mismatched metadata entities and restricted storage") {
  boost::asio::io_context executor;
  fake_service transport;
  librespot::oauth::service_auth auth{transport, {.device_id{"device"}}, stored(), transport.worker.get_executor()};
  librespot::service::client service{transport, auth};
  transport.wrong_entity = true;
  auto wrong{boost::asio::co_spawn(executor, service.get_track({}), boost::asio::use_future)};
  executor.run();
  CHECK_THROWS(wrong.get());
  executor.restart();
  transport.restricted = true;
  auto denied{boost::asio::co_spawn(executor, service.resolve_audio({}), boost::asio::use_future)};
  executor.run();
  CHECK_THROWS_AS(denied.get(), librespot::service::storage_unavailable);
}

TEST_CASE("Hashcash is bounded, cancellable and matches an independent SHA1 proof") {
  auto const solution{librespot::crypto::solve_hashcash({}, std::as_bytes(std::span{"prefix", 6}), 8)};
  CHECK(valid_solution("prefix", {reinterpret_cast<char const *>(solution.suffix.data()), solution.suffix.size()}, 8));
  CHECK_THROWS(librespot::crypto::solve_hashcash({}, {}, 65));
  CHECK_THROWS(librespot::crypto::solve_hashcash({}, {}, 64, std::chrono::seconds{1}));
  std::stop_source stop;
  stop.request_stop();
  CHECK_THROWS(librespot::crypto::solve_hashcash({}, {}, 0, std::chrono::seconds{5}, stop.get_token()));
}

TEST_CASE("Login5 rejects unsupported challenges and invalid token lifetimes") {
  for(bool const unsupported : {true, false}) {
    boost::asio::io_context executor;
    fake_service transport;
    transport.unsupported_challenge = unsupported;
    transport.token_lifetime = -1;
    librespot::oauth::service_auth auth{transport, {.device_id{"device"}}, stored(), transport.worker.get_executor()};
    auto response{boost::asio::co_spawn(executor, auth.token(), boost::asio::use_future)};
    executor.run();
    CHECK_THROWS_AS(response.get(), std::runtime_error);
  }
}

TEST_CASE("A resolved Spotify track reaches PCM through service authentication, audio-key dispatch and encrypted CDN input") {
  std::ifstream fixture{std::filesystem::path{LIBRESPOT_TEST_FIXTURES} / "tone.flac", std::ios::binary};
  REQUIRE(fixture.good());
  fake_service transport;
  transport.encrypted_audio.assign(std::istreambuf_iterator<char>{fixture}, std::istreambuf_iterator<char>{});
  librespot::audio::decrypt({}, 0, std::as_writable_bytes(std::span{transport.encrypted_audio}));
  boost::asio::io_context executor;
  boost::asio::thread_pool worker{1};
  auto connection{std::make_shared<fake_access_point>(executor.get_executor())};
  librespot::session session{executor.get_executor(), transport, {.device_id{"device"}}, connection};
  auto exercise{[&]()->boost::asio::awaitable<void> {
    try {
      auto reusable{co_await session.connect(stored())};
      librespot::oauth::service_auth auth{transport, {.device_id{"device"}}, reusable, worker.get_executor()};
      librespot::service::client service{transport, auth};
      auto const track{co_await service.get_track({})};
      auto const file{librespot::service::select_audio(track)};
      auto urls{co_await service.resolve_audio(file.id)};
      auto const key{co_await session.request_audio_key(track.id, file.id)};
      auto source{std::make_shared<librespot::audio::cdn_source>(executor.get_executor(), transport, std::move(urls), key)};
      auto const pcm{co_await boost::asio::co_spawn(worker, consume(source), boost::asio::use_awaitable)};
      librespot::audio::decoder reference{std::filesystem::path{LIBRESPOT_TEST_FIXTURES} / "tone.flac"};
      std::vector<std::byte> expected;
      while(auto frame{reference.next()}) {
        auto const bytes{frame->plane(0)};
        expected.insert(expected.end(), bytes.begin(), bytes.end());
      }
      REQUIRE_FALSE(pcm.empty());
      CHECK(pcm == expected);
      source->cancel();
      session.close();
    } catch(...) {
      session.close();
      throw;
    }
  }};
  auto done{boost::asio::co_spawn(executor, exercise(), boost::asio::use_future)};
  executor.run();
  worker.join();
  CHECK_NOTHROW(done.get());
}
