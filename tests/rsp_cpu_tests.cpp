#include "n64/bus.h"
#include "n64/rsp_cpu.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>

using m64plus::n64::Bus;
using m64plus::n64::RspCpu;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
std::uint32_t iType(unsigned op, unsigned rs, unsigned rt, std::uint16_t imm) {
    return (op << 26U) | (rs << 21U) | (rt << 16U) | imm;
}
std::uint32_t vType(unsigned element, unsigned vt, unsigned vs, unsigned vd, unsigned function) {
    return (0x12U << 26U) | ((0x10U | element) << 21U) |
        (vt << 16U) | (vs << 11U) | (vd << 6U) | function;
}
void put(Bus& bus, std::uint32_t pc, std::uint32_t instruction) {
    require(bus.write32(Bus::kSpImemBase + pc, instruction), "write RSP instruction");
}
void testArithmeticAndMemory() {
    Bus bus;
    RspCpu rsp;
    put(bus, 0x00, iType(0x0F, 0, 1, 0x1234));
    put(bus, 0x04, iType(0x0D, 1, 1, 0x5678));
    put(bus, 0x08, iType(0x09, 0, 2, 0x0040));
    put(bus, 0x0C, iType(0x2B, 2, 1, 0));
    put(bus, 0x10, iType(0x23, 2, 3, 0));
    put(bus, 0x14, 0x0000000DU);
    for (int i = 0; i < 6; ++i) require(rsp.step(bus), "execute arithmetic program");
    require(rsp.reg(1) == 0x12345678U, "LUI/ORI result");
    require(rsp.reg(3) == 0x12345678U, "LW result");
    require(rsp.halted(), "BREAK halts RSP");
    require(rsp.reg(0) == 0U, "zero register remains zero");
}
void testBranchDelaySlot() {
    Bus bus;
    RspCpu rsp;
    put(bus, 0x00, iType(0x09, 0, 1, 1));
    put(bus, 0x04, iType(0x04, 1, 1, 2));
    put(bus, 0x08, iType(0x09, 0, 2, 7));
    put(bus, 0x0C, iType(0x09, 0, 3, 99));
    put(bus, 0x10, 0x0000000DU);
    require(rsp.step(bus), "branch setup");
    require(rsp.step(bus), "branch instruction");
    require(rsp.pc() == 0x08U, "branch enters delay slot");
    require(rsp.step(bus), "execute delay slot");
    require(rsp.reg(2) == 7U, "delay slot executed");
    require(rsp.pc() == 0x10U, "branch target selected after delay slot");
    require(rsp.reg(3) == 0U, "instruction after delay slot skipped");
    require(rsp.step(bus), "execute target BREAK");
    require(rsp.halted(), "target BREAK halts");
}
void testByteAndHalfwordMemory() {
    Bus bus;
    RspCpu rsp;
    put(bus, 0x00, iType(0x0F, 0, 1, 0xFFFF));
    put(bus, 0x04, iType(0x0D, 1, 1, 0xFFFF));
    put(bus, 0x08, iType(0x09, 0, 2, 0x0100));
    put(bus, 0x0C, iType(0x28, 2, 1, 1));
    put(bus, 0x10, iType(0x20, 2, 3, 1));
    put(bus, 0x14, iType(0x24, 2, 4, 1));
    put(bus, 0x18, iType(0x29, 2, 1, 2));
    put(bus, 0x1C, iType(0x21, 2, 5, 2));
    put(bus, 0x20, iType(0x25, 2, 6, 2));
    put(bus, 0x24, 0x0000000DU);
    for (int i = 0; i < 10; ++i) require(rsp.step(bus), "execute byte/halfword program");
    require(rsp.reg(3) == 0xFFFFFFFFU, "LB sign extends");
    require(rsp.reg(4) == 0x000000FFU, "LBU zero extends");
    require(rsp.reg(5) == 0xFFFFFFFFU, "LH sign extends");
    require(rsp.reg(6) == 0x0000FFFFU, "LHU zero extends");
    std::uint8_t byte = 0;
    std::uint16_t half = 0;
    require(bus.read8(Bus::kSpDmemBase + 0x101U, byte) && byte == 0xFFU,
            "SB writes one byte");
    require(bus.read16(Bus::kSpDmemBase + 0x102U, half) && half == 0xFFFFU,
            "SH writes big-endian halfword");
}
void testRegimmBranches() {
    Bus bus;
    RspCpu rsp;
    put(bus, 0x00, iType(0x0F, 0, 1, 0xFFFF));
    put(bus, 0x04, iType(0x01, 1, 0, 2));
    put(bus, 0x08, iType(0x09, 0, 2, 7));
    put(bus, 0x0C, iType(0x09, 0, 3, 99));
    put(bus, 0x10, iType(0x01, 1, 1, 2));
    put(bus, 0x14, iType(0x09, 0, 4, 8));
    put(bus, 0x18, iType(0x09, 0, 5, 9));
    put(bus, 0x1C, 0x0000000DU);
    for (int i = 0; i < 7; ++i) require(rsp.step(bus), "execute REGIMM program");
    require(rsp.reg(2) == 7U, "BLTZ delay slot");
    require(rsp.reg(3) == 0U, "BLTZ target skips sequential instruction");
    require(rsp.reg(4) == 8U, "not-taken BGEZ delay slot");
    require(rsp.reg(5) == 9U, "not-taken BGEZ falls through after delay slot");
    require(rsp.halted(), "REGIMM program reaches BREAK");
}
void testVectorArithmetic() {
    Bus bus;
    RspCpu rsp;
    for (unsigned lane = 0; lane < 8U; ++lane) {
        rsp.setVectorLane(1, lane, 30000U);
        rsp.setVectorLane(2, lane, 10000U);
    }
    rsp.setVectorLane(1, 1, 0x8AD0U);
    rsp.setVectorLane(2, 1, 0xD8F0U);
    put(bus, 0, vType(0, 2, 1, 3, 0x10)); // VADD
    put(bus, 4, vType(0, 2, 1, 4, 0x11)); // VSUB
    require(rsp.step(bus), "execute VADD");
    require(rsp.vectorLane(3, 0) == 32767U, "VADD positive saturation");
    require(rsp.vectorLane(3, 1) == 0x8000U, "VADD negative saturation");
    require(rsp.vectorCarryFlags() == 0U, "VADD clears VCO");
    require(rsp.step(bus), "execute VSUB");
    require(rsp.vectorLane(4, 0) == 20000U, "VSUB positive difference");
    require(rsp.vectorLane(4, 1) == static_cast<std::uint16_t>(-20000), "VSUB negative difference");
}
void testVectorCarryAndLogic() {
    Bus bus;
    RspCpu rsp;
    rsp.setVectorLane(1, 0, 0xFFFFU);
    rsp.setVectorLane(2, 0, 1U);
    rsp.setVectorLane(1, 1, 0U);
    rsp.setVectorLane(2, 1, 1U);
    put(bus, 0, vType(0, 2, 1, 3, 0x14)); // VADDC
    put(bus, 4, vType(0, 2, 1, 4, 0x10)); // VADD consumes VCO_LO
    put(bus, 8, vType(0, 2, 1, 5, 0x15)); // VSUBC
    put(bus, 12, vType(0, 2, 1, 6, 0x2C)); // VXOR
    require(rsp.step(bus), "execute VADDC");
    require(rsp.vectorLane(3, 0) == 0U, "VADDC low result");
    require((rsp.vectorCarryFlags() & 1U) != 0U, "VADDC records carry in VCO_LO");
    require(rsp.step(bus), "execute VADD with carry input");
    require(rsp.vectorLane(4, 0) == 1U, "VADD consumes VCO_LO carry");
    require(rsp.vectorCarryFlags() == 0U, "VADD clears both VCO fields");
    require(rsp.step(bus), "execute VSUBC");
    require(rsp.vectorLane(5, 0) == 0xFFFEU, "VSUBC subtraction result");
    require(rsp.vectorLane(5, 1) == 0xFFFFU, "VSUBC underflow result");
    require((rsp.vectorCarryFlags() & (1U << 1U)) != 0U, "VSUBC borrow flag");
    require((rsp.vectorCarryFlags() & (1U << 9U)) != 0U, "VSUBC unequal flag");
    require(rsp.step(bus), "execute VXOR");
    require(rsp.vectorLane(6, 0) == 0xFFFEU, "VXOR lane result");
}
void testVectorInstructionInBranchDelaySlot() {
    Bus bus;
    RspCpu rsp;
    for (unsigned lane = 0; lane < 8U; ++lane) {
        rsp.setVectorLane(1, lane, 0x00F0U);
        rsp.setVectorLane(2, lane, 0x000FU);
    }
    put(bus, 0, iType(0x04, 0, 0, 2)); // BEQ zero,zero,target 0x0C
    put(bus, 4, vType(0, 2, 1, 3, 0x2A)); // VOR in delay slot
    put(bus, 8, iType(0x09, 0, 4, 99)); // skipped
    put(bus, 12, 0x0000000DU); // BREAK
    require(rsp.step(bus), "execute branch before vector delay slot");
    require(rsp.step(bus), "execute vector instruction in delay slot");
    require(rsp.vectorLane(3, 0) == 0x00FFU, "vector delay slot executes");
    require(rsp.pc() == 12U, "branch target follows vector delay slot");
    require(rsp.reg(4) == 0U, "instruction after delay slot is skipped");
    require(rsp.step(bus), "execute BREAK at branch target");
    require(rsp.halted(), "branch target BREAK halts");
}
void testVectorPairBroadcastSelectors() {
    Bus bus;
    RspCpu rsp;
    for (unsigned lane = 0; lane < 8U; ++lane) {
        rsp.setVectorLane(1, lane, 0x0100U);
        rsp.setVectorLane(2, lane, static_cast<std::uint16_t>(lane + 1U));
    }
    put(bus, 0, vType(2, 2, 1, 3, 0x2A)); // e2: lanes 0,0,2,2,4,4,6,6
    require(rsp.step(bus), "execute paired-lane broadcast VOR");
    const std::uint16_t expected[8] = {
        0x0101U, 0x0101U, 0x0103U, 0x0103U,
        0x0105U, 0x0105U, 0x0107U, 0x0107U
    };
    for (unsigned lane = 0; lane < 8U; ++lane)
        require(rsp.vectorLane(3, lane) == expected[lane],
                "element 2 selects paired even lanes");
}
void testVectorElementBroadcast() {
    Bus bus;
    RspCpu rsp;
    for (unsigned lane = 0; lane < 8U; ++lane) {
        rsp.setVectorLane(1, lane, 1U);
        rsp.setVectorLane(2, lane, 0U);
    }
    rsp.setVectorLane(2, 7, 16U);
    put(bus, 0, vType(15, 2, 1, 3, 0x2A));
    require(rsp.step(bus), "execute broadcast-element VOR");
    for (unsigned lane = 0; lane < 8U; ++lane)
        require(rsp.vectorLane(3, lane) == 17U, "element 15 broadcasts lane 7");
}
} // namespace

int main() {
    testArithmeticAndMemory();
    testBranchDelaySlot();
    testByteAndHalfwordMemory();
    testRegimmBranches();
    testVectorArithmetic();
    testVectorCarryAndLogic();
    testVectorPairBroadcastSelectors();
    testVectorElementBroadcast();
    testVectorInstructionInBranchDelaySlot();
    std::cout << "All scalar and vector RSP host tests passed.\n";
    return 0;
}
