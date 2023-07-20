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
#include "./protocol.hpp"
#include "./logging.hpp"

#include <sio/async_channel.hpp>

#include <sio/io_uring/socket_handle.hpp>
#include <sio/local/stream_protocol.hpp>

#include <sio/sequence/empty_sequence.hpp>
#include <sio/sequence/ignore_all.hpp>
#include <sio/sequence/iterate.hpp>
#include <sio/sequence/then_each.hpp>
#include <sio/sequence/merge_each.hpp>

#include <sio/tap.hpp>

#include <exec/repeat_effect_until.hpp>
#include <exec/variant_sender.hpp>

namespace wayland {
  namespace {
    struct object;

    using event_handler = void (*)(object*, message_header, std::span<std::byte>) noexcept;

    struct object {
      std::span<event_handler> events_;
      id id_{};
      void (*destroyed_)(object*) noexcept {nullptr};
    };

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

    using socket_type = sio::io_uring::socket<sio::local::stream_protocol>;
    using socket_handle = sio::io_uring::socket_handle<sio::local::stream_protocol>;

    using channel_type =
      sio::async_channel<stdexec::completion_signatures<stdexec::set_value_t(std::span<std::byte>)>>;
    using channel_handle = sio::async_channel_handle<
      stdexec::completion_signatures<stdexec::set_value_t(std::span<std::byte>)>>;

    struct connection_context_base {
      connection_context_base() = default;

      explicit connection_context_base(socket_handle sock, channel_handle chan)
        : buffer_{}
        , socket_{sock}
        , channel_{chan} {
      }

      connection_context_base(const connection_context_base&) = delete;
      connection_context_base& operator=(const connection_context_base&) = delete;

      connection_context_base(connection_context_base&&) = delete;
      connection_context_base& operator=(connection_context_base&&) = delete;

      alignas(4096) std::array<std::byte, 8192> buffer_{};
      socket_handle socket_{};
      channel_handle channel_{};
      stdexec::in_place_stop_source stop_source_{};
      int ref_counter_{0};
      void* complete_close_data_{nullptr};
      void (*complete_close_)(void*) noexcept = nullptr;
    };

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

    enum process_result {
      disconnected,
      need_more,
      processed,
    };

    auto notify_all_listeners(channel_handle channel, std::span<std::byte> message) {
      return channel.notify_all(stdexec::just(message));
    }

    auto process_buffer(channel_handle channel, std::span<std::byte> buffer) {
      return sio::reduce(
        sio::iterate(std::ranges::subrange(buffer_iterator{buffer}, buffer_sentinel{})) //
          | sio::let_value_each([=](std::span<std::byte> message) {
              message_header header{};
              std::memcpy(&header, message.data(), sizeof(message_header));
              log(
                "connection",
                "Received message to Object ID: {}, Message length: {}, Opcode: {}",
                static_cast<int>(header.object_id),
                header.message_length,
                header.opcode);
              log_recv_buffer(message);
              return stdexec::then(
                channel.notify_all(stdexec::just(message)),
                [len = header.message_length] { return len; });
            }),
        0);
    }

    auto receive_all_messages_until_disconnect(connection_context_base& context) {
      return stdexec::just(std::span<std::byte>{}) //
           | stdexec::let_value([&context](std::span<std::byte>& filled) {
               std::span buffer{context.buffer_};
               return sio::async::read_some(context.socket_, buffer.subspan(filled.size())) //
                    | stdexec::let_value([&context, &filled, buffer](int n) {
                        filled = buffer.subspan(0, filled.size() + n);
                        return if_then_else(
                          n == 0,
                          stdexec::just() | stdexec::then([] {
                            log("connection", "Disconnected from Wayland server.");
                            return -1;
                          }),
                          process_buffer(context.channel_, filled));
                      }) //
                    | stdexec::then([&](int n) {
                        if (n < 0) {
                          return true;
                        }
                        auto consumed = filled.subspan(0, n);
                        auto rest = filled.subspan(n);
                        std::memmove(filled.data(), rest.data(), rest.size());
                        filled = filled.subspan(0, rest.size());
                        return false;
                      })
                    | exec::repeat_effect_until();
             });
    }

    struct receive_all_receiver {
      using is_receiver = void;

      connection_context_base* context_;

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

    struct connection_context : connection_context_base {
      using receive_all_sender =
        decltype(receive_all_messages_until_disconnect(std::declval<connection_context_base&>()));

      connection_context()
        : connection_context_base{}
        , receive_all_(stdexec::connect(
            receive_all_messages_until_disconnect(*static_cast<connection_context_base*>(this)),
            receive_all_receiver{static_cast<connection_context_base*>(this)})) {
      }

      connection_context(socket_handle sock, channel_handle chan)
        : connection_context_base{sock, chan}
        , receive_all_(stdexec::connect(
            receive_all_messages_until_disconnect(*static_cast<connection_context_base*>(this)),
            receive_all_receiver{static_cast<connection_context_base*>(this)})) {
      }

