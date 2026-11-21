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
std::uint32_t cop2Move(unsigned rs, unsigned rt, unsigned rd, unsigned element) {
    return (0x12U << 26U) | (rs << 21U) | (rt << 16U) |
        (rd << 11U) | (element << 7U);
}
void put(Bus& bus, std::uint32_t pc, std::uint32_t instruction) {
    require(bus.write32(Bus::kSpImemBase + pc, instruction), "write RSP instruction");
}
void testUnsupportedInstructionFaultIsLatched() {
    Bus bus;
    RspCpu rsp;
    put(bus, 0x00, 0xFFFFFFFFU); // Unsupported SPECIAL/COP opcode.
    require(!rsp.step(bus), "unsupported instruction reports failure");
    require(rsp.halted(), "unsupported instruction halts RSP");
    require(rsp.faulted(), "unsupported instruction records a fault");
    require(!rsp.step(bus), "faulted RSP does not retry without restart");
    require(rsp.instructionsExecuted() == 0U,
            "unsupported instruction is not counted as executed");
    rsp.setPc(0x04U);
    require(!rsp.halted() && !rsp.faulted(),
            "explicit PC change clears fault state");
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
    require(!rsp.faulted(), "normal BREAK is not an execution fault");
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
    require(!rsp.faulted(), "BREAK after a branch is not an execution fault");
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
void testVectorMultiplyFraction() {
    Bus bus;
    RspCpu rsp;
    rsp.setVectorLane(1, 0, 0x4000U);
    rsp.setVectorLane(2, 0, 0x4000U);
    rsp.setVectorLane(1, 1, 0xC000U);
    rsp.setVectorLane(2, 1, 0x4000U);
    rsp.setVectorLane(1, 2, 0x8000U);
    rsp.setVectorLane(2, 2, 0x8000U);
    put(bus, 0, vType(0, 2, 1, 3, 0x00)); // VMULF
    put(bus, 4, vType(0, 0, 0, 4, 0x1D)); // VSAR ACC_LO
    put(bus, 8, vType(1, 0, 0, 5, 0x1D)); // VSAR ACC_MD
    require(rsp.step(bus), "execute VMULF");
    require(rsp.vectorLane(3, 0) == 0x2000U, "VMULF positive fractional product");
    require(rsp.vectorLane(3, 1) == 0xE000U, "VMULF negative fractional product");
    require(rsp.vectorLane(3, 2) == 0x7FFFU, "VMULF saturates positive overflow");
    require(rsp.step(bus), "read VMULF accumulator low slice");
    require(rsp.vectorLane(4, 0) == 0x8000U,
            "VMULF stores rounded product in ACC_LO");
    require(rsp.step(bus), "read VMULF accumulator middle slice");
    require(rsp.vectorLane(5, 0) == 0x2000U,
            "VMULF stores high product bits in ACC_MD");
}

void testVectorMultiplyUnsigned() {
    Bus bus;
    RspCpu rsp;
    rsp.setVectorLane(1, 0, 0x4000U);
    rsp.setVectorLane(2, 0, 0x4000U);
    rsp.setVectorLane(1, 1, 0xC000U);
    rsp.setVectorLane(2, 1, 0x4000U);
    rsp.setVectorLane(1, 2, 0x8000U);
    rsp.setVectorLane(2, 2, 0x8000U);
    put(bus, 0, vType(0, 2, 1, 3, 0x01)); // VMULU
    require(rsp.step(bus), "execute VMULU");
    require(rsp.vectorLane(3, 0) == 0x2000U, "VMULU positive product");
    require(rsp.vectorLane(3, 1) == 0U, "VMULU clips negative product to zero");
    require(rsp.vectorLane(3, 2) == 0xFFFFU, "VMULU reports positive overflow");
}


void testVectorMultiplyAccumulateFraction() {
    Bus bus;
    RspCpu rsp;
    rsp.setVectorLane(1, 0, 0x4000U);
    rsp.setVectorLane(2, 0, 0x4000U);
    rsp.setVectorLane(1, 1, 0xC000U);
    rsp.setVectorLane(2, 1, 0x4000U);
    rsp.setVectorLane(1, 2, 0x8000U);
    rsp.setVectorLane(2, 2, 0x8000U);
    put(bus, 0, vType(0, 2, 1, 3, 0x00)); // VMULF initializes ACC.
    put(bus, 4, vType(0, 2, 1, 4, 0x08)); // VMACF accumulates without rounding.
    put(bus, 8, vType(0, 0, 0, 5, 0x1D)); // VSAR ACC_LO
    put(bus, 12, vType(1, 0, 0, 6, 0x1D)); // VSAR ACC_MD
    require(rsp.step(bus), "initialize accumulator with VMULF");
    require(rsp.step(bus), "execute VMACF");
    require(rsp.vectorLane(4, 0) == 0x4000U, "VMACF accumulates positive fraction");
    require(rsp.vectorLane(4, 1) == 0xC000U, "VMACF accumulates negative fraction");
    require(rsp.vectorLane(4, 2) == 0x7FFFU, "VMACF saturates positive overflow");
    require(rsp.step(bus), "read VMACF accumulator low slice");
    require(rsp.vectorLane(5, 0) == 0x8000U,
            "VMACF preserves accumulator low precision without rounding");
    require(rsp.step(bus), "read VMACF accumulator middle slice");
    require(rsp.vectorLane(6, 0) == 0x4000U,
            "VMACF carries accumulation into middle accumulator slice");
}

void testVectorMultiplyAccumulateUnsigned() {
    Bus bus;
    RspCpu rsp;
    rsp.setVectorLane(1, 0, 0x4000U);
    rsp.setVectorLane(2, 0, 0x4000U);
    rsp.setVectorLane(1, 1, 0xC000U);
    rsp.setVectorLane(2, 1, 0x4000U);
    rsp.setVectorLane(1, 2, 0x8000U);
    rsp.setVectorLane(2, 2, 0x8000U);
    put(bus, 0, vType(0, 2, 1, 3, 0x00)); // VMULF initializes ACC.
    put(bus, 4, vType(0, 2, 1, 4, 0x09)); // VMACU
    require(rsp.step(bus), "initialize accumulator before VMACU");
    require(rsp.step(bus), "execute VMACU");
    require(rsp.vectorLane(4, 0) == 0x4000U, "VMACU returns accumulated positive fraction");
    require(rsp.vectorLane(4, 1) == 0U, "VMACU clips accumulated negative result to zero");
    require(rsp.vectorLane(4, 2) == 0xFFFFU, "VMACU reports positive overflow as 0xFFFF");
}

void testVectorMultiplyDoublePrecision() {
    {
        Bus bus; RspCpu rsp;
        rsp.setVectorLane(1, 0, 0xFFFFU);
        rsp.setVectorLane(2, 0, 0xFFFFU);
        put(bus, 0, vType(0, 2, 1, 3, 0x04)); // VMUDL
        put(bus, 4, vType(0, 0, 0, 4, 0x1D)); // ACC_LO
        put(bus, 8, vType(1, 0, 0, 5, 0x1D)); // ACC_MD
        require(rsp.step(bus), "execute VMUDL");
        require(rsp.vectorLane(3, 0) == 0xFFFEU, "VMUDL returns upper unsigned product");
        require(rsp.step(bus) && rsp.step(bus), "read VMUDL accumulator slices");
        require(rsp.vectorLane(4, 0) == 0xFFFEU &&
                rsp.vectorLane(5, 0) == 0U, "VMUDL stores product high half in ACC_LO");
    }
    {
        Bus bus; RspCpu rsp;
        rsp.setVectorLane(1, 0, 0xFFFEU); // -2 signed
        rsp.setVectorLane(2, 0, 3U);      // unsigned
        put(bus, 0, vType(0, 2, 1, 3, 0x05)); // VMUDM
        put(bus, 4, vType(1, 0, 0, 4, 0x1D)); // ACC_MD
        put(bus, 8, vType(2, 0, 0, 5, 0x1D)); // ACC_HI
        require(rsp.step(bus), "execute VMUDM");
        require(rsp.vectorLane(3, 0) == 0xFFFFU, "VMUDM returns signed middle result");
        require(rsp.step(bus) && rsp.step(bus), "read VMUDM accumulator slices");
        require(rsp.vectorLane(4, 0) == 0xFFFFU &&
                rsp.vectorLane(5, 0) == 0xFFFFU, "VMUDM sign extends product in accumulator");
    }
    {
        Bus bus; RspCpu rsp;
        rsp.setVectorLane(1, 0, 3U);      // unsigned
        rsp.setVectorLane(2, 0, 0xFFFEU); // -2 signed
        put(bus, 0, vType(0, 2, 1, 3, 0x06)); // VMUDN
        put(bus, 4, vType(0, 0, 0, 4, 0x1D)); // ACC_LO
        require(rsp.step(bus), "execute VMUDN");
        require(rsp.vectorLane(3, 0) == 0U, "VMUDN clamps negative unsigned result");
        require(rsp.step(bus), "read VMUDN accumulator low slice");
        require(rsp.vectorLane(4, 0) == 0xFFFAU, "VMUDN stores signed product low bits");
    }
    {
        Bus bus; RspCpu rsp;
        rsp.setVectorLane(1, 0, 2U);
        rsp.setVectorLane(2, 0, 3U);
        put(bus, 0, vType(0, 2, 1, 3, 0x07)); // VMUDH
        put(bus, 4, vType(1, 0, 0, 4, 0x1D)); // ACC_MD
        put(bus, 8, vType(2, 0, 0, 5, 0x1D)); // ACC_HI
        require(rsp.step(bus), "execute VMUDH");
        require(rsp.vectorLane(3, 0) == 6U, "VMUDH returns signed product");
        require(rsp.step(bus) && rsp.step(bus), "read VMUDH accumulator slices");
        require(rsp.vectorLane(4, 0) == 6U &&
                rsp.vectorLane(5, 0) == 0U, "VMUDH places product in upper accumulator bits");
    }
}

void testVectorMultiplyDoubleAccumulate() {
    {
        Bus bus; RspCpu rsp;
        rsp.setVectorLane(1, 0, 0xFFFFU); rsp.setVectorLane(2, 0, 0xFFFFU);
        put(bus, 0, vType(0, 2, 1, 3, 0x04)); // VMUDL
        put(bus, 4, vType(0, 2, 1, 4, 0x0C)); // VMADL
        put(bus, 8, vType(1, 0, 0, 5, 0x1D)); // ACC_MD
        require(rsp.step(bus) && rsp.step(bus), "execute VMUDL then VMADL");
        require(rsp.vectorLane(4, 0) == 0xFFFFU, "VMADL unsigned-saturates accumulated result");
        require(rsp.step(bus), "read VMADL middle accumulator slice");
        require(rsp.vectorLane(5, 0) == 1U, "VMADL accumulates product into ACC_MD");
    }
    {
        Bus bus; RspCpu rsp;
        rsp.setVectorLane(1, 0, 0x4000U); rsp.setVectorLane(2, 0, 0x4000U);
        put(bus, 0, vType(0, 2, 1, 3, 0x05)); // VMUDM
        put(bus, 4, vType(0, 2, 1, 4, 0x0D)); // VMADM
        require(rsp.step(bus) && rsp.step(bus), "execute VMUDM then VMADM");
        require(rsp.vectorLane(4, 0) == 0x2000U, "VMADM accumulates signed middle product");
    }
    {
        Bus bus; RspCpu rsp;
        rsp.setVectorLane(1, 0, 2U); rsp.setVectorLane(2, 0, 3U);
        put(bus, 0, vType(0, 2, 1, 3, 0x06)); // VMUDN
        put(bus, 4, vType(0, 2, 1, 4, 0x0E)); // VMADN
        require(rsp.step(bus) && rsp.step(bus), "execute VMUDN then VMADN");
        require(rsp.vectorLane(4, 0) == 12U, "VMADN accumulates unsigned-by-signed product");
    }
    {
        Bus bus; RspCpu rsp;
        rsp.setVectorLane(1, 0, 1U); rsp.setVectorLane(2, 0, 2U);
        put(bus, 0, vType(0, 2, 1, 3, 0x07)); // VMUDH
        put(bus, 4, vType(0, 2, 1, 4, 0x0F)); // VMADH
        require(rsp.step(bus) && rsp.step(bus), "execute VMUDH then VMADH");
        require(rsp.vectorLane(4, 0) == 4U, "VMADH accumulates signed high product");
    }
}

void testVectorComparisonsAndMerge() {
    Bus bus; RspCpu rsp;
    // VCO: lane 2 has both bits set (VLT equal tie); lanes 4 and 5 have VCO_LO.
    put(bus, 0, iType(0x09, 0, 1, 0x0434));
    put(bus, 4, cop2Move(0x06, 1, 0, 0)); // CTC2 VCO
    for (unsigned lane = 0; lane < 8U; ++lane) {
        rsp.setVectorLane(1, lane, static_cast<std::uint16_t>(10U + lane));
        rsp.setVectorLane(2, lane, static_cast<std::uint16_t>(10U + lane));
    }
    rsp.setVectorLane(1, 0, 5U); rsp.setVectorLane(2, 0, 6U); // VLT true
    rsp.setVectorLane(1, 1, 9U); rsp.setVectorLane(2, 1, 8U); // VLT false
    rsp.setVectorLane(1, 2, 12U); rsp.setVectorLane(2, 2, 12U); // VLT tie + VCO both
    rsp.setVectorLane(1, 3, 13U); rsp.setVectorLane(2, 3, 13U); // VEQ true
    rsp.setVectorLane(1, 4, 14U); rsp.setVectorLane(2, 4, 14U); // VEQ suppressed by VCO_LO
    rsp.setVectorLane(1, 5, 15U); rsp.setVectorLane(2, 5, 15U); // VNE true via VCO_LO
    rsp.setVectorLane(1, 6, 17U); rsp.setVectorLane(2, 6, 16U); // VNE true
    rsp.setVectorLane(1, 7, 18U); rsp.setVectorLane(2, 7, 18U); // VGE equal, no VCO pair
    put(bus, 8, vType(0, 2, 1, 3, 0x20));  // VLT
    put(bus, 12, vType(0, 2, 1, 4, 0x21)); // VEQ
    put(bus, 16, vType(0, 2, 1, 5, 0x22)); // VNE
    put(bus, 20, vType(0, 2, 1, 6, 0x23)); // VGE
    put(bus, 24, vType(0, 2, 1, 7, 0x27)); // VMRG uses VCC from VGE
    require(rsp.step(bus) && rsp.step(bus), "load VCO comparison tie flags");
    require(rsp.vectorCarryFlags() == 0x0434U, "CTC2 prepares VCO flags");
    require(rsp.step(bus), "execute VLT");
    require(rsp.vectorLane(3, 0) == 5U && rsp.vectorLane(3, 1) == 8U &&
            rsp.vectorLane(3, 2) == 12U, "VLT compares signed lanes and handles equal tie");
    require((rsp.vectorCompareFlags() & 0x0005U) == 0x0005U,
            "VLT writes VCC_LO selection flags");
    require(rsp.vectorCarryFlags() == 0U, "VLT clears VCO");
    require(rsp.step(bus), "execute VEQ");
    require(rsp.vectorLane(4, 3) == 13U && rsp.vectorLane(4, 4) == 14U,
            "VEQ selects equal lanes except VCO_LO-set lanes");
    require((rsp.vectorCompareFlags() & (1U << 3U)) != 0U &&
            (rsp.vectorCompareFlags() & (1U << 4U)) == 0U,
            "VEQ records equality selection in VCC");
    require(rsp.step(bus), "execute VNE");
    require(rsp.vectorLane(5, 5) == 15U && rsp.vectorLane(5, 6) == 17U,
            "VNE selects unequal lanes or VCO_LO-set equal lanes");
    require(rsp.step(bus), "execute VGE");
    require(rsp.vectorLane(6, 7) == 18U, "VGE selects greater-or-equal lane");
    require(rsp.step(bus), "execute VMRG");
    require(rsp.vectorLane(7, 7) == 18U, "VMRG selects VS where VCC_LO is set");
}

void testVectorAccumulatorLowAndVsar() {
    Bus bus;
    RspCpu rsp;
    rsp.setVectorLane(1, 0, 0xFFFFU);
    rsp.setVectorLane(2, 0, 1U);
    put(bus, 0, vType(0, 2, 1, 3, 0x14)); // VADDC
    put(bus, 4, vType(0, 0, 0, 4, 0x1D)); // VSAR ACC_LO
    put(bus, 8, vType(1, 0, 0, 5, 0x1D)); // VSAR ACC_MD
    put(bus, 12, vType(2, 0, 0, 6, 0x1D)); // VSAR ACC_HI
    require(rsp.step(bus), "execute VADDC before accumulator read");
    require(rsp.step(bus), "read ACC_LO through VSAR");
    require(rsp.vectorLane(4, 0) == 0U,
            "VSAR ACC_LO reads low 16-bit sum, not carry bit");
    require(rsp.step(bus), "read ACC_MD through VSAR");
    require(rsp.vectorLane(5, 0) == 0U, "ACC_MD starts independently at zero");
    require(rsp.step(bus), "read ACC_HI through VSAR");
    require(rsp.vectorLane(6, 0) == 0U, "ACC_HI starts independently at zero");
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
void testCop2VectorAndControlMoves() {
    Bus bus;
    RspCpu rsp;
    rsp.setVectorLane(2, 0, 0x5678U);
    rsp.setVectorLane(2, 1, 0x9ABCU);
    const std::uint32_t program[] = {
        iType(0x09, 0, 1, 0x1234),
        cop2Move(0x04, 1, 2, 1),   // MTC2 at odd byte offset
        cop2Move(0x00, 3, 2, 1),   // MFC2 reads across lane boundary
        cop2Move(0x04, 1, 2, 15),  // MTC2 byte 15 does not wrap
        cop2Move(0x00, 5, 2, 15),  // MFC2 byte 15 wraps its second byte
        cop2Move(0x06, 1, 0, 0),   // CTC2 VCO
        cop2Move(0x02, 4, 0, 0),   // CFC2 VCO
        cop2Move(0x06, 1, 1, 0),   // CTC2 VCC
        cop2Move(0x02, 6, 1, 0),   // CFC2 VCC
        cop2Move(0x06, 1, 2, 0),   // CTC2 VCE
        cop2Move(0x02, 7, 2, 0),   // CFC2 VCE
        0x0000000DU
    };
    for (std::uint32_t i = 0; i < sizeof(program) / sizeof(program[0]); ++i)
        put(bus, i * 4U, program[i]);
    for (unsigned i = 0; i < sizeof(program) / sizeof(program[0]); ++i)
        require(rsp.step(bus), "execute COP2 move program");

    require(rsp.vectorLane(2, 0) == 0x5612U, "MTC2 writes odd byte offset");
    require(rsp.vectorLane(2, 1) == 0x34BCU, "MTC2 crosses into next vector lane");
    require(rsp.vectorLane(2, 7) == 0x0012U, "MTC2 byte 15 writes final byte only");
    require(rsp.reg(3) == 0x1234U, "MFC2 reads unaligned 16-bit byte window");
    require(rsp.reg(5) == 0x1256U, "MFC2 wraps byte 15 to byte 0");
    require(rsp.vectorCarryFlags() == 0x1234U, "CTC2 writes VCO");
    require(rsp.vectorCompareFlags() == 0x1234U, "CTC2 writes VCC");
    require(rsp.vectorClipFlags() == 0x34U, "CTC2 writes VCE");
    require(rsp.reg(4) == 0x1234U && rsp.reg(6) == 0x1234U,
            "CFC2 reads 16-bit control registers");
    require(rsp.reg(7) == 0x34U, "CFC2 reads 8-bit VCE");
}
} // namespace

int main() {
    testUnsupportedInstructionFaultIsLatched();
    testArithmeticAndMemory();
    testBranchDelaySlot();
    testByteAndHalfwordMemory();
    testRegimmBranches();
    testVectorArithmetic();
    testVectorCarryAndLogic();
    testVectorAccumulatorLowAndVsar();
    testVectorMultiplyFraction();
    testVectorMultiplyUnsigned();
    testVectorMultiplyAccumulateFraction();
    testVectorMultiplyAccumulateUnsigned();
    testVectorMultiplyDoublePrecision();
    testVectorMultiplyDoubleAccumulate();
    testVectorComparisonsAndMerge();
    testVectorPairBroadcastSelectors();
    testVectorElementBroadcast();
    testVectorInstructionInBranchDelaySlot();
    testCop2VectorAndControlMoves();
    std::cout << "All scalar and vector RSP host tests passed.\n";
    return 0;
}
