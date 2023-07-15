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
#include "./connection.hpp"
#include "./construct.hpp"
#include "./message_header.hpp"

#include <sio/sequence/then_each.hpp>

namespace wayland {
  struct object;

  using event_handler = void (*)(object*, message_header, std::span<std::byte>) noexcept;

  struct object {
    std::span<event_handler> events_;
    id id_{};
    void (*destroyed_)(object*) noexcept {nullptr};
  };

  class display;

  class registry {
   public:
    registry() = default;

    any_sequence_of<name, std::string_view, id> on_global();
    any_sequence_of<name> on_global_removal();

    template <class Tp>
    any_sequence_of<Tp> bind() {
      auto which = find_interface(Tp::interface_name);
      if (which) {
        return bind(*which, Tp::get_vtable())
             | sio::then_each([](wayland::object obj, void* display) {
                 return Tp{obj, display};
               });
      } else {
        return stdexec::just_error(std::make_error_code(std::errc::invalid_argument));
      }
    }

    std::optional<name> find_interface(std::string_view name) const;

   private:
    friend class display;
    explicit registry(void* impl);

    any_sequence_of<wayland::object, void*> bind(name which, std::span<event_handler> vtable);

    void* impl_{nullptr};
  };

  class display;

  class display_handle {
   public:
    any_sequence_of<registry> get_registry();

   private:
    friend class sio::async::close_t;
    any_sender_of<> close(sio::async::close_t) const;


    friend class display;
    explicit display_handle(void* resource) noexcept;
    void* resource_;
  };

  class display {
   public:
    display() = default;
    explicit display(connection_handle connection);

   private:
    friend class sio::async::open_t;
    any_sender_of<display_handle> open(sio::async::open_t);

   private:
    connection_handle connection_;
  };

  class surface {
   public:
    inline static const char* interface_name = "wl_surface";
    static std::span<event_handler> get_vtable();

    surface() = default;
    explicit surface(wayland::object object, wayland::display* display);

   private:
    void* impl_{nullptr};
  };

  class region {
   public:
    inline static const char* interface_name = "wl_region";
    static std::span<event_handler> get_vtable();

    region() = default;
    explicit region(wayland::object object, wayland::display* display);

   private:
    void* impl_;
  };

  class buffer {
   public:
    inline static const char* interface_name = "wl_buffer";
    static std::span<event_handler> get_vtable();

    buffer() = default;
    explicit buffer(wayland::object object, wayland::display* display);

    any_sequence_of<> on_release();

   private:
    void* impl_;
  };

  class shm;

  class shm_pool {
   public:
    inline static const char* interface_name = "wl_shm_pool";
    static std::span<event_handler> get_vtable();

    shm_pool() = default;
    explicit shm_pool(wayland::object object, void* display);

    any_sequence_of<buffer>
      create_buffer(int32_t offset, int32_t width, int32_t height, int32_t stride, uint32_t format);

   private:
    friend class shm;
    wayland::object obj_;
    void* impl_{nullptr};
  };

  class shm {
   public:
    inline static const char* interface_name = "wl_shm";
    static std::span<event_handler> get_vtable();

    shm() = default;
    explicit shm(wayland::object object, void* data);

    any_sequence_of<shm_pool> create_pool(int fd, int32_t size);
   private:
    wayland::object obj_;
    void* impl_{nullptr};
  };

  class compositor {
   public:
    inline static const char* interface_name = "wl_compositor";
    static std::span<event_handler> get_vtable();

    compositor() = default;
    explicit compositor(wayland::object object, void* data);

    any_sequence_of<surface> create_surface();

    any_sequence_of<region> create_region();

   private:
    void* impl_{nullptr};
  };

}