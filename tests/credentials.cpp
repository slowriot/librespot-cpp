#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include "librespot/cache/credentials.h"

namespace {

struct temporary_directory {
  std::filesystem::path path;

  temporary_directory() {
    std::string pattern{"/tmp/librespot-credentials-XXXXXX"};
    auto const name{::mkdtemp(pattern.data())};
    if(!name) throw std::runtime_error{"cannot create credential test directory"};
    path = name;
  }
  ~temporary_directory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

} // anonymous namespace

TEST_CASE("Credentials round trip binary zeros with atomic owner-only replacement") {
  temporary_directory directory;
  auto const path{directory.path / "credentials.json"};
  CHECK_FALSE(librespot::cache::load_credentials(path));
  librespot::credentials const original{
    .username{"user"},
    .type{librespot::authentication_type::stored_spotify},
    .data{std::string{"\0\1\2\0\0", 5}},
  };
  librespot::cache::save_credentials(path, original);
  auto const loaded{librespot::cache::load_credentials(path)};
  REQUIRE(loaded);
  CHECK(loaded->username == original.username);
  CHECK(loaded->type == original.type);
  CHECK(loaded->data == original.data);
  struct stat info{};
  REQUIRE(::stat(path.c_str(), &info) == 0);
  CHECK((info.st_mode & 0777) == 0600);
  REQUIRE(::chmod(path.c_str(), 0644) == 0);
  CHECK_THROWS(librespot::cache::load_credentials(path));
  librespot::cache::save_credentials(path, original);
  CHECK(librespot::cache::load_credentials(path)->data == original.data);
  CHECK(std::distance(std::filesystem::directory_iterator{directory.path}, std::filesystem::directory_iterator{}) == 1);
}

TEST_CASE("Credentials reject symlinks and noncanonical base64") {
  temporary_directory directory;
  auto const path{directory.path / "credentials.json"};
  auto const link{directory.path / "symlink.json"};
  librespot::cache::save_credentials(path, {.username{"user"}, .type{librespot::authentication_type::stored_spotify}, .data{"secret"}});
  std::filesystem::create_symlink(path, link);
  CHECK_THROWS(librespot::cache::load_credentials(link));
  for(auto const json : {
    R"({"auth_type":1,"auth_data":"A==="})",
    R"({"auth_type":1,"auth_data":"AA=A"})",
    R"({"auth_type":1,"auth_data":"AB=="})",
    R"({"auth_type":1,"auth_data":"!AAA"})",
    R"({"auth_type":4294967296,"auth_data":"AAAA"})",
    R"({"auth_type":-1,"auth_data":"AAAA"})",
    R"({"auth_type":1.0,"auth_data":"AAAA"})",
  }) {
    {
      std::ofstream file{path};
      file << json;
    }
    CHECK_THROWS(librespot::cache::load_credentials(path));
  }
}
