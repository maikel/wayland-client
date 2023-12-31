#include "./window.hpp"

#include <sio/sequence/merge_each.hpp>
#include <sio/sequence/then_each.hpp>
#include <sio/sequence/let_value_each.hpp>

#include <sio/async_channel.hpp>

namespace wayland {
  template <class Fn>
  auto then_use(Fn&& fn) {
    return sio::then_each(std::forward<Fn>(fn)) | sio::merge_each();
  }

  using mouse_channel =
    sio::async_channel<stdexec::completion_signatures<stdexec::set_value_t(mouse_event)>>;

  using mouse_channel_handle =
    sio::async_channel_handle<stdexec::completion_signatures<stdexec::set_value_t(mouse_event)>>;

  struct window_context {
    display_handle display{};
    mouse_channel_handle mouse_events{};
    wayland::registry registry{};
    wayland::compositor compositor{};
    wayland::shm shm{};
    wayland::shm_pool shm_pool{};
    wayland::buffer buffer{};
    wayland::surface surface{};
    wayland::xdg_wm_base wm_base{};
    wayland::xdg_surface xdg_surface{};
    wayland::xdg_toplevel xdg_toplevel{};
  };

  auto make_buffer(window_context& ctx, wayland::registry r, dynamic_extents<2> extents) {
    return                                                  //
      just_invoke([r]() { return r.bind<wayland::shm>(); }) //
      | sio::merge_each()                                   //
      | then_use([&ctx, extents](wayland::shm shm) {
          ctx.shm = shm;
          return shm.create_pool(fd, size);
        }) //
      | then_use([&ctx](wayland::shm_pool pool) {
          ctx.shm_pool = pool;
          return pool.create_buffer(0, wayland::dynamic_extents<2>{640, 480});
        });
  }

  any_sequence_of<window> make_window(exec::io_uring_context& ioc) {
    return                                                     //
      stdexec::just(wayland::display{}, mouse_channel{}, &ioc) //
      | stdexec::let_value(
        [](wayland::display& d, mouse_channel& mouse, exec::io_uring_context* ioc) {
          d = wayland::display{*ioc};
          return stdexec::just(sio::zip(sio::async::use(d), sio::async::use(mouse)));
        })                //
      | sio::merge_each() //
      | sio::then_each([](display_handle display, mouse_channel_handle mouse_events) {
          return window_context{display, mouse_events};
        }) //
      | sio::let_value_each([](window_context& window_ctx) {
          return stdexec::just(
            sio::zip(stdexec::just(&window_ctx), window_ctx.display.get_registry()));
        })                //
      | sio::merge_each() //
      | then_use([](window_context* window_ctx, wayland::registry registry) {
          window_ctx->registry = registry;
          auto get_surface = registry.bind<wayland::compositor>() //
                           | then_use([window_ctx](wayland::compositor compositor) {
                               window_ctx->compositor = compositor;
                               return compositor.create_surface();
                             });
          auto get_buffer = make_buffer(*window_ctx, registry, dynamic_extents<2>{640, 480});
          return sio::zip(
            stdexec::just(window_ctx),
            registry.bind<wayland::xdg_wm_base>(),
            std::move(get_surface));
        }) //
      | then_use(
        [](window_context* window_ctx, wayland::xdg_wm_base wm_base, wayland::surface surface) {
          window_ctx->wm_base = wm_base;
          window_ctx->surface = surface;
          // return window{window_ctx};
          return sio::zip(stdexec::just(window_ctx), wm_base.get_xdg_surface(surface));
        })
      | then_use([](window_context* window_ctx, wayland::xdg_surface xdg_surface) {
          window_ctx->xdg_surface = xdg_surface;
          return sio::zip(stdexec::just(window_ctx), xdg_surface.get_toplevel());
        })
      | sio::then_each([](window_context* window_ctx, wayland::xdg_toplevel xdg_toplevel) {
          window_ctx->xdg_toplevel = xdg_toplevel;
          return window{window_ctx};
          // return sio::zip(stdexec::just(window_ctx), xdg_surface.get_toplevel());
        });
  }

  any_sequence_of<mouse_event> window::on_mouse_event() {
    return context_->mouse_events.subscribe();
  }
}