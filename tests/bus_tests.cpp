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


void testMiVersionRegisterReportsRcpRevision() {
    Bus bus;
    std::uint32_t value = 0U;
    require(bus.read32(Bus::kMiVersion, value),
            "MI_VERSION register is readable");
    require(value == 0x02020102U,
            "MI_VERSION reports the N64 RCP revision");
    require(bus.write32(Bus::kMiVersion, 0xFFFFFFFFU),
            "writes to read-only MI_VERSION are ignored");
    require(bus.read32(Bus::kMiVersion, value) && value == 0x02020102U,
            "MI_VERSION remains unchanged after a write");
}

void testMiInterruptRegisterReflectsSourcesAndIsReadOnly() {
    Bus bus;
    std::uint32_t pending = 0U;
    for (unsigned source = 0U; source < 6U; ++source) {
        const std::uint32_t sourceBit = 1U << source;
        bus.setMiInterrupt(source, true);
        require(bus.read32(Bus::kMiInterrupt, pending),
                "MI interrupt register is readable");
        require((pending & sourceBit) != 0U,
                "MI interrupt register exposes asserted source");
        bus.setMiInterrupt(source, false);
        require(bus.read32(Bus::kMiInterrupt, pending) &&
                (pending & sourceBit) == 0U,
                "MI interrupt register clears deasserted source");
    }
    require(bus.write32(Bus::kMiInterrupt, 0xFFFFFFFFU),
            "writes to read-only MI interrupt register are ignored");
    require(bus.read32(Bus::kMiInterrupt, pending) && pending == 0U,
            "MI interrupt register remains clear after ignored write");
}

void testMiInterruptSourcesRemainIndependent() {
    Bus bus;
    constexpr std::uint32_t source0 = 1U << 0U;
    constexpr std::uint32_t source2 = 1U << 2U;
    constexpr std::uint32_t setMask0 = 1U << 1U;
    constexpr std::uint32_t setMask2 = 1U << 5U;
    constexpr std::uint32_t clearMask0 = 1U << 0U;

    bus.setMiInterrupt(0U, true);
    bus.setMiInterrupt(2U, true);
    std::uint32_t pending = 0U;
    require(bus.read32(Bus::kMiInterrupt, pending) &&
            pending == (source0 | source2),
            "multiple MI interrupt sources can remain asserted together");

    require(bus.write32(Bus::kMiInterruptMask, setMask0),
            "enable only first asserted MI source");
    require(bus.miInterruptPending(),
            "one enabled source makes combined MI interrupt pending");
    require(bus.write32(Bus::kMiInterruptMask, clearMask0),
            "disable first MI source without acknowledging it");
    require(!bus.miInterruptPending(),
            "masked second source does not trigger combined pending state");
    require(bus.read32(Bus::kMiInterrupt, pending) &&
            pending == (source0 | source2),
            "mask changes do not clear asserted interrupt sources");

    require(bus.write32(Bus::kMiInterruptMask, setMask2),
            "enable second asserted MI source");
    require(bus.miInterruptPending(),
            "second source independently triggers pending state");
    bus.setMiInterrupt(0U, false);
    require(bus.miInterruptPending(),
            "deasserting first source does not clear enabled second source");
    bus.setMiInterrupt(2U, false);
    require(!bus.miInterruptPending(),
            "combined pending state clears after all enabled sources deassert");
}

void testMiInterruptMaskSetAndClearCommands() {
    Bus bus;
    std::uint32_t mask = 0U;
    for (unsigned source = 0U; source < 6U; ++source) {
        const std::uint32_t sourceBit = 1U << source;
        const std::uint32_t setCommand = 1U << (source * 2U + 1U);
        const std::uint32_t clearCommand = 1U << (source * 2U);
        bus.setMiInterrupt(source, true);
        require(bus.write32(Bus::kMiInterruptMask, setCommand),
                "MI interrupt mask accepts per-source set command");
        require(bus.read32(Bus::kMiInterruptMask, mask) &&
                (mask & sourceBit) != 0U,
                "MI interrupt mask enables selected source");
        require(bus.miInterruptPending(),
                "enabled asserted MI source becomes pending");
        require(bus.write32(Bus::kMiInterruptMask, clearCommand),
                "MI interrupt mask accepts per-source clear command");
        require(bus.read32(Bus::kMiInterruptMask, mask) &&
                (mask & sourceBit) == 0U,
                "MI interrupt mask disables selected source");
        require(!bus.miInterruptPending(),
                "disabled MI source is not reported pending");
        bus.setMiInterrupt(source, false);
    }
}


