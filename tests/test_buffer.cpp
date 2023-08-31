#include <sys/mman.h>
#include <unistd.h>

#include "../source/protocol.hpp"
#include "../source/logging.hpp"

#include <exec/when_any.hpp>

#include <sio/sequence/then_each.hpp>
#include <sio/sequence/ignore_all.hpp>
#include <sio/sequence/merge_each.hpp>

template <class Sender>
void run_on(exec::io_uring_context& ctx, Sender&& sndr) {
  stdexec::sync_wait(
    exec::when_any(std::forward<Sender>(sndr), ctx.run())
    | stdexec::then([](auto&&...) noexcept {}));
}

template <class Fn>
auto then_use(Fn&& fn) {
  return sio::then_each(std::forward<Fn>(fn)) | sio::merge_each();
}

void throw_error_code_if(bool condition, int ec) {
  if (condition) {
    throw std::system_error{ec, std::system_category()};
  }
}

int main() {
  const exec::safe_file_descriptor sfd{memfd_create("buffers", 0)};
  const int fd = sfd.native_handle();
  const int32_t size = 640 * 480 * 8;
  throw_error_code_if(fd == -1, errno);
  throw_error_code_if(::ftruncate(fd, size) == -1, errno);

  exec::io_uring_context io_context{};
  wayland::display display{io_context};
  wayland::display_handle disp{};
  wayland::registry reg{};
  wayland::buffer buf{};
  wayland::surface surface{};
  auto use =
    sio::async::use(display) //
    | then_use([&disp](wayland::display_handle d) {
        disp = d;
        return d.get_registry(); //
      })                         //
    | then_use([&reg](wayland::registry r) {
        reg = r;
        return r.bind<wayland::shm>();
      })                                                                           //
    | then_use([fd, size](wayland::shm shm) { return shm.create_pool(fd, size); }) //
    | then_use([](wayland::shm_pool pool) {
        return pool.create_buffer(0, wayland::dynamic_extents<2>{640, 480});
      }) //
    | then_use([&](wayland::buffer b) {
        buf = b;
        return reg.bind<wayland::compositor>();
      })                                                                 //
    | then_use([](wayland::compositor c) { return c.create_surface(); }) //
    | sio::let_value_each([&](wayland::surface s) {
        surface = s;
        return stdexec::when_all(
          s.attach(buf, {0, 0}),
          s.damage({0, 0}, wayland::dynamic_extents<2>{640, 480}),
          s.commit(),
          disp.sync());
      }) //
    | sio::ignore_all();

  run_on(io_context, std::move(use));
}
