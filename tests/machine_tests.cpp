#include "n64/machine.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>

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
} // namespace

int main() {
    testCpuKsegDirectMappingWithoutTlb();
    testMachineDoesNotRetryUnsupportedRspInstruction();
    testSharedCpuRspBusAndSpBreak();
    std::cout << "N64 machine CPU/RSP integration tests passed.\n";
    return 0;
}
