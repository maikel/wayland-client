#pragma once

#include <cstdint>
#include <cstring>

namespace wayland {

  enum class name : uint32_t {
  };
  enum class id : uint32_t {
  };
  enum class version : uint32_t {
  };

  struct message_header {
    id object_id;
    uint16_t opcode;
    uint16_t message_length;
  };

} // namespace wayland