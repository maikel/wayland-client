#include "./display.hpp"

namespace wayland { namespace {
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
  std::tuple<Tps...> extract(std::span<std::byte> buffer) noexcept {
    return {extract_and_advance<Tps>(buffer)...};
  }

  class display_ : object {
    static void on_error(object* obj, std::span<std::byte> buffer) noexcept {
      display_* self = static_cast<display_*>(obj);
      auto [header, which, ec, reason] = extract<message_header, id, uint32_t, std::string_view>(buffer);
      log("display", "object '{}': Error ({}): {}", static_cast<int>(which), ec, reason);
    }

    static void on_delete_id(object* obj, std::span<std::byte> buffer) noexcept {
      display_* self = static_cast<display_*>(obj);
      auto [header, which] = extract<message_header, id>(buffer);
      log("display", "deleted object '{}'", static_cast<int>(which));
      uint32_t value = static_cast<uint32_t>(which);
      if (value < objects_.size()) {
        objects_[value] = nullptr;
      }
    }

    static std::span<event_handler, 2> get_vtable() noexcept {
      static std::array<event_handler, 2> vtable = {&on_error, &on_delete_id};
      return vtable;
    }

   public:
    explicit display_(connection_handle connection) noexcept
      : object(connection, 1, get_vtable()) {
    }

   private:
    std::vector<object*> objects_;
  };
}}