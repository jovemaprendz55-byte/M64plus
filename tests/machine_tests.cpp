#include "n64/machine.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

using m64plus::n64::Bus;
using m64plus::n64::N64Machine;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void testMachineOwnsAndProcessesRdpState() {
    N64Machine machine;
    Bus& bus = machine.bus();
    // SET_COLOR_IMAGE: RGBA16, width 320, address 0x2000.
    const std::uint64_t setColorImage = (0x3FULL << 56U) |
        (2ULL << 51U) | (319ULL << 32U) | 0x2000ULL;
    require(bus.write64(0x100U, setColorImage), "write RDP SET_COLOR_IMAGE command");
    require(bus.write32(Bus::kDpStart, 0x100U), "set DPC_START");
    require(bus.write32(Bus::kDpEnd, 0x108U), "set DPC_END");
    require(machine.runRdp(4U) == 1U, "machine consumes one RDP command");
    const auto& image = machine.rdp().colorImage;
    require(machine.rdp().hasColorImage, "machine retains RDP color image state");
    require(image.format == 0U && image.size == 2U && image.width == 320U &&
            image.address == 0x2000U, "machine exposes decoded color image descriptor");
    require(machine.runRdp(4U) == 0U, "RDP runner stops when command stream is empty");

    // Reset must clear RDP state as well as CPU/RSP and bus state.
    machine.reset();
    require(!machine.rdp().hasColorImage, "machine reset clears RDP image state");
}

void testCpuKsegDirectMappingWithoutTlb() {
    N64Machine machine;
    Bus& bus = machine.bus();

    // Execute from KSEG0 while the software-managed TLB is disabled.
    machine.cpu().reset(0xFFFFFFFF80000000ULL);
    const std::uint32_t program[] = {
        0x3C08A000U, // LUI t0,0xA000 -> KSEG1 base
        0x35080100U, // ORI t0,t0,0x0100 -> KSEG1 + 0x100
        0x240955AAU, // ADDIU t1,zero,0x55AA
        0xAD090000U  // SW t1,0(t0) -> physical 0x100
    };
    for (std::uint32_t i = 0; i < 4U; ++i)
        require(bus.write32(i * 4U, program[i]), "load KSEG direct-map test program");

    for (unsigned i = 0; i < 4U; ++i)
        require(machine.stepCpu(), "CPU executes through KSEG0 with TLB disabled");

    std::uint32_t stored = 0;
    require(bus.read32(0x100U, stored), "read physical destination after KSEG1 store");
    require(stored == 0x000055AAU, "KSEG1 store maps to the corresponding physical address");
    require(machine.cpu().pc() == 0xFFFFFFFF80000010ULL,
            "CPU PC remains in canonical KSEG0 after instruction fetches");
}

void testCpuFetchesAndExecutesFromCartridgeKseg1() {
    N64Machine machine;
    std::uint8_t rom[0x40U + 8U]{};
    rom[0] = 0x80U;
    rom[1] = 0x37U;
    rom[2] = 0x12U;
    rom[3] = 0x40U;
    // IPL3 entry point in a cartridge image: two simple MIPS instructions.
    const std::uint32_t program[] = {0x24081234U, 0x25090001U};
    for (unsigned i = 0; i < 2U; ++i) {
        const std::uint32_t instruction = program[i];
        const std::size_t offset = 0x40U + i * 4U;
        rom[offset] = static_cast<std::uint8_t>(instruction >> 24U);
        rom[offset + 1U] = static_cast<std::uint8_t>(instruction >> 16U);
        rom[offset + 2U] = static_cast<std::uint8_t>(instruction >> 8U);
        rom[offset + 3U] = static_cast<std::uint8_t>(instruction);
    }
    require(machine.bus().loadCartridge(rom, sizeof(rom)),
            "load test cartridge for CPU execution");

    machine.cpu().reset(0xFFFFFFFFB0000040ULL);
    require(machine.stepCpu(), "CPU fetches first instruction through KSEG1 cartridge mapping");
    require(machine.stepCpu(), "CPU fetches second instruction through KSEG1 cartridge mapping");
    require(machine.cpu().reg(8) == 0x1234U,
            "CPU executes instruction fetched from cartridge IPL3 offset");
    require(machine.cpu().reg(9) == 0x1235U,
            "CPU continues executing sequential cartridge instruction");
    require(machine.cpu().pc() == 0xFFFFFFFFB0000048ULL,
            "CPU PC advances inside KSEG1 cartridge window");
}


