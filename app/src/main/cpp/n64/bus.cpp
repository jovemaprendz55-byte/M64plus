#include "n64/bus.h"

#include <algorithm>

namespace m64plus::n64 {

Bus::Bus() { reset(); }

void Bus::reset() noexcept {
    rdram_.reset();
    spDmem_.fill(0);
    spImem_.fill(0);
    miMode_ = 0;
    miInterrupt_ = 0;
    miInterruptMask_ = 0;
    spMemAddr_ = 0;
    spDramAddr_ = 0;
    spReadLength_ = 0;
    spWriteLength_ = 0;
    spStatus_ = kSpHalt;
    spProgramCounter_ = 0;
    spSemaphore_ = 0;
    spDmaBusy_ = false;
}

BusRegion Bus::region(std::uint32_t address) const noexcept {
    if (address < Rdram::kSize) return BusRegion::Rdram;
    if (address >= kSpDmemBase && address < kSpDmemBase + kSpMemorySize)
        return BusRegion::SpDmem;
    if (address >= kSpImemBase && address < kSpImemBase + kSpMemorySize)
        return BusRegion::SpImem;
    if (address >= kMiBase && address < kMiBase + 0x10U)
        return BusRegion::MiRegisters;
    if ((address >= kSpRegBase && address < kSpRegBase + 0x20U) ||
        (address >= kSpPc && address < kSpPc + 4U))
        return BusRegion::SpRegisters;
    return BusRegion::Unmapped;
}

bool Bus::rangeFits(std::uint32_t address, std::size_t width,
                    BusRegion& selected) const noexcept {
    if (width == 0 || width > UINT32_MAX ||
        static_cast<std::uint64_t>(address) + width - 1U > UINT32_MAX)
        return false;
    selected = region(address);
    if (selected == BusRegion::Unmapped) return false;
    const auto last = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(address) + width - 1U);
    return region(last) == selected;
}

bool Bus::readByte(std::uint32_t address, std::uint8_t& value) const noexcept {
    switch (region(address)) {
    case BusRegion::Rdram:
        return rdram_.read8(address, value);
    case BusRegion::SpDmem:
        value = spDmem_[address - kSpDmemBase];
        return true;
    case BusRegion::SpImem:
        value = spImem_[address - kSpImemBase];
        return true;
    case BusRegion::MiRegisters:
    case BusRegion::SpRegisters:
        return false; // Device registers require aligned 32-bit accesses.
    case BusRegion::Unmapped:
        return false;
    }
    return false;
}

bool Bus::writeByte(std::uint32_t address, std::uint8_t value) noexcept {
    switch (region(address)) {
    case BusRegion::Rdram:
        return rdram_.write8(address, value);
    case BusRegion::SpDmem:
        spDmem_[address - kSpDmemBase] = value;
        return true;
    case BusRegion::SpImem:
        spImem_[address - kSpImemBase] = value;
        return true;
    case BusRegion::MiRegisters:
    case BusRegion::SpRegisters:
        return false; // Device registers require aligned 32-bit accesses.
    case BusRegion::Unmapped:
        return false;
    }
    return false;
}

bool Bus::readBytes(std::uint32_t address, std::uint8_t* output,
                    std::size_t width) const noexcept {
    BusRegion selected = BusRegion::Unmapped;
    if (output == nullptr || !rangeFits(address, width, selected)) return false;
    if (selected == BusRegion::MiRegisters || selected == BusRegion::SpRegisters) {
        if (width != 4U || (address & 3U) != 0) return false;
        std::uint32_t value = 0;
        const bool ok = selected == BusRegion::MiRegisters
            ? readMiRegister(address, value) : readSpRegister(address, value);
        if (!ok) return false;
        output[0] = static_cast<std::uint8_t>(value >> 24U);
        output[1] = static_cast<std::uint8_t>(value >> 16U);
        output[2] = static_cast<std::uint8_t>(value >> 8U);
        output[3] = static_cast<std::uint8_t>(value);
        return true;
    }
    for (std::size_t i = 0; i < width; ++i) {
        if (!readByte(address + static_cast<std::uint32_t>(i), output[i]))
            return false;
    }
    return true;
}

bool Bus::writeBytes(std::uint32_t address, const std::uint8_t* input,
                     std::size_t width) noexcept {
    BusRegion selected = BusRegion::Unmapped;
    if (input == nullptr || !rangeFits(address, width, selected)) return false;
    if (selected == BusRegion::MiRegisters || selected == BusRegion::SpRegisters) {
        if (width != 4U || (address & 3U) != 0) return false;
        const std::uint32_t value = (static_cast<std::uint32_t>(input[0]) << 24U) |
            (static_cast<std::uint32_t>(input[1]) << 16U) |
            (static_cast<std::uint32_t>(input[2]) << 8U) | input[3];
        return selected == BusRegion::MiRegisters
            ? writeMiRegister(address, value) : writeSpRegister(address, value);
    }
    for (std::size_t i = 0; i < width; ++i) {
        if (!writeByte(address + static_cast<std::uint32_t>(i), input[i]))
            return false;
    }
    return true;
}