      stdexec::connect_result_t<receive_all_sender, receive_all_receiver> receive_all_;
    };

    template <class Receiver>
    struct close_operation {
      Receiver rcvr_;
      connection_context* context_;

      void start(stdexec::start_t) noexcept;
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

    class connection_handle {
     private:
      connection_context* connection_{};

     public:
      connection_handle();
      explicit connection_handle(connection_context& connection);

      auto send(const std::span<std::byte>& buffer) {
        if (buffer.size() < sizeof(message_header)) {
          throw std::runtime_error("Buffer too small");
        }
        return stdexec::let_value(stdexec::just(buffer), [this](std::span<std::byte> buffer) {
          message_header header = extract_header(buffer);
          log(
            "connection",
            "Sending message. Object ID: {}, Message length: {}, Opcode: {}",
            static_cast<int>(header.object_id),
            header.message_length,
            header.opcode);
          auto message = buffer.subspan(0, header.message_length);
          return sio::async::write(connection_->socket_, message) //
               | stdexec::then([message](std::size_t) { log_send_buffer(message); });
        });
      }

      template <class Tp>
        requires stdexec::__not_decays_to<Tp, std::span<std::byte>>
      auto send(Tp& msg) {
        const std::span<std::byte> buffer(reinterpret_cast<std::byte*>(&msg), sizeof(Tp));
        return send(buffer);
      }

      auto send_with_fd(std::span<std::byte> buffer, int fd) {
        if (buffer.size() < sizeof(message_header)) {
          throw std::runtime_error("Buffer too small");
        }
        return stdexec::let_value(
          stdexec::just(buffer, ::iovec{}), [this, fd](std::span<std::byte> buffer, ::iovec& iov) {
            message_header header{};
            std::memcpy(&header, buffer.data(), sizeof(message_header));
            log(
              "connection",
              "Sending message. Object ID: {}, Message length: {}, Opcode: {}, FD: {}",
              static_cast<int>(header.object_id),
              header.message_length,
              header.opcode,
              fd);
            ::msghdr msg{};
            iov.iov_base = buffer.data();
            iov.iov_len = buffer.size();
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;
            ::cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type = SCM_RIGHTS;
            cmsg->cmsg_len = CMSG_LEN(sizeof(fd));
            ::memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
            msg.msg_controllen = cmsg->cmsg_len;
            return sio::async::sendmsg(connection_->socket_, msg) //
                 | stdexec::then([buffer](std::size_t) { log_send_buffer(buffer); });
          });
      }

      template <class Tp>
        requires stdexec::__not_decays_to<Tp, std::span<std::byte>>
      auto send_with_fd(Tp& msg, int fd) {
        std::span<std::byte> buffer(reinterpret_cast<std::byte*>(&msg), sizeof(Tp));
        return send_with_fd(buffer, fd);
      }

      auto subscribe() {
        return connection_->channel_.subscribe();
      }

      auto operator<=>(const connection_handle&) const = default;
    };

    auto open_connection_context(connection_context* ctx) {
      return stdexec::let_value(stdexec::just(), [ctx] {
        const char* xdg_runtime_dir = std::getenv("XDG_RUNTIME_DIR");
        if (!xdg_runtime_dir) {
          throw std::runtime_error("XDG_RUNTIME_DIR not set");
        }
        const char* wayland_display = std::getenv("WAYLAND_DISPLAY");
        if (!wayland_display) {
          throw std::runtime_error("WAYLAND_DISPLAY not set");
        }
        const std::filesystem::path path = std::filesystem::path(xdg_runtime_dir) / wayland_display;
        if (!std::filesystem::exists(path)) {
          throw std::runtime_error("Wayland socket does not exist");
        }
        log("connection", "Connecting to the Wayland server socket '{}'...", path.c_str());
        return stdexec::let_value(
          sio::async::connect(ctx->socket_, sio::local::endpoint(path.c_str())), [ctx] {
            log("connection", "Connected to the Wayland server.");
            ctx->ref_counter_ += 1;
            stdexec::start(ctx->receive_all_);
            return stdexec::just(connection_handle{*ctx});
          });
      });
    }

    class connection {
     public:
      explicit connection(exec::io_uring_context& context)
        : context_{&context} {
      }

     private:
      exec::io_uring_context* context_;

      friend struct sio::async::use_t;

      auto use(sio::async::use_t) const {
        auto context = sio::make_deferred<connection_context>();
        return stdexec::let_value(
                 stdexec::just(context),
                 [io_context = context_](auto& ctx) {
                   ctx();
                   auto cptr = std::addressof(*ctx);
                   auto seq =
                     sio::tap(stdexec::just(cptr), close_sender{cptr})
                     | sio::then_each([io_context](connection_context* ctx) {
                         socket_type socket{*io_context};
                         channel_type channel{};
                         return sio::zip(sio::async::use(socket), sio::async::use(channel))
                              | sio::let_value_each([ctx](socket_handle sock, channel_handle chan) {
                                  ctx->socket_ = sock;
                                  ctx->channel_ = chan;
                                  return open_connection_context(ctx);
                                });
                       })
                     | sio::merge_each();
                   return stdexec::just(seq);
                 })
             | sio::merge_each();
      }
    };

