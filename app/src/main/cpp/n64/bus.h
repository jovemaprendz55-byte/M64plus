#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include "n64/rdram.h"

namespace m64plus::n64 {

enum class BusRegion : std::uint8_t {
    Rdram,
    SpDmem,
    SpImem,
    Unmapped
};

class Bus final {
public:
    static constexpr std::uint32_t kRdramBase = 0x00000000U;
    static constexpr std::uint32_t kSpDmemBase = 0x04000000U;
    static constexpr std::uint32_t kSpImemBase = 0x04001000U;
    static constexpr std::size_t kSpMemorySize = 4096U;

    Bus();
    void reset() noexcept;

    BusRegion region(std::uint32_t address) const noexcept;
    bool read8(std::uint32_t address, std::uint8_t& value) const noexcept;
    bool read16(std::uint32_t address, std::uint16_t& value) const noexcept;
    bool read32(std::uint32_t address, std::uint32_t& value) const noexcept;
    bool read64(std::uint32_t address, std::uint64_t& value) const noexcept;
    bool write8(std::uint32_t address, std::uint8_t value) noexcept;
    bool write16(std::uint32_t address, std::uint16_t value) noexcept;
    bool write32(std::uint32_t address, std::uint32_t value) noexcept;
    bool write64(std::uint32_t address, std::uint64_t value) noexcept;

    Rdram& rdram() noexcept;
    const Rdram& rdram() const noexcept;

private:
    bool rangeFits(std::uint32_t address, std::size_t width,
                   BusRegion& selected) const noexcept;
    bool readBytes(std::uint32_t address, std::uint8_t* output,
                   std::size_t width) const noexcept;
    bool writeBytes(std::uint32_t address, const std::uint8_t* input,
                    std::size_t width) noexcept;
    bool readByte(std::uint32_t address, std::uint8_t& value) const noexcept;
    bool writeByte(std::uint32_t address, std::uint8_t value) noexcept;

    Rdram rdram_{};
    std::array<std::uint8_t, kSpMemorySize> spDmem_{};
    std::array<std::uint8_t, kSpMemorySize> spImem_{};
};

} // namespace m64plus::n64
