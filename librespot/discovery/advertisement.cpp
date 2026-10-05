#include "advertisement.h"
#include <stdexcept>
#include <utility>
#include <boost/asio/post.hpp>
#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/alternative.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/thread-watch.h>

namespace librespot::discovery {

struct advertisement::implementation {
  boost::asio::any_io_executor executor;
  std::string name;
  std::uint16_t port;
  std::function<void(std::string)> on_error;
  AvahiThreadedPoll *poll{nullptr};
  AvahiClient *client{nullptr};
  AvahiEntryGroup *group{nullptr};
  bool poll_started{false};

  implementation(boost::asio::any_io_executor executor, std::string name, std::uint16_t port,
    std::function<void(std::string)> on_error)
    : executor{std::move(executor)}, name{std::move(name)}, port{port}, on_error{std::move(on_error)} {
  }

  ~implementation() {
    if(poll_started) avahi_threaded_poll_stop(poll);
    if(group) avahi_entry_group_free(group);
    if(client) avahi_client_free(client);
    if(poll) avahi_threaded_poll_free(poll);
  }

  void fail(std::string message) noexcept {
    try {
      boost::asio::post(executor, [handler{on_error}, message{std::move(message)}]{
        if(handler) handler(message);
      });
    } catch(...) {
      // allocation failures cannot cross the C callback boundary
    }
  }

  void rename() {
    auto *alternative{avahi_alternative_service_name(name.c_str())};
    if(!alternative) throw std::runtime_error{"Avahi name allocation failed"};
    name = alternative;
    avahi_free(alternative);
    avahi_entry_group_reset(group);
    publish();
  }

  static void group_changed(AvahiEntryGroup *, AvahiEntryGroupState status, void *userdata) noexcept {
    auto &self{*static_cast<implementation *>(userdata)};
    try {
      if(status == AVAHI_ENTRY_GROUP_COLLISION) self.rename();
      else if(status == AVAHI_ENTRY_GROUP_FAILURE) self.fail("Avahi publication failed: " + std::string{avahi_strerror(avahi_client_errno(self.client))});
    } catch(std::exception const &error) {
      self.fail(error.what());
    }
  }

  void publish() {
    if(!group) group = avahi_entry_group_new(client, group_changed, this);
    if(!group) throw std::runtime_error{"Avahi entry group creation failed"};
    if(!avahi_entry_group_is_empty(group)) return;
    auto const result{avahi_entry_group_add_service(group, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, static_cast<AvahiPublishFlags>(0),
      name.c_str(), "_spotify-connect._tcp", nullptr, nullptr, port, "VERSION=1.0", "CPath=/", nullptr)};
    if(result == AVAHI_ERR_COLLISION) {
      rename();
      return;
    }
    if(result < 0 || avahi_entry_group_commit(group) < 0) throw std::runtime_error{"Avahi service publication failed"};
  }

  static void client_changed(AvahiClient *client, AvahiClientState status, void *userdata) noexcept {
    auto &self{*static_cast<implementation *>(userdata)};
    self.client = client;
    try {
      if(status == AVAHI_CLIENT_S_RUNNING) self.publish();
      else if(status == AVAHI_CLIENT_FAILURE) self.fail("Avahi daemon connection failed: " + std::string{avahi_strerror(avahi_client_errno(client))});
      else if((status == AVAHI_CLIENT_S_COLLISION || status == AVAHI_CLIENT_S_REGISTERING) && self.group) avahi_entry_group_reset(self.group);
    } catch(std::exception const &error) {
      self.fail(error.what());
    }
  }
};

advertisement::advertisement(boost::asio::any_io_executor executor, std::string name, std::uint16_t port,
  std::function<void(std::string)> on_error)
  : state{std::make_unique<implementation>(std::move(executor), std::move(name), port, std::move(on_error))} {
  if(state->name.empty() || state->name.size() > 63 || port == 0) throw std::invalid_argument{"invalid Avahi service identity"};
  state->poll = avahi_threaded_poll_new();
  if(!state->poll) throw std::runtime_error{"Avahi poll creation failed"};
  int error{0};
  state->client = avahi_client_new(avahi_threaded_poll_get(state->poll), static_cast<AvahiClientFlags>(0), implementation::client_changed, state.get(), &error);
  if(!state->client) throw std::runtime_error{"cannot connect to Avahi daemon: " + std::string{avahi_strerror(error)}};
  if(avahi_threaded_poll_start(state->poll) != 0) throw std::runtime_error{"Avahi poll start failed"};
  state->poll_started = true;
}

advertisement::~advertisement() = default;

} // namespace librespot::discovery
