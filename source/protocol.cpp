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
        log("display", "dispatch_receiver::set_value");
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
        : object{get_vtable(), &on_destroyed_, id{1}}
        , connection_(connection)
        , objects_{}
        , operation_{dispatch(connection.subscribe(), objects_, stop_source_.get_token())} {
        objects_.push_back(this);
        stdexec::start(operation_);
      }

      connection_handle connection_;
      std::vector<object*> objects_;
      using operation_type = decltype(dispatch(
        std::declval<connection_handle&>().subscribe(),
        std::declval<std::vector<object*>&>(),
        std::declval<stdexec::in_place_stop_token>()));
      stdexec::in_place_stop_source stop_source_{};
      operation_type operation_;
    };
  }

  struct display::impl : display_ {
    explicit impl(connection_handle connection) noexcept
      : display_{connection} {
    }
  };

  display::display(connection_handle connection, id new_id)
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

  struct sync_t {
    message_header header;
    uint32_t data;
  };

  struct synced_get_registry_t {
    get_registry_t get_registry;
    sync_t sync;
  };

  any_sender_of<registry> display::get_registry(id new_id) {
    display_* self = this->impl_;
    get_registry_t message{
      .header =
        {
                 .object_id = 1,
                 .opcode = 1,
                 .message_length = sizeof(get_registry_t),
                 },
      .new_id = new_id
    };
    sync_t sync{
      .header =
        {
                 .object_id = 1,
                 .opcode = 0,
                 .message_length = sizeof(sync),
                 },
      .data = static_cast<uint32_t>(new_id) + 1
    };
    return stdexec::let_value(
      stdexec::just(self, message, sync), [](display_* self, auto& message, auto& sync) {
        return stdexec::let_value(
          stdexec::when_all(self->connection_.send(message), self->connection_.send(sync)),
          [] { return construct<registry>(stdexec::just()); });
      });
  }
}