    template <class Receiver>
    void close_operation<Receiver>::start(stdexec::start_t) noexcept {
      if (context_->ref_counter_ == 0) {
        log("connection", "Connection closed.");
        stdexec::set_value(std::move(rcvr_));
      } else {
        context_->complete_close_data_ = this;
        context_->complete_close_ = [](void* ptr) noexcept {
          log("connection", "Connection closed.");
          auto self = static_cast<close_operation*>(ptr);
          stdexec::set_value(std::move(self->rcvr_));
        };
        log("connection", "Closing connection...");
        context_->stop_source_.request_stop();
      }
    }

    connection_handle::connection_handle() = default;

    connection_handle::connection_handle(connection_context& context)
      : connection_{&context} {
    }

    template <class Fn>
    auto just_invoke(Fn fn) {
      return stdexec::then(stdexec::just(), std::move(fn));
    }

    template <class Tp>
    Tp extract(std::span<std::byte> buffer) noexcept {
      Tp result;
      std::memcpy(&result, buffer.data(), sizeof(Tp));
      return result;
    }

    template <>
    std::string_view extract<std::string_view>(std::span<std::byte> buffer) noexcept {
      uint32_t length = extract<uint32_t>(buffer) - 1;
      return {reinterpret_cast<char*>(buffer.data() + 4), length};
    }

    template <class Tp>
    Tp extract_and_advance(std::span<std::byte>& buffer) noexcept {
      Tp result = extract<Tp>(buffer);
      buffer = buffer.subspan(sizeof(Tp));
      return result;
    }

    template <>
    std::string_view extract_and_advance<std::string_view>(std::span<std::byte>& buffer) noexcept {
      std::string_view result = extract<std::string_view>(buffer);
      const uint32_t length = extract<uint32_t>(buffer);
      const uint32_t with_padding = (length + 7) & ~7;
      buffer = buffer.subspan(4 + with_padding);
      return result;
    }

    template <class... Tps>
      requires(sizeof...(Tps) > 1)
    std::tuple<Tps...> extract(std::span<std::byte> buffer) noexcept {
      return {extract_and_advance<Tps>(buffer)...};
    }

    template <class Tp>
      requires std::is_trivially_copyable_v<Tp>
    std::span<std::byte> serialize_to(std::span<std::byte> buffer, const Tp& value) noexcept {
      std::memcpy(buffer.data(), &value, sizeof(value));
      return buffer.subspan(sizeof(value));
    }

    std::span<std::byte> serialize_to(std::span<std::byte> buffer, const std::string& str) noexcept {
      const uint32_t length = str.size() + 1;
      const uint32_t with_padding = (length + 7) & ~7;
      buffer = serialize_to(buffer, length);
      std::memcpy(buffer.data(), str.data(), str.size());
      buffer = buffer.subspan(with_padding);
      return buffer;
    }

    template <class... Args>
    void serialize(std::span<std::byte> buffer, message_header header, Args... args) {
      auto total_msg = buffer;
      buffer = serialize_to(buffer, header);
      ((buffer = serialize_to(buffer, args)), ...);
      const uint32_t length = total_msg.size() - buffer.size();
      header.message_length = length;
      serialize_to(total_msg, header);
    }
  }

  struct display_context;

  namespace {

    struct dispatch_receiver {
      using is_receiver = void;
      display_context* display_;

      exec::make_env_t<exec::with_t<stdexec::get_stop_token_t, stdexec::in_place_stop_token>>
        get_env(stdexec::get_env_t) const noexcept;

      void set_value(stdexec::set_value_t) const noexcept;

      void set_error(stdexec::set_error_t, std::error_code) const noexcept;

      void set_error(stdexec::set_error_t, std::exception_ptr) const noexcept;

      void set_stopped(stdexec::set_stopped_t) const noexcept;
    };

    auto make_dispatch_operation(
      any_sequence_of<std::span<std::byte>> messages,
      std::vector<object*>& objects,
      display_context* self) {
      return stdexec::connect(
        sio::then_each(
          std::move(messages),
          [&objects](std::span<std::byte> message) {
            message_header header = extract_and_advance<message_header>(message);
            auto index = static_cast<uint32_t>(header.object_id) - 1;
            if (index < objects.size()) {
              if (objects[index]) {
                if (header.opcode >= objects[index]->events_.size()) {
                  log(
                    "display",
                    "Unknown opcode {} for object {}",
                    header.opcode,
                    static_cast<uint32_t>(header.object_id));
                } else {
                  objects[index]->events_[header.opcode](objects[index], header, message);
                }
              } else {
                log(
                  "display",
                  "Object {} is not registered.",
                  static_cast<uint32_t>(header.object_id));
              }
            } else {
              log("display", "Object {} is out of range.", static_cast<uint32_t>(header.object_id));
            }
          })
          | sio::ignore_all(),
        dispatch_receiver{self});
    }