bool Bus::read8(std::uint32_t address, std::uint8_t& value) const noexcept {
    return readBytes(address, &value, sizeof(value));
}

bool Bus::read16(std::uint32_t address, std::uint16_t& value) const noexcept {
    std::uint8_t bytes[2]{};
    if (!readBytes(address, bytes, sizeof(bytes))) return false;
    value = (static_cast<std::uint16_t>(bytes[0]) << 8U) |
            static_cast<std::uint16_t>(bytes[1]);
    return true;
}

bool Bus::read32(std::uint32_t address, std::uint32_t& value) const noexcept {
    std::uint8_t bytes[4]{};
    if (!readBytes(address, bytes, sizeof(bytes))) return false;
    value = (static_cast<std::uint32_t>(bytes[0]) << 24U) |
            (static_cast<std::uint32_t>(bytes[1]) << 16U) |
            (static_cast<std::uint32_t>(bytes[2]) << 8U) |
            static_cast<std::uint32_t>(bytes[3]);
    return true;
}

bool Bus::read64(std::uint32_t address, std::uint64_t& value) const noexcept {
    std::uint8_t bytes[8]{};
    if (!readBytes(address, bytes, sizeof(bytes))) return false;
    value = 0;
    for (const auto byte : bytes) value = (value << 8U) | byte;
    return true;
}

bool Bus::write8(std::uint32_t address, std::uint8_t value) noexcept {
    return writeBytes(address, &value, sizeof(value));
}

bool Bus::write16(std::uint32_t address, std::uint16_t value) noexcept {
    const std::uint8_t bytes[] = {
        static_cast<std::uint8_t>(value >> 8U),
        static_cast<std::uint8_t>(value)
    };
    return writeBytes(address, bytes, sizeof(bytes));
}

bool Bus::write32(std::uint32_t address, std::uint32_t value) noexcept {
    const std::uint8_t bytes[] = {
        static_cast<std::uint8_t>(value >> 24U),
        static_cast<std::uint8_t>(value >> 16U),
        static_cast<std::uint8_t>(value >> 8U),
        static_cast<std::uint8_t>(value)
    };
    return writeBytes(address, bytes, sizeof(bytes));
}

bool Bus::write64(std::uint32_t address, std::uint64_t value) noexcept {
    std::uint8_t bytes[8]{};
    for (std::size_t i = 0; i < sizeof(bytes); ++i)
        bytes[i] = static_cast<std::uint8_t>(value >> ((7U - i) * 8U));
    return writeBytes(address, bytes, sizeof(bytes));
}


bool Bus::readMiRegister(std::uint32_t address, std::uint32_t& value) const noexcept {
    switch (address) {
    case kMiMode: value = miMode_; return true;
    case kMiVersion: value = kMiVersionValue; return true;
    case kMiInterrupt: value = miInterrupt_; return true;
    case kMiInterruptMask: value = miInterruptMask_; return true;
    default: return false;
    }
}

bool Bus::writeMiRegister(std::uint32_t address, std::uint32_t value) noexcept {
    switch (address) {
    case kMiMode:
        miMode_ = value & 0x7FU;
        return true;
    case kMiVersion:
    case kMiInterrupt:
        return true; // Hardware-visible read-only registers.
    case kMiInterruptMask:
        for (unsigned source = 0; source < 6U; ++source) {
            const std::uint32_t clearBit = 1U << (source * 2U);
            const std::uint32_t setBit = 1U << (source * 2U + 1U);
            if ((value & clearBit) != 0) miInterruptMask_ &= ~(1U << source);
            if ((value & setBit) != 0) miInterruptMask_ |= 1U << source;
        }
        return true;
    default:
        return false;
    }
}


bool Bus::readSpRegister(std::uint32_t address, std::uint32_t& value) const noexcept {
    switch (address) {
    case kSpPc: value = spProgramCounter_; return true;
    case kSpMemAddr: value = spMemAddr_; return true;
    case kSpDramAddr: value = spDramAddr_; return true;
    case kSpReadLength: value = spReadLength_; return true;
    case kSpWriteLength: value = spWriteLength_; return true;
    case kSpStatus: value = spStatus_; return true;
    case kSpDmaFull: value = 0; return true;
    case kSpDmaBusy: value = spDmaBusy_ ? 1U : 0U; return true;
    case kSpSemaphore:
        value = spSemaphore_;
        spSemaphore_ = 1U;
        return true;
    default: return false;
    }
}

