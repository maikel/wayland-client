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
#pragma once

#include "./protocol.hpp"
#include "./font.hpp"

namespace wayland {

  struct rectangle {
    int32_t width;
    int32_t height;
  };

  struct color {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t alpha;

    static color black() noexcept {
      return {0x00, 0x00, 0x00, 0xFF};
    }
  };

  struct render_context;

  class renderer_handle {
   public:
    explicit renderer_handle(render_context& context) noexcept;

    void draw_rectangle(position lower, position upper, color fill) const noexcept;

    void draw_text(std::string_view text, position pos, color fill) const noexcept;

    void render() const noexcept;

   private:
    render_context* context_;
  };

  struct renderer_options {
    int width;
    int height;
    color background;
  };

  class renderer {
   public:
    renderer(exec::io_uring_context& context, renderer_options opts) noexcept;

   private:
    friend class sio::async::use_t;
    any_sequence_of<renderer_handle> use(sio::async::use_t) noexcept;

    exec::io_uring_context* context_;
    renderer_options opts_;
  };
}