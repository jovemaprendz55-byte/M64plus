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
std::uint32_t vectorMemoryType(unsigned op, unsigned base, unsigned vt,
                                  unsigned subOpcode, unsigned element, unsigned offset) {
    return (op << 26U) | (base << 21U) | (vt << 16U) |
        (subOpcode << 11U) | (element << 7U) | (offset & 0x7FU);
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

void testCop0SpRegisterMoves() {
    Bus bus;
    RspCpu rsp;
    const auto cop0 = [](unsigned move, unsigned rt, unsigned rd) {
        return (0x10U << 26U) | (move << 21U) | (rt << 16U) | (rd << 11U);
    };
    put(bus, 0x00, iType(0x09, 0, 1, 0x0001));
    put(bus, 0x04, cop0(4, 1, 0)); // MTC0 SP_MEM_ADDR
    put(bus, 0x08, cop0(0, 2, 0)); // MFC0 SP_MEM_ADDR
    put(bus, 0x0C, iType(0x09, 0, 3, 0x0001));
    put(bus, 0x10, cop0(4, 3, 4)); // MTC0 SP_STATUS: clear HALT
    put(bus, 0x14, cop0(0, 4, 4)); // MFC0 SP_STATUS
    put(bus, 0x18, cop0(0, 5, 7)); // MFC0 SP_SEMAPHORE (read sets it)
    put(bus, 0x1C, cop0(0, 6, 7)); // second read observes set semaphore
    for (unsigned i = 0; i < 8U; ++i)
        require(rsp.step(bus), "execute COP0 SP register transfer");
    require(rsp.reg(2) == 1U, "MFC0 reads SP_MEM_ADDR");
    require(rsp.reg(4) == 0U, "MTC0 SP_STATUS clears HALT and MFC0 reads it");
    require(rsp.reg(5) == 0U && rsp.reg(6) == 1U,
            "MFC0 SP_SEMAPHORE reads then sets the semaphore");
    require(!rsp.faulted(), "supported SP COP0 moves do not fault");
}


void testCop0DpRegisterMoves() {
    Bus bus;
    RspCpu rsp;
    const auto cop0 = [](unsigned move, unsigned rt, unsigned rd) {
        return (0x10U << 26U) | (move << 21U) | (rt << 16U) | (rd << 11U);
    };
    const std::uint32_t program[] = {
        iType(0x09, 0, 1, 0x1000),
        cop0(4, 1, 8),             // MTC0 DPC_START
        iType(0x09, 0, 2, 0x1100),
        cop0(4, 2, 9),             // MTC0 DPC_END
        cop0(0, 3, 8),             // MFC0 DPC_START
        cop0(0, 4, 9),             // MFC0 DPC_END
        iType(0x09, 0, 5, 0x0002),
        cop0(4, 5, 11),            // MTC0 DPC_STATUS: set XBUS_DMEM_DMA
        cop0(0, 6, 11),            // MFC0 DPC_STATUS
        cop0(0, 7, 10)             // MFC0 DPC_CURRENT
    };
    for (unsigned i = 0; i < sizeof(program) / sizeof(program[0]); ++i)
        put(bus, i * 4U, program[i]);
    for (unsigned i = 0; i < sizeof(program) / sizeof(program[0]); ++i)
        require(rsp.step(bus), "execute COP0 DPC register transfer");
    require(rsp.reg(3) == 0x1000U, "RSP MFC0 reads DPC_START");
    require(rsp.reg(4) == 0x1100U, "RSP MFC0 reads DPC_END");
    require(rsp.reg(6) == Bus::kDpXbusDmemDma,
            "RSP MFC0 reads DPC_STATUS after MTC0 command");
    require(rsp.reg(7) == 0x1000U,
            "RSP MFC0 reads DPC_CURRENT initialized from DPC_START");
    require(!rsp.faulted(), "DPC COP0 moves are supported");
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
void testScalarAddAndSetLessThan() {
    Bus bus;
    RspCpu rsp;
    put(bus, 0x00, iType(0x0F, 0, 1, 0xFFFF));
    put(bus, 0x04, iType(0x0D, 1, 1, 0xFFFF)); // -1
    put(bus, 0x08, iType(0x08, 1, 2, 1)); // ADDI -> 0
    put(bus, 0x0C, iType(0x0A, 1, 3, 1)); // SLTI -1 < 1
    put(bus, 0x10, iType(0x0B, 1, 4, 1)); // SLTIU -1 < 1 unsigned false
    put(bus, 0x14, (1U << 21U) | (2U << 16U) | (5U << 11U) | 0x20U); // ADD
    put(bus, 0x18, (1U << 21U) | (2U << 16U) | (6U << 11U) | 0x22U); // SUB
    put(bus, 0x1C, (1U << 21U) | (2U << 16U) | (7U << 11U) | 0x2AU); // SLT
    put(bus, 0x20, (1U << 21U) | (2U << 16U) | (8U << 11U) | 0x2BU); // SLTU
    for (unsigned i = 0; i < 9U; ++i)
        require(rsp.step(bus), "execute scalar arithmetic and comparisons");
    require(rsp.reg(2) == 0U, "ADDI wraps at 32 bits");
    require(rsp.reg(3) == 1U, "SLTI performs signed comparison");
    require(rsp.reg(4) == 0U, "SLTIU performs unsigned comparison");
    require(rsp.reg(5) == 0xFFFFFFFFU, "ADD performs 32-bit addition");
    require(rsp.reg(6) == 0xFFFFFFFFU, "SUB performs 32-bit subtraction");
    require(rsp.reg(7) == 1U, "SLT performs signed comparison");
    require(rsp.reg(8) == 0U, "SLTU performs unsigned comparison");
    require(!rsp.faulted(), "scalar arithmetic and comparisons are supported");
}

void testScalarVariableShiftsAndJalr() {
    Bus bus;
    RspCpu rsp;
    put(bus, 0x00, iType(0x09, 0, 1, 4)); // shift amount
    put(bus, 0x04, iType(0x0F, 0, 2, 0xFFFF));
    put(bus, 0x08, iType(0x0D, 2, 2, 0xFFF0)); // -16
    put(bus, 0x0C, (1U << 21U) | (2U << 16U) | (3U << 11U) |
                   (4U << 6U) | 0x07U); // SRAV
    put(bus, 0x10, (1U << 21U) | (2U << 16U) | (4U << 11U) |
                   (4U << 6U) | 0x04U); // SLLV
    put(bus, 0x14, (1U << 21U) | (2U << 16U) | (5U << 11U) |
                   (4U << 6U) | 0x06U); // SRLV
    put(bus, 0x18, (2U << 16U) | (6U << 11U) | (4U << 6U) | 0x03U); // SRA
    put(bus, 0x1C, iType(0x09, 0, 7, 0x002C)); // JALR target
    put(bus, 0x20, (7U << 21U) | (31U << 11U) | 0x09U); // JALR ra, t7
    put(bus, 0x24, iType(0x09, 0, 8, 0x0055)); // delay slot
    put(bus, 0x28, iType(0x09, 0, 9, 0x0066)); // skipped
    put(bus, 0x2C, 0x0000000DU); // target BREAK
    for (unsigned i = 0; i < 10U; ++i)
        require(rsp.step(bus), "execute scalar shifts, JALR and delay slot");
    require(rsp.reg(3) == 0xFFFFFFFFU, "SRAV sign extends negative values");
    require(rsp.reg(4) == 0xFFFFFF00U, "SLLV uses low five bits of shift register");
    require(rsp.reg(5) == 0x0FFFFFFFU, "SRLV shifts logically");
    require(rsp.reg(6) == 0xFFFFFFFFU, "SRA shifts arithmetically");
    require(rsp.reg(31) == 0x00000028U, "JALR writes return address to rd");
    require(rsp.reg(8) == 0x55U && rsp.reg(9) == 0U,
            "JALR executes delay slot and skips sequential instruction");
    require(rsp.pc() == 0x02CU, "JALR transfers control to target after delay slot");
    require(rsp.step(bus) && rsp.halted(), "JALR target executes BREAK");
    require(!rsp.faulted(), "scalar shifts and JALR are supported");
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
void testRegimmLinkBranches() {
    {
        Bus bus;
        RspCpu rsp;
        put(bus, 0x00, iType(0x0F, 0, 1, 0xFFFF)); // r1 = -1
        put(bus, 0x04, iType(0x01, 1, 16, 2)); // BLTZAL taken -> 0x10
        put(bus, 0x08, iType(0x09, 0, 2, 7)); // delay slot
        put(bus, 0x0C, iType(0x09, 0, 3, 99)); // skipped
        put(bus, 0x10, 0x0000000DU); // BREAK
        for (unsigned i = 0; i < 4U; ++i)
            require(rsp.step(bus), "execute taken BLTZAL");
        require(rsp.reg(31) == 0x0CU, "taken BLTZAL links to PC + 8");
        require(rsp.reg(2) == 7U && rsp.reg(3) == 0U,
                "taken BLTZAL executes delay slot and skips fallthrough");
        require(rsp.halted() && !rsp.faulted(), "taken BLTZAL reaches BREAK");
    }
    {
        Bus bus;
        RspCpu rsp;
        put(bus, 0x00, iType(0x0F, 0, 1, 0xFFFF)); // r1 = -1
        put(bus, 0x04, iType(0x01, 1, 17, 2)); // BGEZAL not taken
        put(bus, 0x08, iType(0x09, 0, 2, 7)); // delay slot
        put(bus, 0x0C, iType(0x09, 0, 3, 9)); // fallthrough
        put(bus, 0x10, 0x0000000DU); // BREAK
        for (unsigned i = 0; i < 5U; ++i)
            require(rsp.step(bus), "execute not-taken BGEZAL");
        require(rsp.reg(31) == 0x0CU, "not-taken BGEZAL still links to PC + 8");
        require(rsp.reg(2) == 7U && rsp.reg(3) == 9U,
                "not-taken BGEZAL executes delay slot and falls through");
        require(rsp.halted() && !rsp.faulted(), "not-taken BGEZAL reaches BREAK");
    }
}


void testVectorQuantizedMultiplyAndAccumulatorRounding() {
    {
        Bus bus;
        RspCpu rsp;
        rsp.setVectorLane(1, 0, 0x0100U);
        rsp.setVectorLane(2, 0, 0x0001U);
        rsp.setVectorLane(4, 0, 0x0001U);
        put(bus, 0, vType(0, 2, 1, 3, 0x03));  // VMULQ
        put(bus, 4, vType(1, 0, 0, 4, 0x1D));  // VSAR ACC_MD
        put(bus, 8, vType(0, 2, 1, 5, 0x02));  // VRNDP, vt << 16
        put(bus, 12, vType(1, 0, 0, 6, 0x1D)); // VSAR ACC_MD
        put(bus, 16, vType(0, 0, 0, 7, 0x0B)); // VMACQ
        put(bus, 20, vType(1, 0, 0, 8, 0x1D)); // VSAR ACC_MD
        require(rsp.step(bus), "execute VMULQ");
        require(rsp.vectorLane(3, 0) == 0x0080U,
                "VMULQ returns half product with low four bits cleared");
        require(rsp.step(bus), "read VMULQ accumulator middle");
        require(rsp.vectorLane(4, 0) == 0x0100U,
                "VMULQ places product in accumulator middle slice");
        require(rsp.step(bus), "execute VRNDP");
        require(rsp.step(bus), "read VRNDP accumulator middle");
        require(rsp.vectorLane(6, 0) == 0x0101U,
                "VRNDP conditionally adds shifted vt to a non-negative accumulator");
        require(rsp.step(bus), "execute VMACQ");
        require(rsp.vectorLane(7, 0) == 0x0070U,
                "VMACQ rounds positive accumulator and clears low four result bits");
        require(rsp.step(bus), "read VMACQ accumulator middle");
        require(rsp.vectorLane(8, 0) == 0x00E1U,
                "VMACQ preserves rounded accumulator state");
    }
    {
        Bus bus;
        RspCpu rsp;
        rsp.setVectorLane(1, 0, 0xFF00U); // -256
        rsp.setVectorLane(2, 0, 0x0001U);
        rsp.setVectorLane(4, 0, 0x0001U);
        put(bus, 0, vType(0, 2, 1, 3, 0x03)); // VMULQ
        put(bus, 4, vType(0, 4, 1, 5, 0x0A)); // VRNDN, vt << 16
        require(rsp.step(bus), "execute negative VMULQ");
        require(rsp.vectorLane(3, 0) == 0xFF80U,
                "VMULQ rounds negative product toward zero before masking");
        require(rsp.step(bus), "execute VRNDN");
        require(rsp.vectorLane(5, 0) == 0xFF20U,
                "VRNDN conditionally adds shifted vt to a negative accumulator");
    }
}


void testReservedVectorOpcodesFault() {
    const unsigned opcodes[] = {
        0x12U, 0x16U, 0x17U, 0x18U, 0x19U, 0x1AU, 0x1BU, 0x1CU,
        0x1EU, 0x1FU, 0x2EU, 0x2FU,
        0x38U, 0x39U, 0x3AU, 0x3BU, 0x3CU, 0x3DU, 0x3EU, 0x3FU
    };
    for (unsigned opcode : opcodes) {
        Bus bus;
        RspCpu rsp;
        put(bus, 0, vType(0, 2, 1, 5, opcode));
        require(!rsp.step(bus), "reserved vector opcode reports failure");
        require(rsp.halted() && rsp.faulted(),
                "reserved vector opcode latches an execution fault");
        require(rsp.instructionsExecuted() == 0U,
                "reserved vector opcode is not counted as executed");
    }
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
        require(rsp.vectorLane(3, 0) == 0xFFFAU, "VMUDN returns ACC_LO for sign-extended negative product");
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

void testVectorMultiplyUnsignedLowSignedResults() {
    {
        Bus bus;
        RspCpu rsp;
        rsp.setVectorLane(1, 0, 2U);       // unsigned VS
        rsp.setVectorLane(2, 0, 0xFFFFU);  // signed VT = -1
        put(bus, 0, vType(0, 2, 1, 3, 0x06)); // VMUDN
        require(rsp.step(bus), "execute negative VMUDN product");
        require(rsp.vectorLane(3, 0) == 0xFFFEU,
                "VMUDN preserves low result for sign-extended negative accumulator");
    }
    {
        Bus bus;
        RspCpu rsp;
        rsp.setVectorLane(1, 0, 2U);
        rsp.setVectorLane(2, 0, 0xFFFFU);
        put(bus, 0, vType(0, 2, 1, 3, 0x06)); // VMUDN initializes ACC to -2.
        put(bus, 4, vType(0, 2, 1, 4, 0x0E)); // VMADN adds another -2.
        require(rsp.step(bus) && rsp.step(bus), "execute negative VMUDN/VMADN pair");
        require(rsp.vectorLane(4, 0) == 0xFFFCU,
                "VMADN preserves low result for sign-extended negative accumulator");
    }
}

void testVectorUnsignedMultiplyAccumulateSaturationBoundaries() {
    {
        Bus bus;
        RspCpu rsp;
        rsp.setVectorLane(1, 0, 0x7FFFU);
        rsp.setVectorLane(2, 0, 0x7FFFU);
        put(bus, 0, vType(0, 2, 1, 3, 0x07)); // VMUDH: positive value above signed 32-bit range after scaling.
        put(bus, 4, vType(0, 0, 0, 4, 0x0C)); // VMADL adds zero and clamps the 48-bit accumulator.
        put(bus, 8, vType(0, 0, 0, 5, 0x0E)); // VMADN uses the same unsigned clamp path.
        require(rsp.step(bus), "prepare positive overflowing accumulator with VMUDH");
        require(rsp.step(bus), "execute VMADL on positive overflowing accumulator");
        require(rsp.vectorLane(4, 0) == 0xFFFFU,
                "VMADL saturates positive accumulator overflow to 0xFFFF");
        require(rsp.step(bus), "execute VMADN on positive overflowing accumulator");
        require(rsp.vectorLane(5, 0) == 0xFFFFU,
                "VMADN saturates positive accumulator overflow to 0xFFFF");
    }
    {
        Bus bus;
        RspCpu rsp;
        rsp.setVectorLane(1, 0, 0x8000U); // -32768
        rsp.setVectorLane(2, 0, 0x7FFFU); // +32767
        put(bus, 0, vType(0, 2, 1, 3, 0x07)); // VMUDH: negative value below signed 32-bit range after scaling.
        put(bus, 4, vType(0, 0, 0, 4, 0x0C)); // VMADL adds zero and clamps the 48-bit accumulator.
        put(bus, 8, vType(0, 0, 0, 5, 0x0E)); // VMADN uses the same unsigned clamp path.
        require(rsp.step(bus), "prepare negative overflowing accumulator with VMUDH");
        require(rsp.step(bus), "execute VMADL on negative overflowing accumulator");
        require(rsp.vectorLane(4, 0) == 0U,
                "VMADL saturates negative accumulator overflow to zero");
        require(rsp.step(bus), "execute VMADN on negative overflowing accumulator");
        require(rsp.vectorLane(5, 0) == 0U,
                "VMADN saturates negative accumulator overflow to zero");
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
        require(rsp.vectorLane(4, 0) == 0xFFFCU, "VMADL returns ACC_LO when unsigned accumulator does not overflow");
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
    // VCO: lane 2 has both bits set (VLT equal tie); lanes 4 and 5 have VCO_HI.
    put(bus, 0, iType(0x09, 0, 1, 0x3404));
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
    rsp.setVectorLane(1, 5, 15U); rsp.setVectorLane(2, 5, 15U); // VNE true via VCO_HI
    rsp.setVectorLane(1, 6, 17U); rsp.setVectorLane(2, 6, 16U); // VNE true
    rsp.setVectorLane(1, 7, 18U); rsp.setVectorLane(2, 7, 18U); // VGE equal, no VCO pair
    put(bus, 8, vType(0, 2, 1, 3, 0x20));  // VLT
    put(bus, 12, cop2Move(0x06, 1, 0, 0)); // restore VCO for VEQ tie
    put(bus, 16, vType(0, 2, 1, 4, 0x21)); // VEQ
    put(bus, 20, cop2Move(0x06, 1, 0, 0)); // restore VCO for VNE tie
    put(bus, 24, vType(0, 2, 1, 5, 0x22)); // VNE
    put(bus, 28, vType(0, 2, 1, 6, 0x23)); // VGE
    put(bus, 32, vType(0, 2, 1, 7, 0x27)); // VMRG uses VCC from VGE
    require(rsp.step(bus) && rsp.step(bus), "load VCO comparison tie flags");
    require(rsp.vectorCarryFlags() == 0x3404U, "CTC2 prepares VCO flags");
    require(rsp.step(bus), "execute VLT");
    require(rsp.vectorLane(3, 0) == 5U && rsp.vectorLane(3, 1) == 8U &&
            rsp.vectorLane(3, 2) == 12U, "VLT compares signed lanes and handles equal tie");
    require((rsp.vectorCompareFlags() & 0x0005U) == 0x0005U,
            "VLT writes VCC_LO selection flags");
    require(rsp.vectorCarryFlags() == 0U, "VLT clears VCO");
    require(rsp.step(bus), "restore VCO before VEQ");
    require(rsp.step(bus), "execute VEQ");
    require(rsp.vectorLane(4, 3) == 13U && rsp.vectorLane(4, 4) == 14U,
            "VEQ selects equal lanes except VCO_LO-set lanes");
    require((rsp.vectorCompareFlags() & (1U << 3U)) != 0U &&
            (rsp.vectorCompareFlags() & (1U << 4U)) == 0U,
            "VEQ records equality selection in VCC");
    require(rsp.step(bus), "restore VCO before VNE");
    require(rsp.step(bus), "execute VNE");
    require(rsp.vectorLane(5, 5) == 15U && rsp.vectorLane(5, 6) == 17U,
            "VNE selects unequal lanes or VCO_LO-set equal lanes");
    require(rsp.step(bus), "execute VGE");
    require(rsp.vectorLane(6, 7) == 18U, "VGE selects greater-or-equal lane");
    require(rsp.step(bus), "execute VMRG");
    require(rsp.vectorLane(7, 7) == 18U, "VMRG selects VS where VCC_LO is set");
}

void testVectorClipHigh() {
    Bus bus; RspCpu rsp;
    rsp.setVectorLane(1, 0, 0xFFFDU); // -3
    rsp.setVectorLane(2, 0, 5U);
    rsp.setVectorLane(1, 1, 0xFFF9U); // -7
    rsp.setVectorLane(2, 1, 5U);
    rsp.setVectorLane(1, 2, 4U);
    rsp.setVectorLane(2, 2, 5U);
    rsp.setVectorLane(1, 3, 6U);
    rsp.setVectorLane(2, 3, 5U);
    rsp.setVectorLane(1, 4, 0xFFFAU); // -6 == -5 - 1, VCE edge
    rsp.setVectorLane(2, 4, 5U);
    put(bus, 0, vType(0, 2, 1, 3, 0x25)); // VCH
    require(rsp.step(bus), "execute VCH");
    require(rsp.vectorLane(3, 0) == 0xFFFDU,
            "VCH leaves opposite-sign lane unchanged when above negative bound");
    require(rsp.vectorLane(3, 1) == 0xFFFBU,
            "VCH clamps opposite-sign lane to negative bound");
    require(rsp.vectorLane(3, 2) == 4U,
            "VCH leaves same-sign lane below positive bound unchanged");
    require(rsp.vectorLane(3, 3) == 5U,
            "VCH clamps same-sign lane to positive bound");
    require(rsp.vectorLane(3, 4) == 0xFFFBU,
            "VCH handles negative-bound extension edge");
    require((rsp.vectorCarryFlags() & 0x0013U) == 0x0013U,
            "VCH sets VCO_LO for opposite-sign lanes");
    require((rsp.vectorCarryFlags() & (1U << 12U)) == 0U,
            "VCH clears VCO_HI on VCE edge");
    require((rsp.vectorClipFlags() & (1U << 4U)) != 0U,
            "VCH records VCE extension bit");
    require((rsp.vectorCompareFlags() & (1U << 11U)) != 0U,
            "VCH records VCC_HI for positive clamp");
}

void testVectorClipReverse() {
    Bus bus; RspCpu rsp;
    rsp.setVectorLane(1, 0, 0xFFFDU); // -2 in one's-complement representation
    rsp.setVectorLane(2, 0, 5U);
    rsp.setVectorLane(1, 1, 0xFFF9U); // -6 in one's-complement representation
    rsp.setVectorLane(2, 1, 5U);
    rsp.setVectorLane(1, 2, 6U);
    rsp.setVectorLane(2, 2, 5U);
    put(bus, 0, vType(0, 2, 1, 3, 0x26)); // VCR
    require(rsp.step(bus), "execute VCR");
    require(rsp.vectorLane(3, 0) == 0xFFFDU,
            "VCR leaves value above one's-complement negative bound unchanged");
    require(rsp.vectorLane(3, 1) == 0xFFFAU,
            "VCR clamps to one's-complement negative bound");
    require(rsp.vectorLane(3, 2) == 5U,
            "VCR clamps same-sign value to positive bound");
    require(rsp.vectorClipFlags() == 0U, "VCR clears VCE");
}

void testVectorClipLow() {
    Bus bus; RspCpu rsp;
    // VCH prepares lane 2/3 as same-sign equal upper halves and lane 4 as VCE edge.
    rsp.setVectorLane(1, 2, 5U); rsp.setVectorLane(2, 2, 5U);
    rsp.setVectorLane(1, 3, 6U); rsp.setVectorLane(2, 3, 6U);
    rsp.setVectorLane(1, 4, 0xFFFAU); rsp.setVectorLane(2, 4, 5U);
    put(bus, 0, vType(0, 2, 1, 3, 0x25)); // VCH
    put(bus, 4, vType(0, 2, 1, 4, 0x24)); // VCL
    require(rsp.step(bus), "execute VCH before VCL");
    require((rsp.vectorCarryFlags() & (1U << 4U)) != 0U &&
            (rsp.vectorCarryFlags() & (1U << 12U)) == 0U,
            "VCH prepares VCL VCE case");
    // Replace upper halves with lower halves for the second stage of the compare.
    rsp.setVectorLane(1, 2, 2U); rsp.setVectorLane(2, 2, 3U);
    rsp.setVectorLane(1, 3, 4U); rsp.setVectorLane(2, 3, 3U);
    rsp.setVectorLane(1, 4, 0xFFFEU); rsp.setVectorLane(2, 4, 1U);
    require(rsp.step(bus), "execute VCL using VCH flags");
    require(rsp.vectorLane(4, 2) == 2U,
            "VCL keeps VS when same-sign lower half is below VT");
    require(rsp.vectorLane(4, 3) == 3U,
            "VCL selects VT when same-sign lower half is above VT");
    require(rsp.vectorLane(4, 4) == 0xFFFFU,
            "VCL uses VCE to resolve opposite-sign lower-half boundary");
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
void testVectorMoveAndNoop() {
    Bus bus;
    RspCpu rsp;
    for (unsigned lane = 0; lane < 8U; ++lane)
        rsp.setVectorLane(2, lane, static_cast<std::uint16_t>(0x1100U + lane));
    // VMOV uses the element broadcast map and VS as the destination lane.
    put(bus, 0, vType(10, 2, 5, 3, 0x33)); // element 10 selects VT lane 2 -> VD3 lane 5
    put(bus, 4, vType(0, 2, 0, 4, 0x37));  // VNOP
    put(bus, 8, vType(0, 0, 0, 0, 0x1D));  // VSAR ACC_LO
    require(rsp.step(bus), "execute VMOV");
    require(rsp.vectorLane(3, 5) == 0x1102U, "VMOV copies selected scalar lane");
    require(rsp.vectorLane(3, 4) == 0U, "VMOV leaves other destination lanes unchanged");
    require(rsp.step(bus), "execute VNOP");
    require(rsp.step(bus), "read ACC_LO after VMOV");
    for (unsigned lane = 0; lane < 8U; ++lane)
        require(rsp.vectorLane(0, lane) == static_cast<std::uint16_t>(0x1102U),
                "VMOV loads ACC_LO using the element broadcast map");
    require(!rsp.faulted(), "VNOP is a supported instruction");
}

void testFourthPackedVectorLoad() {
    Bus bus;
    RspCpu rsp;
    for (unsigned i = 0; i < 16U; ++i)
        require(bus.write8(Bus::kSpDmemBase + 0x610U + i,
                           static_cast<std::uint8_t>(0x20U + i)),
                "initialize LFV 16-byte source quadword");
    for (unsigned lane = 0; lane < 8U; ++lane) {
        rsp.setVectorLane(2, lane, 0xFFFFU);
        rsp.setVectorLane(3, lane, 0xEEEEU);
    }
    put(bus, 0, iType(0x09, 0, 1, 0x0606)); // unaligned within an 8-byte window
    put(bus, 4, vectorMemoryType(0x32, 1, 2, 9, 0, 1)); // LFV lower half
    put(bus, 8, vectorMemoryType(0x32, 1, 3, 9, 8, 1)); // LFV upper half
    require(rsp.step(bus), "set LFV base");
    require(rsp.step(bus), "execute LFV lower half");
    require(rsp.step(bus), "execute LFV upper half");
    constexpr unsigned lowerPattern[4] = {0U, 4U, 8U, 12U};
    constexpr unsigned upperPattern[4] = {8U, 12U, 0U, 4U};
    for (unsigned i = 0; i < 4U; ++i) {
        const std::uint16_t lower = static_cast<std::uint16_t>(
            (0x20U + ((6U + lowerPattern[i]) & 0x0FU)) << 7U);
        const std::uint16_t upper = static_cast<std::uint16_t>(
            (0x20U + ((6U + upperPattern[i]) & 0x0FU)) << 7U);
        require(rsp.vectorLane(2, i) == lower,
                "LFV lower half follows the 0,4,8,12 byte pattern");
        require(rsp.vectorLane(3, i + 4U) == upper,
                "LFV upper half follows the 8,12,0,4 byte pattern");
    }
    require(rsp.vectorLane(2, 4) == 0xFFFFU &&
            rsp.vectorLane(3, 3) == 0xEEEEU,
            "LFV preserves lanes outside its selected half");
    require(!rsp.faulted(), "LFV is a supported RSP memory instruction");
}

void testFourthPackedVectorStore() {
    Bus bus;
    RspCpu rsp;
    for (unsigned lane = 0; lane < 8U; ++lane)
        rsp.setVectorLane(4, lane, static_cast<std::uint16_t>(
            ((0x20U + lane) << 7U) | 0x003FU));
    put(bus, 0, iType(0x09, 0, 1, 0x0706)); // deliberately unaligned DMEM address
    put(bus, 4, vectorMemoryType(0x3A, 1, 4, 9, 5, 0)); // SFV e5
    require(rsp.step(bus), "set SFV base");
    require(rsp.step(bus), "execute SFV");
    constexpr unsigned expectedLane[4] = {7U, 4U, 5U, 6U};
    constexpr unsigned expectedAddress[4] = {0x706U, 0x70AU, 0x70EU, 0x702U};
    for (unsigned i = 0; i < 4U; ++i) {
        std::uint8_t value = 0;
        require(bus.read8(Bus::kSpDmemBase + expectedAddress[i], value) &&
                value == static_cast<std::uint8_t>(0x20U + expectedLane[i]),
                "SFV selects the documented lane pattern and wraps the 16-byte window");
    }

    put(bus, 8, vectorMemoryType(0x3A, 1, 4, 9, 2, 0));
    rsp.setPc(8U);
    require(rsp.step(bus), "execute SFV zero-table element");
    for (unsigned i = 0; i < 4U; ++i) {
        std::uint8_t value = 0xFFU;
        const unsigned address = (0x706U & ~0x0FU) |
            ((0x706U + i * 4U) & 0x0FU);
        require(bus.read8(Bus::kSpDmemBase + address, value) && value == 0U,
                "SFV writes zero for temporary-table X entries");
    }

    put(bus, 12, iType(0x09, 0, 2, 0x070E)); // bit 3 set within the 16-byte window
    put(bus, 16, vectorMemoryType(0x3A, 2, 4, 9, 5, 0));
    rsp.setPc(12U);
    require(rsp.step(bus) && rsp.step(bus), "execute SFV with address bit 3 set");
    constexpr unsigned wrappedAddresses[4] = {0x70EU, 0x712U, 0x716U, 0x70AU};
    for (unsigned i = 0; i < 4U; ++i) {
        std::uint8_t value = 0;
        require(bus.read8(Bus::kSpDmemBase + wrappedAddresses[i], value) &&
                value == static_cast<std::uint8_t>(0x20U + expectedLane[i]),
                "SFV wraps from an 8-byte-aligned window when address bit 3 is set");
    }
    require(!rsp.faulted(), "SFV is a supported RSP memory instruction");
}

void testHalfPackedVectorTransfers() {
    Bus bus;
    RspCpu rsp;
    for (unsigned byte = 0; byte < 16U; ++byte)
        require(bus.write8(Bus::kSpDmemBase + 0x510U + byte,
                           static_cast<std::uint8_t>(0x40U + byte)),
                "initialize half-packed vector load window");
    for (unsigned lane = 0; lane < 8U; ++lane)
        rsp.setVectorLane(6, lane, static_cast<std::uint16_t>(
            ((0x50U + lane) << 7U) | 0x003FU));
    put(bus, 0, iType(0x09, 0, 1, 0x0506)); // unaligned within an 8-byte window
    put(bus, 4, vectorMemoryType(0x32, 1, 2, 8, 0, 1)); // LHV offset * 16
    put(bus, 8, vectorMemoryType(0x3A, 1, 6, 8, 0, 2)); // SHV offset * 16
    require(rsp.step(bus), "set half-packed vector base");
    require(rsp.step(bus), "execute LHV");
    require(rsp.step(bus), "execute SHV");
    for (unsigned lane = 0; lane < 8U; ++lane) {
        const unsigned memoryOffset = (6U + lane * 2U) & 0x0FU;
        const std::uint16_t loaded = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(0x40U + memoryOffset) << 7U);
        require(rsp.vectorLane(2, lane) == loaded,
                "LHV wraps every second byte inside the 16-byte window");
        std::uint8_t value = 0;
        require(bus.read8(Bus::kSpDmemBase + 0x520U + memoryOffset, value) &&
                value == static_cast<std::uint8_t>(0x50U + lane),
                "SHV wraps every second byte and stores bits 14 through 7");
    }
    require(!rsp.faulted(), "LHV and SHV are supported RSP memory instructions");
}

void testPackedVectorStores() {
    Bus bus;
    RspCpu rsp;
    for (unsigned lane = 0; lane < 8U; ++lane) {
        rsp.setVectorLane(4, lane, static_cast<std::uint16_t>(
            ((0x20U + lane) << 8U) | 0x0055U));
        rsp.setVectorLane(5, lane, static_cast<std::uint16_t>(
            ((0x30U + lane) << 7U) | 0x003FU));
    }
    put(bus, 0, iType(0x09, 0, 1, 0x0400));
    put(bus, 4, vectorMemoryType(0x3A, 1, 4, 6, 0, 1)); // SPV offset * 8
    put(bus, 8, vectorMemoryType(0x3A, 1, 5, 7, 0, 2)); // SUV offset * 8
    require(rsp.step(bus), "set packed vector store base");
    require(rsp.step(bus), "execute SPV");
    require(rsp.step(bus), "execute SUV");
    for (unsigned lane = 0; lane < 8U; ++lane) {
        std::uint8_t value = 0;
        require(bus.read8(Bus::kSpDmemBase + 0x408U + lane, value) &&
                value == static_cast<std::uint8_t>(0x20U + lane),
                "SPV stores vector high byte");
        require(bus.read8(Bus::kSpDmemBase + 0x410U + lane, value) &&
                value == static_cast<std::uint8_t>(0x30U + lane),
                "SUV stores vector bits 14 through 7");
    }
    require(!rsp.faulted(), "SPV and SUV are supported RSP memory instructions");
}

void testPackedVectorLoads() {
    Bus bus;
    RspCpu rsp;
    for (unsigned i = 0; i < 8U; ++i)
        require(bus.write8(Bus::kSpDmemBase + 0x308U + i,
                           static_cast<std::uint8_t>(0x10U + i)),
                "initialize packed vector load bytes");
    put(bus, 0, iType(0x09, 0, 1, 0x0300));
    put(bus, 4, vectorMemoryType(0x32, 1, 2, 6, 0, 1)); // LPV offset * 8
    put(bus, 8, vectorMemoryType(0x32, 1, 3, 7, 0, 1)); // LUV offset * 8
    require(rsp.step(bus), "set packed vector load base");
    require(rsp.step(bus), "execute LPV");
    require(rsp.step(bus), "execute LUV");
    for (unsigned lane = 0; lane < 8U; ++lane) {
        const std::uint16_t byte = static_cast<std::uint16_t>(0x10U + lane);
        require(rsp.vectorLane(2, lane) == static_cast<std::uint16_t>(byte << 8U),
                "LPV places each byte at bit 15 and clears lower bits");
        require(rsp.vectorLane(3, lane) == static_cast<std::uint16_t>(byte << 7U),
                "LUV places each byte at bit 14 and clears remaining bits");
    }
    require(!rsp.faulted(), "LPV and LUV are supported RSP memory instructions");
}

void testVectorQuadMemoryTransfers() {
    Bus bus;
    RspCpu rsp;
    rsp.setVectorLane(8, 0, 0xEEEEU);
    rsp.setVectorLane(8, 1, 0xEEEEU);
    rsp.setVectorLane(8, 2, 0xEEEEU);
    rsp.setVectorLane(8, 3, 0xEEEEU);
    rsp.setVectorLane(8, 4, 0xEEEEU);
    rsp.setVectorLane(8, 5, 0xEEEEU);
    rsp.setVectorLane(8, 6, 0xEEEEU);
    rsp.setVectorLane(8, 7, 0xEEEEU);
    rsp.setVectorLane(9, 0, 0xEEEEU);
    rsp.setVectorLane(9, 1, 0xEEEEU);
    for (unsigned i = 0; i < 32U; ++i)
        require(bus.write8(Bus::kSpDmemBase + 0x200U + i,
                           static_cast<std::uint8_t>(i)),
                "initialize quad-load DMEM");
    rsp.setVectorLane(7, 0, 0xACADU);
    rsp.setVectorLane(7, 1, 0xAEAFU);
    rsp.setVectorLane(7, 2, 0xA0A1U);
    rsp.setVectorLane(7, 3, 0xA2A3U);
    rsp.setVectorLane(7, 4, 0xA4A5U);
    rsp.setVectorLane(7, 5, 0xA6A7U);
    rsp.setVectorLane(7, 6, 0xA8A9U);
    rsp.setVectorLane(7, 7, 0xAAABU);
    put(bus, 0, iType(0x09, 0, 1, 0x0208)); // address 0x208
    put(bus, 4, iType(0x09, 0, 2, 0x0218)); // address 0x218
    put(bus, 8, vectorMemoryType(0x32, 1, 8, 4, 4, 0)); // LQV
    put(bus, 12, vectorMemoryType(0x32, 2, 9, 5, 4, 0)); // LRV
    put(bus, 16, vectorMemoryType(0x3A, 1, 7, 4, 4, 0)); // SQV
    put(bus, 20, vectorMemoryType(0x3A, 2, 7, 5, 4, 0)); // SRV
    for (unsigned i = 0; i < 6U; ++i)
        require(rsp.step(bus), "execute RSP quad memory transfer program");

    require(rsp.vectorLane(8, 0) == 0xEEEEU && rsp.vectorLane(8, 1) == 0xEEEEU,
            "LQV preserves vector bytes before element");
    require(rsp.vectorLane(8, 2) == 0x0809U && rsp.vectorLane(8, 5) == 0x0E0FU,
            "LQV loads through the next 16-byte boundary");
    require(rsp.vectorLane(9, 6) == 0x1011U && rsp.vectorLane(9, 7) == 0x1213U,
            "LRV loads right-aligned bytes into the vector tail");

    std::uint8_t value = 0;
    require(bus.read8(Bus::kSpDmemBase + 0x208U, value) && value == 0xA0U,
            "SQV stores from the selected vector element");
    require(bus.read8(Bus::kSpDmemBase + 0x20FU, value) && value == 0xA7U,
            "SQV stops at the next 16-byte boundary");
    require(bus.read8(Bus::kSpDmemBase + 0x210U, value) && value == 0xA8U,
            "SRV stores right-aligned vector tail");
    require(bus.read8(Bus::kSpDmemBase + 0x213U, value) && value == 0xABU,
            "SRV stores remaining tail bytes");
    require(bus.read8(Bus::kSpDmemBase + 0x214U, value) && value == 0xACU,
            "SRV wraps vector selection to byte zero");
    require(bus.read8(Bus::kSpDmemBase + 0x217U, value) && value == 0xAFU,
            "SRV completes wrapped vector store");
    require(!rsp.faulted(), "LQV/LRV/SQV/SRV are supported");
}

void testVectorBasicMemoryTransfers() {
    Bus bus;
    RspCpu rsp;
    for (unsigned i = 0; i < 16U; ++i)
        require(bus.write8(Bus::kSpDmemBase + 0x100U + i,
                           static_cast<std::uint8_t>(0x10U + i)),
                "initialize vector-load DMEM");
    rsp.setVectorLane(6, 0, 0xAABBU);
    rsp.setVectorLane(6, 1, 0x1122U);
    rsp.setVectorLane(6, 2, 0x3344U);
    rsp.setVectorLane(6, 3, 0x5566U);
    rsp.setVectorLane(6, 4, 0x7788U);
    rsp.setVectorLane(6, 5, 0x99AAU);
    rsp.setVectorLane(6, 6, 0xBBCCU);
    rsp.setVectorLane(6, 7, 0xDDEEU);
    put(bus, 0, iType(0x09, 0, 1, 0x0100)); // base = DMEM offset 0x100
    put(bus, 4, vectorMemoryType(0x32, 1, 2, 0, 0, 0)); // LBV v2[e0]
    put(bus, 8, vectorMemoryType(0x32, 1, 3, 1, 14, 0)); // LSV v3[e14]
    put(bus, 12, vectorMemoryType(0x32, 1, 4, 2, 12, 0)); // LLV v4[e12]
    put(bus, 16, vectorMemoryType(0x32, 1, 5, 3, 8, 0)); // LDV v5[e8]
    put(bus, 20, vectorMemoryType(0x3A, 1, 6, 0, 0, 0)); // SBV
    put(bus, 24, vectorMemoryType(0x3A, 1, 6, 1, 14, 1)); // SSV at base + 2
    put(bus, 28, vectorMemoryType(0x3A, 1, 6, 2, 14, 2)); // SLV at base + 8, wraps vector bytes
    put(bus, 32, vectorMemoryType(0x3A, 1, 6, 3, 8, 3)); // SDV at base + 24
    for (unsigned i = 0; i < 9U; ++i)
        require(rsp.step(bus), "execute RSP vector memory transfer program");

    require(rsp.vectorLane(2, 0) == 0x1000U, "LBV loads one byte into vector high byte");
    require(rsp.vectorLane(3, 7) == 0x1011U, "LSV loads two bytes at element 14");
    require(rsp.vectorLane(4, 6) == 0x1011U && rsp.vectorLane(4, 7) == 0x1213U,
            "LLV loads four bytes at element 12");
    require(rsp.vectorLane(5, 4) == 0x1011U && rsp.vectorLane(5, 7) == 0x1617U,
            "LDV loads eight bytes at element 8");
    std::uint8_t value = 0;
    require(bus.read8(Bus::kSpDmemBase + 0x100U, value) && value == 0xAAU,
            "SBV stores one byte");
    require(bus.read8(Bus::kSpDmemBase + 0x102U, value) && value == 0xDDU,
            "SSV uses size-scaled offset and stores first selected byte");
    require(bus.read8(Bus::kSpDmemBase + 0x108U, value) && value == 0xDDU,
            "SLV uses word-scaled offset and wraps vector byte selection");
    require(bus.read8(Bus::kSpDmemBase + 0x10AU, value) && value == 0xAAU,
            "SLV wraps selected vector bytes back to byte zero");
    require(bus.read8(Bus::kSpDmemBase + 0x118U, value) && value == 0x77U,
            "SDV uses doubleword-scaled offset");
    require(!rsp.faulted(), "basic RSP vector memory instructions are supported");
}

void testVectorTransposeTransfers() {
    Bus bus;
    RspCpu rsp;
    for (unsigned reg = 0; reg < 32U; ++reg)
        for (unsigned lane = 0; lane < 8U; ++lane)
            rsp.setVectorLane(reg, lane, 0xDEADU);

    put(bus, 0, iType(0x09, 0, 1, 0x0100)); // LTV source base
    put(bus, 4, iType(0x09, 0, 2, 0x0400)); // STV destination base

    for (unsigned group = 0; group < 4U; ++group) {
        for (unsigned diagonal = 0; diagonal < 8U; ++diagonal) {
            const unsigned transfer = group * 8U + diagonal;
            for (unsigned slice = 0; slice < 8U; ++slice) {
                const std::uint16_t value = static_cast<std::uint16_t>(
                    0x1000U + group * 0x100U + diagonal * 8U + slice);
                require(bus.write16(Bus::kSpDmemBase + 0x0100U + transfer * 16U +
                                     slice * 2U, value),
                        "initialize LTV transpose source word");
            }
            put(bus, 8U + transfer * 4U,
                vectorMemoryType(0x32, 1, group * 8U + 7U, 0x0BU,
                                 diagonal * 2U, transfer));
            put(bus, 8U + (32U + transfer) * 4U,
                vectorMemoryType(0x3A, 2, group * 8U + 7U, 0x0BU,
                                 diagonal * 2U, transfer));
        }
    }

    for (unsigned instruction = 0; instruction < 66U; ++instruction)
        require(rsp.step(bus), "execute LTV/STV transpose transfer program");

    for (unsigned group = 0; group < 4U; ++group) {
        for (unsigned diagonal = 0; diagonal < 8U; ++diagonal) {
            const unsigned transfer = group * 8U + diagonal;
            for (unsigned slice = 0; slice < 8U; ++slice) {
                const unsigned vectorRegister =
                    group * 8U + ((slice + diagonal) & 7U);
                const std::uint16_t expected = static_cast<std::uint16_t>(
                    0x1000U + group * 0x100U + diagonal * 8U +
                    ((slice + diagonal) & 7U));
                require(rsp.vectorLane(vectorRegister, slice) == expected,
                        "LTV rotates source halfwords into the correct diagonal lane");
                const unsigned storedOffset =
                    ((16U - diagonal * 2U) + slice * 2U) & 0x0FU;
                std::uint16_t stored = 0;
                require(bus.read16(Bus::kSpDmemBase + 0x0400U +
                                   transfer * 16U + storedOffset, stored),
                        "read back rotated STV transpose destination");
                require(stored == expected,
                        "STV stores each diagonal lane at its rotated memory position");
            }
        }
    }
    require(!rsp.faulted(), "LTV and STV are supported");

    Bus wrapBus;
    RspCpu wrapRsp;
    for (unsigned slice = 0; slice < 8U; ++slice)
        require(wrapBus.write16(Bus::kSpDmemBase + 0x0FF0U + slice * 2U,
                                static_cast<std::uint16_t>(0xA000U + slice)),
                "initialize final aligned DMEM transpose word");
    put(wrapBus, 0, iType(0x09, 0, 1, 0x0FF0));
    put(wrapBus, 4, vectorMemoryType(0x32, 1, 7, 0x0BU, 0, 0));
    require(wrapRsp.step(wrapBus) && wrapRsp.step(wrapBus),
            "execute LTV at final aligned DMEM word");
    for (unsigned slice = 0; slice < 8U; ++slice)
        require(wrapRsp.vectorLane(slice, slice) ==
                    static_cast<std::uint16_t>(0xA000U + slice),
                "LTV reads all eight halfwords through the end of DMEM");
    require(!wrapRsp.faulted(), "aligned final DMEM LTV is valid");
}

void testVectorWrapStoreAndAbsentWrapLoad() {
    Bus bus;
    RspCpu rsp;
    for (unsigned lane = 0; lane < 8U; ++lane)
        rsp.setVectorLane(9, lane, static_cast<std::uint16_t>(0x1020U + lane * 0x0101U));
    put(bus, 0, iType(0x09, 0, 1, 0x0107)); // deliberately 8-byte-offset base
    put(bus, 4, vectorMemoryType(0x3A, 1, 9, 0x0AU, 13, 1)); // SWV, scaled offset 16
    require(rsp.step(bus) && rsp.step(bus), "execute SWV wrapped store");

    for (unsigned byte = 0; byte < 16U; ++byte) {
        const unsigned sourceByte = (13U + byte) & 0x0FU;
        const std::uint16_t sourceLane = rsp.vectorLane(9, sourceByte / 2U);
        const std::uint8_t expected = static_cast<std::uint8_t>(
            (sourceByte & 1U) == 0U ? sourceLane >> 8U : sourceLane);
        const unsigned destinationOffset = (7U + byte) & 0x0FU;
        std::uint8_t actual = 0;
        require(bus.read8(Bus::kSpDmemBase + 0x0110U + destinationOffset, actual),
                "read SWV destination byte");
        require(actual == expected,
                "SWV wraps vector source and 16-byte DMEM window independently");
    }
    require(!rsp.faulted(), "SWV is supported");

    Bus loadBus;
    RspCpu loadRsp;
    for (unsigned lane = 0; lane < 8U; ++lane)
        loadRsp.setVectorLane(4, lane, static_cast<std::uint16_t>(0xCA00U + lane));
    put(loadBus, 0, vectorMemoryType(0x32, 0, 4, 0x0AU, 0, 0)); // LWV is absent on retail N64
    require(loadRsp.step(loadBus), "execute reserved LWV encoding as a no-op");
    for (unsigned lane = 0; lane < 8U; ++lane)
        require(loadRsp.vectorLane(4, lane) == static_cast<std::uint16_t>(0xCA00U + lane),
                "LWV no-op leaves its destination vector unchanged");
    require(!loadRsp.faulted(), "LWV encoding is a no-op, not an unsupported-opcode fault");
}

void testVectorAbsolute() {
    Bus bus;
    RspCpu rsp;
    rsp.setVectorLane(1, 0, 1U);
    rsp.setVectorLane(1, 1, 0xFFFFU);
    rsp.setVectorLane(1, 2, 0xFFFFU);
    rsp.setVectorLane(1, 3, 0U);
    rsp.setVectorLane(1, 4, 1U);
    rsp.setVectorLane(2, 0, 0x8000U);
    rsp.setVectorLane(2, 1, 0x8000U);
    rsp.setVectorLane(2, 2, 0x1234U);
    rsp.setVectorLane(2, 3, 0x5555U);
    rsp.setVectorLane(2, 4, 0xFFFFU);
    rsp.setVectorLane(3, 0, 0xFFFFU);
    rsp.setVectorLane(4, 0, 1U);
    put(bus, 0, vType(0, 4, 3, 5, 0x14)); // VADDC sets VCO_LO bit 0
    put(bus, 4, vType(0, 2, 1, 6, 0x13)); // VABS
    put(bus, 8, vType(0, 0, 0, 7, 0x1D)); // VSAR ACC_LO
    require(rsp.step(bus), "prepare VCO before VABS");
    require(rsp.vectorCarryFlags() == 1U, "VADDC sets carry flag for VABS preservation test");
    require(rsp.step(bus), "execute VABS");
    require(rsp.vectorLane(6, 0) == 0x8000U, "VABS preserves positive VT value 0x8000");
    require(rsp.vectorLane(6, 1) == 0x7FFFU, "VABS saturates negation of -32768");
    require(rsp.vectorLane(6, 2) == 0xEDCCU, "VABS negates signed VT for negative VS");
    require(rsp.vectorLane(6, 3) == 0U, "VABS returns zero when VS is zero");
    require(rsp.vectorLane(6, 4) == 0xFFFFU, "VABS copies VT for positive VS");
    require(rsp.vectorCarryFlags() == 1U, "VABS leaves VCO unchanged");
    require(rsp.step(bus), "read VABS accumulator through VSAR");
    require(rsp.vectorLane(7, 0) == 0x8000U, "VABS ACC_LO retains unsaturated -32768");
    require(rsp.vectorLane(7, 1) == 0x8000U, "VABS ACC_LO records unsaturated negative extreme");
    require(rsp.vectorLane(7, 2) == 0xEDCCU, "VABS ACC_LO records negated lane");
    require(rsp.vectorLane(7, 3) == 0U, "VABS ACC_LO is zero for zero sign lane");
    require(!rsp.faulted(), "VABS is a supported instruction");
}

void testVectorReciprocalAndDivideLatch() {
    Bus bus;
    RspCpu rsp;
    rsp.setVectorLane(2, 0, 0U);
    rsp.setVectorLane(2, 1, 0U);
    rsp.setVectorLane(2, 2, 0U);
    rsp.setVectorLane(2, 3, 1U); // element 0 selects the destination-corresponding source lane
    put(bus, 0, vType(0, 2, 3, 4, 0x30)); // VRCP zero -> 0x7fffffff
    put(bus, 4, vType(0, 0, 0, 7, 0x1D)); // VSAR ACC_LO after VRCP
    put(bus, 8, vType(0, 2, 5, 4, 0x32)); // VRCPH reads prior DIV_OUT
    put(bus, 12, vType(0, 0, 0, 8, 0x1D)); // VSAR ACC_LO after VRCPH
    put(bus, 16, vType(0, 2, 6, 4, 0x31)); // VRCPL combines staged high + low
    put(bus, 20, vType(0, 2, 7, 4, 0x31)); // no pending high: sign-extended low
    put(bus, 24, vType(0, 2, 1, 4, 0x34)); // VRSQ zero
    put(bus, 28, vType(0, 0, 0, 9, 0x1D)); // VSAR ACC_LO after VRSQ
    put(bus, 32, vType(0, 2, 2, 4, 0x36)); // VRSQH reads prior DIV_OUT
    put(bus, 36, vType(0, 0, 0, 10, 0x1D)); // VSAR ACC_LO after VRSQH
    put(bus, 40, vType(0, 2, 1, 4, 0x35)); // VRSQL consumes staged square-root high half
    put(bus, 44, vType(0, 0, 0, 11, 0x1D)); // VSAR ACC_LO after VRSQL
    require(rsp.step(bus), "execute VRCP");
    require(rsp.vectorLane(4, 3) == 0xC000U, "VRCP element zero uses destination-corresponding VT lane");
    require(rsp.step(bus), "read VRCP accumulator side effect");
    for (unsigned lane = 0; lane < 8U; ++lane)
        require(rsp.vectorLane(7, lane) == rsp.vectorLane(2, lane),
                "VRCP copies the selected VT vector into ACC_LO");
    require(rsp.step(bus), "execute VRCPH");
    require(rsp.vectorLane(4, 5) == 0x7FFFU, "VRCPH returns prior result high half");
    require(rsp.step(bus), "read VRCPH accumulator side effect");
    for (unsigned lane = 0; lane < 8U; ++lane)
        require(rsp.vectorLane(8, lane) == rsp.vectorLane(2, lane),
                "VRCPH copies the selected VT vector into ACC_LO");
    require(rsp.step(bus), "execute VRCPL with staged high half");
    require(rsp.vectorLane(4, 6) == 0xFFFFU, "VRCPL zero 32-bit input low result");
    require(rsp.step(bus), "execute VRCPL without staged high half");
    require(rsp.vectorLane(4, 7) == 0xFFFFU, "VRCPL without high half uses signed 16-bit input");
    require(rsp.step(bus), "execute VRSQ");
    require(rsp.vectorLane(4, 1) == 0xFFFFU, "VRSQ zero low result is all ones");
    require(rsp.step(bus), "read VRSQ accumulator side effect");
    for (unsigned lane = 0; lane < 8U; ++lane)
        require(rsp.vectorLane(9, lane) == rsp.vectorLane(2, lane),
                "VRSQ copies the selected VT vector into ACC_LO");
    require(rsp.step(bus), "execute VRSQH");
    require(rsp.step(bus), "read VRSQH accumulator side effect");
    for (unsigned lane = 0; lane < 8U; ++lane)
        require(rsp.vectorLane(10, lane) == rsp.vectorLane(2, lane),
                "VRSQH copies the selected VT vector into ACC_LO");
    require(rsp.step(bus), "execute VRSQL with staged square-root high half");
    require(rsp.step(bus), "read VRSQL accumulator side effect");
    for (unsigned lane = 0; lane < 8U; ++lane)
        require(rsp.vectorLane(11, lane) == rsp.vectorLane(2, lane),
                "VRSQL copies the selected VT vector into ACC_LO");
    require(!rsp.faulted(), "reciprocal instructions are supported");
}

void testReciprocalNumericBoundaries() {
    Bus bus;
    RspCpu rsp;
    const std::uint16_t inputs[] = {0x0001U, 0x0002U, 0xFFFFU, 0x8000U,
                                    0x0001U, 0x0002U, 0xFFFFU, 0x8000U};
    const std::uint16_t reciprocalExpected[] = {0xC000U, 0xE000U, 0x3FFFU, 0x0000U};
    const std::uint16_t squareRootExpected[] = {0x0000U, 0xC000U, 0xFFFFU, 0x0000U};
    for (unsigned lane = 0; lane < 8U; ++lane) {
        rsp.setVectorLane(2, lane, inputs[lane]);
        const unsigned function = lane < 4U ? 0x30U : 0x34U;
        put(bus, lane * 4U, vType(0, 2, lane, 4, function));
    }
    for (unsigned lane = 0; lane < 8U; ++lane)
        require(rsp.step(bus), "execute numeric reciprocal boundary instruction");
    for (unsigned lane = 0; lane < 4U; ++lane) {
        require(rsp.vectorLane(4, lane) == reciprocalExpected[lane],
                "VRCP matches checked result for positive, negative, and extreme inputs");
        require(rsp.vectorLane(4, lane + 4U) == squareRootExpected[lane],
                "VRSQ matches checked result for positive, negative, and extreme inputs");
    }
    require(!rsp.faulted(), "numeric reciprocal boundary cases are supported");
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
    testCop0SpRegisterMoves();
    testCop0DpRegisterMoves();
    testArithmeticAndMemory();
    testBranchDelaySlot();
    testScalarAddAndSetLessThan();
    testScalarVariableShiftsAndJalr();
    testByteAndHalfwordMemory();
    testRegimmBranches();
    testRegimmLinkBranches();
    testVectorArithmetic();
    testReservedVectorOpcodesFault();
    testVectorQuantizedMultiplyAndAccumulatorRounding();
    testVectorCarryAndLogic();
    testVectorAccumulatorLowAndVsar();
    testVectorMultiplyFraction();
    testVectorMultiplyUnsigned();
    testVectorMultiplyAccumulateFraction();
    testVectorMultiplyAccumulateUnsigned();
    testVectorMultiplyDoublePrecision();
    testVectorMultiplyUnsignedLowSignedResults();
    testVectorUnsignedMultiplyAccumulateSaturationBoundaries();
    testVectorMultiplyDoubleAccumulate();
    testVectorComparisonsAndMerge();
    testVectorClipHigh();
    testVectorClipReverse();
    testVectorClipLow();
    testVectorPairBroadcastSelectors();
    testVectorElementBroadcast();
    testVectorInstructionInBranchDelaySlot();
    testVectorMoveAndNoop();
    testVectorTransposeTransfers();
    testVectorWrapStoreAndAbsentWrapLoad();
    testVectorAbsolute();
    testVectorBasicMemoryTransfers();
    testVectorQuadMemoryTransfers();
    testPackedVectorLoads();
    testPackedVectorStores();
    testHalfPackedVectorTransfers();
    testFourthPackedVectorLoad();
    testFourthPackedVectorStore();
    testVectorReciprocalAndDivideLatch();
    testReciprocalNumericBoundaries();
    testCop2VectorAndControlMoves();
    std::cout << "All scalar and vector RSP host tests passed.\n";
    return 0;
}
