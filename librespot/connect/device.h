#pragma once
#include <cstdint>
#include <string>

namespace librespot::connect {

enum class device_type : std::uint8_t {
  computer = 1,
  tablet = 2,
  smartphone = 3,
  speaker = 4,
  television = 5,
  audio_dongle = 8,
};

struct device_config {
  std::string id;
  std::string name;
  device_type type{device_type::speaker};
  std::string brand;
  std::string model;
  std::string client_id{"65b708073fc0480ea92a077233ca87bd"};
  std::uint16_t initial_volume{32768};
};

} // namespace librespot::connect
