#include "n64/bus.h"

#include <algorithm>
#include <new>

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
    dpStart_ = 0;
    dpEnd_ = 0;
    dpCurrent_ = 0;
    dpStatus_ = 0;
    dpClock_ = 0;
    dpBufferBusy_ = 0;
    dpPipeBusy_ = 0;
    dpTmem_ = 0;
    dpXbusDmemDma_ = false;
    piDramAddr_ = 0;
    piCartAddr_ = kCartridgeBase;
    piReadLength_ = 0;
    piWriteLength_ = 0;
    piStatus_ = 0;
    piDomainTiming_.fill(0);
}

bool Bus::loadCartridge(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size < 0x40U || size > kMaxCartridgeSize ||
        (size & 3U) != 0U) return false;
    const bool byteSwapped = data[0] == 0x37U && data[1] == 0x80U &&
        data[2] == 0x40U && data[3] == 0x12U;
    const bool wordSwapped = data[0] == 0x40U && data[1] == 0x12U &&
        data[2] == 0x37U && data[3] == 0x80U;
    const std::uint32_t magic = (static_cast<std::uint32_t>(data[0]) << 24U) |
        (static_cast<std::uint32_t>(data[1]) << 16U) |
        (static_cast<std::uint32_t>(data[2]) << 8U) | data[3];
    if (magic != kN64RomMagic && !byteSwapped && !wordSwapped) return false;
    try {
        std::vector<std::uint8_t> replacement(data, data + size);
        if (byteSwapped) {
            for (std::size_t i = 0; i < size; i += 2U)
                std::swap(replacement[i], replacement[i + 1U]);
        } else if (wordSwapped) {
            for (std::size_t i = 0; i < size; i += 4U)
                std::reverse(replacement.begin() + static_cast<std::ptrdiff_t>(i),
                             replacement.begin() + static_cast<std::ptrdiff_t>(i + 4U));
        }
        cartridge_.swap(replacement);
    } catch (const std::bad_alloc&) {
        return false;
    }
    return true;
}

void Bus::unloadCartridge() noexcept { cartridge_.clear(); }
std::size_t Bus::cartridgeSize() const noexcept { return cartridge_.size(); }