void testMiResetClearsInterruptSourcesAndMasks() {
    Bus bus;
    constexpr std::uint32_t spSource = 1U << 0U;
    constexpr std::uint32_t piSource = 1U << 4U;
    constexpr std::uint32_t enableSp = 1U << 1U;
    constexpr std::uint32_t enablePi = 1U << 9U;

    bus.setMiInterrupt(0U, true);
    bus.setMiInterrupt(4U, true);
    require(bus.write32(Bus::kMiInterruptMask, enableSp | enablePi),
            "enable SP and PI interrupts before reset");
    require(bus.miInterruptPending(),
            "enabled SP and PI sources assert combined interrupt before reset");

    bus.reset();

    std::uint32_t value = 0U;
    require(bus.read32(Bus::kMiInterrupt, value) && value == 0U,
            "MI reset clears all asserted interrupt sources");
    require(bus.read32(Bus::kMiInterruptMask, value) && value == 0U,
            "MI reset disables all interrupt masks");
    require(!bus.miInterruptPending(),
            "MI reset deasserts combined interrupt output");

    bus.setMiInterrupt(0U, true);
    require(!bus.miInterruptPending(),
            "an asserted source remains masked after reset");
    require(bus.read32(Bus::kMiInterrupt, value) && value == spSource,
            "new SP source can assert after reset");
    require((value & piSource) == 0U,
            "stale PI source does not survive reset");
}

void testDpRegisterWindow() {
    Bus bus;
    std::uint32_t value = 0;
    require(bus.region(Bus::kDpStart) == m64plus::n64::BusRegion::DpRegisters,
            "DPC registers have a dedicated bus region");
    require(bus.write32(Bus::kDpStart, 0x00123457U), "write DPC_START");
    require(bus.read32(Bus::kDpStart, value) && value == 0x00123450U,
            "DPC_START is eight-byte aligned");
    require(bus.read32(Bus::kDpCurrent, value) && value == 0x00123450U,
            "DPC_START initializes DPC_CURRENT");
    require(bus.write32(Bus::kDpEnd, 0x00123807U), "write DPC_END");
    require(bus.read32(Bus::kDpEnd, value) && value == 0x00123800U,
            "DPC_END stores aligned command-list end");
    require(bus.read32(Bus::kDpCurrent, value) && value == 0x00123450U,
            "writing DPC_END does not pretend RDP consumed commands");
    require(bus.write32(Bus::kDpStatus, 1U << 1U), "set DPC XBUS mode");
    require(bus.read32(Bus::kDpStatus, value) &&
            (value & Bus::kDpXbusDmemDma) != 0U, "DPC XBUS status set");
    require(bus.write32(Bus::kDpStatus, 1U << 0U), "clear DPC XBUS mode");
    require(bus.read32(Bus::kDpStatus, value) &&
            (value & Bus::kDpXbusDmemDma) == 0U, "DPC XBUS status cleared");
    require(bus.write32(Bus::kDpStatus, 1U << 3U), "freeze DPC");
    require(bus.read32(Bus::kDpStatus, value) &&
            (value & Bus::kDpFreeze) != 0U, "DPC freeze status set");
    require(bus.write32(Bus::kDpStatus, 1U << 2U), "unfreeze DPC");
    require(bus.read32(Bus::kDpStatus, value) &&
            (value & Bus::kDpFreeze) == 0U, "DPC freeze status cleared");
    require(bus.write32(Bus::kDpClock, 99U), "ignore write to read-only DPC clock");
    require(bus.read32(Bus::kDpClock, value) && value == 0U,
            "DPC clock remains read-only and zero without RDP");
    require(!bus.read32(Bus::kDpRegBase + 0x20U, value),
            "address after DPC register window remains unmapped");
}


