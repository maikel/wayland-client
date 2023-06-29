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
#include "./message_header.hpp"

#include <sio/io_uring/socket_handle.hpp>
#include <sio/local/stream_protocol.hpp>
#include <sio/intrusive_list.hpp>
#include <sio/tap.hpp>

#include <exec/async_scope.hpp>
#include <exec/sequence/empty_sequence.hpp>
#include <exec/repeat_effect_until.hpp>
#include <exec/variant_sender.hpp>

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
    auto iter = std::ranges::begin(impl_->receivers_);
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
             log("connection", "Connecting to the Wayland server socket '{}'...", path.c_str());
             return sio::tap(
               sio::async::connect(sock, local::endpoint(path.c_str())),
               stdexec::just() | stdexec::then([this] {
                 log("connection", "Connected to the Wayland server.");
                 return connection_handle{*this};
               }));
           });
  }

  namespace {
    template <class ThenSender, class ElseSender>
    exec::variant_sender<ThenSender, ElseSender>
      if_then_else(bool condition, ThenSender then, ElseSender otherwise) {
      if (condition) {
        return then;
      }
      return otherwise;
    }

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

    void log_buffer(std::string prefix, std::span<std::byte> buffer) {
      std::string first = prefix;
      while (buffer.size() >= 4) {
        std::array<std::string, 8> columns{};
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

    void log_send_buffer(std::span<std::byte> buffer) {
      log_buffer("C->S:", buffer);
    }

    void log_recv_buffer(std::span<std::byte> buffer) {
      log_buffer("S->C:", buffer);
    }

    socket_handle get_handle(socket_type& socket) {
      return socket_handle{socket.context_, socket.fd_, socket.protocol_};
    }

    enum process_result {
      disconnected,
      need_more,
      processed,
    };

    process_result process_buffer(std::span<std::byte>* buffer) {
      if (buffer->empty()) {
        return disconnected;
      }
      while (buffer->size() >= sizeof(message_header)) {
        message_header header{};
        std::memcpy(&header, buffer->data(), sizeof(message_header));
        if (buffer->size() < header.message_length) {
          return need_more;
        }
        log("connection", "Received message to Object ID: {}, Message length: {}, Opcode: {}",
            header.object_id,
            header.message_length,
            header.opcode);
        std::span<std::byte> message = buffer->subspan(0, header.message_length);
        log_recv_buffer(message);
        std::span<std::byte> rest = buffer->subspan(header.message_length);
        std::memmove(buffer->data(), rest.data(), rest.size());
        *buffer = buffer->subspan(0, rest.size());
      }
      return need_more;
    }

    auto receive_all_messages_until_disconnect(socket_handle socket, std::span<std::byte> buffer) {
      return stdexec::just(socket, buffer, std::span<std::byte>{}) //
           | stdexec::let_value(
               [](socket_handle socket, std::span<std::byte> buffer, std::span<std::byte>& filled) {
                 return sio::async::read_some(socket, buffer.subspan(filled.size())) //
                      | stdexec::then([&filled, buffer](int n) -> std::span<std::byte>* {
                          if (n == 0) {
                            filled = std::span<std::byte>{};
                          } else {
                            filled = buffer.subspan(0, filled.size() + n);
                          }
                          return &filled;
                        })                             //
                      | stdexec::then(&process_buffer) //
                      | stdexec::then([](process_result res) { return res == disconnected; })
                      | exec::repeat_effect_until();
               });
    }
  }

  any_sender_of<> connection_handle::close(async::close_t) const {
    return async::close(get_handle(connection_->impl_->socket_));
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

  any_sequence_of<std::span<std::byte>> connection_handle::receive() {
    return exec::empty_sequence();
  }

  any_sender_of<> connection_handle::receive_all() {
    return receive_all_messages_until_disconnect(
      get_handle(connection_->impl_->socket_), connection_->impl_->buffer_);
  }

} // namespace wayland