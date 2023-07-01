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
#include <sio/sequence/iterate.hpp>
#include <sio/sequence/then_each.hpp>
#include <sio/sequence/let_value_each.hpp>
#include <sio/sequence/ignore_all.hpp>
#include <sio/tap.hpp>

#include <exec/async_scope.hpp>
#include <exec/finally.hpp>
#include <exec/sequence/empty_sequence.hpp>
#include <exec/repeat_effect_until.hpp>
#include <exec/variant_sender.hpp>

#include <ranges>

namespace wayland {
  using namespace sio;

  struct buffer_sentinel { };

  message_header extract_header(std::span<std::byte> bytes) {
    message_header header{};
    if (bytes.size() >= sizeof(message_header)) {
      std::memcpy(&header, bytes.data(), sizeof(message_header));
    }
    return header;
  }

  struct buffer_iterator {
    std::span<std::byte> range_;
    std::ptrdiff_t position_;

    using iterator_category = std::forward_iterator_tag;
    using value_type = std::span<std::byte>;
    using difference_type = std::ptrdiff_t;

    explicit buffer_iterator(std::span<std::byte> range, std::ptrdiff_t position = 0)
      : range_(range)
      , position_(position) {
    }

    buffer_iterator& operator++() noexcept {
      message_header header = extract_header(range_.subspan(position_));
      position_ += header.message_length;
      return *this;
    }

    buffer_iterator operator++(int) noexcept {
      buffer_iterator tmp = *this;
      ++*this;
      return tmp;
    }

    std::span<std::byte> operator*() const noexcept {
      message_header header = extract_header(range_.subspan(position_));
      return range_.subspan(position_, header.message_length);
    }

    bool operator==(const buffer_sentinel&) const noexcept {
      return range_.size() < position_ + sizeof(message_header);
    }

    bool operator!=(const buffer_sentinel&) const noexcept {
      return !(*this == buffer_sentinel{});
    }
  };

  bool operator==(const buffer_sentinel&, const buffer_iterator& iter) noexcept {
    return iter == buffer_sentinel{};
  }

  bool operator!=(const buffer_sentinel&, const buffer_iterator& iter) noexcept {
    return !(iter == buffer_sentinel{});
  }

  static_assert(std::input_iterator<buffer_iterator>);
  static_assert(std::sentinel_for<buffer_sentinel, buffer_iterator>);

  struct receiver_base {
    any_sequence_receiver<std::span<std::byte>> receiver_;
    receiver_base* next_{};
    receiver_base* prev_{};
  };

  using socket_type = io_uring::socket<local::stream_protocol>;
  using socket_handle = io_uring::socket_handle<local::stream_protocol>;

  struct connection_context {
    explicit connection_context(exec::io_uring_context& context)
      : buffer_{}
      , socket_{context, local::stream_protocol()}
      , receivers_{}
      , scope_{} {
    }

    alignas(4096) std::array<std::byte, 8192> buffer_;
    socket_type socket_;
    intrusive_list<&receiver_base::next_, &receiver_base::prev_> receivers_;
    stdexec::in_place_stop_source stop_source_;
    exec::async_scope scope_;
    int ref_counter_{0};
    void* complete_close_data_{nullptr};
    void (*complete_close_)(void*) noexcept = nullptr;
  };

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

    auto notify_all_listeners(
      exec::async_scope& scope,
      intrusive_list<&receiver_base::next_, &receiver_base::prev_>& listeners,
      std::span<std::byte> message) {
      return sio::iterate(listeners) //
           | sio::then_each([&scope, message](receiver_base& listener) {
               scope.spawn(exec::set_next(listener.receiver_, stdexec::just(message)));
             }) //
           | sio::ignore_all();
    }

