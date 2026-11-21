#include "n64/bus.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>

using m64plus::n64::Bus;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void testDramToSpAndSpToDram() {
    Bus bus;
    for (std::uint32_t i = 0; i < 16; ++i)
        require(bus.rdram().write8(0x100U + i, static_cast<std::uint8_t>(0xA0U + i)),
                "seed RDRAM");

    require(bus.write32(Bus::kSpMemAddr, 0x00000020U), "set SP DMEM address");
    require(bus.write32(Bus::kSpDramAddr, 0x100U), "set DRAM address");
    require(bus.write32(Bus::kSpReadLength, 7U), "trigger DRAM-to-SP DMA");
    for (std::uint32_t i = 0; i < 8; ++i) {
        std::uint8_t actual = 0;
        require(bus.read8(Bus::kSpDmemBase + 0x20U + i, actual), "read DMEM");
        require(actual == 0xA0U + i, "DRAM-to-SP copied bytes");
    }

    require(bus.write32(Bus::kSpMemAddr, 0x00000020U), "reset SP address");
    require(bus.write32(Bus::kSpDramAddr, 0x200U), "set destination DRAM address");
    require(bus.write32(Bus::kSpWriteLength, 7U), "trigger SP-to-DRAM DMA");
    for (std::uint32_t i = 0; i < 8; ++i) {
        std::uint8_t actual = 0;
        require(bus.rdram().read8(0x200U + i, actual), "read copied RDRAM");
        require(actual == 0xA0U + i, "SP-to-DRAM copied bytes");
    }
}

void testRowsAndSkip() {
    Bus bus;
    for (std::uint32_t i = 0; i < 24; ++i)
        require(bus.rdram().write8(0x300U + i, static_cast<std::uint8_t>(i + 1U)),
                "seed rows");

    require(bus.write32(Bus::kSpMemAddr, 0x40U), "set row destination");
    require(bus.write32(Bus::kSpDramAddr, 0x300U), "set row source");
    // Two 8-byte rows; skip 8 bytes in RDRAM between rows.
    require(bus.write32(Bus::kSpReadLength, 0x00801007U), "trigger row DMA");
    for (std::uint32_t i = 0; i < 8; ++i) {
        std::uint8_t first = 0, second = 0;
        require(bus.read8(Bus::kSpDmemBase + 0x40U + i, first), "read first row");
        require(bus.read8(Bus::kSpDmemBase + 0x48U + i, second), "read second row");
        require(first == i + 1U, "first row contents");
        require(second == i + 17U, "second row skips RDRAM gap");
    }
}

void testImemBankAndWrapping() {
    Bus bus;
    for (std::uint32_t i = 0; i < 8; ++i)
        require(bus.rdram().write8(0x500U + i, static_cast<std::uint8_t>(0xD0U + i)),
                "seed IMEM data");
    require(bus.write32(Bus::kSpMemAddr, 0x1FFCU), "select IMEM end");
    require(bus.write32(Bus::kSpDramAddr, 0x500U), "set IMEM DMA source");
    require(bus.write32(Bus::kSpReadLength, 7U), "DMA across DMEM/IMEM bank boundary");
    for (std::uint32_t i = 0; i < 4; ++i) {
        std::uint8_t actual = 0;
        require(bus.read8(Bus::kSpImemBase + 0xFFCU + i, actual), "read IMEM tail");
        require(actual == 0xD0U + i, "IMEM tail contents");
    }
    for (std::uint32_t i = 0; i < 4; ++i) {
        std::uint8_t actual = 0;
        require(bus.read8(Bus::kSpDmemBase + i, actual), "read wrapped DMEM");
        require(actual == 0xD4U + i, "DMA wraps to DMEM");
    }
}

void testOutOfRangeIsRejectedBeforeCopy() {
    Bus bus;
    require(bus.write32(Bus::kSpMemAddr, 0x80U), "set failure-test SP address");
    require(bus.write32(Bus::kSpDramAddr,
                        static_cast<std::uint32_t>(m64plus::n64::Rdram::kSize - 4U)),
            "set near-end DRAM address");
    require(!bus.write32(Bus::kSpReadLength, 7U), "reject DMA beyond RDRAM");
    std::uint32_t spAddress = 0;
    require(bus.read32(Bus::kSpMemAddr, spAddress), "read SP address after rejection");
    require(spAddress == 0x80U, "failed DMA does not advance SP address");
}
} // namespace

int main() {
    testDramToSpAndSpToDram();
    testRowsAndSkip();
    testImemBankAndWrapping();
    testOutOfRangeIsRejectedBeforeCopy();
    std::cout << "All SP DMA host tests passed.\n";
    return 0;
}
