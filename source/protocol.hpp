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

#include "./any_sender_of.hpp"
#include "./construct.hpp"
#include "./mdspan.hpp"
#include "./message_header.hpp"

#include <sio/async_resource.hpp>
#include <sio/sequence/then_each.hpp>

#include <exec/linux/io_uring_context.hpp>

#include <span>

namespace wayland {
  struct display_context;

  class registry;

  class display_handle {
   public:
    display_handle();
    explicit display_handle(display_context* resource) noexcept;

    any_sequence_of<registry> get_registry() const;

    any_sender_of<> sync() const;

    friend auto operator<=>(const display_handle&, const display_handle&) noexcept = default;

   private:
    display_context* context_;
  };

  class display {
   public:
    display();
    explicit display(exec::io_uring_context& ctx) noexcept;

   private:
    friend class sio::async::use_t;
    any_sequence_of<display_handle> use(sio::async::use_t) const;

    exec::io_uring_context* context_{};
  };

  struct registry_context;

  class registry {
   public:
    registry() = default;
    explicit registry(registry_context* context);

    any_sequence_of<name, std::string_view, id> on_global();
    any_sequence_of<name> on_global_removal();

    display_handle get_display() const;

    template <class Tp>
    any_sequence_of<Tp> bind() const {
      return Tp::bind(*context_);
    }

    std::optional<name> find_interface(std::string_view name) const;

    friend auto operator<=>(const registry&, const registry&) noexcept = default;

   private:
    // any_sequence_of<wayland::object, void*> bind(name which, std::span<event_handler> vtable);
    registry_context* context_{nullptr};
  };

  struct buffer_context;

  class buffer {
   public:
    buffer() = default;
    explicit buffer(buffer_context& context) noexcept;

    void* data() const noexcept;
    dynamic_extents<2> extents() const noexcept;
    uint32_t format() const noexcept;

    id id() const noexcept;

   private:
    buffer_context* context_;
  };

  // class shm;

  struct shm_pool_context;

  class shm_pool {
   public:
    shm_pool() = default;
    explicit shm_pool(shm_pool_context& context) noexcept;

    any_sequence_of<buffer>
      create_buffer(int32_t offset, dynamic_extents<2> extents, uint32_t format = 0);

   private:
    shm_pool_context* context_{nullptr};
  };

  struct shm_context;

  class shm {
   public:
    static any_sequence_of<shm> bind(registry_context& registry);

    shm() = default;

    explicit shm(shm_context& context) noexcept
      : context_(&context) {
    }

    any_sequence_of<shm_pool> create_pool(int fd, int32_t size);

   private:
    shm_context* context_{nullptr};
  };

  struct surface_context;

  struct position {
    int32_t x;
    int32_t y;
  };

  class surface {
   public:
    surface();
    explicit surface(surface_context& context) noexcept;

    id id() const noexcept;

    any_sender_of<> attach(buffer buffer, position offset) const;

    any_sender_of<> damage(position offset, dynamic_extents<2> size) const;

    any_sender_of<> commit() const;

   private:
    surface_context* context_{nullptr};
  };

  struct compositor_context;

  class compositor {
   public:
    static any_sequence_of<compositor> bind(registry_context& registry);

    any_sequence_of<surface> create_surface();

    // any_sequence_of<region> create_region();

    explicit compositor(compositor_context& context) noexcept
      : context_(&context) {
    }

   private:
    compositor_context* context_{nullptr};
  };

  class xdg_toplevel;
  class xdg_surface;

  struct xdg_wm_base_context;

  class xdg_wm_base {
   public:
    static any_sequence_of<xdg_wm_base> bind(registry_context& registry);

    any_sequence_of<xdg_surface> get_xdg_surface(surface surface);

    xdg_wm_base();
    explicit xdg_wm_base(xdg_wm_base_context& context) noexcept;

   private:
    xdg_wm_base_context* context_{nullptr};
  };

  struct xdg_surface_context;

  class xdg_surface {
   public:
    xdg_surface() = default;

    explicit xdg_surface(xdg_surface_context* context) noexcept;

    any_sequence_of<xdg_toplevel> get_toplevel() const;

    any_sequence_of<uint32_t> on_configure() const;

    any_sender_of<> set_window_geometry(position pos, dynamic_extents<2> size) const;

    any_sender_of<> ack_configure(uint32_t serial) const;

   private:
    xdg_surface_context* context_{nullptr};
  };

  struct xdg_toplevel_context;

  class xdg_toplevel {
   public:
    xdg_toplevel() = default;

    explicit xdg_toplevel(xdg_toplevel_context* context) noexcept;

    any_sequence_of<uint32_t> on_configure() const;

    any_sender_of<> set_title(std::string title) const;

    any_sender_of<> set_app_id(std::string app_id) const;

    any_sender_of<> set_max_size(dynamic_extents<2> size) const;

    any_sender_of<> set_min_size(dynamic_extents<2> size) const;

    any_sender_of<> set_maximized() const;

    any_sender_of<> unset_maximized() const;

    any_sender_of<> set_fullscreen() const;

    any_sender_of<> unset_fullscreen() const;

    any_sender_of<> set_minimized() const;

   private:
    xdg_toplevel_context* context_{nullptr};
  };
}