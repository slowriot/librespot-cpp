#pragma once
#include <memory>
#include <string>
#include "librespot/connect/device.h"
#include "librespot/net/access_point.h"

namespace librespot::discovery {

class pairing {
private:
  struct implementation;
  std::unique_ptr<implementation> state;

public:
  explicit pairing(connect::device_config config);
  ~pairing();
  pairing(pairing const &) = delete;
  pairing &operator=(pairing const &) = delete;
  [[nodiscard]] std::string get_info(std::string const &active_user = {}) const;
  [[nodiscard]] credentials decode(std::string const &username, std::string const &blob, std::string const &client_key) const;
};

} // namespace librespot::discovery
