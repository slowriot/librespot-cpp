cmake_minimum_required(VERSION 3.28)
project(librespot_cpp VERSION 0.1.0 LANGUAGES C CXX)
option(LIBRESPOT_BUILD_EXAMPLES "Build example applications" ${PROJECT_IS_TOP_LEVEL})
option(LIBRESPOT_ENABLE_SANITIZERS "Instrument project code and CMake dependencies with address and undefined behaviour sanitizers" OFF)
if(LIBRESPOT_ENABLE_SANITIZERS)
  # Protobuf's inline container annotations require consistent runtime instrumentation
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

include(FetchContent)
include(CTest)
include(GNUInstallDirs)
configure_file(cmake/version.h.in generated/librespot/version.h @ONLY)

find_package(Boost 1.81 CONFIG REQUIRED)
if(LIBRESPOT_BUILD_EXAMPLES)
  find_package(Boost 1.81 CONFIG REQUIRED COMPONENTS program_options)
endif()
find_package(OpenSSL 3.0 REQUIRED)
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(AVAHI REQUIRED IMPORTED_TARGET avahi-client)

fetchcontent_declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.12.0
  GIT_SHALLOW ON
)
fetchcontent_makeavailable(nlohmann_json)
include(cmake/ffmpeg.cmake)
include(cmake/protocol.cmake)

add_library(librespot
  librespot/diagnostics.cpp
  librespot/session.cpp
  librespot/core/spotify_id.cpp
  librespot/core/file_id.cpp
  librespot/core/spotify_uri.cpp
  librespot/audio/decrypt.cpp
  librespot/audio/decoder.cpp
  librespot/audio/fetch.cpp
  librespot/audio/cdn_source.cpp
  librespot/audio/wav_writer.cpp
  librespot/net/http_client.cpp
  librespot/net/access_point.cpp
  librespot/net/resolve.cpp
  librespot/net/mercury.cpp
  librespot/oauth/device_auth.cpp
  librespot/oauth/access_token.cpp
  librespot/oauth/service_auth.cpp
  librespot/service/client.cpp
  librespot/crypto/shannon.cpp
  librespot/crypto/key_exchange.cpp
  librespot/crypto/hashcash.cpp
  librespot/cache/credentials.cpp
  librespot/encoding/base64.cpp
  librespot/encoding/protocol.cpp
  librespot/discovery/pairing.cpp
  librespot/discovery/server.cpp
  librespot/discovery/advertisement.cpp
  librespot/net/dealer.cpp
  librespot/connect/control.cpp
  librespot/connect/receiver.cpp
)
add_library(librespot::librespot ALIAS librespot)
target_compile_features(librespot PUBLIC cxx_std_23)
set_target_properties(librespot PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(librespot
  PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
  PUBLIC $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated>
  PUBLIC $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(librespot
  PUBLIC Boost::headers
  PRIVATE OpenSSL::SSL
  PRIVATE OpenSSL::Crypto
  PRIVATE nlohmann_json::nlohmann_json
  PRIVATE Threads::Threads
  PRIVATE librespot_ffmpeg
  PRIVATE librespot_protocol
  PRIVATE ZLIB::ZLIB
  PRIVATE PkgConfig::AVAHI
)
target_compile_options(librespot PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
if(LIBRESPOT_ENABLE_SANITIZERS)
  target_link_options(librespot INTERFACE -fsanitize=address,undefined)
endif()

if(LIBRESPOT_BUILD_EXAMPLES)
  add_executable(device_login examples/device_login.cpp)
  add_executable(pcm_stream examples/pcm_stream.cpp)
  add_executable(connect_receiver examples/connect_receiver.cpp)
  target_link_libraries(connect_receiver PRIVATE librespot::librespot PRIVATE Boost::program_options PRIVATE OpenSSL::Crypto)
  target_link_libraries(pcm_stream PRIVATE librespot::librespot PRIVATE Boost::program_options)
  target_link_libraries(device_login
    PRIVATE librespot::librespot
    PRIVATE Boost::program_options
  )
endif()

if(BUILD_TESTING)
  fetchcontent_declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.8.1
    GIT_SHALLOW ON
  )
  fetchcontent_makeavailable(Catch2)
  add_executable(librespot_tests
    tests/identifiers.cpp
    tests/decrypt.cpp
    tests/device_auth.cpp
    tests/decoder.cpp
    tests/crypto.cpp
    tests/credentials.cpp
    tests/http_client.cpp
    tests/mercury.cpp
    tests/fetch.cpp
    tests/session.cpp
    tests/access_token.cpp
    tests/service.cpp
    tests/streaming.cpp
    tests/discovery.cpp
    tests/connect.cpp
    tests/diagnostics.cpp
  )
  target_link_libraries(librespot_tests
    PRIVATE librespot::librespot
    PRIVATE Catch2::Catch2WithMain
    PRIVATE OpenSSL::Crypto
    PRIVATE OpenSSL::SSL
    PRIVATE librespot_protocol
    PRIVATE nlohmann_json::nlohmann_json
    PRIVATE ZLIB::ZLIB
  )
  target_compile_definitions(librespot_tests PRIVATE LIBRESPOT_TEST_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures")
  list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
  include(Catch)
  catch_discover_tests(librespot_tests PROPERTIES TIMEOUT 30)
endif()
