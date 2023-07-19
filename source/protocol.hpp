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
#include "./message_header.hpp"

#include <sio/async_resource.hpp>
#include <sio/sequence/then_each.hpp>

#include <exec/linux/io_uring_context.hpp>

#include <span>

namespace wayland {
  struct registry_context;

  class registry {
   public:
    registry() = default;
    explicit registry(registry_context* context);

    any_sequence_of<name, std::string_view, id> on_global();
    any_sequence_of<name> on_global_removal();

    template <class Tp>
    any_sequence_of<Tp> bind() const { return Tp::bind(*context_); }

    std::optional<name> find_interface(std::string_view name) const;

    friend auto operator<=>(const registry&, const registry&) noexcept = default;

   private:
    // any_sequence_of<wayland::object, void*> bind(name which, std::span<event_handler> vtable);
    registry_context* context_{nullptr};
  };

  struct display_context;

  class display_handle {
   public:
    display_handle();
    explicit display_handle(display_context* resource) noexcept;

    any_sequence_of<registry> get_registry() const;

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

  // class surface {
  //  public:
  //   inline static const char* interface_name = "wl_surface";
  //   static std::span<event_handler> get_vtable();

  //   surface() = default;
  //   explicit surface(wayland::object object, wayland::display* display);

  //  private:
  //   void* impl_{nullptr};
  // };

  // class region {
  //  public:
  //   inline static const char* interface_name = "wl_region";
  //   static std::span<event_handler> get_vtable();

  //   region() = default;
  //   explicit region(wayland::object object, wayland::display* display);

  //  private:
  //   void* impl_;
  // };

  // class buffer {
  //  public:
  //   inline static const char* interface_name = "wl_buffer";
  //   static std::span<event_handler> get_vtable();

  //   buffer() = default;
  //   explicit buffer(wayland::object object, wayland::display* display);

  //   any_sequence_of<> on_release();

  //  private:
  //   void* impl_;
  // };

  // class shm;

  // class shm_pool {
  //  public:
  //   inline static const char* interface_name = "wl_shm_pool";
  //   static std::span<event_handler> get_vtable();

  //   shm_pool() = default;
  //   explicit shm_pool(wayland::object object, void* display);

  //   any_sequence_of<buffer>
  //     create_buffer(int32_t offset, int32_t width, int32_t height, int32_t stride, uint32_t format);

  //  private:
  //   friend class shm;
  //   wayland::object obj_;
  //   void* impl_{nullptr};
  // };

  // class shm {
  //  public:
  //   inline static const char* interface_name = "wl_shm";
  //   static std::span<event_handler> get_vtable();

  //   shm() = default;
  //   explicit shm(wayland::object object, void* data);

  //   any_sequence_of<shm_pool> create_pool(int fd, int32_t size);
  //  private:
  //   wayland::object obj_;
  //   void* impl_{nullptr};
  // };

  struct compositor_context;

  class compositor {
   public:
    static any_sequence_of<compositor> bind(registry_context& registry);

    // any_sequence_of<surface> create_surface();

    // any_sequence_of<region> create_region();

   private:
    compositor_context* impl_{nullptr};
  };
}