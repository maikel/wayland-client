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

#include "./connection.hpp"
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
#include <sio/finally.hpp>

#include <exec/repeat_effect_until.hpp>
#include <exec/variant_sender.hpp>

namespace wayland {
  namespace {
    struct object;

    using event_handler =
      any_sender_of<> (*)(object*, message_header, std::span<std::byte>) noexcept;

    struct object {
      std::span<const event_handler> events_;
      id id_{};
      void (*destroyed_)(object*) noexcept {nullptr};
    };

    template <class ThenSender, class ElseSender>
    exec::variant_sender<ThenSender, ElseSender>
      if_then_else(bool condition, ThenSender then, ElseSender otherwise) {
      if (condition) {
        return then;
      }
      return otherwise;
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

    std::span<std::byte>
      serialize_to(std::span<std::byte> buffer, const std::string& str) noexcept {
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
      any_sequence_of<std::span<std::byte>>&& messages,
      std::vector<object*>& objects,
      display_context* self) {
      return stdexec::connect(
        sio::ignore_all(sio::let_value_each(
          std::move(messages),
          [&objects](std::span<std::byte> message) -> any_sender_of<> {
            message_header header = extract_and_advance<message_header>(message);
            auto index = static_cast<uint32_t>(header.object_id) - 1;
            if (index < objects.size()) {
              if (objects[index]) {
                if (header.opcode >= objects[index]->events_.size()) {
                  log(
                    "wl_display",
                    "Unknown opcode {} for object {}",
                    header.opcode,
                    static_cast<uint32_t>(header.object_id));
                } else {
                  return objects[index]->events_[header.opcode](objects[index], header, message);
                }
              } else {
                log(
                  "wl_display",
                  "Object {} is not registered.",
                  static_cast<uint32_t>(header.object_id));
              }
            } else {
              log(
                "wl_display",
                "Object {} is out of range.",
                static_cast<uint32_t>(header.object_id));
            }
            return stdexec::just();
          })),
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

      static any_sender_of<> on_done(object* obj, message_header, std::span<std::byte>) noexcept;

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

    static any_sender_of<>
      on_error(object* obj, message_header, std::span<std::byte> buffer) noexcept;

    static any_sender_of<>
      on_delete_id(object* obj, message_header, std::span<std::byte> buffer) noexcept;

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
    static any_sender_of<>
      on_global(object* obj, message_header header, std::span<std::byte> message) noexcept;
    static any_sender_of<>
      on_global_remove(object* obj, message_header header, std::span<std::byte> message) noexcept;

    static std::span<event_handler, 2> get_vtable() noexcept;

    template <class Tp, class... Args>
    any_sequence_of<typename Tp::token_type> bind(Args&&... args);

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
    using token_type = compositor;

    static constexpr auto interface = "wl_compositor";

    static std::span<event_handler, 0> get_vtable() noexcept {
      return {};
    }

    explicit compositor_context(display_context* display) noexcept
      : object{get_vtable(), display->get_new_id()}
      , display_{display} {
      display->register_object(*this);
    }

    display_context* display_;
  };

  struct shm_context : object {
    using token_type = shm;

    static constexpr auto interface = "wl_shm";

    static any_sender_of<>
      on_format(object* obj, message_header header, std::span<std::byte> message) noexcept {
      return just_invoke([obj, message] {
        shm_context* self = static_cast<shm_context*>(obj);
        auto format = extract<uint32_t>(message);
        log(interface, "Format: {:#010x}", format);
      });
    }

    static std::span<event_handler, 1> get_vtable() noexcept {
      static std::array<event_handler, 1> vtable = {&on_format};
      return vtable;
    }

    explicit shm_context(display_context* display) noexcept
      : object{get_vtable(), display->get_new_id()}
      , display_{display} {
      display_->register_object(*this);
    }

    display_context* display_;
  };

  struct shm_pool_context : object {
    using token_type = shm_pool;

    static constexpr auto interface = "wl_shm_pool";

    static std::span<event_handler, 0> get_vtable() noexcept {
      return {};
    }

    explicit shm_pool_context(shm_context* shm, int fd, int32_t size) noexcept
      : object{get_vtable(), shm->display_->get_new_id()}
      , shm_{shm}
      , fd_{fd}
      , size_{size} {
      log(
        interface,
        "Created shm_pool (id={}) with fd {} and size {}",
        static_cast<int>(this->id_),
        fd_,
        size_);
      shm_->display_->register_object(*this);
    }

    shm_context* shm_;
    int fd_;
    int32_t size_;
  };

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
    log("wl_callback", "Sent message");
  }

  template <class Receiver>
  void callback_receiver<Receiver>::set_error(
    stdexec::set_error_t,
    std::error_code ec) const noexcept {
    log("wl_callback", "Error sending message: {} (Error Code: {})", ec.message(), ec.value());
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
      log("wl_callback", "Error sending message: {}", e.what());
    } catch (...) {
      log("wl_callback", "Error sending message: Unknown error type.");
    }
    op_->display_->unregister_object(*op_);
    stdexec::set_error(std::move(op_->receiver_), std::move(ep));
  }

