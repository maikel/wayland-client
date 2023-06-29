/*
 * Copyright (c) 2023 Maikel Nadolski
 *
 * Licensed under the Apache License Version 2.0 with LLVM Exceptions
 * (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 *   https://llvm.org/LICENSE.txt
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "./any_sender_of.hpp"
#include "./connection.hpp"
#include "./logging.hpp"

#include <sio/sequence/ignore_all.hpp>

#include <exec/when_any.hpp>

// #include <sio/sequence/then_each.hpp>

// #include "sio/local/stream_protocol.hpp"
// #include "sio/io_uring/socket_handle.hpp"
// #include "sio/net_concepts.hpp"
// #include "sio/tap.hpp"

// #include <stdexec/execution.hpp>
// #include <exec/async_scope.hpp>
// #include <exec/repeat_effect_until.hpp>

// #include <ranges>
// #include <iostream>

// using local_socket = sio::io_uring::socket_handle<sio::local::stream_protocol>;

// std::array<char, 2> to_chars(std::byte b) {
//   static constexpr const char map[] = "0123456789ABCDEF";
//   const auto c = std::bit_cast<unsigned char>(b);
//   std::array<char, 2> result;
//   result[0] = map[(c & 0xF0) >> 4];
//   result[1] = map[c & 0x0F];
//   return result;
// }

// bool print_bytes_(std::span<std::byte> buffer, int in) {
//   if (buffer.size() == 0) {
//     std::cout << "Connection closed.\n";
//     return true;
//   }
//   while (buffer.size() >= 4) {
//     auto row = buffer.subspan(0, 4);
//     if (in) {
//       std::cout << "> ";
//     } else {
//       std::cout << "< ";
//     }
//     for (std::byte b: std::ranges::views::reverse(row)) {
//       auto [l, r] = to_chars(b);
//       std::cout << l << r;
//     }
//     std::cout << '\n';
//     buffer = buffer.subspan(4);
//   }
//   return buffer.size() > 0;
// }

// bool print_bytes(std::span<std::byte> buffer) {
//   return print_bytes_(buffer, 1);
// }

// auto print_all_input(local_socket socket) {
//   return stdexec::just(socket, std::array<std::byte, 1024>{}) //
//        | stdexec::let_value([](local_socket socket, std::span<std::byte> buffer) {
//            std::cout << "Connected to wayland.\n";
//            std::cout << "Read input.\n";
//            std::cout << std::string(78, '-') << '\n';
//            return sio::async::read_some(socket, buffer)                           //
//                 | stdexec::then([buffer](int n) { return buffer.subspan(0, n); }) //
//                 | stdexec::then(&print_bytes)                                     //
//                 | stdexec::then([](bool b) {
//                     std::cout << std::string(78, '-') << '\n';
//                     return b;
//                   })
//                 | exec::repeat_effect_until();
//          });
// }

// struct registry {
//   uint32_t id_;
// };

// template <class Tp>
// void copy_to2(std::span<std::byte>& buffer, const Tp& data)
// {
//   std::memcpy(buffer.data(), &data, sizeof(Tp));
//   buffer = buffer.subspan(sizeof(Tp));
// }

// template <class... Ts>
// void copy_to(std::span<std::byte> buffer, const Ts&... datas) {
//   if (buffer.size() < (sizeof(datas) + ...)) {
//     throw std::logic_error("Size of buffer is too small");
//   }
//   (copy_to2(buffer, datas), ...);
// }

// struct display;
// struct display_handle {
//   display* resource_;

//   wayland::any_sender_of<> close(sio::async::close_t);
// };

// using object_id = uint32_t;
// using size_type = uint16_t;

// struct message {
//   uint32_t id;
//   uint16_t size;
//   uint16_t method_id;
// };

// struct display {
//   uint32_t id_;
//   sio::io_uring::socket_resource<sio::local::stream_protocol> socket_resource_;
//   std::optional<socket_t> socket_;

//   any_sender_of<display_handle> open(sio::async::open_t);

//   auto get_registry(uint32_t name) {
//     std::array<std::byte, 12> data;
//     copy_to(data, uint32_t{id_}, uint16_t{1}, uint16_t{12}, name);
//     return stdexec::just(socket_, data) //
//            | stdexec::let_value([](local_socket socket, std::span<std::byte> buffer) {
//               print_bytes_(buffer, 0);
//               return sio::async::write(socket, buffer);
//            });
//   }
// };

// auto on_connection(local_socket socket) {
//   display disp{1, socket};
//   return stdexec::when_all(print_all_input(socket), disp.get_registry(2));
// }

template <class Sender>
void run_on(exec::io_uring_context& ctx, Sender&& sndr) {
  stdexec::sync_wait(
    exec::when_any(std::forward<Sender>(sndr), ctx.run())
    | stdexec::then([](auto&&...) noexcept {}));
}

int main() {
  using namespace wayland;
  exec::io_uring_context context{};

  connection conn{context};
  auto run = sio::async::run(conn)                                                    //
           | sio::let_value_each([](connection_handle h) { return stdexec::just(); }) //
           | sio::ignore_all();

  run_on(context, std::move(run));

  // auto socket = sio::io_uring::make_deferred_socket(&context, sio::local::stream_protocol());
  // auto connect = sio::async::use_resources(
  //   [](display_t display) {
  //     return sio::tap(
  //       sio::async::connect(s, sio::local::endpoint("/run/user/1000/wayland-0")), on_connection(s));
  //   },
  //   make_deferred<display>());

  // stdexec::sync_wait(
  //   exec::when_any(connect, context.run()) | stdexec::then([](auto &&...) noexcept {}));
}