    auto process_buffer(
      exec::async_scope& scope,
      intrusive_list<&receiver_base::next_, &receiver_base::prev_>& listeners,
      std::span<std::byte> buffer) {
      return sio::reduce(
        sio::iterate(std::ranges::subrange(buffer_iterator{buffer}, buffer_sentinel{})) //
          | sio::let_value_each([&](std::span<std::byte> message) {
              message_header header{};
              std::memcpy(&header, message.data(), sizeof(message_header));
              log(
                "connection",
                "Received message to Object ID: {}, Message length: {}, Opcode: {}",
                header.object_id,
                header.message_length,
                header.opcode);
              log_recv_buffer(message);
              return sio::tap(
                notify_all_listeners(scope, listeners, message),
                stdexec::just(header.message_length));
            }),
        0);
    }

    auto receive_all_messages_until_disconnect(connection_context& context) {
      return stdexec::just(std::span<std::byte>{}) //
           | stdexec::let_value(
               [&context](std::span<std::byte>& filled) {
                 auto socket = get_handle(context.socket_);
                 std::span buffer{context.buffer_};
                 return sio::async::read_some(socket, buffer.subspan(filled.size())) //
                      | stdexec::let_value([&context, &filled, buffer](int n) {
                          filled = buffer.subspan(0, filled.size() + n);
                          return exec::finally(
                            if_then_else(
                              n == 0,
                              stdexec::just() | stdexec::then([] {
                                log("connection", "Disconnected from Wayland server.");
                                return -1;
                              }),
                              process_buffer(context.scope_, context.receivers_, filled)),
                            context.scope_.on_empty());
                        }) //
                      | stdexec::then([&](int n) {
                          auto consumed = filled.subspan(0, n);
                          auto rest = filled.subspan(n);
                          std::memmove(filled.data(), rest.data(), rest.size());
                          filled = filled.subspan(0, rest.size());
                          return n < 0;
                        })
                      | exec::repeat_effect_until();
               });
    }
  }

  template <class Receiver>
  struct subscribe_operation;

  template <class Receiver>
  struct subscribe_receiver {
    using is_receiver = void;
    subscribe_operation<Receiver>* op_;

    stdexec::env_of_t<Receiver> get_env(stdexec::get_env_t) const noexcept {
      return stdexec::get_env(op_->rcvr_);
    }

    template <class Item>
    exec::next_sender_of_t<Receiver, Item> set_next(exec::set_next_t, Item&& item) {
      return exec::set_next(op_->rcvr_, std::forward<Item>(item));
    }

    void set_value(stdexec::set_value_t) && noexcept {
      op_->subscriptions_->erase(&op_->this_subscription_);
      stdexec::set_value(std::move(op_->rcvr_));
    }

    void set_stopped(stdexec::set_stopped_t) && noexcept {
      op_->subscriptions_->erase(&op_->this_subscription_);
      stdexec::set_stopped(std::move(op_->rcvr_));
    }

    void set_error(stdexec::set_error_t, std::exception_ptr e) && noexcept {
      op_->subscriptions_->erase(&op_->this_subscription_);
      stdexec::set_error(std::move(op_->rcvr_), std::move(e));
    }

    void set_error(stdexec::set_error_t, std::error_code e) && noexcept {
      op_->subscriptions_->erase(&op_->this_subscription_);
      stdexec::set_error(std::move(op_->rcvr_), std::move(e));
    }
  };

  template <class Receiver>
  struct subscribe_operation {
    Receiver rcvr_;
    subscribe_receiver<Receiver> sub_rcvr_;
    intrusive_list<&receiver_base::next_, &receiver_base::prev_>* subscriptions_;
    receiver_base this_subscription_;

    subscribe_operation(
      Receiver rcvr,
      intrusive_list<&receiver_base::next_, &receiver_base::prev_>* subscriptions)
      : rcvr_(std::move(rcvr))
      , sub_rcvr_{this}
      , subscriptions_(subscriptions)
      , this_subscription_{sub_rcvr_} {
    }

    void start(stdexec::start_t) noexcept {
      subscriptions_->push_back(&this_subscription_);
    }
  };

  struct subscribe_to_wayland {
    using is_sender = exec::sequence_tag;
    intrusive_list<&receiver_base::next_, &receiver_base::prev_>* subscriptions_;

    using completion_signatures =
      stdexec::completion_signatures_of_t<any_sequence_of<std::span<std::byte>>, stdexec::empty_env>;

    template <class Receiver>
    subscribe_operation<Receiver> subscribe(exec::subscribe_t, Receiver rcvr) const {
      return {std::move(rcvr), subscriptions_};
    }
  };

  struct receive_all_receiver {
    using is_receiver = void;

    connection_context* context_;

    auto get_env(stdexec::get_env_t) const noexcept {
      return exec::make_env(
        exec::with(stdexec::get_stop_token, context_->stop_source_.get_token()));
    }

    void set_value(stdexec::set_value_t) && noexcept {
      context_->ref_counter_ -= 1;
      if (context_->ref_counter_ == 0 && context_->complete_close_) {
        (*context_->complete_close_)(context_->complete_close_data_);
      }
    }

    void set_stopped(stdexec::set_stopped_t) && noexcept {
      context_->ref_counter_ -= 1;
      if (context_->ref_counter_ == 0 && context_->complete_close_) {
        (*context_->complete_close_)(context_->complete_close_data_);
      }
    }

    void set_error(stdexec::set_error_t, std::error_code err) && noexcept {
      log("connection", "Error Code ({}): {}", err.value(), err.message());
      context_->ref_counter_ -= 1;
      if (context_->ref_counter_ == 0 && context_->complete_close_) {
        (*context_->complete_close_)(context_->complete_close_data_);
      }
    }

    void set_error(stdexec::set_error_t, std::exception_ptr err) && noexcept {
      try { 
        std::rethrow_exception(err);
      } catch (std::exception& e) {
        log("connection", "Error: {}", e.what());
      }
      context_->ref_counter_ -= 1;
      if (context_->ref_counter_ == 0 && context_->complete_close_) {
        (*context_->complete_close_)(context_->complete_close_data_);
      }
    }
  };

  struct connection::impl : connection_context {
    using receive_all_sender =
      decltype(receive_all_messages_until_disconnect(std::declval<connection_context&>()));

    impl(exec::io_uring_context& ctx)
      : connection_context{ctx}
      , receive_all_(stdexec::connect(
          receive_all_messages_until_disconnect(*this),
          receive_all_receiver{this})) {
    }

    stdexec::connect_result_t<receive_all_sender, receive_all_receiver> receive_all_;
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
                 this->impl_->ref_counter_ += 1;
                 stdexec::start(this->impl_->receive_all_);
                 return connection_handle{*this};
               }));
           });
  }

  template <class Receiver>
  struct close_operation {
    Receiver rcvr_;
    connection_context* context_;

    void start(stdexec::start_t) noexcept {
      if (context_->ref_counter_ == 0) {
        stdexec::set_value(std::move(rcvr_));
      } else {
        context_->complete_close_data_ = this;
        context_->complete_close_ = [](void* ptr) noexcept {
          auto self = static_cast<close_operation*>(ptr);
          stdexec::set_value(std::move(self->rcvr_));
        };
      }
    }
  };

  struct close_sender {
    using is_sender = void;

    connection_context* context_;

    using completion_signatures = stdexec::completion_signatures<stdexec::set_value_t()>;

    template <class Receiver>
    close_operation<Receiver> connect(stdexec::connect_t, Receiver rcvr) const {
      return {std::move(rcvr), context_};
    }
  };

  any_sender_of<> connection_handle::close(async::close_t) const {
    return sio::tap(
      close_sender{connection_->impl_.get()},
      async::close(get_handle(connection_->impl_->socket_)));
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

  any_sequence_of<std::span<std::byte>> connection_handle::subscribe() {
    return subscribe_to_wayland{&connection_->impl_->receivers_};
  }

} // namespace wayland