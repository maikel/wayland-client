#include "../source/protocol.hpp"
#include "../source/logging.hpp"

#include <exec/when_any.hpp>

#include <sio/sequence/then_each.hpp>
#include <sio/sequence/ignore_all.hpp>
#include <sio/sequence/merge_each.hpp>

#include <iostream>

#include <cstdlib>

template <class Sender>
void run_on(exec::io_uring_context& ctx, Sender&& sndr) {
  stdexec::sync_wait(
    exec::when_any(std::forward<Sender>(sndr), ctx.run())
    | stdexec::then([](auto&&...) noexcept {}));
}

// void* operator new(std::size_t n) {
//   void* ptr = std::malloc(n);
//   wayland::log("allocator", "Allocate {} bytes at {}", n, ptr);
//   return ptr;
// }

// void operator delete(void* p) noexcept {
//   wayland::log("allocator", "Free memory at {}", p);
//   std::free(p);
// }

// void* operator new[](std::size_t s) {
//   void* ptr = std::malloc(s);
//   wayland::log("allocator", "Allocate {} bytes at {}", s, ptr);
//   return ptr;
// }

// void operator delete[](void* p) noexcept {
//   wayland::log("allocator", "Free memory at {}", p);
//   std::free(p);
// }

int main() {
  exec::io_uring_context io_context{};
  wayland::display display{io_context};
  auto use = sio::async::use_resources(
    [](wayland::display_handle d) {
      std::cout << "display used\n";
      return d.get_registry()
           | sio::then_each([](wayland::registry r) { return r.bind<wayland::compositor>(); }) //
           | sio::merge_each()                                                                 //
           | sio::let_value_each([d](wayland::compositor c) {
              return d.sync();
           }) //
           | sio::ignore_all();
    },
    display);
  run_on(io_context, std::move(use));
}
