#pragma once

#include "./any_sender_of.hpp"

#include <sio/async_resource.hpp>

#include <exec/linux/io_uring_context.hpp>

#include <span>

namespace wayland {

  struct connection_context;

  class connection_handle {
     private:
      connection_context* connection_{};

     public:
      connection_handle();
      explicit connection_handle(connection_context& connection);

      any_sender_of<> send(const std::span<std::byte>& buffer);

      template <class Tp>
        requires stdexec::__not_decays_to<Tp, std::span<std::byte>>
      auto send(Tp& msg) {
        const std::span<std::byte> buffer(reinterpret_cast<std::byte*>(&msg), sizeof(Tp));
        return send(buffer);
      }

      any_sender_of<> send_with_fd(std::span<std::byte> buffer, int fd);

      template <class Tp>
        requires stdexec::__not_decays_to<Tp, std::span<std::byte>>
      auto send_with_fd(Tp& msg, int fd) {
        std::span<std::byte> buffer(reinterpret_cast<std::byte*>(&msg), sizeof(Tp));
        return send_with_fd(buffer, fd);
      }

      any_sequence_of<std::span<std::byte>> subscribe();

      auto operator<=>(const connection_handle&) const = default;
    };

    class connection {
     public:
      explicit connection(exec::io_uring_context& context)
        : context_{&context} {
      }

     private:
      exec::io_uring_context* context_;

      friend struct sio::async::use_t;
      any_sequence_of<connection_handle> use(sio::async::use_t) const;
    };

}