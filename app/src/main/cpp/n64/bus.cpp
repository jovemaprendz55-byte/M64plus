#include "n64/bus.h"

#include <algorithm>

namespace m64plus::n64 {

Bus::Bus() { reset(); }

void Bus::reset() noexcept {
    rdram_.reset();
    spDmem_.fill(0);
    spImem_.fill(0);
}

BusRegion Bus::region(std::uint32_t address) const noexcept {
    if (address < Rdram::kSize) return BusRegion::Rdram;
    if (address >= kSpDmemBase && address < kSpDmemBase + kSpMemorySize)
        return BusRegion::SpDmem;
    if (address >= kSpImemBase && address < kSpImemBase + kSpMemorySize)
        return BusRegion::SpImem;
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
    case BusRegion::Unmapped:
        return false;
    }
    return false;
}

bool Bus::readBytes(std::uint32_t address, std::uint8_t* output,
                    std::size_t width) const noexcept {
    BusRegion selected = BusRegion::Unmapped;
    if (output == nullptr || !rangeFits(address, width, selected)) return false;
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

Rdram& Bus::rdram() noexcept { return rdram_; }
const Rdram& Bus::rdram() const noexcept { return rdram_; }

} // namespace m64plus::n64
