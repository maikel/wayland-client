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
  class connection;

  class connection_handle {
   private:
    connection* connection_;

    friend struct sio::async::close_t;
    any_sender_of<> close(sio::async::close_t) const;

   public:
    explicit connection_handle(connection& connection) noexcept
      : connection_(&connection) {
    }

    connection* get_resource() const noexcept {
      return connection_;
    }

    template <class Tp>
    any_sender_of<> send(Tp& msg) {
      std::span<std::byte> buffer(reinterpret_cast<std::byte*>(&msg), sizeof(Tp));
      return send(buffer);
    }

    any_sender_of<> send(std::span<std::byte> buffer);
    any_sequence_of<std::span<std::byte>> subscribe();

    any_sender_of<> receive_all();
  };

  class connection {
   public:
    explicit connection(exec::io_uring_context& context);
    ~connection();

   private:
    friend class connection_handle;
    struct impl;
    std::unique_ptr<impl> impl_;

    friend struct sio::async::open_t;
    any_sender_of<connection_handle> open(sio::async::open_t);
  };

}