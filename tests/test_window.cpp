#include "../source/window.hpp"

#include <sio/sequence/ignore_all.hpp>

#include <exec/when_any.hpp>

#include <fmt/core.h>

using namespace std::chrono_literals;

int main() {
  exec::io_uring_context io_context{};
  auto use = wayland::make_window(io_context) //
           | sio::let_value_each([&](wayland::window window) {
               auto listen_to_mouse =
                 window.on_mouse_event() //
                 | sio::then_each([](wayland::mouse_event ev) {
                     fmt::print("mouse position: ({}, {})\n", ev.position.x, ev.position.y);
                   })
                 | sio::ignore_all();
               auto timeout = exec::schedule_after(io_context.get_scheduler(), 5s);
               auto timed_listen = exec::when_any(std::move(listen_to_mouse), std::move(timeout));
               return timed_listen;
             })
           | sio::ignore_all();


  stdexec::sync_wait(exec::when_any(std::move(use), io_context.run()));
}