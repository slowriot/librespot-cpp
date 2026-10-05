#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <stop_token>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/program_options.hpp>
#include "librespot/audio/cdn_source.h"
#include "librespot/audio/wav_writer.h"
#include "librespot/cache/credentials.h"
#include "librespot/core/spotify_uri.h"
#include "librespot/oauth/device_auth.h"
#include "librespot/service/client.h"
#include "librespot/session.h"

namespace {

struct options {
  std::optional<std::filesystem::path> input;
  std::optional<std::filesystem::path> output;
  std::optional<std::filesystem::path> credentials;
  std::optional<librespot::spotify_id> track;
  std::string client_id;
  std::string service_client_id;
  std::string device_id;
};

std::string device_id() {
  std::random_device random;
  std::string result;
  for(unsigned int index{0}; index < 20; ++index) {
    auto const byte{random() & 255u};
    result += "0123456789abcdef"[byte >> 4];
    result += "0123456789abcdef"[byte & 15];
  }
  return result;
}

boost::asio::awaitable<void> decode(std::shared_ptr<librespot::audio::cdn_source> source,
  std::optional<std::filesystem::path> input, std::optional<std::filesystem::path> output, std::stop_token stop) {
  std::stop_callback cancel{stop, [source]{
    if(source) source->cancel();
  }};
  std::unique_ptr<librespot::audio::decoder> decoder;
  if(source) decoder = std::make_unique<librespot::audio::decoder>(source);
  else decoder = std::make_unique<librespot::audio::decoder>(*input);
  std::optional<librespot::audio::wav_writer> wav;
  std::uint64_t samples{0};
  std::uint64_t frames{0};
  std::uint64_t checksum{14695981039346656037ull};
  while(auto frame{decoder->next()}) {
    if(stop.stop_requested()) throw std::runtime_error{"PCM stream cancelled"};
    if(frames == 0) {
      auto const format{frame->format()};
      std::cout << "First PCM frame: " << format.sample_rate << " Hz, " << format.channels << " channels, "
        << frame->sample_count() << " samples/channel, " << frame->plane_count() << " planes" << std::endl;
      if(output) wav.emplace(*output, format);
    }
    if(wav) wav->append(*frame);
    for(std::size_t plane{0}; plane < frame->plane_count(); ++plane) {
      for(auto byte : frame->plane(plane)) {
        checksum ^= std::to_integer<std::uint64_t>(byte);
        checksum *= 1099511628211ull;
      }
    }
    samples += frame->sample_count();
    ++frames;
  }
  if(frames == 0) throw std::runtime_error{"audio produced no PCM frames"};
  if(wav) wav->finish();
  std::cout << "Decoded " << frames << " frames and " << samples << " samples/channel; PCM checksum " << checksum << std::endl;
  co_return;
}

boost::asio::awaitable<librespot::credentials> pair(librespot::net::http_transport &transport, std::string client_id) {
  if(client_id.empty()) throw std::invalid_argument{"first login requires --client-id for device authorisation"};
  librespot::oauth::device_auth auth{transport, std::move(client_id)};
  auto const challenge{co_await auth.start()};
  std::cout << "Open " << challenge.verification_uri << " and enter " << challenge.user_code << std::endl;
  auto const deadline{std::chrono::steady_clock::now() + challenge.expires_in};
  auto interval{challenge.interval};
  boost::asio::steady_timer timer{co_await boost::asio::this_coro::executor};
  while(std::chrono::steady_clock::now() + interval < deadline) {
    timer.expires_after(interval);
    co_await timer.async_wait(boost::asio::use_awaitable);
    auto result{co_await auth.poll(challenge.device_code)};
    if(auto token{std::get_if<librespot::oauth::token>(&result)}) {
      co_return librespot::credentials{.username{}, .type{librespot::authentication_type::spotify_token}, .data{std::move(token->access_token)}};
    }
    if(std::get<librespot::oauth::poll_status>(result) == librespot::oauth::poll_status::slow_down) interval += std::chrono::seconds{5};
  }
  throw std::runtime_error{"device authorisation expired"};
}

boost::asio::awaitable<void> run(options options, librespot::net::http_transport &transport,
  boost::asio::thread_pool &worker, std::stop_token stop) {
  if(options.input) {
    co_await boost::asio::co_spawn(worker, decode({}, options.input, options.output, stop), boost::asio::use_awaitable);
    co_return;
  }
  auto executor{co_await boost::asio::this_coro::executor};
  std::optional<librespot::credentials> login;
  if(options.credentials) login = librespot::cache::load_credentials(*options.credentials);
  if(!login) login = co_await pair(transport, options.client_id);
  librespot::session session{executor, transport, {.device_id{options.device_id}}};
  try {
    auto stored{co_await session.connect(std::move(*login))};
    if(options.credentials) librespot::cache::save_credentials(*options.credentials, stored);
    librespot::oauth::service_auth auth{transport, {.device_id{options.device_id}, .client_id{options.service_client_id}}, stored, worker.get_executor()};
    librespot::service::client service{transport, auth};
    auto track{co_await service.get_track(*options.track)};
    auto file{librespot::service::select_audio(track)};
    std::vector<std::string> urls;
    for(;;) {
      try {
        urls = co_await service.resolve_audio(file.id);
        break;
      } catch(librespot::service::storage_unavailable const &) {
        std::erase_if(track.files, [&file](librespot::service::audio_file const &candidate){
          return candidate.id == file.id;
        });
        if(track.files.empty()) throw;
        file = librespot::service::select_audio(track);
      }
    }
    std::cout << "Track: " << track.name << "; duration " << track.duration.count() << " ms; " << librespot::service::to_string(file.format) << std::endl;
    std::optional<librespot::audio::audio_key> key;
    try {
      key = co_await session.request_audio_key(track.id, file.id);
    } catch(boost::system::system_error const &error) {
      if(error.code() != boost::asio::error::access_denied) throw;
      std::cout << "No audio key granted; trying an unencrypted container" << std::endl;
    }
    auto source{std::make_shared<librespot::audio::cdn_source>(executor, transport, std::move(urls), key,
      librespot::audio::cdn_source_config{.container_offset{librespot::service::is_vorbis(file.format) ? 167u : 0u}})};
    co_await boost::asio::co_spawn(worker, decode(source, {}, options.output, stop), boost::asio::use_awaitable);
    source->cancel();
    session.close();
  } catch(...) {
    session.close();
    throw;
  }
}

} // anonymous namespace