void testDpCommandStreamFromRdramAndDmem() {
    Bus bus;
    std::uint64_t command = 0;
    require(bus.write64(0x200U, 0x123456789ABCDEF0ULL), "seed first RDP command in RDRAM");
    require(bus.write64(0x208U, 0xFEDCBA9876543210ULL), "seed second RDP command in RDRAM");
    require(bus.write32(Bus::kDpStart, 0x200U), "set DPC_START for RDRAM list");
    require(bus.write32(Bus::kDpEnd, 0x210U), "set DPC_END for RDRAM list");
    require(bus.dpCommandsRemaining() == 2U, "DPC reports two pending 64-bit commands");
    require(bus.readDpCommand(command) && command == 0x123456789ABCDEF0ULL,
            "DPC fetches first big-endian command from RDRAM");
    require(bus.dpCommandsRemaining() == 1U, "DPC advances by eight bytes");
    require(bus.readDpCommand(command) && command == 0xFEDCBA9876543210ULL,
            "DPC fetches second command from RDRAM");
    require(bus.dpCommandsRemaining() == 0U, "DPC reaches exclusive end address");
    require(!bus.readDpCommand(command), "DPC does not read beyond DPC_END");

    require(bus.write64(Bus::kSpDmemBase + 0x20U, 0x8877665544332211ULL),
            "seed command in SP DMEM");
    require(bus.write32(Bus::kDpStart, 0x20U), "set DPC_START for XBUS list");
    require(bus.write32(Bus::kDpEnd, 0x28U), "set DPC_END for XBUS list");
    require(bus.write32(Bus::kDpStatus, 1U << 1U), "enable XBUS DMEM DMA");
    require(bus.readDpCommand(command) && command == 0x8877665544332211ULL,
            "DPC fetches command from SP DMEM in XBUS mode");
    require(bus.dpCommandsRemaining() == 0U, "XBUS command advances DPC_CURRENT");

    require(bus.write32(Bus::kDpStart, 0x20U), "reset XBUS command cursor");
    require(bus.write32(Bus::kDpEnd, 0x28U), "republish XBUS command end");
    require(bus.write32(Bus::kDpStatus, 1U << 3U), "freeze DPC command stream");
    require(!bus.readDpCommand(command), "frozen DPC does not fetch commands");
    require(bus.dpCommandsRemaining() == 1U, "freeze preserves command cursor");
    require(bus.write32(Bus::kDpStatus, 1U << 2U), "unfreeze DPC command stream");
    require(bus.readDpCommand(command), "unfrozen DPC resumes command fetch");
}