void testCpuRunnerHonorsInstructionBudgetAndStopsAtBudget() {
    N64Machine machine;
    Bus& bus = machine.bus();
    const std::uint32_t program[] = {
        0x24080001U, // ADDIU t0,zero,1
        0x25080001U, // ADDIU t0,t0,1
        0x25080001U, // ADDIU t0,t0,1
        0x25080001U  // ADDIU t0,t0,1
    };
    for (std::uint32_t i = 0; i < 4U; ++i)
        require(bus.write32(i * 4U, program[i]), "load bounded CPU runner program");
    machine.cpu().reset(0xFFFFFFFF80000000ULL);
    require(machine.runCpuInstructions(0U) == 0U,
            "zero CPU instruction budget executes nothing");
    require(machine.cpu().instructionsExecuted() == 0U,
            "zero budget leaves CPU instruction counter unchanged");
    require(machine.runCpuInstructions(2U) == 2U,
            "CPU runner executes exactly the requested first slice");
    require(machine.cpu().reg(8U) == 2U &&
            machine.cpu().pc() == 0xFFFFFFFF80000008ULL,
            "first CPU slice advances state by two instructions");
    require(machine.runCpuInstructions(1U) == 1U,
            "CPU runner executes the next bounded slice");
    require(machine.cpu().reg(8U) == 3U &&
            machine.cpu().pc() == 0xFFFFFFFF8000000CULL,
            "second CPU slice continues from prior PC");
    require(machine.runCpuInstructions(1U) == 1U,
            "CPU runner can execute final instruction in a slice");
    require(machine.cpu().reg(8U) == 4U,
            "CPU runner executes final instruction without exceeding budget");
}

void testMachineDoesNotRetryUnsupportedRspInstruction() {
    N64Machine machine;
    Bus& bus = machine.bus();
    require(bus.write32(Bus::kSpImemBase, 0xFFFFFFFFU),
            "write unsupported RSP instruction");
    require(bus.write32(Bus::kSpPc, 0U), "set faulting RSP PC");
    require(bus.write32(Bus::kSpStatus, 1U), "release RSP HALT");
    require(!machine.stepRsp(), "unsupported RSP instruction fails");
    require(machine.rsp().faulted(), "machine preserves RSP fault state");
    require(!machine.stepRsp(), "machine does not restart faulting instruction");
    require(machine.rsp().instructionsExecuted() == 0U,
            "faulting instruction is not repeatedly executed");
    require(bus.write32(Bus::kSpPc, 4U), "move SP_PC to a new task location");
    require(bus.write32(Bus::kSpImemBase + 4U, 0x0000000DU),
            "write BREAK at new task location");
    require(machine.stepRsp(), "new SP_PC permits a new RSP task");
    require(bus.spHalted() && bus.spBroke(), "new task BREAK halts normally");
}

