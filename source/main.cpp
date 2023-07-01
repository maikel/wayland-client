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

#include <sio/sequence/ignore_all.hpp>

#include <exec/when_any.hpp>

template <class Sender>
void run_on(exec::io_uring_context& ctx, Sender&& sndr) {
  stdexec::sync_wait(
    exec::when_any(std::forward<Sender>(sndr), ctx.run())
    | stdexec::then([](auto&&...) noexcept {}));
}

struct get_registry {
  wayland::message_header header;
  uint32_t name;
};

int main() {
  using namespace wayland;
  exec::io_uring_context context{};

  connection conn{context};
  auto using_connection =
    sio::async::run(conn) //
    | sio::let_value_each([](connection_handle h) {
        wayland::display display(h);
        // return stdexec::let_value(display.get_registry(2), [](registry r) {
        //   return r.on_global() //
        //        | stdexec::then_each([](name which, std::string_view interface, version ver) {
        //            log("global: {}" interface);
        //          }) //
        //        | sio::ignore_all();
        // });
        return stdexec::just();
      })
    | sio::ignore_all();

  run_on(context, std::move(using_connection));
}