void testDpMultiwordCommandIsAtomic() {
    Bus bus;
    constexpr std::uint64_t first = 0x2400000012345678ULL;
    constexpr std::uint64_t second = 0x9ABCDEF001020304ULL;
    require(bus.write64(0x300U, first), "seed first texture-rectangle word");
    require(bus.write64(0x308U, second), "seed second texture-rectangle word");
    require(bus.write32(Bus::kDpStart, 0x300U), "set DPC_START for partial command");
    require(bus.write32(Bus::kDpEnd, 0x308U), "publish only one word of command");

    std::uint64_t header = 0;
    require(bus.peekDpCommand(header) && header == first,
            "peek reads command header without consuming it");
    std::uint64_t words[2] = {0xAAAAAAAAAAAAAAAAULL, 0xBBBBBBBBBBBBBBBBULL};
    require(!bus.readDpCommandWords(words, 2U),
            "incomplete multiword command is not consumed");
    std::uint32_t current = 0;
    require(bus.read32(Bus::kDpCurrent, current) && current == 0x300U,
            "failed atomic read preserves DPC_CURRENT");
    require(words[0] == 0xAAAAAAAAAAAAAAAAULL &&
            words[1] == 0xBBBBBBBBBBBBBBBBULL,
            "failed atomic read does not modify output buffer");

    require(bus.write32(Bus::kDpEnd, 0x310U), "publish complete two-word command");
    require(bus.readDpCommandWords(words, 2U), "consume complete multiword command");
    require(words[0] == first && words[1] == second,
            "multiword command words preserve big-endian order");
    require(bus.read32(Bus::kDpCurrent, current) && current == 0x310U,
            "successful atomic read advances by full command length");
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


void testRdramWriteEpochTracksDirectAndSpDmaWrites() {
    Bus bus;
    const std::uint64_t initialEpoch = bus.rdram().writeEpoch();
    require(bus.rdram().write8(0x700U, 0xA5U),
            "direct RDRAM write succeeds for epoch test");
    const std::uint64_t directEpoch = bus.rdram().writeEpoch();
    require(directEpoch > initialEpoch,
            "direct RDRAM write advances reservation generation");

    require(bus.write8(Bus::kSpDmemBase + 0x40U, 0x5AU),
            "seed SP DMEM for DMA epoch test");
    require(bus.write32(Bus::kSpMemAddr, 0x40U),
            "set SP DMA source for epoch test");
    require(bus.write32(Bus::kSpDramAddr, 0x780U),
            "set RDRAM DMA destination for epoch test");
    require(bus.write32(Bus::kSpWriteLength, 0U),
            "trigger one-byte SP-to-RDRAM DMA");
    require(bus.rdram().writeEpoch() > directEpoch,
            "SP DMA write advances RDRAM reservation generation");
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

void testSpStatusCommandBits() {
    Bus bus;
    std::uint32_t status = 0;

    require(bus.write32(Bus::kSpStatus, Bus::kSpSetSingleStep),
            "set SP single-step");
    require(bus.read32(Bus::kSpStatus, status), "read SP status after SSTEP set");
    require((status & Bus::kSpSingleStep) != 0U, "SSTEP status bit is set");

    require(bus.write32(Bus::kSpStatus, Bus::kSpClearSingleStep),
            "clear SP single-step");
    require(bus.read32(Bus::kSpStatus, status), "read SP status after SSTEP clear");
    require((status & Bus::kSpSingleStep) == 0U, "SSTEP status bit is cleared");

    require(bus.write32(Bus::kSpStatus, Bus::kSpSetInterruptOnBreak),
            "set SP interrupt-on-break");
    require(bus.read32(Bus::kSpStatus, status), "read SP status after INTR_BREAK set");
    require((status & Bus::kSpInterruptOnBreak) != 0U,
            "interrupt-on-break status bit is set");

    require(bus.write32(Bus::kSpStatus, 1U << 10U), "set SP SIG0");
    require(bus.read32(Bus::kSpStatus, status), "read SP status after SIG0 set");
    require((status & Bus::kSpSignal0) != 0U, "SIG0 status bit is set");

    require(bus.write32(Bus::kSpStatus, 1U << 9U), "clear SP SIG0");
    require(bus.read32(Bus::kSpStatus, status), "read SP status after SIG0 clear");
    require((status & Bus::kSpSignal0) == 0U, "SIG0 status bit is cleared");

    require(bus.write32(Bus::kMiInterruptMask, 1U << 1U),
            "enable MI SP interrupt for BREAK test");
    require(bus.write32(Bus::kSpStatus, 1U), "clear SP HALT");
    bus.signalSpBreak();
    require(bus.miInterruptPending(), "BREAK raises SP interrupt when INTR_BREAK is enabled");
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

void testMultirowDmaRejectsBeforeAnyRowIsCopied() {
    Bus bus;
    constexpr std::uint32_t spStart = 0x80U;
    const std::uint32_t dramStart =
        static_cast<std::uint32_t>(m64plus::n64::Rdram::kSize - 8U);

    for (std::uint32_t i = 0U; i < 16U; ++i) {
        require(bus.write8(Bus::kSpDmemBase + spStart + i, 0xCCU),
                "seed SP destination before rejected multirow DMA");
    }
    for (std::uint32_t i = 0U; i < 8U; ++i) {
        require(bus.rdram().write8(dramStart + i, static_cast<std::uint8_t>(0x40U + i)),
                "seed final valid RDRAM row");
    }

    require(bus.write32(Bus::kSpMemAddr, spStart),
            "set SP address for rejected multirow DMA");
    require(bus.write32(Bus::kSpDramAddr, dramStart),
            "set DRAM address for rejected multirow DMA");
    // The first 8-byte row is valid, but the second begins exactly past RDRAM.
    require(!bus.write32(Bus::kSpReadLength, 0x00001007U),
            "reject multirow DMA when a later row exceeds RDRAM");

    for (std::uint32_t i = 0U; i < 16U; ++i) {
        std::uint8_t actual = 0U;
        require(bus.read8(Bus::kSpDmemBase + spStart + i, actual),
                "read SP destination after rejected multirow DMA");
        require(actual == 0xCCU,
                "rejected multirow DMA copies none of its earlier valid rows");
    }

    std::uint32_t address = 0U;
    require(bus.read32(Bus::kSpMemAddr, address) && address == spStart,
            "rejected multirow DMA preserves SP address");
    require(bus.read32(Bus::kSpDramAddr, address) && address == dramStart,
            "rejected multirow DMA preserves DRAM address");
}

void testMultirowSpToDramDmaRejectsWithoutPartialWrites() {
    Bus bus;
    constexpr std::uint32_t spStart = 0xA0U;
    const std::uint32_t dramStart =
        static_cast<std::uint32_t>(m64plus::n64::Rdram::kSize - 8U);

    for (std::uint32_t i = 0U; i < 16U; ++i) {
        require(bus.write8(Bus::kSpDmemBase + spStart + i,
                           static_cast<std::uint8_t>(0x80U + i)),
                "seed SP source for rejected write DMA");
    }
    for (std::uint32_t i = 0U; i < 8U; ++i) {
        require(bus.rdram().write8(dramStart + i, 0x5AU),
                "seed final valid RDRAM row before rejected write DMA");
    }
    const std::uint64_t epochBeforeDma = bus.rdram().writeEpoch();

    require(bus.write32(Bus::kSpMemAddr, spStart),
            "set SP source for rejected write DMA");
    require(bus.write32(Bus::kSpDramAddr, dramStart),
            "set DRAM destination for rejected write DMA");
    // The first row fits, but the second row would begin beyond RDRAM.
    require(!bus.write32(Bus::kSpWriteLength, 0x00001007U),
            "reject SP-to-RDRAM DMA when a later row exceeds RDRAM");

    for (std::uint32_t i = 0U; i < 8U; ++i) {
        std::uint8_t actual = 0U;
        require(bus.rdram().read8(dramStart + i, actual),
                "read RDRAM after rejected write DMA");
        require(actual == 0x5AU,
                "rejected write DMA leaves its earlier valid row untouched");
    }
    require(bus.rdram().writeEpoch() == epochBeforeDma,
            "rejected write DMA does not advance RDRAM write epoch");

    std::uint32_t address = 0U;
    require(bus.read32(Bus::kSpMemAddr, address) && address == spStart,
            "rejected write DMA preserves SP address");
    require(bus.read32(Bus::kSpDramAddr, address) && address == dramStart,
            "rejected write DMA preserves DRAM address");
}

void testPiCartridgeToRdramDmaAndInterrupt() {
    Bus bus;
    std::uint8_t rom[72]{};
    rom[0] = 0x80U; rom[1] = 0x37U; rom[2] = 0x12U; rom[3] = 0x40U;
    const std::uint8_t payload[8] = {0x11U, 0x22U, 0x33U, 0x44U,
                                     0x55U, 0x66U, 0x77U, 0x88U};
    for (unsigned i = 0; i < 8U; ++i) rom[0x40U + i] = payload[i];
    require(bus.loadCartridge(rom, sizeof(rom)), "load ROM for PI DMA test");
    require(bus.region(Bus::kPiBase) == m64plus::n64::BusRegion::PiRegisters,
            "PI register window has a dedicated bus region");
    require(bus.write32(Bus::kPiDramAddr, 0x200U), "set PI DRAM destination");
    require(bus.write32(Bus::kPiCartAddr, Bus::kCartridgeBase + 0x40U),
            "set PI cartridge source");
    const std::uint64_t epochBeforePiDma = bus.rdram().writeEpoch();
    require(bus.write32(Bus::kPiWriteLength, 7U),
            "PI_WR_LEN starts eight-byte cartridge-to-RDRAM DMA");
    std::uint32_t copied = 0;
    require(bus.read32(0x200U, copied) && copied == 0x11223344U,
            "PI DMA copies first big-endian word into RDRAM");
    require(bus.read32(0x204U, copied) && copied == 0x55667788U,
            "PI DMA copies second big-endian word into RDRAM");
    require(bus.rdram().writeEpoch() > epochBeforePiDma,
            "PI cartridge DMA advances RDRAM reservation generation");
    require(bus.read32(Bus::kPiDramAddr, copied) && copied == 0x208U,
            "PI DRAM address advances by transferred byte count");
    require(bus.read32(Bus::kPiCartAddr, copied) &&
            copied == Bus::kCartridgeBase + 0x48U,
            "PI cartridge address advances by transferred byte count");
    require(bus.read32(Bus::kPiStatus, copied) &&
            (copied & Bus::kPiStatusInterrupt) != 0U,
            "PI DMA completion sets PI interrupt status");
    require(bus.write32(Bus::kMiInterruptMask, 1U << (4U * 2U + 1U)),
            "enable MI PI interrupt");
    require(bus.miInterruptPending(), "PI DMA completion asserts MI PI interrupt");
    require(bus.write32(Bus::kPiStatus, 2U), "acknowledge PI interrupt");
    require(!bus.miInterruptPending(), "PI status acknowledgement clears MI PI interrupt");
    require(bus.read32(Bus::kPiStatus, copied) &&
            (copied & Bus::kPiStatusInterrupt) == 0U,
            "PI interrupt status bit clears on acknowledgement");

    require(bus.rdram().write32(0x300U, 0xAABBCCDDU), "seed destination before invalid DMA");
    require(bus.write32(Bus::kPiDramAddr, 0x300U), "set invalid DMA destination");
    require(bus.write32(Bus::kPiCartAddr, Bus::kCartridgeBase + 68U),
            "set cartridge address near ROM end");
    require(!bus.write32(Bus::kPiWriteLength, 7U),
            "reject PI DMA that exceeds cartridge image");
    require(bus.rdram().read32(0x300U, copied) && copied == 0xAABBCCDDU,
            "failed PI DMA leaves destination unchanged");
    require(bus.read32(Bus::kPiStatus, copied) &&
            (copied & Bus::kPiStatusError) != 0U,
            "failed PI DMA sets PI error status");
    require(bus.write32(Bus::kPiStatus, 1U), "clear PI DMA error");
    require(!bus.write32(Bus::kPiReadLength, 3U),
            "reject RDRAM-to-read-only-ROM DMA direction");

    require(bus.rdram().write32(0x400U, 0xCAFEBABEU),
            "seed destination for PI alignment rejection tests");
    require(bus.write32(Bus::kPiDramAddr, 0x404U) &&
            bus.write32(Bus::kPiCartAddr, Bus::kCartridgeBase + 0x40U),
            "set unaligned RDRAM destination");
    require(!bus.write32(Bus::kPiWriteLength, 7U),
            "reject PI DMA with RDRAM destination not eight-byte aligned");
    require(bus.rdram().read32(0x400U, copied) && copied == 0xCAFEBABEU,
            "unaligned RDRAM DMA leaves destination unchanged");

    require(bus.write32(Bus::kPiStatus, 1U), "clear PI alignment error");
    require(bus.write32(Bus::kPiDramAddr, 0x400U) &&
            bus.write32(Bus::kPiCartAddr, Bus::kCartridgeBase + 0x41U),
            "set unaligned cartridge source");
    require(!bus.write32(Bus::kPiWriteLength, 7U),
            "reject PI DMA with odd cartridge address");
    require(bus.rdram().read32(0x400U, copied) && copied == 0xCAFEBABEU,
            "unaligned cartridge DMA leaves destination unchanged");

    require(bus.write32(Bus::kPiStatus, 1U), "clear odd-address PI error");
    require(bus.write32(Bus::kPiCartAddr, Bus::kCartridgeBase + 0x40U),
            "restore aligned cartridge source");
    require(!bus.write32(Bus::kPiWriteLength, 0U),
            "reject PI DMA with odd byte count");
    require(bus.rdram().read32(0x400U, copied) && copied == 0xCAFEBABEU,
            "odd-length PI DMA leaves destination unchanged");
}

void testDpResetClearsCommandStreamAndModes() {
    Bus bus;
    std::uint32_t value = 0U;
    std::uint64_t command = 0U;

    require(bus.write64(0x500U, 0x123456789ABCDEF0ULL),
            "seed RDRAM command before DPC reset");
    require(bus.write64(Bus::kSpDmemBase + 0x40U, 0x0FEDCBA987654321ULL),
            "seed XBUS command before DPC reset");
    require(bus.write32(Bus::kDpStart, 0x500U),
            "set DPC start before reset");
    require(bus.write32(Bus::kDpEnd, 0x508U),
            "set DPC end before reset");
    require(bus.write32(Bus::kDpStatus, (1U << 1U) | (1U << 3U) | (1U << 5U)),
            "enable XBUS, freeze, and flush before reset");
    require(bus.read32(Bus::kDpStatus, value) &&
            (value & (Bus::kDpXbusDmemDma | Bus::kDpFreeze | Bus::kDpFlush)) ==
                (Bus::kDpXbusDmemDma | Bus::kDpFreeze | Bus::kDpFlush),
            "DPC modes are active before reset");

    bus.reset();

    require(bus.read32(Bus::kDpStart, value) && value == 0U,
            "DPC reset clears start address");
    require(bus.read32(Bus::kDpEnd, value) && value == 0U,
            "DPC reset clears end address");
    require(bus.read32(Bus::kDpCurrent, value) && value == 0U,
            "DPC reset clears command cursor");
    require(bus.read32(Bus::kDpStatus, value) && value == 0U,
            "DPC reset clears XBUS, freeze, and flush modes");
    require(bus.dpCommandsRemaining() == 0U,
            "DPC reset removes pending command stream");

    require(bus.write64(0x500U, 0xA1A2A3A4A5A6A7A8ULL),
            "seed fresh RDRAM command after reset");
    require(bus.write32(Bus::kDpStart, 0x500U) &&
            bus.write32(Bus::kDpEnd, 0x508U),
            "publish fresh RDRAM command after reset");
    require(bus.readDpCommand(command) && command == 0xA1A2A3A4A5A6A7A8ULL,
            "DPC reset restores RDRAM command mode");
}

void testCartridgeByteOrderNormalization() {
    const std::uint8_t bigEndianRom[64] = {
        0x80U, 0x37U, 0x12U, 0x40U, 0x11U, 0x22U, 0x33U, 0x44U
    };
    const std::uint8_t byteSwappedRom[64] = {
        0x37U, 0x80U, 0x40U, 0x12U, 0x22U, 0x11U, 0x44U, 0x33U
    };
    const std::uint8_t wordSwappedRom[64] = {
        0x40U, 0x12U, 0x37U, 0x80U, 0x44U, 0x33U, 0x22U, 0x11U
    };

    Bus bus;
    std::uint32_t value = 0U;
    require(bus.loadCartridge(bigEndianRom, sizeof(bigEndianRom)),
            "load big-endian z64 ROM");
    require(bus.read32(Bus::kCartridgeBase + 4U, value) && value == 0x11223344U,
            "z64 ROM data remains in canonical byte order");

    require(bus.loadCartridge(byteSwappedRom, sizeof(byteSwappedRom)),
            "load byte-swapped v64 ROM");
    require(bus.read32(Bus::kCartridgeBase, value) && value == Bus::kN64RomMagic,
            "v64 header is normalized to canonical byte order");
    require(bus.read32(Bus::kCartridgeBase + 4U, value) && value == 0x11223344U,
            "v64 payload is normalized to canonical byte order");

    require(bus.loadCartridge(wordSwappedRom, sizeof(wordSwappedRom)),
            "load word-swapped n64 ROM");
    require(bus.read32(Bus::kCartridgeBase, value) && value == Bus::kN64RomMagic,
            "n64 header is normalized to canonical byte order");
    require(bus.read32(Bus::kCartridgeBase + 4U, value) && value == 0x11223344U,
            "n64 payload is normalized to canonical byte order");
}

void testCartridgeRomMappingAndValidation() {
    Bus bus;
    const std::uint8_t rom[] = {
        0x80U, 0x37U, 0x12U, 0x40U, 0x11U, 0x22U, 0x33U, 0x44U,
        0x55U, 0x66U, 0x77U, 0x88U, 0x99U, 0xAAU, 0xBBU, 0xCCU,
        0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
        0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
        0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U
    };
    require(bus.loadCartridge(rom, sizeof(rom)), "load valid big-endian N64 ROM");
    require(bus.cartridgeSize() == sizeof(rom), "cartridge size is exposed");
    std::uint32_t word = 0;
    require(bus.region(Bus::kCartridgeBase) == m64plus::n64::BusRegion::Cartridge,
            "PI cartridge window maps loaded ROM");
    require(bus.read32(Bus::kCartridgeBase, word) && word == 0x80371240U,
            "ROM header is readable in big-endian order");
    require(bus.read32(Bus::kCartridgeBase + 4U, word) && word == 0x11223344U,
            "ROM data is readable through the physical bus");
    require(!bus.write32(Bus::kCartridgeBase + 4U, 0U), "cartridge ROM rejects writes");
    require(!bus.read32(Bus::kCartridgeBase + 64U, word),
            "read beyond end of loaded ROM is rejected");
    const std::uint8_t invalid[64] = {};
    require(!bus.loadCartridge(invalid, sizeof(invalid)), "reject invalid ROM magic");
    require(bus.read32(Bus::kCartridgeBase, word) && word == 0x80371240U,
            "failed ROM replacement preserves previously loaded cartridge");
    bus.reset();
    require(bus.read32(Bus::kCartridgeBase + 4U, word) && word == 0x11223344U,
            "hardware reset preserves inserted cartridge");
    bus.unloadCartridge();
    require(bus.cartridgeSize() == 0U &&
            bus.region(Bus::kCartridgeBase) == m64plus::n64::BusRegion::Unmapped,
            "unloading removes cartridge mapping");
}

} // namespace

int main() {
    testMiInterruptRegisterReflectsSourcesAndIsReadOnly();
    testMiInterruptSourcesRemainIndependent();
    testMiInterruptMaskSetAndClearCommands();
    testMiResetClearsInterruptSourcesAndMasks();
    testMiVersionRegisterReportsRcpRevision();
    testDpRegisterWindow();
    testDpResetClearsCommandStreamAndModes();
    testCartridgeByteOrderNormalization();
    testCartridgeRomMappingAndValidation();
    testPiCartridgeToRdramDmaAndInterrupt();
    testDpCommandStreamFromRdramAndDmem();
    testDpMultiwordCommandIsAtomic();
    testDramToSpAndSpToDram();
    testRdramWriteEpochTracksDirectAndSpDmaWrites();
    testRowsAndSkip();
    testImemBankAndWrapping();
    testOutOfRangeIsRejectedBeforeCopy();
    testMultirowDmaRejectsBeforeAnyRowIsCopied();
    testMultirowSpToDramDmaRejectsWithoutPartialWrites();
    testSpStatusCommandBits();
    std::cout << "All SP DMA host tests passed.\n";
    return 0;
}
