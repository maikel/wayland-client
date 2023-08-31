#pragma once

#include "./protocol.hpp"

namespace wayland {

  struct window_context;

  struct mouse_event {
    struct position_t {
      int32_t x;
      int32_t y;
    } position;
  };

  class window {
   public:
    window() = default;

    explicit window(window_context* context) noexcept
      : context_{context} {
    }

    static any_sequence_of<window> bind(registry_context& context);

    any_sequence_of<mouse_event> on_mouse_event();

   private:
    window_context* context_{};
  };

  any_sequence_of<window> make_window(exec::io_uring_context& context);

}