auto main(int argc, char const *argv[])->int try {
  boost::program_options::options_description description{"PCM stream demonstration"};
  description.add_options()
    ("help,h", "Show usage")
    ("track", boost::program_options::value<std::string>(), "Spotify track URI")
    ("input", boost::program_options::value<std::string>(), "Decode a local audio file without network access")
    ("output", boost::program_options::value<std::string>(), "Write native-precision mono/stereo PCM to WAV")
    ("credentials", boost::program_options::value<std::string>(), "Load/save reusable credentials in an owner-only file")
    ("client-id", boost::program_options::value<std::string>(), "OAuth client ID enabled for device authorisation")
    ("service-client-id", boost::program_options::value<std::string>()->default_value(librespot::oauth::service_auth_config{}.client_id), "Spotify service protocol client identity")
    ("device-id", boost::program_options::value<std::string>(), "Device identity; generated if omitted")
    ("user-agent", boost::program_options::value<std::string>()->default_value("pcm_stream"), "Application HTTP user agent");
  boost::program_options::variables_map arguments;
  boost::program_options::store(boost::program_options::parse_command_line(argc, argv, description), arguments);
  if(arguments.contains("help")) {
    std::cout << description << std::endl;
    return EXIT_SUCCESS;
  }
  boost::program_options::notify(arguments);
  if(arguments.contains("input") == arguments.contains("track")) throw std::invalid_argument{"provide exactly one of --input and --track"};
  options config{
    .client_id{arguments.contains("client-id") ? arguments.at("client-id").as<std::string>() : ""},
    .service_client_id{arguments.at("service-client-id").as<std::string>()},
    .device_id{arguments.contains("device-id") ? arguments.at("device-id").as<std::string>() : device_id()},
  };
  if(arguments.contains("input")) config.input = arguments.at("input").as<std::string>();
  if(arguments.contains("output")) config.output = arguments.at("output").as<std::string>();
  if(arguments.contains("credentials")) config.credentials = arguments.at("credentials").as<std::string>();
  if(arguments.contains("track")) {
    auto const uri{librespot::parse_uri(arguments.at("track").as<std::string>())};
    if(!uri || !std::holds_alternative<librespot::catalog_uri>(*uri) || std::get<librespot::catalog_uri>(*uri).type != librespot::item_type::track) {
      throw std::invalid_argument{"--track must be a Spotify track URI"};
    }
    config.track = std::get<librespot::catalog_uri>(*uri).id;
  }
  boost::asio::io_context executor;
  boost::asio::thread_pool worker{1};
  librespot::net::http_client transport{executor.get_executor(), {.user_agent{arguments.at("user-agent").as<std::string>()}}};
  std::stop_source stop;
  boost::asio::cancellation_signal cancellation;
  boost::asio::signal_set signals{executor, SIGINT, SIGTERM};
  signals.async_wait([&](boost::system::error_code error, int){
    if(error) return;
    stop.request_stop();
    cancellation.emit(boost::asio::cancellation_type::all);
  });
  int status{EXIT_SUCCESS};
  boost::asio::co_spawn(executor, run(std::move(config), transport, worker, stop.get_token()),
    boost::asio::bind_cancellation_slot(cancellation.slot(), [&](std::exception_ptr failure){
      signals.cancel();
      if(!failure) return;
      status = EXIT_FAILURE;
      try {
        std::rethrow_exception(failure);
      } catch(std::exception const &error) {
        std::cerr << error.what() << std::endl;
      }
    }));
  executor.run();
  worker.join();
  return status;
} catch(std::exception const &error) {
  std::cerr << error.what() << std::endl;
  return EXIT_FAILURE;
}
