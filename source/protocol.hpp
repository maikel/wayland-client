#pragma once

#include "./any_sender_of.hpp"
#include "./connection.hpp"
#include "./construct.hpp"
#include "./message_header.hpp"

namespace wayland {
  struct object;

  using event_handler = void (*)(object*, message_header, std::span<std::byte>) noexcept;

  enum class name : uint32_t {
  };
  enum class id : uint32_t {
  };
  enum class version : uint32_t {
  };

  struct object {
    std::span<event_handler> events_;
    void (*destroyed_)(object*) noexcept;
    id id_;
  };

  class display;

  class registry {
   public:
    registry() = default;
    ~registry() {}

    any_sequence_of<name, std::string_view, id> on_global();
    any_sequence_of<name> on_global_removal();

    template <class Tp>
    any_sender_of<Tp> bind(name which, id new_id) {
      return bind(which, new_id, Tp::get_vtable()) | construct<Tp>();
    }

    const wayland::object& object() const noexcept;

   private:
    any_sender_of<wayland::object> bind(name which, id new_id, std::span<event_handler> vtable);

    friend class display;

    struct impl;
    impl* impl_{nullptr};
  };

  class display : public object {
   public:
    any_sender_of<registry> get_registry(id new_id);

    explicit display(connection_handle connection, id new_id = id{1});

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