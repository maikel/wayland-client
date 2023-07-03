#include "./protocol.hpp"
#include "./logging.hpp"

#include <sio/sequence/ignore_all.hpp>
#include <sio/sequence/then_each.hpp>

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
      return {reinterpret_cast<char*>(buffer.data()), buffer.size()};
    }

    template <class Tp>
    Tp extract_and_advance(std::span<std::byte>& buffer) noexcept {
      Tp result = extract<Tp>(buffer);
      buffer = buffer.subspan(sizeof(Tp));
      return result;
    }

    template <class... Tps>
      requires(sizeof...(Tps) > 1)
    std::tuple<Tps...> extract(std::span<std::byte> buffer) noexcept {
      return {extract_and_advance<Tps>(buffer)...};
    }

    struct dispatch_receiver {
      using is_receiver = void;
      stdexec::in_place_stop_token stop_token_;

      auto get_env(stdexec::get_env_t) const noexcept {
        return exec::make_env(exec::with(stdexec::get_stop_token, stop_token_));
      }

      void set_value(stdexec::set_value_t) const noexcept {
        log("display", "Message dispatching is completed.");
      }

      void set_error(stdexec::set_error_t, std::error_code) const noexcept {
        log("display", "dispatch_receiver::set_error");
      }

      void set_error(stdexec::set_error_t, std::exception_ptr) const noexcept {
        log("display", "dispatch_receiver::set_error");
      }

      void set_stopped(stdexec::set_stopped_t) const noexcept {
        log("display", "dispatch_receiver::set_stopped");
      }
    };

    auto dispatch(
      any_sequence_of<std::span<std::byte>> messages,
      std::vector<object*>& objects,
      stdexec::in_place_stop_token stop_token) {
      return stdexec::connect(
        sio::then_each(
          std::move(messages),
          [&objects](std::span<std::byte> message) {
            message_header header = extract_and_advance<message_header>(message);
            auto index = header.object_id - 1;
            if (index < objects.size()) {
              if (objects[index]) {
                if (header.opcode >= objects[index]->events_.size()) {
                  log(
                    "display", "Unknown opcode {} for object {}", header.opcode, header.object_id);
                } else {
                  objects[index]->events_[header.opcode](objects[index], header, message);
                }
              } else {
                log("display", "Object {} is not registered.", header.object_id);
              }
            } else {
              log("display", "Object {} is out of range.", header.object_id);
            }
          })
          | sio::ignore_all(),
        dispatch_receiver{stop_token});
    }

    struct display_;

    template <class Receiver>
    struct callback_base : object {

      callback_base(
        Receiver rcvr,
        wayland::display_* display,
        std::span<event_handler> vtable) noexcept
        : object{vtable}
        , receiver_(std::move(rcvr))
        , display_(display) {
      }

      Receiver receiver_;
      wayland::display_* display_;
      int op_counter_{0};
    };

    template <class Receiver>
    struct callback_receiver {
      using is_receiver = void;
      using __id = callback_receiver;
      using __t = callback_receiver;

      callback_base<Receiver>* op_;

      stdexec::env_of_t<Receiver> get_env(stdexec::get_env_t) const noexcept {
        return stdexec::get_env(op_->receiver_);
      }

      void unregister_callback() const noexcept;

      void set_value(stdexec::set_value_t) const noexcept {
        log("callback", "Sent message");
      }

      void set_error(stdexec::set_error_t, std::error_code ec) const noexcept {
        log("callback", "Error sending message: {} (Error Code: {})", ec.message(), ec.value());
        unregister_callback();
        stdexec::set_error(std::move(op_->receiver_), std::move(ec));
      }

      void set_error(stdexec::set_error_t, std::exception_ptr ep) const noexcept {
        try {
          std::rethrow_exception(ep);
        } catch (const std::exception& e) {
          log("callback", "Error sending message: {}", e.what());
        } catch (...) {
          log("callback", "Error sending message: Unknown error type.");
        }
        unregister_callback();
        stdexec::set_error(std::move(op_->receiver_), std::move(ep));
      }

      void set_stopped(stdexec::set_stopped_t) const noexcept {
        log("callback", "Stopped sending message");
        unregister_callback();
        stdexec::set_stopped(std::move(op_->receiver_));
      }
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

      callback(Receiver rcvr, wayland::display_* display);

      void start(stdexec::start_t) noexcept;
    };

    struct callback_sender {
      wayland::display_* display_;

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

    struct display_ : object {
      static void on_error(object* obj, message_header, std::span<std::byte> buffer) noexcept {
        display_* self = static_cast<display_*>(obj);
        auto [which, ec, reason] = extract<id, uint32_t, std::string_view>(buffer);
        log("display", "Object '{}': Error ({}): {}", static_cast<int>(which), ec, reason);
      }

      static void on_delete_id(object* obj, message_header, std::span<std::byte> buffer) noexcept {
        display_* self = static_cast<display_*>(obj);
        id which = extract<id>(buffer);
        log("display", "Deleted object '{}'", static_cast<int>(which));
        uint32_t value = static_cast<uint32_t>(which);
        if (value < self->objects_.size()) {
          if (self->objects_[value]) {
            self->objects_[value]->destroyed_(self->objects_[value]);
          }
          self->objects_[value] = nullptr;
        }
      }

      [[noreturn]] static void on_destroyed_(object* obj) noexcept {
        std::terminate();
      }

      static std::span<event_handler, 2> get_vtable() noexcept {
        static std::array<event_handler, 2> vtable = {&on_error, &on_delete_id};
        return vtable;
      }

      explicit display_(connection_handle connection) noexcept
        : object{get_vtable(), id{1}, &on_destroyed_}
        , connection_(connection)
        , objects_{}
        , operation_{dispatch(connection.subscribe(), objects_, stop_source_.get_token())} {
        objects_.push_back(this);
        stdexec::start(operation_);
      }

      void register_object(object& obj) noexcept {
        uint32_t index = static_cast<uint32_t>(obj.id_);
        if (index > objects_.size()) {
          objects_.resize(2 * index);
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

      void unregister_object(object& obj) noexcept {
        uint32_t index = static_cast<uint32_t>(obj.id_);
        assert(index > 0);
        assert(index <= objects_.size());
        index -= 1;
        if (objects_[index] == &obj) {
          objects_[index] = nullptr;
          free_ids_.push_back(index + 1);
          log("display", "Unregistered object {}.", static_cast<int>(obj.id_));
        } else {
          log("display", "Object {} is not registered.", static_cast<int>(obj.id_));
        }
      }

      id get_new_id() noexcept {
        if (free_ids_.empty()) {
          return static_cast<id>(objects_.size() + 1);
        } else {
          uint32_t index = free_ids_.back();
          free_ids_.pop_back();
          return id{index};
        }
      }

      callback_sender sync() noexcept {
        return callback_sender{this};
      }

      connection_handle connection_;
      std::vector<object*> objects_;
      std::vector<uint32_t> free_ids_;
      using operation_type = decltype(dispatch(
        std::declval<connection_handle&>().subscribe(),
        std::declval<std::vector<object*>&>(),
        std::declval<stdexec::in_place_stop_token>()));
      stdexec::in_place_stop_source stop_source_{};
      operation_type operation_;
    };

    template <class Receiver>
    callback<Receiver>::callback(Receiver rcvr, wayland::display_* display)
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
      sync_message_.header.object_id = static_cast<uint32_t>(this->display_->id_);
      sync_message_.header.opcode = 0;
      sync_message_.header.message_length = sizeof(sync_message_t);
      sync_message_.new_id = this->id_;
      log("callback", "Register as id {}.", static_cast<int>(this->id_));
      this->display_->register_object(*this);
      log("callback", "Sending sync message.");
      stdexec::start(this->send_operation_);
    }
  }

  struct display::impl : display_ {
    explicit impl(connection_handle connection) noexcept
      : display_{connection} {
    }
  };

  display::display(connection_handle connection)
    : impl_{std::bit_cast<impl*>(connection)} {
  }

  any_sender_of<display> display::open(sio::async::open_t) {
    return just_invoke([this] {
      log("display", "Opening a new display.");
      connection_handle connection = std::bit_cast<connection_handle>(this->impl_);
      this->impl_ = new impl(connection);
      return *this;
    });
  }

  any_sender_of<> display::close(sio::async::close_t) const {
    display_* self = this->impl_;
    return just_invoke([self] {
      log("display", "Closing this display.");
      self->stop_source_.request_stop();
    });
  }

  struct get_registry_t {
    message_header header;
    id new_id;
  };

  any_sender_of<registry> display::get_registry() {
    display_* self = this->impl_;
    registry reg{};
    get_registry_t message{};
    auto sender = stdexec::let_value(
      stdexec::just(self, reg, message), [](display_* self, registry reg, get_registry_t& message) {
        return stdexec::then(self->sync(), [reg] { return reg; });
      });
    return sender;
  }
}