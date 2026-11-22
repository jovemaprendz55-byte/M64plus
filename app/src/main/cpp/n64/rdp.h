#pragma once

#include <array>
#include <cstdint>

namespace m64plus::n64 {

class Bus;

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

struct RdpCommandPacket final {
    static constexpr std::size_t kMaxWords = 22U;
    RdpCommandInfo info{};
    std::array<std::uint64_t, kMaxWords> words{};
};

class RdpCommandStream final {
public:
    // Fetches one complete decoded command from DPC; failure leaves the packet and cursor unchanged.
    static bool fetchNext(Bus& bus, RdpCommandPacket& packet) noexcept;
};

} // namespace m64plus::n64
