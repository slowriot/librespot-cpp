#include "client.h"
#include <charconv>
#include <optional>
#include <span>
#include <stdexcept>
#include <boost/asio/error.hpp>
#include <boost/system/system_error.hpp>
#include <nlohmann/json.hpp>
#include "extended_metadata.pb.h"
#include "metadata.pb.h"
#include "storage-resolve.pb.h"

namespace librespot::service {
namespace asio = boost::asio;
namespace {

bool supported(int format) {
  switch(format) {
  case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 16: case 22: return true;
  default: return false;
  }
}

} // anonymous namespace

struct client::implementation {
  net::http_transport &transport;
  oauth::service_auth &auth;
  std::vector<net::endpoint> endpoints;

  implementation(net::http_transport &transport, oauth::service_auth &auth) : transport{transport}, auth{auth} {
  }

  asio::awaitable<void> resolve() {
    if(!endpoints.empty()) co_return;
    auto const response{co_await transport.request({.host{"apresolve.spotify.com"}, .port{"443"},
      .target{"/?type=spclient"}, .method{"GET"}, .headers{}, .body{}})};
    if(response.status != 200) throw std::runtime_error{"service resolver failed"};
    auto object = nlohmann::json::parse(response.body);
    auto const addresses{object.at("spclient").get<std::vector<std::string>>()};
    std::vector<net::endpoint> result;
    for(auto const &address : addresses) {
      auto const separator{address.rfind(':')};
      if(separator == std::string::npos) throw std::runtime_error{"invalid service address"};
      auto host{address.substr(0, separator)};
      auto port{address.substr(separator + 1)};
      unsigned int number{0};
      auto const [end, error]{std::from_chars(port.data(), port.data() + port.size(), number)};
      if(host.empty() || host.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") != std::string::npos
        || error != std::errc{} || end != port.data() + port.size() || number == 0 || number > 65535) throw std::runtime_error{"invalid service address"};
      result.push_back({.host{std::move(host)}, .port{std::move(port)}});
    }
    if(result.empty() || result.size() > 32) throw std::runtime_error{"invalid service address list"};
    endpoints = std::move(result);
  }

