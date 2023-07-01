#pragma once

#include "./any_sender_of.hpp"
#include "./connection.hpp"
#include "./construct.hpp"
#include "./message_header.hpp"

namespace wayland {
  struct object;

  using event_handler = void (*)(object*, std::span<std::byte>) noexcept;

  enum class name : uint32_t {};
  enum class id : uint32_t {};
  enum class version : uint32_t {};

  struct object {
    std::span<event_handler> events_;
    id id_;
  };

  class display;

  class registry {
   public:
    ~registry();

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
    impl* impl_;
  };

  class display : public object {
   public:
    explicit display(connection_handle connection);
    ~display();

    any_sender_of<registry> get_registry(uint32_t id);

   private:
    struct impl;
    impl* impl_;
  };

}