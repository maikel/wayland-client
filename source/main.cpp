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
#include "./protocol.hpp"
#include "./logging.hpp"
#include "./message_header.hpp"
#include "./renderer.hpp"
#include "./mdspan.hpp"

#include <sio/sequence/ignore_all.hpp>

#include <exec/when_any.hpp>

template <class Sender>
void run_on(exec::io_uring_context& ctx, Sender&& sndr) {
  stdexec::sync_wait(
    exec::when_any(std::forward<Sender>(sndr), ctx.run())
    | stdexec::then([](auto&&...) noexcept {}));
}

namespace stdx = std::experimental;

template <std::size_t... Is>
constexpr auto make_dynamic_extents(std::index_sequence<Is...>)
  -> stdx::extents<int, ((void)Is, std::dynamic_extent)...> {
  return {};
}

template <std::size_t Rank>
using dynamic_extents = decltype(make_dynamic_extents(std::make_index_sequence<Rank>{}));

int main() {
  using namespace wayland;
  exec::io_uring_context io_context{};

  wayland::render_context renderer{io_context};
  run_on(io_context, sio::async::use(renderer) | sio::ignore_all());

  // connection conn{context};
  // auto using_connection =
  //   sio::async::run(conn) //
  //   | sio::let_value_each([&](connection_handle h) {
  //       return sio::async::use_resources(
  //         [](wayland::display display) {
  //           return display.use_registry([](wayland::registry registry) {
  //             return sio::async::use_resources(
  //               [](wayland::compositor compositor, wayland::shm shm) {
  //                 return stdexec::just();
  //               },
  //               registry.bind<wayland::compositor>(),
  //               registry.bind<wayland::shm>());
  //           });
  //         },
  //         sio::make_deferred<wayland::display>(h));
  //     })
  //   | sio::ignore_all();

  // run_on(context, std::move(using_connection));
}
