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

#include <sio/sequence/empty_sequence.hpp>
#include <sio/sequence/ignore_all.hpp>
#include <sio/sequence/then_each.hpp>
#include <sio/sequence/merge_each.hpp>

#include <sio/tap.hpp>

namespace wayland {
  namespace {
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
        wayland::display_context* display,
        std::span<event_handler> vtable) noexcept;

      Receiver receiver_;
      wayland::display_context* display_;
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

      callback(Receiver rcvr, wayland::display_context* display);

      void start(stdexec::start_t) noexcept;
    };

    struct callback_sender {
      wayland::display_context* display_;

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
  }

  struct display_context : object {
    // [event callbacks]

    static void on_error(object* obj, message_header, std::span<std::byte> buffer) noexcept;

    static void on_delete_id(object* obj, message_header, std::span<std::byte> buffer) noexcept;

    [[noreturn]] static void on_destroyed_(object* obj) noexcept;

    static std::span<event_handler, 2> get_vtable() noexcept;

    // Constructors

    explicit display_context(connection_handle connection) noexcept;

    ~display_context() {
      log("display", "Destroying display context. ptr = {}", (void*) this);
    }

    // Methods

    void register_object(object& obj) noexcept;

    void unregister_object(const object& obj) noexcept;

    id get_new_id() noexcept;

    callback_sender sync() noexcept;

    connection_handle connection_;
    std::vector<object*> objects_;
    std::vector<uint32_t> free_ids_;
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

  // struct global {
  //   wayland::name name;
  //   std::string interface;
  //   wayland::version version;
  // };

  // struct bind_message_t {
  //   message_header header;
  //   wayland::name which;
  //   wayland::id new_id;
  // };

  // auto make_bind_sender(id registry_id, display_context* display, name which, object& obj) {
  //   display->register_object(obj);
  //   bind_message_t message{};
  //   message.header.message_length = sizeof(bind_message_t);
  //   message.header.object_id = display->id_;
  //   message.header.opcode = 0;
  //   message.which = which;
  //   message.new_id = obj.id_;
  //   return stdexec::let_value(stdexec::just(message), [display](bind_message_t& msg) {
  //     return stdexec::when_all(display->connection_.send(msg), display->sync());
  //   });
  // }

  // struct get_registry_t {
  //   message_header header;
  //   id new_id;
  // };


  // struct registry_;

  // struct registry_handle {
  //   registry_* impl_;
  //   any_sender_of<> close(sio::async::close_t) const;
  // };

  // struct registry_ : object {
  //   static void
  //     on_global(object* obj, message_header header, std::span<std::byte> message) noexcept;
  //   static void
  //     on_global_remove(object* obj, message_header header, std::span<std::byte> message) noexcept;

  //   static std::span<event_handler, 2> get_vtable() noexcept;

  //   explicit registry_(wayland::display_context* display) noexcept;

  //   using bind_sender_t = decltype(make_bind_sender(
  //     id{},
  //     std::declval<wayland::display_context*>(),
  //     name{},
  //     std::declval<object&>()));

  //   bind_sender_t bind(name global, object& obj) const;

  //   auto open(sio::async::open_t) {
  //     return sio::let_value_each(
  //       stdexec::just(this, get_registry_t{}), [](registry_* impl, get_registry_t& message) {
  //         impl->id_ = impl->display_->get_new_id();
  //         log("registry", "Register registry object as id {}.", static_cast<int>(impl->id_));
  //         impl->display_->register_object(*impl);
  //         message.header.object_id = impl->display_->id_;
  //         message.header.opcode = 1;
  //         message.header.message_length = sizeof(get_registry_t);
  //         message.new_id = impl->id_;
  //         return stdexec::when_all(
  //           impl->display_->connection_.send(message),
  //           impl->display_->sync(),
  //           stdexec::just(registry_handle{impl}));
  //       });
  //   }

  //   wayland::display_context* display_;
  //   std::vector<global> globals_;
  // };

  // struct shm_ : object {
  //   static std::span<event_handler, 0> get_vtable() noexcept;

  //   explicit shm_(wayland::display_context* display) noexcept;
  // };

  // // <interface name="wl_compositor" version="5">
  // //   <request name="create_surface">
  // //     <arg name="id" type="new_id" interface="wl_surface" summary="the new surface"/>
  // //   </request>

  // //   <request name="create_region">
  // //     <arg name="id" type="new_id" interface="wl_region" summary="the new region"/>
  // //   </request>
  // // </interface>
  // struct create_surface_sender;
  // struct create_region_sender;

  // struct compositor_ : object {
  //   static std::span<event_handler, 0> get_vtable() noexcept;

  //   explicit compositor_(wayland::display_context* display) noexcept;

  //   create_surface_sender create_surface() const noexcept;

  //   create_region_sender create_region() const noexcept;
  // };

  ///////////////////////////////////////////////////////////////////////////
  //                                                          IMPLEMENTATION

  // Implementation of the synchroneous callback sender

  // callback_base

  template <class Receiver>
  callback_base<Receiver>::callback_base(
    Receiver rcvr,
    wayland::display_context* display,
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
  callback<Receiver>::callback(Receiver rcvr, wayland::display_context* display)
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
    self->free_ids_.push_back(value);
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
    if (objects_[index]) {
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
      uint32_t index = free_ids_.back();
      free_ids_.pop_back();
      return id{index};
    }
  }

  callback_sender display_context::sync() noexcept {
    return callback_sender{this};
  }

  display_handle::display_handle() = default;

  display_handle::display_handle(display_context* context) noexcept
    : context_{context} {
  }

  display::display() = default;

  display::display(connection_handle connection)
    : connection_{connection} {
  }

  any_sequence_of<display_handle> display::use(sio::async::use_t) const {
    auto context = sio::make_deferred<display_context>(connection_);
    return stdexec::let_value(
             stdexec::just(context),
             [](auto& ctx) {
               ctx();
               log("display", "Opened display.");
               display_context* ptr = ctx.get();
               auto seq = sio::tap(stdexec::just(display_handle{ptr}), close_display_sender{ptr});
               return stdexec::just(seq);
             })
         | sio::merge_each();
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

  /////////////////////////////////////////////////////////////////////////////
  //                                                    REGISTRY IMPLEMENTATION

  // registry_::registry_(wayland::display_context* display) noexcept
  //   : object{get_vtable(), id{}}
  //   , display_{display} {
  //   log("registry", "Creating registry object.");
  // }

  // std::span<event_handler, 2> registry_::get_vtable() noexcept {
  //   static std::array<event_handler, 2> vtable = {&on_global, &on_global_remove};
  //   return vtable;
  // }

  // void registry_::on_global(object* obj, message_header, std::span<std::byte> buffer) noexcept {
  //   registry_* self = static_cast<registry_*>(obj);
  //   auto [which, interface, ver] = extract<name, std::string_view, version>(buffer);
  //   log(
  //     "registry",
  //     "New global object '{}': interface '{}' (version {})",
  //     static_cast<int>(which),
  //     interface,
  //     static_cast<int>(ver));
  //   self->globals_.push_back(global{which, std::string{interface}, ver});
  // }

  // void
  //   registry_::on_global_remove(object* obj, message_header, std::span<std::byte> buffer) noexcept {
  //   registry_* self = static_cast<registry_*>(obj);
  //   auto which = extract<name>(buffer);
  //   log("registry", "Removed global object '{}'", static_cast<int>(which));
  //   auto elem = std::find_if(
  //     self->globals_.begin(), self->globals_.end(), [which](const global& g) {
  //       return g.name == which;
  //     });
  //   if (elem != self->globals_.end()) {
  //     self->globals_.erase(elem);
  //   } else {
  //     log("registry", "Global object '{}' was not found.", static_cast<int>(which));
  //   }
  // }

  // registry::registry(void* impl)
  //   : impl_{impl} {
  // }

  // struct bind_message {
  //   message_header header;
  //   name name;
  //   id new_id;
  // };

  // struct registered_object {
  //   object obj;
  //   display_* display;

  //   auto open(sio::async::open_t) {
  //     return just_invoke([this] {
  //       obj.id_ = display->get_new_id();
  //       display->register_object(obj);
  //       return *this;
  //     });
  //   }

  //   auto close(sio::async::close_t) const {
  //     return just_invoke([this] { //
  //       display->unregister_object(obj);
  //     });
  //   }
  // };

  // any_sequence_of<object, void*> registry::bind(name which, std::span<event_handler> vtable) {
  //   registry_* self = static_cast<registry_*>(this->impl_);
  //   auto elem = std::find_if(
  //     self->globals_.begin(), self->globals_.end(), [which](const global& g) {
  //       return g.name == which;
  //     });
  //   if (elem != self->globals_.end()) {
  //     log(
  //       "registry",
  //       "Binding interface '{}' to object name '{}'.",
  //       elem->interface,
  //       static_cast<int>(which));
  //     object obj{vtable, id{}};
  //     return stdexec::just(self, bind_message{}, registered_object{obj, self->display_}) //
  //          | stdexec::let_value(
  //              [which](registry_* self, bind_message& msg, registered_object& obj) {
  //                return stdexec::just(sio::let_value_each(
  //                  sio::async::run(obj), [&msg, which, self](registered_object obj) {
  //                    msg.header.opcode = 0;
  //                    msg.header.object_id = self->id_;
  //                    msg.header.message_length = sizeof(bind_message);
  //                    msg.name = which;
  //                    msg.new_id = obj.obj.id_;
  //                    return stdexec::when_all(
  //                      self->display_->connection_.send(msg),
  //                      stdexec::just(obj.obj, static_cast<void*>(self->display_)));
  //                  }));
  //              })
  //          | sio::merge_each();
  //   } else {
  //     log("registry", "Interface '{}' was not found.", static_cast<int>(which));
  //     return stdexec::just_error(std::make_error_code(std::errc::no_such_file_or_directory));
  //   }
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

  // any_sender_of<> registry_handle::close(sio::async::close_t) const {
  //   return just_invoke([impl = this->impl_] {
  //     log("registry", "Closing registry object.");
  //     auto self = static_cast<registry_*>(impl);
  //     wayland::display_context* d = self->display_;
  //     d->unregister_object(*self);
  //     delete self;
  //   });
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