    template <class Receiver>
    struct callback_base : object {
      callback_base(
        Receiver rcvr,
        display_context* display,
        std::span<event_handler> vtable) noexcept;

      Receiver receiver_;
      display_context* display_;
      int op_counter_{0};
    };

    template <class Receiver>
    struct callback_receiver {
      using is_receiver = void;

      callback_base<Receiver>* op_;

      stdexec::env_of_t<Receiver> get_env(stdexec::get_env_t) const noexcept;

      void unregister_callback() const noexcept;

      void set_value(stdexec::set_value_t) const noexcept;

      void set_error(stdexec::set_error_t, std::error_code ec) const noexcept;

      void set_error(stdexec::set_error_t, std::exception_ptr ep) const noexcept;

      void set_stopped(stdexec::set_stopped_t) const noexcept;
    };

    struct sync_message_t {
      message_header header;
      id new_id;
    };

    template <class Receiver>
    struct callback : callback_base<Receiver> {
      using send_operation_t = stdexec::connect_result_t<
        decltype(std::declval<connection_handle&>().send(std::declval<sync_message_t&>())),
        callback_receiver<Receiver> >;
      sync_message_t sync_message_;
      send_operation_t send_operation_;

      static void on_done(object* obj, message_header, std::span<std::byte>) noexcept;

      static std::span<event_handler, 1> get_vtable() noexcept {
        static std::array<event_handler, 1> vtable = {&on_done};
        return vtable;
      }

      callback(Receiver rcvr, display_context* display);

      void start(stdexec::start_t) noexcept;
    };
  }

  struct callback_sender {
    display_context* display_;

    using is_sender = void;

    using completion_signatures = stdexec::completion_signatures<
      stdexec::set_value_t(),
      stdexec::set_error_t(std::error_code),
      stdexec::set_error_t(std::exception_ptr),
      stdexec::set_stopped_t()>;

    template <class Receiver>
    callback<Receiver> connect(stdexec::connect_t, Receiver rcvr) const noexcept {
      return callback<Receiver>{std::move(rcvr), display_};
    }
  };

  struct display_context : object {
    // [event callbacks]

    static void on_error(object* obj, message_header, std::span<std::byte> buffer) noexcept;

    static void on_delete_id(object* obj, message_header, std::span<std::byte> buffer) noexcept;

    [[noreturn]] static void on_destroyed_(object* obj) noexcept;

    static std::span<event_handler, 2> get_vtable() noexcept;

    // Constructors

    explicit display_context(connection_handle connection) noexcept;

    // Methods

    void register_object(object& obj) noexcept;

    void unregister_object(const object& obj) noexcept;

    id get_new_id() noexcept;

    callback_sender sync() noexcept;

    connection_handle connection_;
    std::vector<object*> objects_;
    std::vector<id> free_ids_;
    using operation_type = decltype(make_dispatch_operation(
      std::declval<connection_handle&>().subscribe(),
      std::declval<std::vector<object*>&>(),
      std::declval<display_context*>()));
    stdexec::in_place_stop_source stop_source_{};
    operation_type operation_;
    std::atomic<void*> on_closed_;
    void* on_closed_user_data_;
  };

  template <class Receiver>
  struct close_display_operation {
    display_context* display_;
    Receiver rcvr_;

    static void on_close(void* ptr) noexcept;

    void start(stdexec::start_t) noexcept;
  };

  struct close_display_sender {
    using is_sender = void;
    using completion_signatures = stdexec::completion_signatures<stdexec::set_value_t()>;

    display_context* display_;

    template <class Receiver>
    close_display_operation<Receiver> connect(stdexec::connect_t, Receiver rcvr) const noexcept;
  };

  // <interface name="wl_registry" version="1">
  //   <request name="bind">
  //     <arg name="name" type="uint" summary="unique numeric name of the object"/>
  //     <arg name="id" type="new_id" summary="bounded object"/>
  //   </request>
  //   <event name="global">
  //     <arg name="name" type="uint" summary="numeric name of the global object"/>
  //     <arg name="interface" type="string" summary="interface implemented by the object"/>
  //     <arg name="version" type="uint" summary="interface version"/>
  //   </event>
  //   <event name="global_remove">
  //     <arg name="name" type="uint" summary="numeric name of the global object"/>
  //   </event>
  // </interface>

  struct global {
    wayland::name name;
    std::string interface;
    wayland::version version;
  };

  struct registry_context : object {
    static void
      on_global(object* obj, message_header header, std::span<std::byte> message) noexcept;
    static void
      on_global_remove(object* obj, message_header header, std::span<std::byte> message) noexcept;

    static std::span<event_handler, 2> get_vtable() noexcept;

    explicit registry_context(display_context* display) noexcept;

    display_context* display_;
    std::vector<global> globals_{};
  };

  // struct shm_ : object {
  //   static std::span<event_handler, 0> get_vtable() noexcept;

  //   explicit shm_(display_context* display) noexcept;
  // };