BusRegion Bus::region(std::uint32_t address) const noexcept {
    if (address >= kCartridgeBase &&
        static_cast<std::uint64_t>(address - kCartridgeBase) < cartridge_.size())
        return BusRegion::Cartridge;
    if (address < Rdram::kSize) return BusRegion::Rdram;
    if (address >= kSpDmemBase && address < kSpDmemBase + kSpMemorySize)
        return BusRegion::SpDmem;
    if (address >= kSpImemBase && address < kSpImemBase + kSpMemorySize)
        return BusRegion::SpImem;
    if (address >= kMiBase && address < kMiBase + 0x10U)
        return BusRegion::MiRegisters;
    if (address >= kDpRegBase && address < kDpRegBase + 0x20U)
        return BusRegion::DpRegisters;
    if (address >= kPiBase && address < kPiBase + 0x34U)
        return BusRegion::PiRegisters;
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
    case BusRegion::Cartridge:
        value = cartridge_[address - kCartridgeBase];
        return true;
    case BusRegion::SpDmem:
        value = spDmem_[address - kSpDmemBase];
        return true;
    case BusRegion::SpImem:
        value = spImem_[address - kSpImemBase];
        return true;
    case BusRegion::MiRegisters:
    case BusRegion::SpRegisters:
    case BusRegion::DpRegisters:
    case BusRegion::PiRegisters:
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
    case BusRegion::Cartridge:
        return false; // Cartridge ROM is read-only.
    case BusRegion::SpDmem:
        spDmem_[address - kSpDmemBase] = value;
        return true;
    case BusRegion::SpImem:
        spImem_[address - kSpImemBase] = value;
        return true;
    case BusRegion::MiRegisters:
    case BusRegion::SpRegisters:
    case BusRegion::DpRegisters:
    case BusRegion::PiRegisters:
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
    if (selected == BusRegion::MiRegisters || selected == BusRegion::SpRegisters || selected == BusRegion::DpRegisters || selected == BusRegion::PiRegisters) {
        if (width != 4U || (address & 3U) != 0) return false;
        std::uint32_t value = 0;
        const bool ok = selected == BusRegion::MiRegisters
            ? readMiRegister(address, value)
            : selected == BusRegion::SpRegisters ? readSpRegister(address, value)
            : selected == BusRegion::DpRegisters ? readDpRegister(address, value)
            : readPiRegister(address, value);
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
    if (selected == BusRegion::MiRegisters || selected == BusRegion::SpRegisters || selected == BusRegion::DpRegisters || selected == BusRegion::PiRegisters) {
        if (width != 4U || (address & 3U) != 0) return false;
        const std::uint32_t value = (static_cast<std::uint32_t>(input[0]) << 24U) |
            (static_cast<std::uint32_t>(input[1]) << 16U) |
            (static_cast<std::uint32_t>(input[2]) << 8U) | input[3];
        return selected == BusRegion::MiRegisters
            ? writeMiRegister(address, value)
            : selected == BusRegion::SpRegisters ? writeSpRegister(address, value)
            : selected == BusRegion::DpRegisters ? writeDpRegister(address, value)
            : writePiRegister(address, value);
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
        if ((value & kSpClearSingleStep) != 0) spStatus_ &= ~kSpSingleStep;
        if ((value & kSpSetSingleStep) != 0) spStatus_ |= kSpSingleStep;
        if ((value & kSpClearInterruptOnBreak) != 0)
            spStatus_ &= ~kSpInterruptOnBreak;
        if ((value & kSpSetInterruptOnBreak) != 0)
            spStatus_ |= kSpInterruptOnBreak;
        for (unsigned signal = 0; signal < 8U; ++signal) {
            const std::uint32_t statusBit = kSpSignal0 << signal;
            const std::uint32_t clearCommand = 1U << (9U + signal * 2U);
            const std::uint32_t setCommand = 1U << (10U + signal * 2U);
            if ((value & clearCommand) != 0) spStatus_ &= ~statusBit;
            if ((value & setCommand) != 0) spStatus_ |= statusBit;
        }
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



std::uint32_t Bus::dpCommandsRemaining() const noexcept {
    if (dpCurrent_ >= dpEnd_) return 0U;
    return (dpEnd_ - dpCurrent_) / 8U;
}

bool Bus::peekDpCommand(std::uint64_t& command) const noexcept {
    if ((dpStatus_ & kDpFreeze) != 0U || dpCurrent_ > dpEnd_ ||
        dpEnd_ - dpCurrent_ < 8U || (dpCurrent_ & 7U) != 0U)
        return false;

    if (dpXbusDmemDma_) {
        const std::uint32_t dmemAddress =
            kSpDmemBase + (dpCurrent_ & 0x0FF8U);
        return read64(dmemAddress, command);
    }
    return rdram_.read64(dpCurrent_, command);
}

bool Bus::readDpCommandWords(std::uint64_t* words, std::size_t count) noexcept {
    if (words == nullptr || count == 0U ||
        (dpStatus_ & kDpFreeze) != 0U || dpCurrent_ > dpEnd_ ||
        (dpCurrent_ & 7U) != 0U ||
        count > static_cast<std::size_t>((dpEnd_ - dpCurrent_) / 8U) ||
        count > static_cast<std::size_t>(UINT32_MAX / 8U))
        return false;

    const std::uint64_t byteCount = static_cast<std::uint64_t>(count) * 8U;
    if (!dpXbusDmemDma_ &&
        static_cast<std::uint64_t>(dpCurrent_) + byteCount > Rdram::kSize)
        return false;

    // Validate the complete range before writing output or moving the cursor.
    // XBUS accesses wrap inside the 4 KiB SP DMEM window.
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t address = dpXbusDmemDma_
            ? kSpDmemBase + ((dpCurrent_ + static_cast<std::uint32_t>(i * 8U)) & 0x0FF8U)
            : dpCurrent_ + static_cast<std::uint32_t>(i * 8U);
        std::uint64_t value = 0;
        if (!read64(address, value)) return false;
        words[i] = value;
    }

    dpCurrent_ += static_cast<std::uint32_t>(byteCount);
    return true;
}

bool Bus::readDpCommand(std::uint64_t& command) noexcept {
    return readDpCommandWords(&command, 1U);
}

bool Bus::readDpRegister(std::uint32_t address, std::uint32_t& value) const noexcept {
    switch (address) {
    case kDpStart: value = dpStart_; return true;
    case kDpEnd: value = dpEnd_; return true;
    case kDpCurrent: value = dpCurrent_; return true;
    case kDpStatus: value = dpStatus_; return true;
    case kDpClock: value = dpClock_; return true;
    case kDpBufferBusy: value = dpBufferBusy_; return true;
    case kDpPipeBusy: value = dpPipeBusy_; return true;
    case kDpTmem: value = dpTmem_; return true;
    default: return false;
    }
}

bool Bus::writeDpRegister(std::uint32_t address, std::uint32_t value) noexcept {
    switch (address) {
    case kDpStart:
        dpStart_ = value & 0x00FFFFF8U;
        dpCurrent_ = dpStart_;
        return true;
    case kDpEnd:
        // Publish the exclusive end address; the RDP pulls commands through readDpCommand().
        dpEnd_ = value & 0x00FFFFF8U;
        return true;
    case kDpCurrent:
    case kDpClock:
    case kDpBufferBusy:
    case kDpPipeBusy:
    case kDpTmem:
        return true; // Read-only hardware registers ignore writes.
    case kDpStatus:
        if ((value & (1U << 0U)) != 0U) dpXbusDmemDma_ = false;
        if ((value & (1U << 1U)) != 0U) dpXbusDmemDma_ = true;
        dpStatus_ = (dpStatus_ & ~kDpXbusDmemDma) |
            (dpXbusDmemDma_ ? kDpXbusDmemDma : 0U);
        if ((value & (1U << 2U)) != 0U) dpStatus_ &= ~kDpFreeze;
        if ((value & (1U << 3U)) != 0U) dpStatus_ |= kDpFreeze;
        if ((value & (1U << 4U)) != 0U) dpStatus_ &= ~kDpFlush;
        if ((value & (1U << 5U)) != 0U) dpStatus_ |= kDpFlush;
        if ((value & (1U << 6U)) != 0U) dpTmem_ = 0;
        if ((value & (1U << 7U)) != 0U) dpPipeBusy_ = 0;
        if ((value & (1U << 8U)) != 0U) dpBufferBusy_ = 0;
        if ((value & (1U << 9U)) != 0U) dpClock_ = 0;
        return true;
    default:
        return false;
    }
}

bool Bus::readPiRegister(std::uint32_t address, std::uint32_t& value) const noexcept {
    switch (address) {
    case kPiDramAddr: value = piDramAddr_; return true;
    case kPiCartAddr: value = piCartAddr_; return true;
    case kPiReadLength: value = piReadLength_; return true;
    case kPiWriteLength: value = piWriteLength_; return true;
    case kPiStatus: value = piStatus_; return true;
    case kPiDomain1Latency: value = piDomainTiming_[0]; return true;
    case kPiDomain1Pulse: value = piDomainTiming_[1]; return true;
    case kPiDomain1Page: value = piDomainTiming_[2]; return true;
    case kPiDomain1Release: value = piDomainTiming_[3]; return true;
    case kPiDomain2Latency: value = piDomainTiming_[4]; return true;
    case kPiDomain2Pulse: value = piDomainTiming_[5]; return true;
    case kPiDomain2Page: value = piDomainTiming_[6]; return true;
    case kPiDomain2Release: value = piDomainTiming_[7]; return true;
    default: return false;
    }
}

bool Bus::writePiRegister(std::uint32_t address, std::uint32_t value) noexcept {
    switch (address) {
    case kPiDramAddr:
        piDramAddr_ = value & 0x00FFFFFFU;
        return true;
    case kPiCartAddr:
        piCartAddr_ = value;
        return true;
    case kPiReadLength:
        // PI_RD_LEN transfers from RDRAM toward the cartridge bus. This
        // implementation models read-only ROM and deliberately rejects writes.
        piReadLength_ = value & 0x00FFFFFFU;
        piStatus_ |= kPiStatusError;
        return false;
    case kPiWriteLength:
        piWriteLength_ = value & 0x00FFFFFFU;
        if (!performPiCartridgeToRdramDma(piWriteLength_)) {
            piStatus_ |= kPiStatusError;
            return false;
        }
        return true;
    case kPiStatus:
        if ((value & 1U) != 0U) {
            piStatus_ &= ~(kPiStatusDmaBusy | kPiStatusIoBusy | kPiStatusError);
        }
        if ((value & 2U) != 0U) {
            piStatus_ &= ~kPiStatusInterrupt;
            setMiInterrupt(4U, false);
        }
        return true;
    case kPiDomain1Latency: piDomainTiming_[0] = value & 0xFFU; return true;
    case kPiDomain1Pulse: piDomainTiming_[1] = value & 0xFFU; return true;
    case kPiDomain1Page: piDomainTiming_[2] = value & 0x0FU; return true;
    case kPiDomain1Release: piDomainTiming_[3] = value & 0x03U; return true;
    case kPiDomain2Latency: piDomainTiming_[4] = value & 0xFFU; return true;
    case kPiDomain2Pulse: piDomainTiming_[5] = value & 0xFFU; return true;
    case kPiDomain2Page: piDomainTiming_[6] = value & 0x0FU; return true;
    case kPiDomain2Release: piDomainTiming_[7] = value & 0x03U; return true;
    default: return false;
    }
}

bool Bus::performPiCartridgeToRdramDma(std::uint32_t lengthRegister) noexcept {
    const std::uint64_t length = static_cast<std::uint64_t>(lengthRegister) + 1U;
    // PI DMA uses an 8-byte-aligned RDRAM address, a 2-byte-aligned
    // cartridge address, and an even transfer length.
    if ((piDramAddr_ & 7U) != 0U || (piCartAddr_ & 1U) != 0U ||
        (length & 1U) != 0U || piCartAddr_ < kCartridgeBase ||
        piDramAddr_ >= Rdram::kSize ||
        static_cast<std::uint64_t>(piDramAddr_) + length > Rdram::kSize)
        return false;
    const std::uint64_t cartridgeOffset =
        static_cast<std::uint64_t>(piCartAddr_) - kCartridgeBase;
    if (cartridgeOffset + length > cartridge_.size()) return false;

    piStatus_ |= kPiStatusDmaBusy | kPiStatusIoBusy;
    for (std::uint64_t i = 0; i < length; ++i) {
        if (!rdram_.write8(piDramAddr_ + static_cast<std::uint32_t>(i),
                           cartridge_[static_cast<std::size_t>(cartridgeOffset + i)])) {
            piStatus_ &= ~(kPiStatusDmaBusy | kPiStatusIoBusy);
            return false;
        }
    }
    piDramAddr_ = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(piDramAddr_) + length) & 0x00FFFFFFU;
    piCartAddr_ = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(piCartAddr_) + length);
    piStatus_ &= ~(kPiStatusDmaBusy | kPiStatusIoBusy | kPiStatusError);
    piStatus_ |= kPiStatusInterrupt;
    setMiInterrupt(4U, true);
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
