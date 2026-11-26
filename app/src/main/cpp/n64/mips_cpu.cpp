#include "n64/mips_cpu.h"
#include "n64/rdram.h"
#include "n64/bus.h"
#include "n64/cp0.h"
#include "n64/tlb.h"
#include <cstdint>
namespace m64plus::n64 {
namespace {
std::uint64_t memoryWriteEpoch(const Rdram& memory, std::uint32_t address) noexcept {
    return memory.reservationEpoch(address);
}
std::uint64_t memoryWriteEpoch(const Bus& memory, std::uint32_t address) noexcept {
    return memory.rdramReservationEpoch(address);
}
std::uint64_t signExtend32(std::uint32_t value) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(value)));
}
std::uint64_t signExtend16(std::uint16_t value) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int16_t>(value)));
}

}
MipsCpu::MipsCpu() { reset(); }
void MipsCpu::reset(std::uint64_t pc) {
    gpr_.fill(0);
    pc_ = pc;
    executed_ = 0;
    hi_ = 0;
    lo_ = 0;
    reservationValid_ = false;
    reservationAddress_ = 0;
    reservationWriteEpoch_ = 0;
    pendingBranch_ = false;
    pendingBranchTarget_ = 0;
    cp0_.reset();
    tlb_.reset();
    tlbEnabled_ = false;
    hasLastException_ = false;
    lastExceptionPc_ = 0;
    lastExceptionInstruction_ = 0;
    lastExceptionCode_ = 0;
}
std::uint64_t MipsCpu::reg(unsigned index) const noexcept {
    return index < gpr_.size() ? (index == 0 ? 0 : gpr_[index]) : 0;
}
std::uint64_t MipsCpu::pc() const noexcept { return pc_; }
std::uint64_t MipsCpu::instructionsExecuted() const noexcept { return executed_; }
bool MipsCpu::hasLastException() const noexcept { return hasLastException_; }
std::uint64_t MipsCpu::lastExceptionPc() const noexcept { return lastExceptionPc_; }
std::uint32_t MipsCpu::lastExceptionInstruction() const noexcept { return lastExceptionInstruction_; }
std::uint8_t MipsCpu::lastExceptionCode() const noexcept { return lastExceptionCode_; }
void MipsCpu::setTlbEnabled(bool enabled) noexcept { tlbEnabled_ = enabled; }
void MipsCpu::setRegister(unsigned index, std::uint64_t value) noexcept {
    if (index > 0U && index < gpr_.size()) gpr_[index] = value;
}
bool MipsCpu::setCp0Register(unsigned index, std::uint64_t value) noexcept {
    return cp0_.write(index, value);
}
const Cp0& MipsCpu::cp0() const noexcept { return cp0_; }
template <typename Memory>
bool MipsCpu::stepMemory(Memory& memory) {
    const bool inDelaySlot = pendingBranch_;
    std::uint32_t currentInstruction = 0;
    const auto takeException = [&](ExceptionCode code, std::uint64_t badAddress = 0, bool tlbRefill = false) {
        hasLastException_ = true;
        reservationValid_ = false;
        lastExceptionPc_ = pc_;
        lastExceptionInstruction_ = currentInstruction;
        lastExceptionCode_ = static_cast<std::uint8_t>(code);
        pc_ = cp0_.raiseException(code, pc_, inDelaySlot, badAddress, tlbRefill);
        pendingBranch_ = false;
        pendingBranchTarget_ = 0;
        cp0_.tick();
        return true;
    };
    // MIPS accepts an interrupt only between instructions, never in a branch delay slot.
    if (!inDelaySlot && cp0_.interruptRequested())
        return takeException(ExceptionCode::Interrupt);
    if ((pc_ & 3U) != 0) return takeException(ExceptionCode::AddressLoad, pc_);
    std::uint32_t instructionAddress = static_cast<std::uint32_t>(pc_);
    if (pc_ > 0xFFFFFFFFULL &&
        pc_ != static_cast<std::uint64_t>(static_cast<std::int64_t>(
            static_cast<std::int32_t>(instructionAddress))))
        return takeException(ExceptionCode::AddressLoad, pc_);
    const std::uint32_t pc32 = static_cast<std::uint32_t>(pc_);
    const bool pcInDirectSegment = pc32 >= 0x80000000U && pc32 < 0xC0000000U;
    const bool pcInSupervisorSegment = pc32 >= 0xC0000000U && pc32 < 0xE0000000U;
    const bool pcInKernelSegment3 = pc32 >= 0xE0000000U;
    if ((pcInDirectSegment || pcInKernelSegment3) && !cp0_.kernelMode())
        return takeException(ExceptionCode::AddressLoad, pc_);
    if (pcInSupervisorSegment && !cp0_.supervisorMode())
        return takeException(ExceptionCode::AddressLoad, pc_);
    // KSEG0/KSEG1 are unmapped segments: their direct mapping applies even
    // while the software-managed TLB is disabled.
    if (tlbEnabled_ || pcInDirectSegment || pcInSupervisorSegment || pcInKernelSegment3) {
        std::uint32_t translated = 0;
        const auto result = tlb_.translate(pc_, static_cast<std::uint8_t>(
            cp0_.read(Cp0::kEntryHi) & 0xFFU), false, translated);
        if (result == TlbResult::Miss)
            return takeException(ExceptionCode::TlbLoad, pc_, true);
        if (result == TlbResult::Invalid)
            return takeException(ExceptionCode::TlbLoad, pc_);
        if (result == TlbResult::Modified)
            return takeException(ExceptionCode::TlbModification, pc_);
        instructionAddress = translated;
    }
    std::uint32_t instruction = 0;
    if (!memory.read32(instructionAddress, instruction))
        return takeException(ExceptionCode::BusInstruction, pc_);
    currentInstruction = instruction;
    const std::uint64_t oldBranchTarget = pendingBranchTarget_;
    const unsigned opcode = instruction >> 26U;
    const unsigned rs = (instruction >> 21U) & 31U;
    const unsigned rt = (instruction >> 16U) & 31U;
    const unsigned rd = (instruction >> 11U) & 31U;
    const unsigned shift = (instruction >> 6U) & 31U;
    const unsigned function = instruction & 63U;
    const std::uint16_t immediate = static_cast<std::uint16_t>(instruction);
    const std::uint64_t a = gpr_[rs];
    const std::uint64_t b = gpr_[rt];
    const std::uint64_t sequentialPc = pc_ + 4U;
    std::uint64_t result = 0;
    unsigned destination = 0;
    bool writeRegister = false;
    bool scheduleBranch = false;
    bool annulDelaySlot = false;
    bool doExceptionReturn = false;
    std::uint64_t branchTarget = sequentialPc + 4U;
    const auto branchRelative = [&]() {
        const auto offset = static_cast<std::int64_t>(static_cast<std::int16_t>(immediate)) * 4;
        return static_cast<std::uint64_t>(static_cast<std::int64_t>(sequentialPc) + offset);
    };
    const auto addOverflow64 = [](std::uint64_t left, std::uint64_t right,
                                   std::uint64_t sum) {
        return ((~(left ^ right) & (left ^ sum)) >> 63U) != 0U;
    };
    const auto subOverflow64 = [](std::uint64_t left, std::uint64_t right,
                                   std::uint64_t difference) {
        return (((left ^ right) & (left ^ difference)) >> 63U) != 0U;
    };
    switch (opcode) {
    case 0x00:
        // Reserved fields must be zero for fixed/variable shifts and jumps.
        if (((function == 0x00 || function == 0x02 || function == 0x03 ||
              function == 0x38 || function == 0x3A || function == 0x3B ||
              function == 0x3C || function == 0x3E || function == 0x3F) && rs != 0U) ||
            ((function == 0x04 || function == 0x06 || function == 0x07 ||
              function == 0x14 || function == 0x16 || function == 0x17) && shift != 0U) ||
            (function == 0x08 && (rt != 0U || rd != 0U || shift != 0U)) ||
            (function == 0x09 && (rt != 0U || shift != 0U)) ||
            ((function == 0x10 || function == 0x12) &&
             (rs != 0U || rt != 0U || shift != 0U)) ||
            ((function == 0x11 || function == 0x13) &&
             (rt != 0U || rd != 0U || shift != 0U)) ||
            ((function >= 0x18 && function <= 0x1F) &&
             (rd != 0U || shift != 0U)) ||
            (function == 0x0F && (rs != 0U || rt != 0U || rd != 0U)) ||
            ((function == 0x20 || function == 0x21 || function == 0x22 ||
              function == 0x23 || function == 0x24 || function == 0x25 ||
              function == 0x26 || function == 0x27 || function == 0x2A ||
              function == 0x2B || function == 0x2C || function == 0x2D ||
              function == 0x0A || function == 0x0B ||
              function == 0x2E || function == 0x2F) && shift != 0U))
            return takeException(ExceptionCode::ReservedInstruction);
        destination = rd;
        switch (function) {
        case 0x00: result = signExtend32(static_cast<std::uint32_t>(b) << shift); break; // SLL
        case 0x02: result = signExtend32(static_cast<std::uint32_t>(b) >> shift); break; // SRL
        case 0x03: result = signExtend32(static_cast<std::uint32_t>(
            static_cast<std::int32_t>(b) >> shift)); break; // SRA
        case 0x04: result = signExtend32(static_cast<std::uint32_t>(b) <<
            (static_cast<unsigned>(a) & 31U)); break; // SLLV
        case 0x06: result = signExtend32(static_cast<std::uint32_t>(b) >>
            (static_cast<unsigned>(a) & 31U)); break; // SRLV
        case 0x07: result = signExtend32(static_cast<std::uint32_t>(
            static_cast<std::int32_t>(b) >> (static_cast<unsigned>(a) & 31U))); break; // SRAV
        case 0x14: result = b << (static_cast<unsigned>(a) & 63U); break; // DSLLV
        case 0x16: result = b >> (static_cast<unsigned>(a) & 63U); break; // DSRLV
        case 0x17: result = static_cast<std::uint64_t>(
            static_cast<std::int64_t>(b) >> (static_cast<unsigned>(a) & 63U)); break; // DSRAV
        case 0x10: result = hi_; break; // MFHI
        case 0x11: hi_ = a; destination = 0; writeRegister = false; break; // MTHI
        case 0x12: result = lo_; break; // MFLO
        case 0x0C: return takeException(ExceptionCode::Syscall); // SYSCALL
        case 0x0D: return takeException(ExceptionCode::Breakpoint); // BREAK
        case 0x0F: // SYNC: ordering is preserved by the serialized CPU execution loop.
            destination = 0; writeRegister = false; break;
        case 0x0A: // MOVZ: move when rt is zero.
            result = a;
            if (b == 0U) writeRegister = true;
            else destination = 0;
            break;
        case 0x0B: // MOVN: move when rt is nonzero.
            result = a;
            if (b != 0U) writeRegister = true;
            else destination = 0;
            break;
        case 0x13: lo_ = a; destination = 0; writeRegister = false; break; // MTLO
        case 0x18: { // MULT (signed 32-bit operands)
            const auto product = static_cast<std::int64_t>(static_cast<std::int32_t>(a)) *
                                 static_cast<std::int64_t>(static_cast<std::int32_t>(b));
            const auto bits = static_cast<std::uint64_t>(product);
            hi_ = signExtend32(static_cast<std::uint32_t>(bits >> 32U));
            lo_ = signExtend32(static_cast<std::uint32_t>(bits));
            destination = 0;
            writeRegister = false;
            break;
        }
        case 0x19: { // MULTU (unsigned 32-bit operands)
            const std::uint64_t product =
                static_cast<std::uint64_t>(static_cast<std::uint32_t>(a)) *
                static_cast<std::uint64_t>(static_cast<std::uint32_t>(b));
            hi_ = signExtend32(static_cast<std::uint32_t>(product >> 32U));
            lo_ = signExtend32(static_cast<std::uint32_t>(product));
            writeRegister = false;
            break;
        }
        case 0x1A: { // DIV (signed 32-bit operands)
            const auto dividend = static_cast<std::int32_t>(a);
            const auto divisor = static_cast<std::int32_t>(b);
            std::int32_t quotient = 0;
            std::int32_t remainder = dividend;
            if (divisor == 0) {
                quotient = dividend < 0 ? 1 : -1;
            } else if (dividend == INT32_MIN && divisor == -1) {
                quotient = INT32_MIN;
                remainder = 0;
            } else {
                quotient = dividend / divisor;
                remainder = dividend % divisor;
            }
            lo_ = signExtend32(static_cast<std::uint32_t>(quotient));
            hi_ = signExtend32(static_cast<std::uint32_t>(remainder));
            destination = 0;
            writeRegister = false;
            break;
        }
        case 0x1B: { // DIVU (unsigned 32-bit operands)
            const auto dividend = static_cast<std::uint32_t>(a);
            const auto divisor = static_cast<std::uint32_t>(b);
            const std::uint32_t quotient = divisor == 0U ? 0xFFFFFFFFU : dividend / divisor;
            const std::uint32_t remainder = divisor == 0U ? dividend : dividend % divisor;
            lo_ = signExtend32(quotient);
            hi_ = signExtend32(remainder);
            destination = 0;
            writeRegister = false;
            break;
        }
        case 0x1C: { // DMULT (signed 64-bit operands)
            const std::uint64_t a0 = static_cast<std::uint32_t>(a);
            const std::uint64_t a1 = a >> 32U;
            const std::uint64_t b0 = static_cast<std::uint32_t>(b);
            const std::uint64_t b1 = b >> 32U;
            const std::uint64_t p00 = a0 * b0;
            const std::uint64_t p01 = a0 * b1;
            const std::uint64_t p10 = a1 * b0;
            const std::uint64_t p11 = a1 * b1;
            const std::uint64_t middle = (p00 >> 32U) +
                static_cast<std::uint32_t>(p01) + static_cast<std::uint32_t>(p10);
            const std::uint64_t low = (middle << 32U) | static_cast<std::uint32_t>(p00);
            std::uint64_t high = p11 + (p01 >> 32U) + (p10 >> 32U) + (middle >> 32U);
            if ((a >> 63U) != 0U) high -= b;
            if ((b >> 63U) != 0U) high -= a;
            hi_ = high;
            lo_ = low;
            destination = 0;
            writeRegister = false;
            break;
        }
        case 0x1D: { // DMULTU (unsigned 64-bit operands)
            const std::uint64_t a0 = static_cast<std::uint32_t>(a);
            const std::uint64_t a1 = a >> 32U;
            const std::uint64_t b0 = static_cast<std::uint32_t>(b);
            const std::uint64_t b1 = b >> 32U;
            const std::uint64_t p00 = a0 * b0;
            const std::uint64_t p01 = a0 * b1;
            const std::uint64_t p10 = a1 * b0;
            const std::uint64_t p11 = a1 * b1;
            const std::uint64_t middle = (p00 >> 32U) +
                static_cast<std::uint32_t>(p01) + static_cast<std::uint32_t>(p10);
            lo_ = (middle << 32U) | static_cast<std::uint32_t>(p00);
            hi_ = p11 + (p01 >> 32U) + (p10 >> 32U) + (middle >> 32U);
            destination = 0;
            writeRegister = false;
            break;
        }
        case 0x1E: { // DDIV (signed 64-bit operands)
            const auto dividend = static_cast<std::int64_t>(a);
            const auto divisor = static_cast<std::int64_t>(b);
            std::int64_t quotient = 0;
            std::int64_t remainder = dividend;
            if (divisor == 0) {
                quotient = dividend < 0 ? 1 : -1;
            } else if (dividend == INT64_MIN && divisor == -1) {
                quotient = INT64_MIN;
                remainder = 0;
            } else {
                quotient = dividend / divisor;
                remainder = dividend % divisor;
            }
            lo_ = static_cast<std::uint64_t>(quotient);
            hi_ = static_cast<std::uint64_t>(remainder);
            destination = 0;
            writeRegister = false;
            break;
        }
        case 0x1F: { // DDIVU (unsigned 64-bit operands)
            const std::uint64_t quotient = b == 0U ? UINT64_MAX : a / b;
            const std::uint64_t remainder = b == 0U ? a : a % b;
            lo_ = quotient;
            hi_ = remainder;
            destination = 0;
            writeRegister = false;
            break;
        }
        case 0x30: // TGE
            if (static_cast<std::int64_t>(a) >= static_cast<std::int64_t>(b))
                return takeException(ExceptionCode::Trap);
            destination = 0; writeRegister = false; break;
        case 0x31: // TGEU
            if (a >= b) return takeException(ExceptionCode::Trap);
            destination = 0; writeRegister = false; break;
        case 0x32: // TLT
            if (static_cast<std::int64_t>(a) < static_cast<std::int64_t>(b))
                return takeException(ExceptionCode::Trap);
            destination = 0; writeRegister = false; break;
        case 0x33: // TLTU
            if (a < b) return takeException(ExceptionCode::Trap);
            destination = 0; writeRegister = false; break;
        case 0x34: // TEQ
            if (a == b) return takeException(ExceptionCode::Trap);
            destination = 0; writeRegister = false; break;
        case 0x36: // TNE
            if (a != b) return takeException(ExceptionCode::Trap);
            destination = 0; writeRegister = false; break;
        case 0x38: result = b << shift; break; // DSLL
        case 0x3A: result = b >> shift; break; // DSRL
        case 0x3B: result = static_cast<std::uint64_t>(
            static_cast<std::int64_t>(b) >> shift); break; // DSRA
        case 0x3C: result = b << (shift + 32U); break; // DSLL32
        case 0x3E: result = b >> (shift + 32U); break; // DSRL32
        case 0x3F: result = static_cast<std::uint64_t>(
            static_cast<std::int64_t>(b) >> (shift + 32U)); break; // DSRA32
        case 0x08:
            if (inDelaySlot) return takeException(ExceptionCode::ReservedInstruction);
            scheduleBranch = true; branchTarget = a; break;
        case 0x09:
            if (inDelaySlot) return takeException(ExceptionCode::ReservedInstruction);
            destination = rd;
            result = sequentialPc + 4U; writeRegister = true;
            scheduleBranch = true; branchTarget = a; break;
        case 0x20: { // ADD (signed 32-bit, traps on overflow)
            const auto sum = static_cast<std::int64_t>(static_cast<std::int32_t>(a)) +
                             static_cast<std::int64_t>(static_cast<std::int32_t>(b));
            if (sum < INT32_MIN || sum > INT32_MAX)
                return takeException(ExceptionCode::Overflow);
            result = signExtend32(static_cast<std::uint32_t>(sum));
            break;
        }
        case 0x21: result = signExtend32(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b)); break;
        case 0x22: { // SUB (signed 32-bit, traps on overflow)
            const auto difference = static_cast<std::int64_t>(static_cast<std::int32_t>(a)) -
                                    static_cast<std::int64_t>(static_cast<std::int32_t>(b));
            if (difference < INT32_MIN || difference > INT32_MAX)
                return takeException(ExceptionCode::Overflow);
            result = signExtend32(static_cast<std::uint32_t>(difference));
            break;
        }
        case 0x23: result = signExtend32(static_cast<std::uint32_t>(a) - static_cast<std::uint32_t>(b)); break;
        case 0x24: result = a & b; break;
        case 0x25: result = a | b; break;
        case 0x26: result = a ^ b; break;
        case 0x27: result = ~(a | b); break;
        case 0x2A: result = static_cast<std::int64_t>(a) < static_cast<std::int64_t>(b); break;
        case 0x2B: result = a < b; break;
        case 0x2C: { // DADD (signed 64-bit, traps on overflow)
            const std::uint64_t sum = a + b;
            if (addOverflow64(a, b, sum)) return takeException(ExceptionCode::Overflow);
            result = sum;
            break;
        }
        case 0x2D: result = a + b; break; // DADDU
        case 0x2E: { // DSUB (signed 64-bit, traps on overflow)
            const std::uint64_t difference = a - b;
            if (subOverflow64(a, b, difference)) return takeException(ExceptionCode::Overflow);
            result = difference;
            break;
        }
        case 0x2F: result = a - b; break; // DSUBU
        default: return takeException(ExceptionCode::ReservedInstruction);
        }
        writeRegister = writeRegister || (function != 0x08 && function != 0x09);
        break;
    case 0x01: {
        const auto signedA = static_cast<std::int64_t>(a);
        const auto signedImmediate = static_cast<std::int64_t>(
            static_cast<std::int16_t>(immediate));
        const auto unsignedImmediate = static_cast<std::uint64_t>(signedImmediate);
        // Trap-immediate instructions are ordinary ALU operations and are
        // legal in a branch delay slot; only REGIMM branch forms are rejected there.
        switch (rt) {
        case 0x08: // TGEI
            if (signedA >= signedImmediate) return takeException(ExceptionCode::Trap);
            break;
        case 0x09: // TGEIU
            if (a >= unsignedImmediate) return takeException(ExceptionCode::Trap);
            break;
        case 0x0A: // TLTI
            if (signedA < signedImmediate) return takeException(ExceptionCode::Trap);
            break;
        case 0x0B: // TLTIU
            if (a < unsignedImmediate) return takeException(ExceptionCode::Trap);
            break;
        case 0x0C: // TEQI
            if (a == unsignedImmediate) return takeException(ExceptionCode::Trap);
            break;
        case 0x0E: // TNEI
            if (a != unsignedImmediate) return takeException(ExceptionCode::Trap);
            break;
        default: break;
        }
        if (rt == 0x08U || rt == 0x09U || rt == 0x0AU ||
            rt == 0x0BU || rt == 0x0CU || rt == 0x0EU) {
            destination = 0;
            writeRegister = false;
            break;
        }
        if (inDelaySlot) return takeException(ExceptionCode::ReservedInstruction);
        bool take = false;
        switch (rt) {
        case 0x00: take = signedA < 0; break;
        case 0x01: take = signedA >= 0; break;
        case 0x02: take = signedA < 0; annulDelaySlot = !take; break; // BLTZL
        case 0x03: take = signedA >= 0; annulDelaySlot = !take; break; // BGEZL
        case 0x10: take = signedA < 0; destination = 31; result = sequentialPc + 4U; writeRegister = take; break;
        case 0x11: take = signedA >= 0; destination = 31; result = sequentialPc + 4U; writeRegister = take; break;
        case 0x12: // BLTZALL: link and annul the delay slot when not taken.
            take = signedA < 0;
            destination = 31;
            result = sequentialPc + 4U;
            writeRegister = take;
            annulDelaySlot = !take;
            break;
        case 0x13: // BGEZALL: link only when taken; annul the delay slot otherwise.
            take = signedA >= 0;
            destination = 31;
            result = sequentialPc + 4U;
            writeRegister = take;
            annulDelaySlot = !take;
            break;
        default: return takeException(ExceptionCode::ReservedInstruction);
        }
        scheduleBranch = true;
        branchTarget = take ? branchRelative() : sequentialPc + 4U;
        break;
    }
    case 0x02: case 0x03:
        if (inDelaySlot) return takeException(ExceptionCode::ReservedInstruction);
        if (opcode == 0x03) { destination = 31; result = sequentialPc + 4U; writeRegister = true; }
        scheduleBranch = true;
        branchTarget = ((sequentialPc) & 0xFFFFFFFFF0000000ULL) |
                       (static_cast<std::uint64_t>(instruction & 0x03FFFFFFU) << 2U);
        break;
    case 0x04: case 0x05: case 0x06: case 0x07:
    case 0x14: case 0x15: case 0x16: case 0x17:
        if (inDelaySlot) return takeException(ExceptionCode::ReservedInstruction);
        // BLEZ/BGTZ and their likely variants require rt == 0 in the encoding.
        if ((opcode == 0x06 || opcode == 0x07 || opcode == 0x16 || opcode == 0x17) &&
            rt != 0U)
            return takeException(ExceptionCode::ReservedInstruction);
        if (opcode == 0x04 || opcode == 0x05 || opcode == 0x14 || opcode == 0x15) {
            const bool equal = a == b;
            const bool equalBranch = opcode == 0x04 || opcode == 0x14;
            scheduleBranch = equalBranch ? equal : !equal;
        } else if (opcode == 0x06 || opcode == 0x16) {
            scheduleBranch = static_cast<std::int64_t>(a) <= 0;
        } else {
            scheduleBranch = static_cast<std::int64_t>(a) > 0;
        }
        annulDelaySlot = opcode >= 0x14 && !scheduleBranch;
        branchTarget = scheduleBranch ? branchRelative() : sequentialPc + 4U;
        scheduleBranch = !annulDelaySlot; // ordinary branches retain their delay slot
        break;
    // COP0: MFC0, MTC0 and ERET (register select 0 only).
    case 0x10:
        if (inDelaySlot)
            return takeException(ExceptionCode::ReservedInstruction);
        if (!cp0_.kernelMode())
            return takeException(ExceptionCode::CoprocessorUnusable);
        if (rs == 0x00) { // MFC0, select 0 only
            if ((instruction & 0x7FFU) != 0)
                return takeException(ExceptionCode::ReservedInstruction);
            destination = rt;
            result = signExtend32(static_cast<std::uint32_t>(cp0_.read(rd)));
            writeRegister = true;
        } else if (rs == 0x04) { // MTC0, select 0 only
            if ((instruction & 0x7FFU) != 0 || !cp0_.write(rd, gpr_[rt]))
                return takeException(ExceptionCode::ReservedInstruction);
        } else if (rs == 0x10) { // COP0 privileged operations
            // ERET/TLB operations use only the CO function field; bits 25:6 are reserved.
            if ((instruction & 0x001FFFC0U) != 0U)
                return takeException(ExceptionCode::ReservedInstruction);
            if (function == 0x18) { // ERET
                // ERET clears the architectural LLbit on either ERL or EXL return.
                reservationValid_ = false;
                doExceptionReturn = true;
            } else if (function == 0x08) { // TLBP
                const int found = tlb_.probe(cp0_.read(Cp0::kEntryHi),
                    static_cast<std::uint8_t>(cp0_.read(Cp0::kEntryHi) & 0xFFU));
                cp0_.write(Cp0::kIndex, found < 0 ? 0x80000000ULL : static_cast<std::uint64_t>(found));
            } else if (function == 0x01) { // TLBR
                TlbEntryData entry{};
                const auto index = static_cast<std::size_t>(cp0_.read(Cp0::kIndex) & 0x1FU);
                if (!tlb_.readEntry(index, entry)) return takeException(ExceptionCode::ReservedInstruction);
                const std::uint64_t globalBit = entry.global ? 1ULL : 0ULL;
                const std::uint64_t evenLo = (static_cast<std::uint64_t>(entry.evenPfn) << 6U) |
                    (entry.evenDirty ? 4ULL : 0ULL) | (entry.evenValid ? 2ULL : 0ULL) | globalBit;
                const std::uint64_t oddLo = (static_cast<std::uint64_t>(entry.oddPfn) << 6U) |
                    (entry.oddDirty ? 4ULL : 0ULL) | (entry.oddValid ? 2ULL : 0ULL) | globalBit;
                cp0_.write(Cp0::kEntryHi, static_cast<std::uint64_t>(entry.virtualPairBase) | entry.asid);
                cp0_.write(Cp0::kEntryLo0, evenLo);
                cp0_.write(Cp0::kEntryLo1, oddLo);
            } else if (function == 0x02 || function == 0x06) { // TLBWI / TLBWR
                const std::uint64_t entryHi = cp0_.read(Cp0::kEntryHi);
                const std::uint64_t evenLo = cp0_.read(Cp0::kEntryLo0);
                const std::uint64_t oddLo = cp0_.read(Cp0::kEntryLo1);
                const std::size_t index = static_cast<std::size_t>(
                    (function == 0x02 ? cp0_.read(Cp0::kIndex) : cp0_.read(Cp0::kRandom)) & 0x1FU);
                const bool global = (evenLo & 1ULL) != 0 && (oddLo & 1ULL) != 0;
                if (!tlb_.writeEntry(index, static_cast<std::uint32_t>(entryHi) & 0xFFFFE000U,
                    static_cast<std::uint8_t>(entryHi & 0xFFU),
                    static_cast<std::uint32_t>((evenLo >> 6U) & 0xFFFFFU),
                    static_cast<std::uint32_t>((oddLo >> 6U) & 0xFFFFFU),
                    (evenLo & 2ULL) != 0, (oddLo & 2ULL) != 0,
                    (evenLo & 4ULL) != 0, (oddLo & 4ULL) != 0, global))
                    return takeException(ExceptionCode::ReservedInstruction);
            } else return takeException(ExceptionCode::ReservedInstruction);
        } else return takeException(ExceptionCode::ReservedInstruction);
        break;
    case 0x08: { // ADDI (signed 32-bit, traps on overflow)
        const auto sum = static_cast<std::int64_t>(static_cast<std::int32_t>(a)) +
                         static_cast<std::int64_t>(static_cast<std::int16_t>(immediate));
        if (sum < INT32_MIN || sum > INT32_MAX)
            return takeException(ExceptionCode::Overflow);
        destination = rt;
        result = signExtend32(static_cast<std::uint32_t>(sum));
        writeRegister = true;
        break;
    }
    case 0x09:
        destination = rt;
        result = signExtend32(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(signExtend16(immediate)));
        writeRegister = true;
        break;
    case 0x18: { // DADDI (signed 64-bit, traps on overflow)
        const std::uint64_t operand = signExtend16(immediate);
        const std::uint64_t sum = a + operand;
        if (addOverflow64(a, operand, sum)) return takeException(ExceptionCode::Overflow);
        destination = rt;
        result = sum;
        writeRegister = true;
        break;
    }
    case 0x19:
        destination = rt;
        result = a + signExtend16(immediate);
        writeRegister = true;
        break;
    case 0x0A: // SLTI
        destination = rt;
        result = static_cast<std::int64_t>(a) <
                 static_cast<std::int64_t>(static_cast<std::int16_t>(immediate));
        writeRegister = true;
        break;
    case 0x0B: // SLTIU compares against the sign-extended immediate as unsigned
        destination = rt;
        result = a < signExtend16(immediate);
        writeRegister = true;
        break;
    case 0x0C: destination = rt; result = a & immediate; writeRegister = true; break;
    case 0x0D: destination = rt; result = a | immediate; writeRegister = true; break;
    case 0x0E: destination = rt; result = a ^ immediate; writeRegister = true; break;
    case 0x0F: destination = rt; result = signExtend32(static_cast<std::uint32_t>(immediate) << 16U); writeRegister = true; break;
    // Integer loads use the selected physical memory interface; optional TLB
    // translation is applied before the bus or RDRAM access.
    case 0x1A: case 0x1B: case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x30: case 0x34: case 0x37: {
        const std::uint64_t effective = a + signExtend16(immediate);
        std::uint32_t address = static_cast<std::uint32_t>(effective);
        if (effective > 0xFFFFFFFFULL &&
            effective != static_cast<std::uint64_t>(static_cast<std::int64_t>(
                static_cast<std::int32_t>(address))))
            return takeException(ExceptionCode::AddressLoad, effective);
        // Address errors have priority over TLB faults for data accesses.
        if ((opcode == 0x21 || opcode == 0x25) && (effective & 1U))
            return takeException(ExceptionCode::AddressLoad, effective);
        if ((opcode == 0x23 || opcode == 0x27 || opcode == 0x30) && (effective & 3U))
            return takeException(ExceptionCode::AddressLoad, effective);
        if ((opcode == 0x34 || opcode == 0x37) && (effective & 7U))
            return takeException(ExceptionCode::AddressLoad, effective);
        const bool inDirectSegment = address >= 0x80000000U && address < 0xC0000000U;
        const bool inSupervisorSegment = address >= 0xC0000000U && address < 0xE0000000U;
        const bool inKernelSegment3 = address >= 0xE0000000U;
        if ((inDirectSegment || inKernelSegment3) && !cp0_.kernelMode())
            return takeException(ExceptionCode::AddressLoad, effective);
        if (inSupervisorSegment && !cp0_.supervisorMode())
            return takeException(ExceptionCode::AddressLoad, effective);
        if (tlbEnabled_ || inDirectSegment || inSupervisorSegment || inKernelSegment3) {
            std::uint32_t translated = 0;
            const auto translation = tlb_.translate(effective, static_cast<std::uint8_t>(
                cp0_.read(Cp0::kEntryHi) & 0xFFU), false, translated);
            if (translation == TlbResult::Miss)
                return takeException(ExceptionCode::TlbLoad, effective, true);
            if (translation == TlbResult::Invalid)
                return takeException(ExceptionCode::TlbLoad, effective);
            if (translation == TlbResult::Modified)
                return takeException(ExceptionCode::TlbModification, effective);
            address = translated;
        }
        if (opcode == 0x20 || opcode == 0x24) {
            std::uint8_t value = 0;
            if (!memory.read8(address, value)) return takeException(ExceptionCode::BusData, effective);
            result = opcode == 0x20
                ? static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int8_t>(value)))
                : value;
        } else if (opcode == 0x21 || opcode == 0x25) {
            std::uint16_t value = 0;
            if (!memory.read16(address, value)) return takeException(ExceptionCode::BusData, effective);
            result = opcode == 0x21 ? signExtend16(value) : value;
        } else if (opcode == 0x1A || opcode == 0x1B) { // LDL / LDR (big-endian)
            std::uint64_t value = 0;
            const std::uint32_t aligned = address & ~7U;
            if (!memory.read64(aligned, value))
                return takeException(ExceptionCode::BusData, effective);
            const unsigned byteOffset = address & 7U;
            if (opcode == 0x1A) {
                const unsigned shift = byteOffset * 8U;
                const std::uint64_t preserve = shift == 0U ? 0ULL :
                    ((1ULL << shift) - 1ULL);
                result = (b & preserve) | (value << shift);
            } else {
                const unsigned shift = (7U - byteOffset) * 8U;
                const std::uint64_t preserve = 0xFFFFFFFFFFFFFF00ULL << (byteOffset * 8U);
                result = (b & preserve) | (value >> shift);
            }
        } else if (opcode == 0x22 || opcode == 0x26) { // LWL / LWR (big-endian)
            std::uint32_t value = 0;
            if (!memory.read32(address & ~3U, value))
                return takeException(ExceptionCode::BusData, effective);
            const unsigned byteOffset = address & 3U;
            const std::uint32_t previous = static_cast<std::uint32_t>(b);
            std::uint32_t merged = 0;
            if (opcode == 0x22) {
                const unsigned shift = byteOffset * 8U;
                const std::uint32_t preserve = shift == 0U ? 0U :
                    ((1U << shift) - 1U);
                merged = (previous & preserve) | (value << shift);
            } else {
                const unsigned shift = (3U - byteOffset) * 8U;
                const std::uint32_t preserve = 0xFFFFFF00U << (byteOffset * 8U);
                merged = (previous & preserve) | (value >> shift);
            }
            result = signExtend32(merged);
        } else if (opcode == 0x23 || opcode == 0x27 || opcode == 0x30) {
            std::uint32_t value = 0;
            if (!memory.read32(address, value)) return takeException(ExceptionCode::BusData, effective);
            result = opcode == 0x23 || opcode == 0x30 ? signExtend32(value) : value;
            if (opcode == 0x30) {
                reservationValid_ = true;
                reservationAddress_ = address;
                reservationWriteEpoch_ = memoryWriteEpoch(memory, address);
            }
        } else {
            if (!memory.read64(address, result)) return takeException(ExceptionCode::BusData, effective);
            if (opcode == 0x34) {
                reservationValid_ = true;
                reservationAddress_ = address;
                reservationWriteEpoch_ = memoryWriteEpoch(memory, address);
            }
        }
        destination = rt;
        writeRegister = true;
        break;
    }
    // Integer stores.
    case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x38: case 0x3C: case 0x3F: {
        const std::uint64_t effective = a + signExtend16(immediate);
        std::uint32_t address = static_cast<std::uint32_t>(effective);
        if (effective > 0xFFFFFFFFULL &&
            effective != static_cast<std::uint64_t>(static_cast<std::int64_t>(
                static_cast<std::int32_t>(address))))
            return takeException(ExceptionCode::AddressStore, effective);
        // Address errors have priority over TLB faults for data accesses.
        if (opcode == 0x29 && (effective & 1U))
            return takeException(ExceptionCode::AddressStore, effective);
        if ((opcode == 0x2B || opcode == 0x38) && (effective & 3U))
            return takeException(ExceptionCode::AddressStore, effective);
        if ((opcode == 0x3F || opcode == 0x3C) && (effective & 7U))
            return takeException(ExceptionCode::AddressStore, effective);
        const bool inDirectSegment = address >= 0x80000000U && address < 0xC0000000U;
        const bool inSupervisorSegment = address >= 0xC0000000U && address < 0xE0000000U;
        const bool inKernelSegment3 = address >= 0xE0000000U;
        if ((inDirectSegment || inKernelSegment3) && !cp0_.kernelMode())
            return takeException(ExceptionCode::AddressStore, effective);
        if (inSupervisorSegment && !cp0_.supervisorMode())
            return takeException(ExceptionCode::AddressStore, effective);
        if (tlbEnabled_ || inDirectSegment || inSupervisorSegment || inKernelSegment3) {
            std::uint32_t translated = 0;
            const auto translation = tlb_.translate(effective, static_cast<std::uint8_t>(
                cp0_.read(Cp0::kEntryHi) & 0xFFU), true, translated);
            if (translation == TlbResult::Miss)
                return takeException(ExceptionCode::TlbStore, effective, true);
            if (translation == TlbResult::Invalid)
                return takeException(ExceptionCode::TlbStore, effective);
            if (translation == TlbResult::Modified)
                return takeException(ExceptionCode::TlbModification, effective);
            address = translated;
        }
        bool stored = false;
        if (opcode == 0x38 || opcode == 0x3C) { // SC / SCD
            const bool reservationMatches = reservationValid_ && reservationAddress_ == address &&
                reservationWriteEpoch_ == memoryWriteEpoch(memory, address);
            reservationValid_ = false;
            if (reservationMatches) {
                stored = opcode == 0x38
                    ? memory.write32(address, static_cast<std::uint32_t>(b))
                    : memory.write64(address, b);
            }
            if (reservationMatches && !stored) return takeException(ExceptionCode::BusData, effective);
            destination = rt;
            result = reservationMatches ? 1U : 0U;
            writeRegister = true;
        }
        else if (opcode == 0x28) stored = memory.write8(address, static_cast<std::uint8_t>(b));
        else if (opcode == 0x29) stored = memory.write16(address, static_cast<std::uint16_t>(b));
        else if (opcode == 0x2A || opcode == 0x2E) { // SWL / SWR (big-endian)
            const std::uint32_t aligned = address & ~3U;
            std::uint32_t previous = 0;
            if (!memory.read32(aligned, previous))
                return takeException(ExceptionCode::BusData, effective);
            const unsigned byteOffset = address & 3U;
            std::uint32_t merged = 0;
            if (opcode == 0x2A) {
                const unsigned shift = byteOffset * 8U;
                const std::uint32_t preserve = byteOffset == 0U ? 0U :
                    (0xFFFFFFFFU << ((4U - byteOffset) * 8U));
                merged = (previous & preserve) |
                         (static_cast<std::uint32_t>(b) >> shift);
            } else {
                const unsigned shift = (3U - byteOffset) * 8U;
                const std::uint32_t preserve = 0x00FFFFFFU >> (byteOffset * 8U);
                merged = (previous & preserve) |
                         (static_cast<std::uint32_t>(b) << shift);
            }
            stored = memory.write32(aligned, merged);
        }
        else if (opcode == 0x2B) stored = memory.write32(address, static_cast<std::uint32_t>(b));
        else if (opcode == 0x2C || opcode == 0x2D) { // SDL / SDR (big-endian)
            const std::uint32_t aligned = address & ~7U;
            std::uint64_t previous = 0;
            if (!memory.read64(aligned, previous))
                return takeException(ExceptionCode::BusData, effective);
            const unsigned byteOffset = address & 7U;
            std::uint64_t merged = 0;
            if (opcode == 0x2C) {
                const unsigned shift = byteOffset * 8U;
                const std::uint64_t preserve = byteOffset == 0U ? 0ULL :
                    (0xFFFFFFFFFFFFFFFFULL << ((8U - byteOffset) * 8U));
                merged = (previous & preserve) | (b >> shift);
            } else {
                const unsigned shift = (7U - byteOffset) * 8U;
                const std::uint64_t preserve = 0x00FFFFFFFFFFFFFFULL >> (byteOffset * 8U);
                merged = (previous & preserve) | (b << shift);
            }
            stored = memory.write64(aligned, merged);
        }
        else stored = memory.write64(address, b);
        if (opcode != 0x38 && opcode != 0x3C && !stored) return takeException(ExceptionCode::BusData, effective);
        if (opcode != 0x38 && opcode != 0x3C) reservationValid_ = false;
        break;
    }
    default: return takeException(ExceptionCode::ReservedInstruction);
    }
    if (writeRegister && destination != 0) gpr_[destination] = result;
    gpr_[0] = 0;
    cp0_.tick();
    if (doExceptionReturn) {
        pc_ = cp0_.exceptionReturn();
        pendingBranch_ = false;
        pendingBranchTarget_ = 0;
    } else if (annulDelaySlot) {
        pendingBranch_ = false;
        pendingBranchTarget_ = 0;
        pc_ = sequentialPc + 4U;
    } else if (inDelaySlot) {
        pendingBranch_ = false;
        pendingBranchTarget_ = 0;
        pc_ = oldBranchTarget;
    } else {
        pc_ = sequentialPc;
        if (scheduleBranch) {
            pendingBranch_ = true;
            pendingBranchTarget_ = branchTarget;
        }
    }
    ++executed_;
    return true;
}
bool MipsCpu::step(Rdram& memory) { return stepMemory(memory); }
bool MipsCpu::step(Bus& memory) {
    cp0_.setExternalInterrupt(memory.miInterruptPending());
    return stepMemory(memory);
}
}
