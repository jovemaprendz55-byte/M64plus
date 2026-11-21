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
    require(bus.write32(Bus::kSpStatus, Bus::kSpInterruptOnBreak | 1U),
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

    // A later task can restart the halted RSP at a CPU-programmed SP_PC.
    require(bus.write32(Bus::kSpImemBase + 0x40U, 0x24020077U), "load second task");
    require(bus.write32(Bus::kSpImemBase + 0x44U, 0x0000000DU), "load second task BREAK");
    require(bus.write32(Bus::kSpPc, 0x40U), "program second task PC");
    require(bus.write32(Bus::kSpStatus, 1U | (1U << 2U)), "clear HALT and BROKE for next task");
    require(machine.runRsp(4U) == 2U, "RSP restarts from the programmed PC");
    require(machine.rsp().reg(2) == 0x77U, "second RSP task executes");
}
} // namespace

int main() {
    testSharedCpuRspBusAndSpBreak();
    std::cout << "N64 machine CPU/RSP integration tests passed.\n";
    return 0;
}