  asio::awaitable<std::string> request(std::string target, std::string method = "GET", std::string body = {},
    std::map<std::string, std::string> extra_headers = {}) {
    co_await resolve();
    std::exception_ptr failure;
    for(unsigned int retry{0}; retry < 2; ++retry) {
      auto const token{co_await auth.token()};
      auto const client_token{co_await auth.client_token()};
      bool refresh{false};
      for(auto const &address : endpoints) {
        net::http_response response;
        try {
          auto headers{extra_headers};
          headers.insert({{"Authorization", token.type + ' ' + token.value}, {"client-token", client_token},
            {"Accept", "application/x-protobuf"}, {"Content-Type", "application/x-protobuf"}});
          response = co_await transport.request({
            .host{address.host}, .port{address.port}, .target{target}, .method{method},
            .headers{std::move(headers)},
            .body{body},
          });
        } catch(boost::system::system_error const &error) {
          if(error.code() == asio::error::operation_aborted) throw;
          failure = std::current_exception();
          continue;
        }
        if(response.status == 401 && retry == 0) {
          auth.invalidate_token();
          refresh = true;
          break;
        }
        if(response.status >= 500) {
          failure = std::make_exception_ptr(std::runtime_error{"service returned HTTP " + std::to_string(response.status)});
          continue;
        }
        if(response.status < 200 || response.status >= 300) throw std::runtime_error{"service returned HTTP " + std::to_string(response.status)};
        co_return std::move(response.body);
      }
      if(!refresh) break;
    }
    if(failure) std::rethrow_exception(failure);
    throw std::runtime_error{"service authentication failed"};
  }
};

client::client(net::http_transport &transport, oauth::service_auth &auth)
  : state{std::make_unique<implementation>(transport, auth)} {
}

client::~client() = default;

asio::awaitable<void> client::put_connect_state(std::string device_id, std::string connection_id, std::string protobuf) {
  if(device_id.empty() || connection_id.empty()) throw std::invalid_argument{"Connect identity is required"};
  co_await state->request("/connect-state/v1/devices/" + net::form_encode(device_id), "PUT", std::move(protobuf),
    {{"Spotify-Connection-Id", std::move(connection_id)}});
}

asio::awaitable<void> client::delete_connect_state(std::string device_id) {
  if(device_id.empty()) throw std::invalid_argument{"Connect device ID is required"};
  co_await state->request("/connect-state/v1/devices/" + net::form_encode(device_id), "DELETE");
}

storage_unavailable::storage_unavailable() : std::runtime_error{"audio file is restricted or has no supported CDN storage"} {
}

asio::awaitable<track> client::get_track(spotify_id id) {
  auto const uri{"spotify:track:" + id.to_base62()};
  spotify::extendedmetadata::BatchedEntityRequest query;
  auto &entity{*query.add_entity_request()};
  entity.set_entity_uri(uri);
  entity.add_query()->set_extension_kind(spotify::extendedmetadata::TRACK_V4);
  auto const body{co_await state->request("/extended-metadata/v0/extended-metadata", "POST", query.SerializeAsString())};
  spotify::extendedmetadata::BatchedExtensionResponse response;
  if(!response.ParseFromString(body)) throw std::runtime_error{"malformed extended metadata response"};
  for(auto const &extension : response.extended_metadata()) {
    if(extension.extension_kind() != spotify::extendedmetadata::TRACK_V4) continue;
    if(extension.header().provider_error_status() != 0 && extension.header().provider_error_status() != 200) throw std::runtime_error{"track metadata provider failed"};
    for(auto const &entity : extension.extension_data()) {
      if(entity.entity_uri() != uri) continue;
      if((entity.header().status_code() != 0 && entity.header().status_code() != 200)
        || !entity.has_extension_data()) throw std::runtime_error{"track metadata unavailable"};
      protocol::metadata::Track decoded;
      if(!decoded.ParseFromString(entity.extension_data().value())) throw std::runtime_error{"malformed track metadata"};
      auto const parsed_id{spotify_id::from_bytes(std::as_bytes(std::span{decoded.gid()}))};
      if(!parsed_id || *parsed_id != id || decoded.duration() < 0) throw std::runtime_error{"invalid track identity or duration"};
      track result{.id{id}, .name{decoded.name()}, .album{decoded.album().name()}, .artists{},
        .duration{decoded.duration()}, .explicit_content{decoded.explicit_()}, .files{}};
      for(auto const &artist : decoded.artist()) result.artists.push_back(artist.name());
      for(auto const &file : decoded.file()) {
        if(!file.has_format() || !supported(file.format())) continue;
        auto const file_id{librespot::file_id::from_bytes(std::as_bytes(std::span{file.file_id()}))};
        if(!file_id) throw std::runtime_error{"invalid track audio file ID"};
        result.files.push_back({.id{*file_id}, .format{static_cast<audio_format>(file.format())}});
      }
      co_return result;
    }
  }
  throw std::runtime_error{"track missing from metadata response"};
}

asio::awaitable<std::vector<std::string>> client::resolve_audio(file_id id) {
  auto const body{co_await state->request("/storage-resolve/files/audio/interactive/" + id.to_hex())};
  spotify::download::proto::StorageResolveResponse response;
  if(!response.ParseFromString(body)) throw std::runtime_error{"malformed storage response"};
  if(response.result() != spotify::download::proto::StorageResolveResponse::CDN) throw storage_unavailable{};
  if(!response.fileid().empty()) {
    auto const parsed{file_id::from_bytes(std::as_bytes(std::span{response.fileid()}))};
    if(!parsed || *parsed != id) throw std::runtime_error{"storage response file ID mismatch"};
  }
  if(response.cdnurl_size() == 0 || response.cdnurl_size() > 32) throw std::runtime_error{"storage response has no usable CDN addresses"};
  co_return std::vector<std::string>{response.cdnurl().begin(), response.cdnurl().end()};
}

audio_file select_audio(track const &track) {
  for(auto const format : {audio_format::flac_24, audio_format::flac_16, audio_format::vorbis_320,
    audio_format::mp3_320, audio_format::mp3_256, audio_format::vorbis_160, audio_format::mp3_160,
    audio_format::vorbis_96, audio_format::mp3_96}) {
    for(auto const &file : track.files) if(file.format == format) return file;
  }
  throw std::runtime_error{"track has no supported audio format"};
}

bool is_vorbis(audio_format format) noexcept {
  return format == audio_format::vorbis_96 || format == audio_format::vorbis_160 || format == audio_format::vorbis_320;
}

std::string_view to_string(audio_format format) noexcept {
  switch(format) {
  case audio_format::vorbis_96: return "Vorbis 96 kbit/s";
  case audio_format::vorbis_160: return "Vorbis 160 kbit/s";
  case audio_format::vorbis_320: return "Vorbis 320 kbit/s";
  case audio_format::mp3_96: return "MP3 96 kbit/s";
  case audio_format::mp3_160: return "MP3 160 kbit/s";
  case audio_format::mp3_256: return "MP3 256 kbit/s";
  case audio_format::mp3_320: return "MP3 320 kbit/s";
  case audio_format::flac_16: return "FLAC 16-bit";
  case audio_format::flac_24: return "FLAC 24-bit";
  }
  return "unknown";
}

} // namespace librespot::service
