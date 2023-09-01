#include "./connection.hpp"
#include "./message_header.hpp"
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

#include <exec/finally.hpp>
#include <exec/repeat_effect_until.hpp>
#include <exec/variant_sender.hpp>

namespace wayland {
  template <class Fn>
  auto just_invoke(Fn fn) {
    return stdexec::then(stdexec::just(), std::move(fn));
  }

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
        "wayland::connection",
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
              "wayland::connection",
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
                          log("wayland::connection", "Disconnected from Wayland server.");
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
      log("wayland::connection", "Error Code ({}): {}", err.value(), err.message());
      context_->ref_counter_ -= 1;
      if (context_->ref_counter_ == 0 && context_->complete_close_) {
        (*context_->complete_close_)(context_->complete_close_data_);
      }
    }

    void set_error(stdexec::set_error_t, std::exception_ptr err) && noexcept {
      try {
        std::rethrow_exception(err);
      } catch (std::exception& e) {
        log("wayland::connection", "Error: {}", e.what());
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

  any_sender_of<> connection_handle::send(const std::span<std::byte>& buffer) {
    if (buffer.size() < sizeof(message_header)) {
      throw std::runtime_error("Buffer too small");
    }
    return stdexec::let_value(stdexec::just(buffer), [this](std::span<std::byte> buffer) {
      message_header header = extract_header(buffer);
      log(
        "wayland::connection",
        "Sending message. Object ID: {}, Message length: {}, Opcode: {}",
        static_cast<int>(header.object_id),
        header.message_length,
        header.opcode);
      auto message = buffer.subspan(0, header.message_length);
      return sio::async::write(connection_->socket_, message) //
           | stdexec::then([message](std::size_t) { log_send_buffer(message); })
           | stdexec::let_stopped([message] {
               log("wayland::connection", "Stopped sending message:");
               log_send_buffer(message);
               return stdexec::just_stopped();
             });
    });
  }

  any_sender_of<> connection_handle::send_with_fd(std::span<std::byte> buffer, int fd) {
    if (buffer.size() < sizeof(message_header)) {
      throw std::runtime_error("Buffer too small");
    }
    return stdexec::let_value(
      stdexec::just(buffer, ::iovec{}, ::msghdr{}, std::array<char, CMSG_SPACE(sizeof(int))>{}),
      [this,
       fd](std::span<std::byte> buffer, ::iovec& iov, ::msghdr& msg, std::span<char> cmsg_buffer) {
        message_header header{};
        std::memcpy(&header, buffer.data(), sizeof(message_header));
        log(
          "wayland::connection",
          "Sending message. Object ID: {}, Message length: {}, Opcode: {}, FD: {}",
          static_cast<int>(header.object_id),
          header.message_length,
          header.opcode,
          fd);
        iov.iov_base = buffer.data();
        iov.iov_len = buffer.size();
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cmsg_buffer.data();
        msg.msg_controllen = cmsg_buffer.size();
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

  any_sequence_of<std::span<std::byte>> connection_handle::subscribe() {
    return connection_->channel_.subscribe();
  }

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
      log("wayland::connection", "Connecting to the Wayland server socket '{}'...", path.c_str());
      return stdexec::let_value(
        sio::async::connect(ctx->socket_, sio::local::endpoint(path.c_str())), [ctx] {
          log("wayland::connection", "Connected to the Wayland server.");
          ctx->ref_counter_ += 1;
          stdexec::start(ctx->receive_all_);
          return stdexec::just(connection_handle{*ctx});
        });
    });
  }

  any_sequence_of<connection_handle> connection::use(sio::async::use_t) const {
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

  template <class Receiver>
  void close_operation<Receiver>::start(stdexec::start_t) noexcept {
    if (context_->ref_counter_ == 0) {
      log("wayland::connection", "Connection closed.");
      stdexec::set_value(std::move(rcvr_));
    } else {
      context_->complete_close_data_ = this;
      context_->complete_close_ = [](void* ptr) noexcept {
        log("wayland::connection", "Connection closed.");
        auto self = static_cast<close_operation*>(ptr);
        stdexec::set_value(std::move(self->rcvr_));
      };
      log("wayland::connection", "Closing connection...");
      context_->stop_source_.request_stop();
    }
  }

  connection_handle::connection_handle() = default;

  connection_handle::connection_handle(connection_context& context)
    : connection_{&context} {
  }

}