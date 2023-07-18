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
#pragma once

#include "./any_sender_of.hpp"

#include <sio/net_concepts.hpp>

#include <exec/linux/io_uring_context.hpp>

#include <memory>
#include <span>

namespace wayland {
  struct connection_context;

  class connection_handle {
   private:
    connection_context* connection_{};

   public:
    connection_handle();
    explicit connection_handle(connection_context& connection);

    template <class Tp>
    any_sender_of<> send(Tp& msg) {
      std::span<std::byte> buffer(reinterpret_cast<std::byte*>(&msg), sizeof(Tp));
      return send(buffer);
    }

    template <class Tp>
    any_sender_of<> send_with_fd(Tp& msg, int fd) {
      std::span<std::byte> buffer(reinterpret_cast<std::byte*>(&msg), sizeof(Tp));
      return send_with_fd(buffer, fd);
    }

    any_sender_of<> send(std::span<std::byte> buffer);
    any_sender_of<> send_with_fd(std::span<std::byte> buffer, int fd);

    any_sequence_of<std::span<std::byte>> subscribe();

    auto operator<=>(const connection_handle&) const = default;
  };

  class connection {
   public:
    explicit connection(exec::io_uring_context& context);

   private:
    exec::io_uring_context* context_;

    friend struct sio::async::use_t;
    any_sequence_of<connection_handle> use(sio::async::use_t) const;
  };

}