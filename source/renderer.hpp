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

namespace wayland {

  struct position {
    int32_t x;
    int32_t y;
  };

  struct rectangle {
    int32_t width;
    int32_t height;
  };

  struct color {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t alpha;
  };

  class render_context;

  class renderer {
   public:
    void draw(rectangle rect, position pos, color col) const;

    void render() const;

   private:
    friend class render_context;
    render_context* context_;
  };

  class render_context {
   public:
    render_context(exec::io_uring_context& context);

   private:
    friend class sio::async::use_t;
    any_sequence_of<renderer> use(sio::async::use_t);

    wayland::connection connection_;
    wayland::display display_{};
    wayland::registry registry_{};
    wayland::compositor compositor_{};
    wayland::shm shm_{};
    wayland::shm_pool pool_{};
    wayland::buffer buffer_{};
  };

}