  // // <interface name="wl_compositor" version="5">
  // //   <request name="create_surface">
  // //     <arg name="id" type="new_id" interface="wl_surface" summary="the new surface"/>
  // //   </request>

  // //   <request name="create_region">
  // //     <arg name="id" type="new_id" interface="wl_region" summary="the new region"/>
  // //   </request>
  // // </interface>


  struct compositor_context : object {
    static std::span<event_handler, 0> get_vtable() noexcept {
      return {};
    }

    explicit compositor_context(display_context* display) noexcept
      : object{get_vtable(), display->get_new_id()} {
      display->register_object(*this);
    }
  };

  any_sequence_of<compositor> compositor::bind(registry_context& registry) {
    auto context = sio::make_deferred<compositor_context>(registry.display_);
    return sio::let_value_each(stdexec::just(context), [&registry](auto& ctx) {
      static constexpr auto interface = "wl_compositor";
      auto it = std::ranges::find_if(registry.globals_, [](const global& g) {
        return g.interface == interface;
      });
      return if_then_else(
        it == registry.globals_.end(),
        stdexec::just_error(std::make_error_code(std::errc::invalid_argument)),
        stdexec::let_value(
          stdexec::just(std::array<std::byte, 256>{}), [&ctx, &registry, it](std::array<std::byte, 256>& msg) {
            compositor_context& context = ctx();
            message_header header{.object_id = registry.id_, .opcode = 0};
            serialize(msg, header, it->name, it->interface, it->version, context.id_);
            auto open =
              stdexec::when_all(registry.display_->connection_.send(msg))
              | stdexec::then([&context] {
                  log("compositor", "Created compositor with id '{}'", static_cast<int>(context.id_));
                  return compositor{context};
                });
            auto seq = sio::tap(
              std::move(open), just_invoke([&] {
                log("compositor", "Destroy compositor with id '{}'", static_cast<int>(context.id_));
                registry.display_->unregister_object(context);
              }));
            return stdexec::just(std::move(seq));
          }));
    }) | sio::merge_each();
  }

  ///////////////////////////////////////////////////////////////////////////
  //                                                          IMPLEMENTATION

  // Implementation of the synchroneous callback sender

  // callback_base

  template <class Receiver>
  callback_base<Receiver>::callback_base(
    Receiver rcvr,
    display_context* display,
    std::span<event_handler> vtable) noexcept
    : object{vtable}
    , receiver_(std::move(rcvr))
    , display_(display) {
  }

  // callback_receiver

  template <class Receiver>
  stdexec::env_of_t<Receiver>
    callback_receiver<Receiver>::get_env(stdexec::get_env_t) const noexcept {
    return stdexec::get_env(op_->receiver_);
  }

  template <class Receiver>
  void callback_receiver<Receiver>::set_value(stdexec::set_value_t) const noexcept {
    log("callback", "Sent message");
  }

  template <class Receiver>
  void callback_receiver<Receiver>::set_error(
    stdexec::set_error_t,
    std::error_code ec) const noexcept {
    log("callback", "Error sending message: {} (Error Code: {})", ec.message(), ec.value());
    op_->display_->unregister_object(*op_);
    stdexec::set_error(std::move(op_->receiver_), std::move(ec));
  }

  template <class Receiver>
  void callback_receiver<Receiver>::set_error(
    stdexec::set_error_t,
    std::exception_ptr ep) const noexcept {
    try {
      std::rethrow_exception(ep);
    } catch (const std::exception& e) {
      log("callback", "Error sending message: {}", e.what());
    } catch (...) {
      log("callback", "Error sending message: Unknown error type.");
    }
    op_->display_->unregister_object(*op_);
    stdexec::set_error(std::move(op_->receiver_), std::move(ep));
  }

  template <class Receiver>
  void callback_receiver<Receiver>::set_stopped(stdexec::set_stopped_t) const noexcept {
    log("callback", "Stopped sending message");
    op_->display_->unregister_object(*op_);
    stdexec::set_stopped(std::move(op_->receiver_));
  }

  // callback operation state

  template <class Receiver>
  callback<Receiver>::callback(Receiver rcvr, display_context* display)
    : callback_base<Receiver>{std::move(rcvr), display, get_vtable()}
    , sync_message_{}
    , send_operation_{stdexec::connect(
        this->display_->connection_.send(sync_message_),
        callback_receiver<Receiver>{this})} {
  }

  template <class Receiver>
  void callback<Receiver>::on_done(object* obj, message_header, std::span<std::byte>) noexcept {
    callback* self = static_cast<callback*>(obj);
    log("callback", "Received sync answer for callback id {}", static_cast<int>(self->id_));
    self->display_->unregister_object(*self);
    stdexec::set_value(std::move(self->receiver_));
  }

  template <class Receiver>
  void callback_receiver<Receiver>::unregister_callback() const noexcept {
    op_->display_->unregister_object(*op_);
  }