void testSharedCpuRspBusAndSpBreak() {
    N64Machine machine;
    Bus& bus = machine.bus();

    // The RSP starts halted until software clears SP_STATUS.HALT.
    require(bus.spHalted(), "SP starts halted after reset");
    require(!machine.stepRsp(), "RSP does not run while SP is halted");

    const std::uint32_t rspProgram[] = {
        0x24011234U, // ADDIU r1,zero,0x1234
        0xAC010000U, // SW r1,0(zero) -> SP DMEM
        0x0000000DU  // BREAK
    };
    for (std::uint32_t i = 0; i < 3U; ++i) {
        require(bus.write32(Bus::kSpImemBase + 0x20U + i * 4U, rspProgram[i]),
                "load RSP program into IMEM");
    }

    // Enable the SP interrupt mask and request an interrupt on RSP BREAK.
    require(bus.write32(Bus::kSpPc, 0x20U), "set RSP start PC through SP_PC");
    require(bus.spProgramCounter() == 0x20U, "SP_PC register stores aligned PC");
    require(bus.write32(Bus::kMiInterruptMask, 1U << 1U),
            "enable MI SP interrupt");
    require(bus.write32(Bus::kSpStatus, Bus::kSpSetInterruptOnBreak | 1U),
            "configure SP interrupt-on-break and clear HALT");
    require(!bus.spHalted(), "SP HALT clears through MMIO command");

    require(machine.runRsp(8U) == 3U, "RSP executes task within instruction budget");
    require(bus.spProgramCounter() == 0x2CU, "SP_PC tracks RSP instruction progress");
    require(machine.rsp().reg(1) == 0x1234U, "RSP scalar result");
    require(bus.spHalted() && bus.spBroke(), "BREAK sets SP HALT and BROKE");
    require(bus.miInterruptPending(), "BREAK asserts masked MI SP interrupt");

    // The main CPU observes data written by the RSP through the same physical bus.
    require(bus.write32(0x00000000U, 0x3C080400U), "write CPU LUI instruction");
    require(bus.write32(0x00000004U, 0x8D090000U), "write CPU LW instruction");
    require(machine.stepCpu(), "main CPU executes LUI");
    require(machine.stepCpu(), "main CPU reads SP DMEM");
    require(machine.cpu().reg(9) == 0x1234U,
            "main CPU sees the RSP result in shared SP DMEM");

    // The main CPU programs SP_PC and releases HALT through MMIO.
    const std::uint32_t startRsp[] = {
        0x3C080408U, // LUI t0,0x0408 -> SP_PC
        0x24090040U, // ADDIU t1,zero,0x40
        0xAD090000U, // SW t1,0(t0) -> SP_PC
        0x3C0A0404U, // LUI t2,0x0404 -> SP registers
        0x240B0005U, // ADDIU t3,zero,clear HALT + clear BROKE
        0xAD4B0010U  // SW t3,0x10(t2) -> SP_STATUS
    };
    for (std::uint32_t i = 0; i < 6U; ++i)
        require(bus.write32(8U + i * 4U, startRsp[i]), "write CPU RSP-start program");
    for (unsigned i = 0; i < 6U; ++i)
        require(machine.stepCpu(), "CPU programs SP_PC and SP_STATUS");
    require(bus.spProgramCounter() == 0x40U && !bus.spHalted(),
            "CPU starts RSP task through MMIO");

    const std::uint32_t vectorTask[] = {
        0x24017530U, // ADDIU r1,zero,30000
        0x48810800U, // MTC2 r1,v1[e0]
        0x24012710U, // ADDIU r1,zero,10000
        0x48811000U, // MTC2 r1,v2[e0]
        0x4A0208D0U, // VADD v3,v1,v2
        0x0000000DU  // BREAK
    };
    for (std::uint32_t i = 0; i < 6U; ++i)
        require(bus.write32(Bus::kSpImemBase + 0x40U + i * 4U, vectorTask[i]),
                "load vector RSP task");
    require(machine.runRsp(8U) == 6U, "RSP runs COP2 vector task");
    require(machine.rsp().vectorLane(3, 0) == 32767U,
            "integrated RSP executes saturated VADD");
    require(bus.spHalted() && bus.spBroke(), "second task also signals BREAK");
}

