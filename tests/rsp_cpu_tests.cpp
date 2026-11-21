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
    put(bus, 0x00, iType(0x0F, 0, 1, 0x1234)); // LUI r1,1234
    put(bus, 0x04, iType(0x0D, 1, 1, 0x5678)); // ORI r1,r1,5678
    put(bus, 0x08, iType(0x09, 0, 2, 0x0040)); // ADDIU r2,zero,40
    put(bus, 0x0C, iType(0x2B, 2, 1, 0));     // SW r1,0(r2)
    put(bus, 0x10, iType(0x23, 2, 3, 0));     // LW r3,0(r2)
    put(bus, 0x14, 0x0000000DU);              // BREAK
    for (int i = 0; i < 6; ++i) require(rsp.step(bus), "execute arithmetic program");
    require(rsp.reg(1) == 0x12345678U, "LUI/ORI result");
    require(rsp.reg(3) == 0x12345678U, "LW result");
    require(rsp.halted(), "BREAK halts RSP");
    require(rsp.reg(0) == 0U, "zero register remains zero");
}
void testBranchDelaySlot() {
    Bus bus;
    RspCpu rsp;
    put(bus, 0x00, iType(0x09, 0, 1, 1));     // ADDIU r1,zero,1
    put(bus, 0x04, iType(0x04, 1, 1, 2));     // BEQ r1,r1,+2 -> 0x10
    put(bus, 0x08, iType(0x09, 0, 2, 7));     // delay slot
    put(bus, 0x0C, iType(0x09, 0, 3, 99));    // skipped
    put(bus, 0x10, 0x0000000DU);              // BREAK
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
} // namespace

int main() {
    testArithmeticAndMemory();
    testBranchDelaySlot();
    std::cout << "All scalar RSP host tests passed.\n";
    return 0;
}
