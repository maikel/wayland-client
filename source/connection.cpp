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
#include "./connection.hpp"

#include "./any_sender_of.hpp"
#include "./logging.hpp"

#include <sio/io_uring/socket_handle.hpp>
#include <sio/local/stream_protocol.hpp>
#include <sio/intrusive_list.hpp>
#include <sio/tap.hpp>

#include <exec/async_scope.hpp>

#include <ranges>

namespace wayland {
  using namespace sio;

  struct receiver_base {
    receiver_base* next_;
    receiver_base* prev_;
    any_sequence_receiver<std::span<std::byte>> receiver_;
  };

  using socket_type = io_uring::socket<local::stream_protocol>;
  using socket_handle = io_uring::socket_handle<local::stream_protocol>;

  struct connection::impl {
    explicit impl(exec::io_uring_context& context)
      : buffer_{}
      , socket_{context, local::stream_protocol()}
      , receivers_{}
      , scope_{} {
    }

    alignas(4096) std::array<std::byte, 8192> buffer_;
    socket_type socket_;
    intrusive_list<&receiver_base::next_, &receiver_base::prev_> receivers_;
    exec::async_scope scope_;
  };

  connection::~connection() = default;

  connection::connection(exec::io_uring_context& context)
    : impl_(std::make_unique<impl>(context)) {
  }

  any_sender_of<connection_handle> connection::open(async::open_t) {
    return async::open(impl_->socket_) //
         | stdexec::let_value([this](socket_handle sock) {
             const char* xdg_runtime_dir = std::getenv("XDG_RUNTIME_DIR");
             if (!xdg_runtime_dir) {
               throw std::runtime_error("XDG_RUNTIME_DIR not set");
             }
             const char* wayland_display = std::getenv("WAYLAND_DISPLAY");
             if (!wayland_display) {
               throw std::runtime_error("WAYLAND_DISPLAY not set");
             }
             const std::filesystem::path path =
               std::filesystem::path(xdg_runtime_dir) / wayland_display;
             if (!std::filesystem::exists(path)) {
               throw std::runtime_error("Wayland socket does not exist");
             }
             log("connection", "Connecting to Wayland socket '{}'.", path.c_str());
             return sio::tap(
               sio::async::connect(sock, local::endpoint(path.c_str())),
               stdexec::just(connection_handle{*this}));
           });
  }

  namespace {
    socket_handle get_handle(socket_type& socket) {
      return socket_handle{socket.context_, socket.fd_, socket.protocol_};
    }
  }

  any_sender_of<> connection_handle::close(async::close_t) const {
    return async::close(get_handle(connection_->impl_->socket_));
  }

  struct message_header {
    uint32_t object_id;
    uint16_t opcode;
    uint16_t message_length;
  };

  std::array<char, 2> to_hex(std::byte b) {
    static constexpr const char map[] = "0123456789ABCDEF";
    const auto c = std::bit_cast<unsigned char>(b);
    std::array<char, 2> result;
    result[0] = map[(c & 0xF0) >> 4];
    result[1] = map[c & 0x0F];
    return result;
  }

  std::string to_hex(std::span<std::byte, 4> bs) {
    std::string result(8, '\0');
    int counter = 0;
    for (std::byte b: std::ranges::views::reverse(bs)) {
      auto chars = to_hex(b);
      result[counter++] = chars[0];
      result[counter++] = chars[1];
    }
    return result;
  }

  char to_char(std::byte b) {
    if (std::isprint(static_cast<int>(b))) {
      return static_cast<char>(b);
    } else {
      return '.';
    }
  }

  std::string to_chars(std::span<std::byte, 4> b) {
    return std::string{to_char(b[0]), to_char(b[1]), to_char(b[2]), to_char(b[3])};
  }

  void consume_front(std::span<std::byte>& buffer, std::string& hex, std::string& ascii) {
    if (buffer.size() >= 4) {
      std::span<std::byte, 4> column = buffer.subspan<0, 4>();
      hex = to_hex(column);
      ascii = to_chars(column);
      buffer = buffer.subspan(4);
    }
  }

  void log_send_buffer(std::span<std::byte> buffer) {
    std::array<std::string, 8> columns{};
    std::string first = "C->S:";
    while (buffer.size() >= 4) {
      consume_front(buffer, columns[0], columns[4]);
      consume_front(buffer, columns[1], columns[5]);
      consume_front(buffer, columns[2], columns[6]);
      consume_front(buffer, columns[3], columns[7]);
      log(
        "connection",
        "{:5} {:8} {:8} {:8} {:8} | {:4} {:4} {:4} {:4}",
        first,
        columns[0],
        columns[1],
        columns[2],
        columns[3],
        columns[4],
        columns[5],
        columns[6],
        columns[7]);
      first = "";
    }
  }

  any_sender_of<> connection_handle::send(std::span<std::byte> buffer) {
    if (buffer.size() < sizeof(message_header)) {
      throw std::runtime_error("Buffer too small");
    }
    message_header header{};
    std::memcpy(&header, buffer.data(), sizeof(message_header));
    log(
      "connection",
      "Sending message. Object ID: {}, Message length: {}, Opcode: {}",
      header.object_id,
      header.message_length,
      header.opcode);
    return async::write(get_handle(connection_->impl_->socket_), buffer) //
         | stdexec::then([buffer](std::size_t) { log_send_buffer(buffer); });
  }

} // namespace wayland