  template <class Receiver>
  void callback<Receiver>::start(stdexec::start_t) noexcept {
    this->id_ = this->display_->get_new_id();
    sync_message_.header.object_id = this->display_->id_;
    sync_message_.header.opcode = 0;
    sync_message_.header.message_length = sizeof(sync_message_t);
    sync_message_.new_id = this->id_;
    log("callback", "Register as id {}.", static_cast<int>(this->id_));
    this->display_->register_object(*this);
    log("callback", "Sending sync message.");
    stdexec::start(this->send_operation_);
  }

  /////////////////////////////////////////////////////////////////////////////
  //                                                    DISPLAY IMPLEMENTATION

  exec::make_env_t<exec::with_t<stdexec::get_stop_token_t, stdexec::in_place_stop_token>>
    dispatch_receiver::get_env(stdexec::get_env_t) const noexcept {
    return exec::make_env(exec::with(stdexec::get_stop_token, display_->stop_source_.get_token()));
  }

  void dispatch_receiver::set_value(stdexec::set_value_t) const noexcept {
    log("display", "Message dispatching is completed successfully.");
    void* ptr = display_->on_closed_.exchange(
      static_cast<void*>(display_), std::memory_order_acq_rel);
    if (ptr) {
      void (*on_closed)(void*) = std::bit_cast<void (*)(void*)>(ptr);
      log("dispatch_receiver", "Calling on_closed callback.");
      on_closed(display_->on_closed_user_data_);
    } else {
      log("dispatch_receiver", "No on_closed callback registered.");
    }
  }

  void dispatch_receiver::set_error(stdexec::set_error_t, std::error_code) const noexcept {
    log("display", "dispatch_receiver::set_error");
    void* ptr = display_->on_closed_.exchange(
      static_cast<void*>(display_), std::memory_order_acq_rel);
    if (ptr) {
      void (*on_closed)(void*) = std::bit_cast<void (*)(void*)>(ptr);
      log("dispatch_receiver", "Calling on_closed callback.");
      on_closed(display_->on_closed_user_data_);
    } else {
      log("dispatch_receiver", "No on_closed callback registered.");
    }
  }

  void dispatch_receiver::set_error(stdexec::set_error_t, std::exception_ptr) const noexcept {
    log("display", "dispatch_receiver::set_error");
    void* ptr = display_->on_closed_.exchange(
      static_cast<void*>(display_), std::memory_order_acq_rel);
    if (ptr) {
      void (*on_closed)(void*) = std::bit_cast<void (*)(void*)>(ptr);
      log("dispatch_receiver", "Calling on_closed callback.");
      on_closed(display_->on_closed_user_data_);
    } else {
      log("dispatch_receiver", "No on_closed callback registered.");
    }
  }

  void dispatch_receiver::set_stopped(stdexec::set_stopped_t) const noexcept {
    log("display", "dispatch_receiver::set_stopped");
    void* ptr = display_->on_closed_.exchange(
      static_cast<void*>(display_), std::memory_order_acq_rel);
    if (ptr) {
      void (*on_closed)(void*) = std::bit_cast<void (*)(void*)>(ptr);
      log("dispatch_receiver", "Calling on_closed callback.");
      on_closed(display_->on_closed_user_data_);
    } else {
      log("dispatch_receiver", "No on_closed callback registered.");
    }
  }

  void
    display_context::on_error(object* obj, message_header, std::span<std::byte> buffer) noexcept {
    display_context* self = static_cast<display_context*>(obj);
    auto [which, ec, reason] = extract<id, uint32_t, std::string_view>(buffer);
    log("display", "Object '{}': Error ({}): {}", static_cast<int>(which), ec, reason);
  }

  void display_context::on_delete_id(
    object* obj,
    message_header,
    std::span<std::byte> buffer) noexcept {
    display_context* self = static_cast<display_context*>(obj);
    id which = extract<id>(buffer);
    log("display", "Deleted object '{}'", static_cast<int>(which));
    uint32_t value = static_cast<uint32_t>(which) - 1;
    if (value < self->objects_.size()) {
      if (self->objects_[value] && self->objects_[value]->destroyed_) {
        self->objects_[value]->destroyed_(self->objects_[value]);
      }
      self->objects_[value] = nullptr;
    }
    self->free_ids_.push_back(which);
  }

  [[noreturn]] void display_context::on_destroyed_(object* obj) noexcept {
    std::terminate();
  }

  std::span<event_handler, 2> display_context::get_vtable() noexcept {
    static std::array<event_handler, 2> vtable = {&on_error, &on_delete_id};
    return vtable;
  }

  display_context::display_context(connection_handle connection) noexcept
    : object{get_vtable(), id{1}, &on_destroyed_}
    , connection_(connection)
    , objects_{}
    , operation_{make_dispatch_operation(connection.subscribe(), objects_, this)} {
    objects_.push_back(this);
    stdexec::start(operation_);
  }

