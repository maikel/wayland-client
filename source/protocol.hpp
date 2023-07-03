#pragma once

#include "./any_sender_of.hpp"
#include "./connection.hpp"
#include "./construct.hpp"
#include "./message_header.hpp"

namespace wayland {
  struct object;

  using event_handler = void (*)(object*, message_header, std::span<std::byte>) noexcept;

  struct object {
    std::span<event_handler> events_;
    id id_{};
    void (*destroyed_)(object*) noexcept{nullptr};
  };

  class display;

  class registry {
   public:
    any_sequence_of<name, std::string_view, id> on_global();
    any_sequence_of<name> on_global_removal();

    template <class Tp>
    any_sender_of<Tp> bind(name which) {
      return bind(which, Tp::get_vtable()) | construct<Tp>();
    }

    const wayland::object& object() const noexcept;

   private:
    any_sender_of<wayland::object, display*> bind(name which, std::span<event_handler> vtable);
    
    struct impl;

    friend class display;
    explicit registry(impl& impl);

    impl* impl_{nullptr};
  };

  class display : public object {
   public:
    any_sender_of<registry> get_registry();

    explicit display(connection_handle connection);

   private:
    friend class sio::async::close_t;
    any_sender_of<> close(sio::async::close_t) const;

    friend class sio::async::open_t;
    any_sender_of<display> open(sio::async::open_t);

   private:
    struct impl;
    impl* impl_;
  };

}