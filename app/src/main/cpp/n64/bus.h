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
    MiRegisters,
    SpRegisters,
    Unmapped
};

class Bus final {
public:
    static constexpr std::uint32_t kRdramBase = 0x00000000U;
    static constexpr std::uint32_t kSpDmemBase = 0x04000000U;
    static constexpr std::uint32_t kSpImemBase = 0x04001000U;
    static constexpr std::size_t kSpMemorySize = 4096U;
    static constexpr std::uint32_t kMiBase = 0x04300000U;
    static constexpr std::uint32_t kMiMode = kMiBase + 0x00U;
    static constexpr std::uint32_t kMiVersion = kMiBase + 0x04U;
    static constexpr std::uint32_t kMiInterrupt = kMiBase + 0x08U;
    static constexpr std::uint32_t kMiInterruptMask = kMiBase + 0x0CU;
    static constexpr std::uint32_t kMiVersionValue = 0x01010101U;
    static constexpr std::uint32_t kSpRegBase = 0x04040000U;
    static constexpr std::uint32_t kSpPc = 0x04080000U;
    static constexpr std::uint32_t kSpMemAddr = kSpRegBase + 0x00U;
    static constexpr std::uint32_t kSpDramAddr = kSpRegBase + 0x04U;
    static constexpr std::uint32_t kSpReadLength = kSpRegBase + 0x08U;
    static constexpr std::uint32_t kSpWriteLength = kSpRegBase + 0x0CU;
    static constexpr std::uint32_t kSpStatus = kSpRegBase + 0x10U;
    static constexpr std::uint32_t kSpDmaFull = kSpRegBase + 0x14U;
    static constexpr std::uint32_t kSpDmaBusy = kSpRegBase + 0x18U;
    static constexpr std::uint32_t kSpSemaphore = kSpRegBase + 0x1CU;
    static constexpr std::uint32_t kSpHalt = 1U;
    static constexpr std::uint32_t kSpBroke = 2U;
    static constexpr std::uint32_t kSpSingleStep = 1U << 5U;
    static constexpr std::uint32_t kSpInterruptOnBreak = 1U << 6U;
    static constexpr std::uint32_t kSpSignal0 = 1U << 7U;
    static constexpr std::uint32_t kSpClearSingleStep = 1U << 5U;
    static constexpr std::uint32_t kSpSetSingleStep = 1U << 6U;
    static constexpr std::uint32_t kSpClearInterruptOnBreak = 1U << 7U;
    static constexpr std::uint32_t kSpSetInterruptOnBreak = 1U << 8U;

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
    void setMiInterrupt(unsigned source, bool asserted) noexcept;
    std::uint32_t miInterruptMask() const noexcept;
    bool miInterruptPending() const noexcept;
    bool spHalted() const noexcept { return (spStatus_ & kSpHalt) != 0U; }
    bool spBroke() const noexcept { return (spStatus_ & kSpBroke) != 0U; }
    std::uint32_t spProgramCounter() const noexcept { return spProgramCounter_; }
    void setSpProgramCounter(std::uint32_t pc) noexcept { spProgramCounter_ = pc & 0x0FFCU; }
    void signalSpBreak() noexcept {
        spStatus_ |= kSpHalt | kSpBroke;
        if ((spStatus_ & kSpInterruptOnBreak) != 0U) setMiInterrupt(0U, true);
    }

private:
    bool rangeFits(std::uint32_t address, std::size_t width,
                   BusRegion& selected) const noexcept;
    bool readBytes(std::uint32_t address, std::uint8_t* output,
                   std::size_t width) const noexcept;
    bool writeBytes(std::uint32_t address, const std::uint8_t* input,
                    std::size_t width) noexcept;
    bool readByte(std::uint32_t address, std::uint8_t& value) const noexcept;
    bool readMiRegister(std::uint32_t address, std::uint32_t& value) const noexcept;
    bool writeMiRegister(std::uint32_t address, std::uint32_t value) noexcept;
    bool readSpRegister(std::uint32_t address, std::uint32_t& value) const noexcept;
    bool writeSpRegister(std::uint32_t address, std::uint32_t value) noexcept;
    bool performSpDma(std::uint32_t lengthRegister, bool dramToSp) noexcept;
    bool writeByte(std::uint32_t address, std::uint8_t value) noexcept;

    Rdram rdram_{};
    std::array<std::uint8_t, kSpMemorySize> spDmem_{};
    std::array<std::uint8_t, kSpMemorySize> spImem_{};
    std::uint32_t miMode_ = 0;
    std::uint32_t miInterrupt_ = 0;
    std::uint32_t miInterruptMask_ = 0;
    std::uint32_t spMemAddr_ = 0;
    std::uint32_t spDramAddr_ = 0;
    std::uint32_t spReadLength_ = 0;
    std::uint32_t spWriteLength_ = 0;
    std::uint32_t spStatus_ = kSpHalt;
    std::uint32_t spProgramCounter_ = 0;
    mutable std::uint32_t spSemaphore_ = 0;
    bool spDmaBusy_ = false;
};

} // namespace m64plus::n64