  void display_context::register_object(object& obj) noexcept {
    uint32_t index = static_cast<uint32_t>(obj.id_);
    if (objects_.capacity() < index) {
      objects_.reserve(2 * index);
    }
    if (objects_.size() < index) {
      objects_.resize(index);
    }
    assert(index > 0);
    index -= 1;
    if (objects_[index]) {
      log("display", "Object {} is already registered.", static_cast<int>(obj.id_));
    } else {
      objects_[index] = &obj;
      log("display", "Registered object {}.", static_cast<int>(obj.id_));
    }
  }

  void display_context::unregister_object(const object& obj) noexcept {
    uint32_t index = static_cast<uint32_t>(obj.id_);
    assert(index > 0);
    assert(index <= objects_.size());
    index -= 1;
    if (objects_[index] == &obj) {
      objects_[index] = nullptr;
      log("display", "Unregistered object {}.", static_cast<int>(obj.id_));
    } else {
      log("display", "Object {} is not registered.", static_cast<int>(obj.id_));
    }
  }

  id display_context::get_new_id() noexcept {
    if (free_ids_.empty()) {
      return static_cast<id>(objects_.size() + 1);
    } else {
      id index = free_ids_.back();
      free_ids_.pop_back();
      return index;
    }
  }

  callback_sender display_context::sync() noexcept {
    return callback_sender{this};
  }

  display_handle::display_handle() = default;

  display_handle::display_handle(display_context* context) noexcept
    : context_{context} {
  }

  display::display(exec::io_uring_context& context) noexcept
    : context_{&context} {
  }

  display::display() = default;

  any_sequence_of<display_handle> display::use(sio::async::use_t) const {
    connection conn(*context_);
    return sio::async::use(conn) | sio::let_value_each([](connection_handle h) {
             auto ctx = sio::make_deferred<display_context>(h);
             return stdexec::just(
               stdexec::just(ctx) | stdexec::let_value([](auto& ctx) {
                 ctx();
                 log("display", "Opened display.");
                 display_context* ptr = ctx.get();
                 return stdexec::just(
                   sio::tap(stdexec::just(display_handle{ptr}), close_display_sender{ptr}));
               })
               | sio::merge_each());
           })
         | sio::merge_each();
  }

  any_sender_of<> display_handle::sync() const {
    return context_->sync();
  }

  template <class Receiver>
  void close_display_operation<Receiver>::on_close(void* ptr) noexcept {
    log("display", "Display closed.");
    auto* self = static_cast<close_display_operation<Receiver>*>(ptr);
    stdexec::set_value(std::move(self->rcvr_));
  }

  template <class Receiver>
  void close_display_operation<Receiver>::start(stdexec::start_t) noexcept {
    display_->stop_source_.request_stop();
    display_->on_closed_user_data_ = this;
    void* expected = nullptr;
    void (*on_closed)(void*) = &on_close;
    void* ptr = std::bit_cast<void*>(on_closed);
    if (!display_->on_closed_.compare_exchange_strong(expected, ptr, std::memory_order_acq_rel)) {
      log("display", "Receive operation has already stopped.");
      on_close(this);
    } else {
      log("display", "Waiting for display to close.");
    }
  }

  template <class Receiver>
  close_display_operation<Receiver>
    close_display_sender::connect(stdexec::connect_t, Receiver rcvr) const noexcept {
    return close_display_operation<Receiver>{this->display_, std::move(rcvr)};
  }

  struct get_registry_t {
    message_header header;
    id new_id;
  };

  any_sequence_of<registry> display_handle::get_registry() const {
    auto reg = sio::make_deferred<registry_context>(context_);
    return stdexec::let_value(
             stdexec::just(reg, get_registry_t{}),
             [](auto& reg, get_registry_t& msg) {
               log("display", "Creating registry object.");
               reg();
               registry_context* ctx = reg.get();
               ctx->id_ = ctx->display_->get_new_id();
               ctx->display_->register_object(*ctx);
               msg.header.object_id = ctx->display_->id_;
               msg.header.message_length = sizeof(get_registry_t);
               msg.header.opcode = 1;
               msg.new_id = ctx->id_;
               auto open = stdexec::then(
                 stdexec::when_all(ctx->display_->connection_.send(msg), ctx->display_->sync()),
                 [ctx] {
                   log("display", "Registry object created.");
                   return registry{ctx};
                 });
               auto seq = sio::tap(std::move(open), just_invoke([ctx] {
                                     log("registry", "Unregistering registry object.");
                                     ctx->display_->unregister_object(*ctx);
                                   }));
               return stdexec::just(std::move(seq));
             })
         | sio::merge_each();
  }

  /////////////////////////////////////////////////////////////////////////////
  //                                                    REGISTRY IMPLEMENTATION

  registry_context::registry_context(display_context* display) noexcept
    : object{get_vtable(), id{}}
    , display_{display} {
    log("registry", "Creating registry object.");
  }

  std::span<event_handler, 2> registry_context::get_vtable() noexcept {
    static std::array<event_handler, 2> vtable = {&on_global, &on_global_remove};
    return vtable;
  }

