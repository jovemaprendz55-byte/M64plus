#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "n64/rdram.h"

namespace m64plus::n64 {

enum class BusRegion : std::uint8_t {
    Rdram,
    SpDmem,
    SpImem,
    MiRegisters,
    SpRegisters,
    DpRegisters,
    PiRegisters,
    Cartridge,
    Unmapped
};

class Bus final {
public:
    static constexpr std::uint32_t kRdramBase = 0x00000000U;
    static constexpr std::uint32_t kCartridgeBase = 0x10000000U;
    static constexpr std::size_t kMaxCartridgeSize = 64U * 1024U * 1024U;
    static constexpr std::uint32_t kN64RomMagic = 0x80371240U;
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
    static constexpr std::uint32_t kDpRegBase = 0x04100000U;
    static constexpr std::uint32_t kPiBase = 0x04600000U;
    static constexpr std::uint32_t kPiDramAddr = kPiBase + 0x00U;
    static constexpr std::uint32_t kPiCartAddr = kPiBase + 0x04U;
    static constexpr std::uint32_t kPiReadLength = kPiBase + 0x08U;
    static constexpr std::uint32_t kPiWriteLength = kPiBase + 0x0CU;
    static constexpr std::uint32_t kPiStatus = kPiBase + 0x10U;
    static constexpr std::uint32_t kPiDomain1Latency = kPiBase + 0x14U;
    static constexpr std::uint32_t kPiDomain1Pulse = kPiBase + 0x18U;
    static constexpr std::uint32_t kPiDomain1Page = kPiBase + 0x1CU;
    static constexpr std::uint32_t kPiDomain1Release = kPiBase + 0x20U;
    static constexpr std::uint32_t kPiDomain2Latency = kPiBase + 0x24U;
    static constexpr std::uint32_t kPiDomain2Pulse = kPiBase + 0x28U;
    static constexpr std::uint32_t kPiDomain2Page = kPiBase + 0x2CU;
    static constexpr std::uint32_t kPiDomain2Release = kPiBase + 0x30U;
    static constexpr std::uint32_t kPiStatusDmaBusy = 1U << 0U;
    static constexpr std::uint32_t kPiStatusIoBusy = 1U << 1U;
    static constexpr std::uint32_t kPiStatusError = 1U << 2U;
    static constexpr std::uint32_t kPiStatusInterrupt = 1U << 3U;
    static constexpr std::uint32_t kDpStart = kDpRegBase + 0x00U;
    static constexpr std::uint32_t kDpEnd = kDpRegBase + 0x04U;
    static constexpr std::uint32_t kDpCurrent = kDpRegBase + 0x08U;
    static constexpr std::uint32_t kDpStatus = kDpRegBase + 0x0CU;
    static constexpr std::uint32_t kDpClock = kDpRegBase + 0x10U;
    static constexpr std::uint32_t kDpBufferBusy = kDpRegBase + 0x14U;
    static constexpr std::uint32_t kDpPipeBusy = kDpRegBase + 0x18U;
    static constexpr std::uint32_t kDpTmem = kDpRegBase + 0x1CU;
    static constexpr std::uint32_t kDpXbusDmemDma = 1U << 0U;
    static constexpr std::uint32_t kDpFreeze = 1U << 1U;
    static constexpr std::uint32_t kDpFlush = 1U << 2U;
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
    // Loads a big-endian .z64 image. Existing cartridge is preserved on failure.
    bool loadCartridge(const std::uint8_t* data, std::size_t size);
    void unloadCartridge() noexcept;
    std::size_t cartridgeSize() const noexcept;
    // Changes after successful writes to RDRAM, including DMA transfers.
    std::uint64_t rdramWriteEpoch() const noexcept { return rdramWriteEpoch_; }

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
    // Inspect the next command word without advancing DPC_CURRENT.
    bool peekDpCommand(std::uint64_t& command) const noexcept;
    // Consume a complete command atomically; DPC_CURRENT advances only on success.
    bool readDpCommandWords(std::uint64_t* words, std::size_t count) noexcept;
    // Convenience wrapper for consuming one 64-bit word.
    bool readDpCommand(std::uint64_t& command) noexcept;
    std::uint32_t dpCommandsRemaining() const noexcept;
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
    bool readDpRegister(std::uint32_t address, std::uint32_t& value) const noexcept;
    bool writeDpRegister(std::uint32_t address, std::uint32_t value) noexcept;
    bool readPiRegister(std::uint32_t address, std::uint32_t& value) const noexcept;
    bool writePiRegister(std::uint32_t address, std::uint32_t value) noexcept;
    bool performPiCartridgeToRdramDma(std::uint32_t lengthRegister) noexcept;
    bool performSpDma(std::uint32_t lengthRegister, bool dramToSp) noexcept;
    bool writeByte(std::uint32_t address, std::uint8_t value) noexcept;

    Rdram rdram_{};
    std::uint64_t rdramWriteEpoch_ = 0;
    std::vector<std::uint8_t> cartridge_{};
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
    std::uint32_t dpStart_ = 0;
    std::uint32_t dpEnd_ = 0;
    std::uint32_t dpCurrent_ = 0;
    std::uint32_t dpStatus_ = 0;
    std::uint32_t dpClock_ = 0;
    std::uint32_t dpBufferBusy_ = 0;
    std::uint32_t dpPipeBusy_ = 0;
    std::uint32_t dpTmem_ = 0;
    bool dpXbusDmemDma_ = false;
    std::uint32_t piDramAddr_ = 0;
    std::uint32_t piCartAddr_ = kCartridgeBase;
    std::uint32_t piReadLength_ = 0;
    std::uint32_t piWriteLength_ = 0;
    std::uint32_t piStatus_ = 0;
    std::array<std::uint32_t, 8> piDomainTiming_{};
};

} // namespace m64plus::n64
