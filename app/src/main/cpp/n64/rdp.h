#pragma once

#include <cstdint>

namespace m64plus::n64 {

struct RdpCommandInfo final {
    std::uint8_t opcode = 0;
    std::uint8_t lengthWords = 1;
    bool knownOpcode = false;
};

// Decodes the command header only. It does not execute rendering operations.
class RdpCommandDecoder final {
public:
    static RdpCommandInfo decode(std::uint64_t header) noexcept;
};

} // namespace m64plus::n64
