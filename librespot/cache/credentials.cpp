#include "credentials.h"
#include <array>
#include <cerrno>
#include <limits>
#include <span>
#include <stdexcept>
#include <system_error>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <openssl/rand.h>
#include <nlohmann/json.hpp>
#include "librespot/encoding/base64.h"

namespace librespot::cache {
namespace {

class file_descriptor {
private:
  int value{-1};

public:
  explicit file_descriptor(int value) : value{value} {
    if(value < 0) throw std::system_error{errno, std::generic_category(), "open credential file"};
  }
  ~file_descriptor() {
    ::close(value);
  }
  file_descriptor(file_descriptor const &) = delete;
  file_descriptor &operator=(file_descriptor const &) = delete;
  [[nodiscard]] int get() const noexcept {
    return value;
  }
};

std::string decode_base64(std::string const &input) {
  /// Validate the canonical alphabet and padding before using the shared decoder
  if(input.size() % 4 != 0) throw std::runtime_error{"invalid credential base64 length"};
  auto const padding{input.ends_with("==") ? 2u : input.ends_with('=') ? 1u : 0u};
  std::string_view constexpr alphabet{"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};
  for(std::size_t index{0}; index < input.size() - padding; ++index) {
    if(alphabet.find(input[index]) == std::string_view::npos) throw std::runtime_error{"invalid credential base64 character"};
  }
  auto const decoded{base64::decode(input)};
  if(base64::encode(decoded) != input) throw std::runtime_error{"noncanonical credential base64 padding"};
  return decoded;
}

void validate(librespot::credentials const &credentials) {
  if(static_cast<unsigned int>(credentials.type) > 4 || credentials.data.empty()) throw std::invalid_argument{"invalid stored credentials"};
}

std::filesystem::path temporary_path(std::filesystem::path const &path) {
  /// Use an unpredictable sibling filename so atomic rename stays on one filesystem
  std::array<unsigned char, 16> bytes;
  if(RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) throw std::runtime_error{"cannot generate credential temporary filename"};
  std::string suffix;
  std::string_view constexpr digits{"0123456789abcdef"};
  for(auto byte : bytes) {
    suffix += digits[byte >> 4];
    suffix += digits[byte & 15u];
  }
  return path.string() + ".tmp." + suffix;
}

struct temporary_file {
  std::filesystem::path path;
  ~temporary_file() {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }
};

} // anonymous namespace

void save_credentials(std::filesystem::path const &path, librespot::credentials const &credentials) {
  /// Persist a complete owner-only file before replacing the old credentials
  validate(credentials);
  nlohmann::json object{
    {"auth_type", static_cast<unsigned int>(credentials.type)},
    {"auth_data", base64::encode(credentials.data)},
  };
  object["username"] = credentials.username ? nlohmann::json(*credentials.username) : nlohmann::json(nullptr);
  auto const data{object.dump()};
  if(data.size() > 1024 * 1024) throw std::invalid_argument{"credential file exceeds size limit"};
  temporary_file temporary{temporary_path(path)};
  file_descriptor file{::open(temporary.path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600)};
  std::size_t offset{0};
  while(offset < data.size()) {
    auto const written{::write(file.get(), data.data() + offset, data.size() - offset)};
    if(written < 0 && errno == EINTR) continue;
    if(written <= 0) throw std::system_error{written < 0 ? errno : EIO, std::generic_category(), "write credential file"};
    offset += static_cast<std::size_t>(written);
  }
  if(::fsync(file.get()) != 0) throw std::system_error{errno, std::generic_category(), "sync credential file"};
  if(::rename(temporary.path.c_str(), path.c_str()) != 0) throw std::system_error{errno, std::generic_category(), "replace credential file"};
  auto const parent{path.has_parent_path() ? path.parent_path() : std::filesystem::path{"."}};
  file_descriptor directory{::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
  if(::fsync(directory.get()) != 0) throw std::system_error{errno, std::generic_category(), "sync credential directory"};
}

std::optional<librespot::credentials> load_credentials(std::filesystem::path const &path) try {
  /// Reject symlinks, non-regular files, foreign ownership and permissive modes
  int const descriptor{::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)};
  if(descriptor < 0 && errno == ENOENT) return std::nullopt;
  file_descriptor file{descriptor};
  struct stat info{};
  if(::fstat(file.get(), &info) != 0) throw std::system_error{errno, std::generic_category(), "inspect credential file"};
  if(!S_ISREG(info.st_mode) || info.st_uid != ::geteuid() || (info.st_mode & 077) != 0) throw std::runtime_error{"credential file must be a regular owner-only file"};
  if(info.st_size < 0 || info.st_size > 1024 * 1024) throw std::runtime_error{"credential file exceeds size limit"};
  std::string data;
  std::array<char, 4096> buffer;
  for(;;) {
    auto const count{::read(file.get(), buffer.data(), buffer.size())};
    if(count < 0 && errno == EINTR) continue;
    if(count < 0) throw std::system_error{errno, std::generic_category(), "read credential file"};
    if(count == 0) break;
    if(data.size() + static_cast<std::size_t>(count) > 1024 * 1024) throw std::runtime_error{"credential file exceeds size limit"};
    data.append(buffer.data(), static_cast<std::size_t>(count));
  }
  auto const object = nlohmann::json::parse(data);
  if(!object.is_object()) throw std::runtime_error{"credential JSON must be an object"};
  auto const &type_value{object.at("auth_type")};
  if(!type_value.is_number_integer() || type_value < 0 || type_value > 4) throw std::runtime_error{"invalid credential authentication type"};
  auto const type{type_value.get<unsigned int>()};
  auto const &encoded{object.contains("auth_data") ? object.at("auth_data") : object.at("encoded_auth_blob")};
  librespot::credentials result{
    .username{std::nullopt},
    .type{static_cast<authentication_type>(type)},
    .data{decode_base64(encoded.get<std::string>())},
  };
  if(object.contains("username") && !object.at("username").is_null()) result.username = object.at("username").get<std::string>();
  validate(result);
  return result;
} catch(nlohmann::json::exception const &) {
  throw std::runtime_error{"malformed credential JSON"};
}

} // namespace librespot::cache
