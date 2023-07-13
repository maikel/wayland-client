#include "./renderer.hpp"

#include <sio/sequence/then_each.hpp>
#include <sio/sequence/merge_each.hpp>

namespace wayland {
  render_context::render_context(exec::io_uring_context& ctx)
    : connection_(ctx) {
  }

  any_sequence_of<renderer> render_context::run(sio::async::run_t) {
    return                         //
      sio::async::run(connection_) //
      | sio::then_each([this](connection_handle handle) {
          this->display_ = display(handle);
          return sio::async::run(display_)       //
               | sio::then_each([this](auto display) {
                   return display.get_registry() //
                        | sio::then_each([this](wayland::registry registry) {
                            this->registry_ = registry;
                            return sio::let_value_each(
                              sio::zip(
                                registry.bind<wayland::compositor>(),
                                registry.bind<wayland::shm>()),
                              [this](wayland::compositor compositor, wayland::shm shm) {
                                return stdexec::just_stopped();
                              });
                          })
                        | sio::merge_each();
                 })
               | sio::merge_each();
        }) //
      | sio::merge_each();
  }

} // namespace wayland