  void
    registry_context::on_global(object* obj, message_header, std::span<std::byte> buffer) noexcept {
    registry_context* self = static_cast<registry_context*>(obj);
    auto [which, interface, ver] = extract<name, std::string_view, version>(buffer);
    log(
      "registry",
      "New global object '{}': interface '{}' (version {})",
      static_cast<int>(which),
      interface,
      static_cast<int>(ver));
    self->globals_.push_back(global{which, std::string{interface}, ver});
  }

  void registry_context::on_global_remove(
    object* obj,
    message_header,
    std::span<std::byte> buffer) noexcept {
    registry_context* self = static_cast<registry_context*>(obj);
    auto which = extract<name>(buffer);
    log("registry", "Removed global object '{}'", static_cast<int>(which));
    auto elem = std::find_if(
      self->globals_.begin(), self->globals_.end(), [which](const global& g) {
        return g.name == which;
      });
    if (elem != self->globals_.end()) {
      self->globals_.erase(elem);
    } else {
      log("registry", "Global object '{}' was not found.", static_cast<int>(which));
    }
  }

  registry::registry(registry_context* context)
    : context_{context} {
  }

  struct bind_message {
    message_header header;
    name name;
    id new_id;
  };

  // static auto register_object(registry_context* ctx, object& obj, name which) {
  //   return stdexec::let_value(stdexec::just(), [ctx, &obj, which] {
  //     auto elem = std::find_if(
  //       ctx->globals_.begin(), ctx->globals_.end(), [which](const global& g) {
  //         return g.name == which;
  //       });
  //     if (elem != ctx->globals_.end()) {
  //       id new_id = ctx->display_->get_new_id();
  //       obj.id_ = new_id;
  //       log(
  //         "registry",
  //         "Binding interface '{}' to id '{}'.",
  //         elem->interface,
  //         static_cast<int>(which),
  //         static_cast<int>(new_id));
  //       ctx->display_->register_object(obj);
  //     } else {
  //       log("registry", "Binding object name '{}' which is not a global.", static_cast<int>(which));
  //     }
  //     return if_then_else(
  //       elem != ctx->globals_.end(),
  //       stdexec::just(&obj),
  //       stdexec::just_error(std::make_error_code(std::errc::invalid_argument)));
  //   });
  // }

  // any_sequence_of<object*> registry::bind(name which, object& base) const {
  //   return stdexec::just(bind_message{}) //
  //        | stdexec::let_value([which, context = context_, &base](bind_message& msg) {
  //            auto register_object = sio::tap(
  //              register_object(context, *obj, which), just_invoke([=] {
  //                log("registry", "Unregistering object id='{}'.", obj->id_);
  //                context->display_->unregister_object(*obj);
  //              }));
  //            return stdexec::just(
  //              sio::let_value_each(std::move(register_object), [&msg, which, context](object* obj) {
  //                msg.header.opcode = 0;
  //                msg.header.object_id = context->id_;
  //                msg.header.message_length = sizeof(bind_message);
  //                msg.name = which;
  //                msg.new_id = obj.id_;
  //                return stdexec::when_all(
  //                  context->display_->connection_.send(msg), stdexec::just(obj, context));
  //              }));
  //          })
  //        | sio::merge_each();
  // }

  // std::optional<name> registry::find_interface(std::string_view name) const {
  //   registry_* self = static_cast<registry_*>(this->impl_);
  //   auto elem = std::find_if(self->globals_.begin(), self->globals_.end(), [name](const global& g) {
  //     return g.interface == name;
  //   });
  //   if (elem != self->globals_.end()) {
  //     return elem->name;
  //   } else {
  //     return std::nullopt;
  //   }
  // }

  // shm::shm(object obj, void* data)
  //   : obj_{obj}
  //   , impl_{data} {
  // }

  // struct create_pool_msg {
  //   message_header header;
  //   id new_id;
  //   int32_t size;
  // };

  // any_sequence_of<shm_pool> shm::create_pool(int fd, int32_t size) {
  //   display_* d = static_cast<display_*>(impl_);
  //   object obj{shm_pool::get_vtable(), id{}};
  //   return stdexec::just(create_pool_msg{}, registered_object{obj, d}) //
  //        | stdexec::let_value([fd, size](create_pool_msg& msg, registered_object& reg) {
  //            return stdexec::just(
  //              sio::let_value_each(sio::async::run(reg), [&msg, fd, size](registered_object& reg) {
  //                msg.header.opcode = 0;
  //                msg.header.object_id = reg.display->id_;
  //                msg.header.message_length = sizeof(create_pool_msg);
  //                msg.new_id = reg.obj.id_;
  //                msg.size = size;
  //                return stdexec::when_all(
  //                  reg.display->connection_.send_with_fd(msg, fd),
  //                  stdexec::just(shm_pool{reg.obj, reg.display}));
  //              }));
  //          })
  //        | sio::merge_each();
  // }

  // std::span<event_handler> shm::get_vtable() {
  //   return std::span<event_handler, 0>{};
  // }

  // compositor::compositor(object, void*) {
  // }

  // std::span<event_handler> compositor::get_vtable() {
  //   return std::span<event_handler, 0>{};
  // }
}