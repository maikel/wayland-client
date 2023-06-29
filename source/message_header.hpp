#pragma once

#include <cstdint>

namespace wayland {

  struct message_header {
    uint32_t object_id;
    uint16_t opcode;
    uint16_t message_length;
  };

} // namespace wayland