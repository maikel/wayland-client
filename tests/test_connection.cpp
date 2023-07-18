#include "../source/connection.hpp"
#include "../source/protocol.hpp"

#include <exec/when_any.hpp>
#include <iostream>

template <class Sender>
void run_on(exec::io_uring_context& ctx, Sender&& sndr) {
  stdexec::sync_wait(
    exec::when_any(std::forward<Sender>(sndr), ctx.run())
    | stdexec::then([](auto&&...) noexcept {}));
}

int main() {
  exec::io_uring_context io_context{};
  wayland::connection conn{io_context};
  auto use = sio::async::use_resources(
    [](wayland::connection_handle h) {
      wayland::display display{h};
      return sio::async::use_resources(
        [](wayland::display_handle d) {
          std::cout << "display used\n";
          return stdexec::just();
        },
        display);
      // return stdexec::just();
    },
    conn);
  run_on(io_context, std::move(use));
}