void testCpuProgramsPiDmaAndExecutesCartridgePayload() {
    N64Machine machine;
    Bus& bus = machine.bus();

    std::uint8_t rom[0x48U]{};
    rom[0] = 0x80U; rom[1] = 0x37U; rom[2] = 0x12U; rom[3] = 0x40U;
    const std::uint32_t payload[] = {0x24081234U, 0x25090001U};
    for (unsigned i = 0; i < 2U; ++i) {
        const std::uint32_t instruction = payload[i];
        const std::size_t offset = 0x40U + i * 4U;
        rom[offset] = static_cast<std::uint8_t>(instruction >> 24U);
        rom[offset + 1U] = static_cast<std::uint8_t>(instruction >> 16U);
        rom[offset + 2U] = static_cast<std::uint8_t>(instruction >> 8U);
        rom[offset + 3U] = static_cast<std::uint8_t>(instruction);
    }
    require(bus.loadCartridge(rom, sizeof(rom)),
            "load cartridge for CPU-driven PI DMA integration");

    // The emulated CPU programs PI through its KSEG1 MMIO address.
    const std::uint32_t dmaProgram[] = {
        0x3C08A460U, // LUI t0,0xA460 -> PI registers
        0x24090200U, // ADDIU t1,zero,0x200 -> RDRAM destination
        0xAD090000U, // SW t1,PI_DRAM_ADDR(t0)
        0x3C091000U, // LUI t1,0x1000
        0x35290040U, // ORI t1,t1,0x40 -> cartridge source
        0xAD090004U, // SW t1,PI_CART_ADDR(t0)
        0x24090007U, // ADDIU t1,zero,7 -> eight bytes
        0xAD09000CU  // SW t1,PI_WR_LEN(t0), starts cart-to-RDRAM DMA
    };
    for (std::uint32_t i = 0; i < 8U; ++i)
        require(bus.write32(i * 4U, dmaProgram[i]),
                "write CPU program that configures PI DMA");
    for (unsigned i = 0; i < 8U; ++i)
        require(machine.stepCpu(), "CPU executes PI DMA setup instruction");

    std::uint32_t copied = 0;
    require(bus.read32(0x200U, copied) && copied == payload[0],
            "CPU-triggered PI DMA copies first instruction into RDRAM");
    require(bus.read32(0x204U, copied) && copied == payload[1],
            "CPU-triggered PI DMA copies second instruction into RDRAM");
    require(bus.read32(Bus::kPiStatus, copied) &&
            (copied & Bus::kPiStatusInterrupt) != 0U,
            "CPU-triggered PI DMA completes and raises PI interrupt");

    machine.cpu().reset(0xFFFFFFFF80000200ULL);
    require(machine.stepCpu(), "CPU fetches first cartridge payload instruction from RDRAM");
    require(machine.stepCpu(), "CPU fetches second cartridge payload instruction from RDRAM");
    require(machine.cpu().reg(8) == 0x1234U,
            "CPU executes first instruction copied from cartridge");
    require(machine.cpu().reg(9) == 0x1235U,
            "CPU executes second instruction copied from cartridge");
}


void testCartridgeHleBootInitializesRdramDmemAndCpuState() {
    N64Machine machine;
    std::vector<std::uint8_t> rom(0x1008U, 0U);
    rom[0] = 0x80U; rom[1] = 0x37U; rom[2] = 0x12U; rom[3] = 0x40U;
    rom[8] = 0x80U; rom[9] = 0x00U; rom[10] = 0x04U; rom[11] = 0x00U;
    const std::uint32_t program[] = {0x24081234U, 0x25090001U};
    for (unsigned i = 0; i < 2U; ++i) {
        const std::uint32_t instruction = program[i];
        const std::size_t offset = 0x1000U + i * 4U;
        rom[offset] = static_cast<std::uint8_t>(instruction >> 24U);
        rom[offset + 1U] = static_cast<std::uint8_t>(instruction >> 16U);
        rom[offset + 2U] = static_cast<std::uint8_t>(instruction >> 8U);
        rom[offset + 3U] = static_cast<std::uint8_t>(instruction);
    }
    require(machine.bus().loadCartridge(rom.data(), rom.size()),
            "load synthetic cartridge for HLE boot");
    require(machine.bootCartridgeHle(), "HLE boot accepts valid cartridge entry point");

    std::uint32_t value = 0;
    require(machine.bus().read32(0x400U, value) && value == program[0],
            "HLE copies cartridge payload to header entry point in RDRAM");
    require(machine.bus().read32(0x404U, value) && value == program[1],
            "HLE copies following payload instruction to RDRAM");
    require(machine.bus().read32(Bus::kSpDmemBase, value) && value == 0x80371240U,
            "HLE copies cartridge header into SP DMEM");
    require(machine.bus().read32(0x300U, value) && value == 1U,
            "HLE publishes NTSC TV boot parameter");
    require(machine.bus().read32(0x308U, value) && value == 0xB0000000U,
            "HLE publishes cartridge ROM base");
    require(machine.bus().read32(0x318U, value) && value == 0x00800000U,
            "HLE publishes 8 MiB RDRAM size");
    require(machine.cpu().pc() == 0xFFFFFFFF80000400ULL,
            "HLE sets CPU PC to ROM header entry point");
    require(machine.cpu().reg(20U) == 1U && machine.cpu().reg(22U) == 0x3FU,
            "HLE sets NTSC and CIC-6102 boot arguments");
    require(machine.cpu().reg(29U) == 0xFFFFFFFFA4001FF0ULL,
            "HLE initializes boot stack in SP DMEM");
    require(machine.cpu().cp0().read(m64plus::n64::Cp0::kStatus) == 0x34000000ULL,
            "HLE initializes CPU CP0 status");
    require(machine.stepCpu() && machine.stepCpu(),
            "CPU executes instructions loaded by HLE boot");
    require(machine.cpu().reg(8U) == 0x1234U && machine.cpu().reg(9U) == 0x1235U,
            "HLE-booted CPU executes cartridge entry code");
}


