set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_LIBUPB OFF CACHE BOOL "" FORCE)
set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
fetchcontent_declare(protobuf
  SYSTEM
  GIT_REPOSITORY https://github.com/protocolbuffers/protobuf.git
  GIT_TAG v29.3
  GIT_SHALLOW ON
  GIT_SUBMODULES third_party/abseil-cpp
)
fetchcontent_makeavailable(protobuf)
include("${protobuf_SOURCE_DIR}/cmake/protobuf-generate.cmake")
# GCC 16 exposes a missing direct include in this pinned Abseil header
set(abseil_memory_header "${protobuf_SOURCE_DIR}/third_party/abseil-cpp/absl/container/internal/container_memory.h")
file(READ "${abseil_memory_header}" abseil_memory_source)
if(NOT abseil_memory_source MATCHES "#include <cstdint>")
  string(REPLACE "#include <cstddef>" "#include <cstddef>\n#include <cstdint>" abseil_memory_source "${abseil_memory_source}")
  file(WRITE "${abseil_memory_header}" "${abseil_memory_source}")
endif()
add_library(librespot_protocol STATIC
  protocol/keyexchange.proto
  protocol/authentication.proto
  protocol/mercury.proto
  protocol/metadata.proto
  protocol/connectivity.proto
  protocol/extended_metadata.proto
  protocol/entity_extension_data.proto
  protocol/extension_kind.proto
  protocol/storage-resolve.proto
  protocol/spotify/clienttoken/v0/clienttoken_http.proto
  protocol/spotify/login5/v3/login5.proto
  protocol/spotify/login5/v3/client_info.proto
  protocol/spotify/login5/v3/user_info.proto
  protocol/spotify/login5/v3/challenges/code.proto
  protocol/spotify/login5/v3/challenges/hashcash.proto
  protocol/spotify/login5/v3/credentials/credentials.proto
  protocol/spotify/login5/v3/identifiers/identifiers.proto
)
target_link_libraries(librespot_protocol PUBLIC protobuf::libprotobuf)
target_include_directories(librespot_protocol PUBLIC "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/protocol")
get_target_property(protobuf_include_dirs libprotobuf INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(librespot_protocol SYSTEM PUBLIC ${protobuf_include_dirs})
protobuf_generate(TARGET librespot_protocol IMPORT_DIRS "${PROJECT_SOURCE_DIR}/protocol" "${protobuf_SOURCE_DIR}/src")