bool Bus::writeSpRegister(std::uint32_t address, std::uint32_t value) noexcept {
    switch (address) {
    case kSpPc: spProgramCounter_ = value & 0x0FFCU; return true;
    case kSpMemAddr: spMemAddr_ = value & 0x1FFFU; return true;
    case kSpDramAddr: spDramAddr_ = value & 0x00FFFFFFU; return true;
    case kSpReadLength:
        spReadLength_ = value & 0x00FFFFFFU;
        return performSpDma(spReadLength_, true);
    case kSpWriteLength:
        spWriteLength_ = value & 0x00FFFFFFU;
        return performSpDma(spWriteLength_, false);
    case kSpStatus:
        if ((value & (1U << 0U)) != 0) spStatus_ &= ~kSpHalt;
        if ((value & (1U << 1U)) != 0) spStatus_ |= kSpHalt;
        if ((value & (1U << 2U)) != 0) spStatus_ &= ~kSpBroke;
        if ((value & (1U << 3U)) != 0) setMiInterrupt(0, false);
        if ((value & (1U << 4U)) != 0) setMiInterrupt(0, true);
        if ((value & (1U << 5U)) != 0) spStatus_ &= ~kSpInterruptOnBreak;
        if ((value & (1U << 6U)) != 0) spStatus_ |= kSpInterruptOnBreak;
        return true;
    case kSpDmaFull: return true;
    case kSpDmaBusy: return false;
    case kSpSemaphore: spSemaphore_ = 0; return true;
    default: return false;
    }
}


bool Bus::performSpDma(std::uint32_t lengthRegister, bool dramToSp) noexcept {
    // SP DMA length is encoded as (length - 1), (count - 1), and row skip.
    // Transfers are rounded up to an 8-byte boundary.
    const std::uint32_t rowLength = ((lengthRegister & 0x0FFFU) + 8U) & ~7U;
    const std::uint32_t rowCount = ((lengthRegister >> 12U) & 0xFFU) + 1U;
    const std::uint32_t rowSkip = (lengthRegister >> 20U) & 0x0FFFU;
    if (rowLength == 0U || spDmaBusy_) return false;

    const std::uint64_t rowStride =
        static_cast<std::uint64_t>(rowLength) + rowSkip;
    for (std::uint32_t row = 0; row < rowCount; ++row) {
        const std::uint64_t dramStart =
            static_cast<std::uint64_t>(spDramAddr_) + row * rowStride;
        if (dramStart + rowLength > Rdram::kSize) return false;
    }

    spDmaBusy_ = true;
    std::uint32_t spAddress = spMemAddr_ & 0x1FFFU;
    for (std::uint32_t row = 0; row < rowCount; ++row) {
        const std::uint32_t dramStart = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(spDramAddr_) + row * rowStride);
        for (std::uint32_t byte = 0; byte < rowLength; ++byte) {
            const std::uint32_t spOffset = spAddress & 0x0FFFU;
            auto& spMemory = (spAddress & 0x1000U) != 0U ? spImem_ : spDmem_;
            if (dramToSp) {
                std::uint8_t value = 0;
                if (!rdram_.read8(dramStart + byte, value)) {
                    spDmaBusy_ = false;
                    return false;
                }
                spMemory[spOffset] = value;
            } else {
                if (!rdram_.write8(dramStart + byte, spMemory[spOffset])) {
                    spDmaBusy_ = false;
                    return false;
                }
            }
            spAddress = (spAddress + 1U) & 0x1FFFU;
        }
    }
    spMemAddr_ = spAddress;
    spDramAddr_ = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(spDramAddr_) + rowCount * rowStride) & 0x00FFFFFFU;
    spDmaBusy_ = false;
    return true;
}

void Bus::setMiInterrupt(unsigned source, bool asserted) noexcept {
    if (source >= 6U) return;
    const std::uint32_t bit = 1U << source;
    if (asserted) miInterrupt_ |= bit;
    else miInterrupt_ &= ~bit;
}

std::uint32_t Bus::miInterruptMask() const noexcept { return miInterruptMask_; }

bool Bus::miInterruptPending() const noexcept {
    return (miInterrupt_ & miInterruptMask_) != 0;
}

Rdram& Bus::rdram() noexcept { return rdram_; }
const Rdram& Bus::rdram() const noexcept { return rdram_; }

} // namespace m64plus::n64
