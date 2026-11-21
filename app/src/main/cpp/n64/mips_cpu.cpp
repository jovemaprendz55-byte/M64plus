#include "n64/mips_cpu.h"
#include "n64/rdram.h"
#include <cstdint>
namespace m64plus::n64 {
namespace {
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
    pendingBranch_ = false;
    pendingBranchTarget_ = 0;
}
std::uint64_t MipsCpu::reg(unsigned index) const noexcept {
    return index < gpr_.size() ? (index == 0 ? 0 : gpr_[index]) : 0;
}
std::uint64_t MipsCpu::pc() const noexcept { return pc_; }
std::uint64_t MipsCpu::instructionsExecuted() const noexcept { return executed_; }
bool MipsCpu::step(const Rdram& memory) {
    if ((pc_ & 3U) != 0 || pc_ > 0xFFFFFFFFULL) return false;
    std::uint32_t instruction = 0;
    if (!memory.read32(static_cast<std::uint32_t>(pc_), instruction)) return false;
    const bool inDelaySlot = pendingBranch_;
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
    std::uint64_t branchTarget = sequentialPc + 4U;
    const auto branchRelative = [&]() {
        const auto offset = static_cast<std::int64_t>(static_cast<std::int16_t>(immediate)) * 4;
        return static_cast<std::uint64_t>(static_cast<std::int64_t>(sequentialPc) + offset);
    };
    switch (opcode) {
    case 0x00:
        destination = rd;
        switch (function) {
        case 0x00: result = signExtend32(static_cast<std::uint32_t>(b) << shift); break;
        case 0x02: result = signExtend32(static_cast<std::uint32_t>(b) >> shift); break;
        case 0x08:
            if (inDelaySlot || (a & 3U) != 0) return false;
            scheduleBranch = true; branchTarget = a; break;
        case 0x09:
            if (inDelaySlot || (a & 3U) != 0) return false;
            destination = rd;
            result = sequentialPc + 4U; writeRegister = true;
            scheduleBranch = true; branchTarget = a; break;
        case 0x21: result = signExtend32(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b)); break;
        case 0x23: result = signExtend32(static_cast<std::uint32_t>(a) - static_cast<std::uint32_t>(b)); break;
        case 0x24: result = a & b; break;
        case 0x25: result = a | b; break;
        case 0x26: result = a ^ b; break;
        case 0x27: result = ~(a | b); break;
        case 0x2A: result = static_cast<std::int64_t>(a) < static_cast<std::int64_t>(b); break;
        case 0x2B: result = a < b; break;
        case 0x2D: result = a + b; break;
        case 0x2F: result = a - b; break;
        default: return false;
        }
        writeRegister = writeRegister || (function != 0x08 && function != 0x09);
        break;
    case 0x01: {
        if (inDelaySlot) return false;
        const auto signedA = static_cast<std::int64_t>(a);
        bool take = false;
        switch (rt) {
        case 0x00: take = signedA < 0; break;
        case 0x01: take = signedA >= 0; break;
        case 0x10: take = signedA < 0; destination = 31; result = sequentialPc + 4U; writeRegister = true; break;
        case 0x11: take = signedA >= 0; destination = 31; result = sequentialPc + 4U; writeRegister = true; break;
        default: return false;
        }
        scheduleBranch = true;
        branchTarget = take ? branchRelative() : sequentialPc + 4U;
        break;
    }
    case 0x02: case 0x03:
        if (inDelaySlot) return false;
        if (opcode == 0x03) { destination = 31; result = sequentialPc + 4U; writeRegister = true; }
        scheduleBranch = true;
        branchTarget = ((sequentialPc) & 0xFFFFFFFFF0000000ULL) |
                       (static_cast<std::uint64_t>(instruction & 0x03FFFFFFU) << 2U);
        break;
    case 0x04: case 0x05: case 0x06: case 0x07:
        if (inDelaySlot) return false;
        if (opcode == 0x04 || opcode == 0x05) {
            const bool equal = a == b;
            scheduleBranch = (opcode == 0x04) ? equal : !equal;
        } else if (opcode == 0x06) {
            scheduleBranch = static_cast<std::int64_t>(a) <= 0;
        } else {
            scheduleBranch = static_cast<std::int64_t>(a) > 0;
        }
        branchTarget = scheduleBranch ? branchRelative() : sequentialPc + 4U;
        scheduleBranch = true; // even a not-taken branch has a delay slot
        break;
    case 0x09:
        destination = rt;
        result = signExtend32(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(signExtend16(immediate)));
        writeRegister = true;
        break;
    case 0x19:
        destination = rt;
        result = a + signExtend16(immediate);
        writeRegister = true;
        break;
    case 0x0C: destination = rt; result = a & immediate; writeRegister = true; break;
    case 0x0D: destination = rt; result = a | immediate; writeRegister = true; break;
    case 0x0E: destination = rt; result = a ^ immediate; writeRegister = true; break;
    case 0x0F: destination = rt; result = signExtend32(static_cast<std::uint32_t>(immediate) << 16U); writeRegister = true; break;
    default: return false;
    }
    if (writeRegister && destination != 0) gpr_[destination] = result;
    gpr_[0] = 0;
    if (inDelaySlot) {
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
}