void testHleBootDetectsCicAndPalRegionFromRom() {
    N64Machine machine;
    std::vector<std::uint8_t> rom(0x1008U, 0U);
    rom[0] = 0x80U; rom[1] = 0x37U; rom[2] = 0x12U; rom[3] = 0x40U;
    rom[8] = 0x80U; rom[9] = 0x00U; rom[10] = 0x04U; rom[11] = 0x00U;
    rom[0x3EU] = static_cast<std::uint8_t>('P'); // PAL region
    // Build an IPL3 whose 1008 little-endian 32-bit words sum to CIC-X103.
    std::uint64_t remaining = 0x000000D6497E414BULL;
    for (std::size_t i = 0; i < 0xFC0U / 4U && remaining != 0U; ++i) {
        const std::uint32_t word = remaining > 0xFFFFFFFFULL
            ? 0xFFFFFFFFU : static_cast<std::uint32_t>(remaining);
        remaining -= word;
        const std::size_t offset = 0x40U + i * 4U;
        rom[offset] = static_cast<std::uint8_t>(word);
        rom[offset + 1U] = static_cast<std::uint8_t>(word >> 8U);
        rom[offset + 2U] = static_cast<std::uint8_t>(word >> 16U);
        rom[offset + 3U] = static_cast<std::uint8_t>(word >> 24U);
    }
    require(remaining == 0U, "synthetic IPL3 checksum fits the bootcode area");
    const std::uint32_t program[] = {0x24081234U, 0x25090001U};
    for (unsigned i = 0; i < 2U; ++i) {
        const std::uint32_t instruction = program[i];
        const std::size_t offset = 0x1000U + i * 4U;
        rom[offset] = static_cast<std::uint8_t>(instruction >> 24U);
        rom[offset + 1U] = static_cast<std::uint8_t>(instruction >> 16U);
        rom[offset + 2U] = static_cast<std::uint8_t>(instruction >> 8U);
        rom[offset + 3U] = static_cast<std::uint8_t>(instruction);
    }
    require(machine.bus().loadCartridge(rom.data(), rom.size()),
            "load synthetic CIC-X103 PAL cartridge");
    require(machine.bootCartridgeHle(), "HLE boots with detected CIC-X103 and PAL");
    std::uint32_t value = 0;
    require(machine.cpu().reg(20U) == 0U,
            "HLE sets s4 to PAL from cartridge country code");
    require(machine.cpu().reg(22U) == 0x78U,
            "HLE sets s6 to CIC-X103 seed");
    require(machine.bus().read32(0x300U, value) && value == 0U,
            "HLE publishes PAL TV type in low-memory boot parameters");
    require(machine.bus().read32(0x310U, value) && value == 6103U,
            "HLE publishes CIC-6103 identifier for X103");
}

} // namespace

int main() {
    testMachineOwnsAndProcessesRdpState();
    testCpuKsegDirectMappingWithoutTlb();
    testCpuFetchesAndExecutesFromCartridgeKseg1();
    testCpuProgramsPiDmaAndExecutesCartridgePayload();
    testCpuRunnerHonorsInstructionBudgetAndStopsAtBudget();
    testCartridgeHleBootInitializesRdramDmemAndCpuState();
    testHleBootDetectsCicAndPalRegionFromRom();
    testMachineDoesNotRetryUnsupportedRspInstruction();
    testSharedCpuRspBusAndSpBreak();
    std::cout << "N64 machine CPU/RSP integration tests passed.\n";
    return 0;
}
