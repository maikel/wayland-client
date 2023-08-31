#include "../source/renderer.hpp"

#include <sio/sequence/ignore_all.hpp>

#include <iostream>

int main() {
  exec::io_uring_context io_context{};
  wayland::renderer_options opts{600, 480, wayland::color::black()};
  wayland::renderer renderer{io_context, opts};
  auto use = sio::async::use(renderer) | sio::then_each([](wayland::renderer_handle r) {
               r.draw_rectangle({0, 0}, {150, 15}, wayland::color{0x00, 0x00, 0x99, 0xFF});
               r.draw_rectangle({150, 15}, {300, 30}, wayland::color{0x00, 0x99, 0x00, 0xFF});
               r.draw_text("Hello, World!", {100, 20}, wayland::color{0xFF, 0xFF, 0xFF, 0xFF});
               r.render();
             })
           | sio::ignore_all();
  stdexec::sync_wait(std::move(use));
}