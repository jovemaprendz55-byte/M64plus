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
    put(bus, 0x00, iType(0x0F, 0, 1, 0xFFFF)); // LUI r1,FFFF0000
    put(bus, 0x04, iType(0x0D, 1, 1, 0xFFFF)); // ORI r1,r1,FFFF -> FFFFFFFF
    put(bus, 0x08, iType(0x09, 0, 2, 0x0100));
    put(bus, 0x0C, iType(0x28, 2, 1, 1));      // SB
    put(bus, 0x10, iType(0x20, 2, 3, 1));      // LB
    put(bus, 0x14, iType(0x24, 2, 4, 1));      // LBU
    put(bus, 0x18, iType(0x29, 2, 1, 2));      // SH
    put(bus, 0x1C, iType(0x21, 2, 5, 2));      // LH
    put(bus, 0x20, iType(0x25, 2, 6, 2));      // LHU
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
    put(bus, 0x04, iType(0x01, 1, 0, 2));      // BLTZ
    put(bus, 0x08, iType(0x09, 0, 2, 7));
    put(bus, 0x0C, iType(0x09, 0, 3, 99));
    put(bus, 0x10, iType(0x01, 1, 1, 2));      // BGEZ not taken
    put(bus, 0x14, iType(0x09, 0, 4, 8));
    put(bus, 0x18, iType(0x09, 0, 5, 9));
    put(bus, 0x1C, 0x0000000DU);
    for (int i = 0; i < 8; ++i) require(rsp.step(bus), "execute REGIMM program");
    require(rsp.reg(2) == 7U, "BLTZ delay slot");
    require(rsp.reg(3) == 0U, "BLTZ target skips sequential instruction");
    require(rsp.reg(4) == 8U, "not-taken BGEZ delay slot");
    require(rsp.reg(5) == 9U, "not-taken BGEZ falls through after delay slot");
    require(rsp.halted(), "REGIMM program reaches BREAK");
}
} // namespace

int main() {
    testArithmeticAndMemory();
    testBranchDelaySlot();
    testByteAndHalfwordMemory();
    testRegimmBranches();
    std::cout << "All scalar RSP host tests passed.\n";
    return 0;
}