  template <class Receiver>
  void callback_receiver<Receiver>::set_stopped(stdexec::set_stopped_t) const noexcept {
    log("wl_callback", "Stopped sending message");
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
  any_sender_of<>
    callback<Receiver>::on_done(object* obj, message_header, std::span<std::byte>) noexcept {
    return just_invoke([obj] {
      callback* self = static_cast<callback*>(obj);
      log("wl_callback", "Received sync answer for callback id {}", static_cast<int>(self->id_));
      self->display_->unregister_object(*self);
      stdexec::set_value(std::move(self->receiver_));
    });
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
    log("wl_callback", "Register as id {}.", static_cast<int>(this->id_));
    this->display_->register_object(*this);
    log("wl_callback", "Sending sync message.");
    stdexec::start(this->send_operation_);
  }

  /////////////////////////////////////////////////////////////////////////////
  //                                                    DISPLAY IMPLEMENTATION

  exec::make_env_t<exec::with_t<stdexec::get_stop_token_t, stdexec::in_place_stop_token>>
    dispatch_receiver::get_env(stdexec::get_env_t) const noexcept {
    return exec::make_env(exec::with(stdexec::get_stop_token, display_->stop_source_.get_token()));
  }

  void dispatch_receiver::set_value(stdexec::set_value_t) const noexcept {
    log("wl_display", "Message dispatching is completed successfully.");
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
    log("wl_display", "dispatch_receiver::set_error");
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
    log("wl_display", "dispatch_receiver::set_error");
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
    log("wl_display", "dispatch_receiver::set_stopped");
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

  any_sender_of<>
    display_context::on_error(object* obj, message_header, std::span<std::byte> buffer) noexcept {
    return just_invoke([obj, buffer = std::move(buffer)] {
      display_context* self = static_cast<display_context*>(obj);
      auto [which, ec, reason] = extract<id, uint32_t, std::string_view>(buffer);
      log("wl_display", "Object '{}': Error ({}): {}", static_cast<int>(which), ec, reason);
    });
  }

  any_sender_of<> display_context::on_delete_id(
    object* obj,
    message_header,
    std::span<std::byte> buffer) noexcept {
    return just_invoke([obj, buffer] {
      display_context* self = static_cast<display_context*>(obj);
      id which = extract<id>(buffer);
      log("wl_display", "Deleted object '{}'", static_cast<int>(which));
      uint32_t value = static_cast<uint32_t>(which) - 1;
      if (value < self->objects_.size()) {
        if (self->objects_[value] && self->objects_[value]->destroyed_) {
          self->objects_[value]->destroyed_(self->objects_[value]);
        }
        self->objects_[value] = nullptr;
      }
      self->free_ids_.push_back(which);
    });
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
      log("wl_display", "Object {} is already registered.", static_cast<int>(obj.id_));
    } else {
      objects_[index] = &obj;
      log("wl_display", "Registered object {}.", static_cast<int>(obj.id_));
    }
  }

  void display_context::unregister_object(const object& obj) noexcept {
    uint32_t index = static_cast<uint32_t>(obj.id_);
    assert(index > 0);
    assert(index <= objects_.size());
    index -= 1;
    if (objects_[index] == &obj) {
      objects_[index] = nullptr;
      log("wl_display", "Unregistered object {}.", static_cast<int>(obj.id_));
    } else {
      log("wl_display", "Object {} is not registered.", static_cast<int>(obj.id_));
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
                 log("wl_display", "Opened display.");
                 display_context* ptr = ctx.get();
                 return stdexec::just(
                   sio::finally(stdexec::just(display_handle{ptr}), close_display_sender{ptr}));
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
    log("wl_display", "Display closed.");
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
      log("wl_display", "Receive operation has already stopped.");
      on_close(this);
    } else {
      log("wl_display", "Waiting for display to close.");
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
               log("wl_display", "Creating registry object.");
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
                   log("wl_display", "Registry object created.");
                   return registry{ctx};
                 });
               auto seq = sio::finally(std::move(open), just_invoke([ctx] {
                                         log("wl_registry", "Unregistering registry object.");
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
    log("wl_registry", "Creating registry object.");
  }

  std::span<event_handler, 2> registry_context::get_vtable() noexcept {
    static std::array<event_handler, 2> vtable = {&on_global, &on_global_remove};
    return vtable;
  }

  any_sender_of<>
    registry_context::on_global(object* obj, message_header, std::span<std::byte> buffer) noexcept {
    return just_invoke([obj, buffer] {
      registry_context* self = static_cast<registry_context*>(obj);
      auto [which, interface, ver] = extract<name, std::string_view, version>(buffer);
      log(
        "wl_registry",
        "New global object '{}': interface '{}' (version {})",
        static_cast<int>(which),
        interface,
        static_cast<int>(ver));
      self->globals_.push_back(global{which, std::string{interface}, ver});
    });
  }

  any_sender_of<> registry_context::on_global_remove(
    object* obj,
    message_header,
    std::span<std::byte> buffer) noexcept {
    return just_invoke([obj, buffer] {
      registry_context* self = static_cast<registry_context*>(obj);
      auto which = extract<name>(buffer);
      log("wl_registry", "Removed global object '{}'", static_cast<int>(which));
      auto elem = std::find_if(
        self->globals_.begin(), self->globals_.end(), [which](const global& g) {
          return g.name == which;
        });
      if (elem != self->globals_.end()) {
        self->globals_.erase(elem);
      } else {
        log("wl_registry", "Global object '{}' was not found.", static_cast<int>(which));
      }
    });
  }

  registry::registry(registry_context* context)
    : context_{context} {
  }

  template <class Tp, class... Args>
  any_sequence_of<typename Tp::token_type> registry_context::bind(Args&&... args) {
    auto context = sio::make_deferred<Tp>(std::forward<Args>(args)...);
    return sio::let_value_each(
             stdexec::just(std::move(context)),
             [this](auto& ctx) {
               auto it = std::ranges::find_if(globals_, [](const global& g) {
                 return g.interface == Tp::interface;
               });
               return if_then_else(
                 it == globals_.end(),
                 stdexec::just_error(std::make_error_code(std::errc::invalid_argument)),
                 stdexec::let_value(
                   stdexec::just(std::array<std::byte, 64>{}),
                   [this, &ctx, it](std::array<std::byte, 64>& msg) {
                     Tp& context = ctx();
                     message_header header{.object_id = id_, .opcode = 0};
                     serialize(msg, header, it->name, it->interface, it->version, context.id_);
                     auto open = display_->connection_.send(msg) | stdexec::then([&context] {
                                   log(
                                     Tp::interface,
                                     "Created object with id '{}'",
                                     static_cast<int>(context.id_));
                                   return typename Tp::token_type{context};
                                 });
                     auto seq = sio::finally(std::move(open), just_invoke([&] {
                                               log(
                                                 Tp::interface,
                                                 "Destroy object with id '{}'",
                                                 static_cast<int>(context.id_));
                                               display_->unregister_object(context);
                                             }));
                     return stdexec::just(std::move(seq));
                   }));
             })
         | sio::merge_each();
  }

  /////////////////////////////////////////////////////////////////////////////
  //                                                 COMPOSITOR IMPLEMENTATION

  any_sequence_of<compositor> compositor::bind(registry_context& ctx) {
    return ctx.bind<compositor_context>(ctx.display_);
  }

  /////////////////////////////////////////////////////////////////////////////
  //                                                        SHM IMPLEMENTATION

  any_sequence_of<shm> shm::bind(registry_context& ctx) {
    return ctx.bind<shm_context>(ctx.display_);
  }

  struct create_pool_msg {
    message_header header;
    id new_id;
    int32_t size;
  };

  static auto open(shm_pool_context& ctx) {
    return stdexec::just(create_pool_msg{}) //
         | stdexec::let_value([&ctx](create_pool_msg& msg) {
             msg.header.opcode = 0;
             msg.header.object_id = ctx.shm_->id_;
             msg.header.message_length = sizeof(create_pool_msg);
             msg.new_id = ctx.id_;
             msg.size = ctx.size_;
             return ctx.shm_->display_->connection_.send_with_fd(msg, ctx.fd_);
           })
         | stdexec::then([&ctx] {
             log("wl_shm_pool", "Opened shm_pool object.");
             return shm_pool{ctx};
           });
  }

  static auto destroy(const shm_pool_context& ctx) {
    return stdexec::just(message_header{}) //
         | stdexec::let_value([&ctx](message_header& msg) {
             msg.opcode = 1;
             msg.object_id = ctx.id_;
             msg.message_length = sizeof(message_header);
             return ctx.shm_->display_->connection_.send(msg);
           });
  }

  any_sequence_of<shm_pool> shm::create_pool(int fd, int32_t size) {
    auto context = sio::make_deferred<shm_pool_context>(context_, fd, size);
    return stdexec::just(create_pool_msg{}, context) //
         | stdexec::let_value([](create_pool_msg& msg, auto& ctx) {
             shm_pool_context& context = ctx();
             auto sequence = sio::finally(
               sio::tap(open(context), destroy(context)), just_invoke([&context] {
                 log("wl_shm", "Unregistering shm_pool object.");
                 context.shm_->display_->unregister_object(context);
               }));
             return stdexec::just(sequence);
           })
         | sio::merge_each();
  }

  /////////////////////////////////////////////////////////////////////////////
  //                                                   SHM_POOL IMPLEMENTATION

  shm_pool::shm_pool(shm_pool_context& context) noexcept
    : context_{&context} {
  }

  struct buffer_context : object {
    static inline constexpr auto interface = "wl_buffer";

    static any_sender_of<> on_release(object*, message_header, std::span<std::byte>) noexcept {
      return just_invoke([] { log(interface, "Released buffer object."); });
    }

    static std::span<const event_handler, 1> get_vtable() noexcept {
      static std::array<event_handler, 1> vtable = {&on_release};
      return vtable;
    }

    buffer_context(
      shm_pool_context* parent,
      int offset,
      dynamic_extents<2> extents,
      uint32_t format) noexcept
      : object{get_vtable(), parent->shm_->display_->get_new_id()}
      , region_{exec::__io_uring::__map_region(
          parent->fd_,
          offset,
          extents.extent(0) * extents.extent(1) * 4)}
      , data_{static_cast<std::uint32_t*>(region_.data()), extents}
      , pool_{parent}
      , format_{format} {
      log(interface, "Registering buffer object.");
      pool_->shm_->display_->register_object(*this);
    }

    exec::memory_mapped_region region_;
    mdspan<std::uint32_t, 2> data_;
    shm_pool_context* pool_;
    uint32_t format_;
  };

  struct create_buffer_msg {
    message_header header;
    id new_id;
    int32_t offset;
    int32_t width;
    int32_t height;
    int32_t stride;
    uint32_t format;
  };

  static auto open(buffer_context& context, create_buffer_msg& msg, int32_t offset) {
    msg.header.opcode = 0;
    msg.header.object_id = context.pool_->id_;
    msg.header.message_length = sizeof(create_buffer_msg);
    msg.new_id = context.id_;
    msg.offset = offset;
    msg.width = context.data_.extent(0);
    msg.height = context.data_.extent(1);
    msg.stride = context.data_.extent(0);
    msg.format = context.format_;
    return context.pool_->shm_->display_->connection_.send(msg) | stdexec::then([&context] {
             log("wl_buffer", "Opened buffer object.");
             return buffer{context};
           });
  }

  static auto destroy(const buffer_context& ctx) {
    return stdexec::just(message_header{}) //
         | stdexec::let_value([&ctx](message_header& msg) {
             msg.opcode = 0;
             msg.object_id = ctx.id_;
             msg.message_length = sizeof(message_header);
             return ctx.pool_->shm_->display_->connection_.send(msg);
           });
  }

  any_sequence_of<buffer>
    shm_pool::create_buffer(int32_t offset, dynamic_extents<2> extents, uint32_t format) {
    auto context = sio::make_deferred<buffer_context>(context_, offset, extents, format);
    return stdexec::just(context, create_buffer_msg{}) //
         | stdexec::let_value([offset](auto& ctx, create_buffer_msg& msg) {
             buffer_context& context = ctx();
             auto sequence = sio::finally(
               sio::tap(open(context, msg, offset), destroy(context)), just_invoke([&context] {
                 log("wl_buffer", "Unregistering buffer object.");
                 context.pool_->shm_->display_->unregister_object(context);
               }));
             return stdexec::just(std::move(sequence));
           })
         | sio::merge_each();
  }

  buffer::buffer(buffer_context& context) noexcept
    : context_{&context} {
  }

  id buffer::id() const noexcept {
    return context_->id_;
  }

  /////////////////////////////////////////////////////////////////////////////
  //                                                    SURFACE IMPLEMENTATION

  struct surface_context : object {
    static inline constexpr auto interface = "wl_surface";

    compositor_context* compositor_;

    static any_sender_of<> on_enter(object*, message_header, std::span<std::byte>) noexcept {
      return just_invoke([] { log(interface, "Entered surface object."); });
    }

    static any_sender_of<> on_leave(object*, message_header, std::span<std::byte>) noexcept {
      return just_invoke([] { log(interface, "Left surface object."); });
    }

    static inline constexpr std::array<event_handler, 2> vtable = {&on_enter, &on_leave};

    static std::span<const event_handler, 2> get_vtable() noexcept {
      return vtable;
    }

    surface_context(compositor_context* parent) noexcept
      : object{get_vtable(), parent->display_->get_new_id()}
      , compositor_{parent} {
      log(interface, "Registering surface object.");
      parent->display_->register_object(*this);
    }
  };

  surface::surface() = default;

  surface::surface(surface_context& context) noexcept
    : context_{&context} {
  }

  struct create_surface_msg {
    message_header header;
    id new_id;
  };

  static auto open(surface_context& context) {
    return stdexec::just(create_surface_msg{}) //
         | stdexec::let_value([&context](create_surface_msg& msg) {
             msg.header.opcode = 0;
             msg.header.object_id = context.compositor_->id_;
             msg.header.message_length = sizeof(create_surface_msg);
             msg.new_id = context.id_;
             return context.compositor_->display_->connection_.send(msg)
                  | stdexec::then([&context] {
                      log("wl_surface", "Opened surface object.");
                      return surface{context};
                    });
           });
  }

  static auto destroy(const surface_context& context) {
    return stdexec::just(message_header{}) //
         | stdexec::let_value([&context](message_header& msg) {
             msg.opcode = 0;
             msg.object_id = context.id_;
             msg.message_length = sizeof(message_header);
             return context.compositor_->display_->connection_.send(msg);
           })
         | stdexec::then([&context] {
             log("wl_surface", "Unregistering surface object.");
             context.compositor_->display_->unregister_object(context);
           });
  }

  any_sequence_of<surface> compositor::create_surface() {
    auto context = sio::make_deferred<surface_context>(context_);
    return                   //
      stdexec::just(context) //
      | sio::let_value_each([](auto& ctx) {
          surface_context& context = ctx();
          auto sequence = //
            open(context) //
            | stdexec::then([&context](surface s) {
                return sio::finally(stdexec::just(s), destroy(context));
              }) //
            | sio::merge_each();
          return stdexec::just(std::move(sequence));
        })
      | sio::merge_each();
  }

  struct attach_msg {
    message_header header;
    id buffer_id;
    position offset;
  };

  any_sender_of<> surface::attach(buffer buf, position offset) const {
    return stdexec::just(attach_msg{}) //
         | stdexec::let_value([ctx = context_, buf, offset](attach_msg& msg) {
             msg.header.opcode = 1;
             msg.header.object_id = ctx->id_;
             msg.header.message_length = sizeof(attach_msg);
             msg.buffer_id = buf.id();
             msg.offset = offset;
             return ctx->compositor_->display_->connection_.send(msg);
           });
  }

  struct damage_msg {
    message_header header;
    position offset;
    int32_t width;
    int32_t height;
  };

  any_sender_of<> surface::damage(position offset, dynamic_extents<2> extents) const {
    return stdexec::just(damage_msg{}) //
         | stdexec::let_value([ctx = context_, offset, extents](damage_msg& msg) {
             msg.header.opcode = 2;
             msg.header.object_id = ctx->id_;
             msg.header.message_length = sizeof(damage_msg);
             msg.offset = offset;
             msg.width = extents.extent(0);
             msg.height = extents.extent(1);
             return ctx->compositor_->display_->connection_.send(msg);
           });
  }

  any_sender_of<> surface::commit() const {
    return stdexec::just(message_header{}) //
         | stdexec::let_value([ctx = context_](message_header& msg) {
             msg.opcode = 6;
             msg.object_id = ctx->id_;
             msg.message_length = sizeof(message_header);
             return ctx->compositor_->display_->connection_.send(msg);
           });
  }

  /////////////////////////////////////////////////////////////////////////////
  //                                                    SHELL IMPLEMENTATION

  struct pong_msg {
    message_header header;
    uint32_t serial;
  };

  enum XDG_WM_BASE_MSG {
    XDG_WM_BASE_DESTROY,
    XDG_WM_BASE_CREATE_POSITIONER,
    XDG_WM_BASE_GET_XDG_SURFACE,
    XDG_WM_BASE_PONG,
  };

  struct xdg_wm_base_context : object {
    using token_type = xdg_wm_base;

    static inline constexpr auto interface = "xdg_wm_base";

    static any_sender_of<> on_ping(object* obj, message_header, std::span<std::byte>) noexcept;

    static std::span<const event_handler, 1> get_vtable() noexcept {
      static std::array<event_handler, 1> vtable = {&on_ping};
      return vtable;
    }

    explicit xdg_wm_base_context(display_context* display) noexcept
      : object{get_vtable(), display->get_new_id()}
      , display_{display} {
      log(interface, "Registering xdg_wm_base object.");
      display_->register_object(*this);
    }

    display_context* display_;
  };

  static auto destroy(const xdg_wm_base_context& context) {
    return stdexec::just(message_header{}) //
         | stdexec::let_value([&context](message_header& msg) {
             msg.opcode = XDG_WM_BASE_DESTROY;
             msg.object_id = context.id_;
             msg.message_length = sizeof(message_header);
             return context.display_->connection_.send(msg);
           });
  }

  any_sender_of<>
    xdg_wm_base_context::on_ping(object* obj, message_header, std::span<std::byte> msg) noexcept {
    log(interface, "Received ping.");
    xdg_wm_base_context* self = static_cast<xdg_wm_base_context*>(obj);
    const std::uint32_t serial = extract<std::uint32_t>(msg);
    return stdexec::just(pong_msg{}) //
         | stdexec::let_value([self, serial](pong_msg& msg) {
             msg.header.opcode = XDG_WM_BASE_PONG;
             msg.header.object_id = self->id_;
             msg.header.message_length = sizeof(pong_msg);
             msg.serial = serial;
             return self->display_->connection_.send(msg);
           });
  }

  xdg_wm_base::xdg_wm_base() = default;

  xdg_wm_base::xdg_wm_base(xdg_wm_base_context& context) noexcept
    : context_{&context} {
  }

  any_sequence_of<xdg_wm_base> xdg_wm_base::bind(registry_context& ctx) {
    return ctx.bind<xdg_wm_base_context>(ctx.display_);
  }

  struct xdg_surface_context : object {

    static any_sender_of<>
      on_configure(object* self, message_header hdr, std::span<std::byte> payload);

    xdg_wm_base_context* xdg_wm_base_;
    surface_context* surface_;

    position offset{0, 0};
    dynamic_extents<2> extents{0, 0};
  };

  struct get_xdg_surface_msg {
    message_header header;
    id new_id;
    id surface_id;
  };

  // static auto open(xdg_surface_context& context) {
  //   return                                 //
  //     stdexec::just(get_xdg_surface_msg{}) //
  //     | stdexec::let_value([&](get_xdg_surface_msg& msg) {
  //         msg.header.opcode = XDG_WM_BASE_GET_XDG_SURFACE;
  //         msg.header.object_id = context.xdg_wm_base_.id_;
  //         msg.header.message_length = sizeof(get_xdg_surface_msg);
  //         return context.xdg_wm_base_.display_->connection_.send(msg) //
  //              | stdexec::then([&]() {
  //                  log("xdg_surface", "Opened xdg_surface object.");
  //                  return xdg_surface{context};
  //                });
  //       });
  // }
}