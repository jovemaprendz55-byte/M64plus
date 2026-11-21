#include "n64/rsp_cpu.h"

#include "n64/bus.h"

#include <cstdint>

namespace m64plus::n64 {
namespace {
constexpr std::uint32_t kPcMask = 0x0FFCU;
constexpr std::uint32_t kDmemBase = Bus::kSpDmemBase;
constexpr std::uint32_t kImemBase = Bus::kSpImemBase;

std::uint32_t signExtend16(std::uint16_t value) noexcept {
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(
        static_cast<std::int16_t>(value)));
}
} // namespace

RspCpu::RspCpu() { reset(); }

void RspCpu::reset() noexcept {
    gpr_.fill(0);
    pc_ = 0;
    pendingBranchTarget_ = 0;
    executed_ = 0;
    pendingBranch_ = false;
    halted_ = false;
}

std::uint32_t RspCpu::reg(unsigned index) const noexcept {
    return index < gpr_.size() ? (index == 0U ? 0U : gpr_[index]) : 0U;
}

std::uint32_t RspCpu::pc() const noexcept { return pc_; }
bool RspCpu::halted() const noexcept { return halted_; }
std::uint64_t RspCpu::instructionsExecuted() const noexcept { return executed_; }

bool RspCpu::step(Bus& bus) noexcept {
    if (halted_) return false;

    const bool inDelaySlot = pendingBranch_;
    const std::uint32_t oldBranchTarget = pendingBranchTarget_;
    std::uint32_t instruction = 0;
    if (!bus.read32(kImemBase + pc_, instruction)) {
        halted_ = true;
        return false;
    }

    const unsigned opcode = instruction >> 26U;
    const unsigned rs = (instruction >> 21U) & 31U;
    const unsigned rt = (instruction >> 16U) & 31U;
    const unsigned rd = (instruction >> 11U) & 31U;
    const unsigned shift = (instruction >> 6U) & 31U;
    const unsigned function = instruction & 63U;
    const auto immediate = static_cast<std::uint16_t>(instruction);
    const std::uint32_t a = gpr_[rs];
    const std::uint32_t b = gpr_[rt];
    const std::uint32_t nextPc = (pc_ + 4U) & kPcMask;
    std::uint32_t destination = 0;
    std::uint32_t result = 0;
    bool writeRegister = false;
    bool scheduleBranch = false;
    bool breakAfterDelay = false;
    std::uint32_t branchTarget = (nextPc + 4U) & kPcMask;

    const auto relativeTarget = [&]() noexcept {
        const auto offset = static_cast<std::int32_t>(
            static_cast<std::int16_t>(immediate)) * 4;
        return static_cast<std::uint32_t>(
            static_cast<std::int32_t>(nextPc) + offset) & kPcMask;
    };

    switch (opcode) {
    case 0x00U:
        destination = rd;
        switch (function) {
        case 0x00U: result = b << shift; break; // SLL
        case 0x02U: result = b >> shift; break; // SRL
        case 0x08U: // JR
            if (inDelaySlot) { halted_ = true; return false; }
            scheduleBranch = true;
            branchTarget = a & kPcMask;
            writeRegister = false;
            break;
        case 0x0DU: // BREAK
            if (inDelaySlot) { halted_ = true; return false; }
            breakAfterDelay = true;
            writeRegister = false;
            break;
        case 0x21U: result = a + b; break; // ADDU
        case 0x23U: result = a - b; break; // SUBU
        case 0x24U: result = a & b; break;
        case 0x25U: result = a | b; break;
        case 0x26U: result = a ^ b; break;
        case 0x27U: result = ~(a | b); break;
        default: halted_ = true; return false;
        }
        if (function != 0x08U && function != 0x0DU) writeRegister = true;
        break;
    case 0x01U: { // REGIMM: BLTZ / BGEZ
        if (inDelaySlot || (rt != 0U && rt != 1U)) {
            halted_ = true;
            return false;
        }
        const bool negative = static_cast<std::int32_t>(a) < 0;
        const bool taken = rt == 0U ? negative : !negative;
        branchTarget = taken ? relativeTarget() : (nextPc + 4U) & kPcMask;
        scheduleBranch = true; // Both outcomes execute the delay slot.
        break;
    }
    case 0x02U: // J
    case 0x03U: // JAL
        if (inDelaySlot) { halted_ = true; return false; }
        scheduleBranch = true;
        branchTarget = (instruction & 0x03FFFFFFU) << 2U;
        if (opcode == 0x03U) {
            destination = 31U;
            result = (nextPc + 4U) & kPcMask;
            writeRegister = true;
        }
        break;
    case 0x04U: // BEQ
    case 0x05U: // BNE
    case 0x06U: // BLEZ
    case 0x07U: // BGTZ
        if (inDelaySlot) { halted_ = true; return false; }
        if (opcode == 0x04U) scheduleBranch = a == b;
        else if (opcode == 0x05U) scheduleBranch = a != b;
        else if (opcode == 0x06U)
            scheduleBranch = static_cast<std::int32_t>(a) <= 0;
        else scheduleBranch = static_cast<std::int32_t>(a) > 0;
        branchTarget = scheduleBranch ? relativeTarget() : (nextPc + 4U) & kPcMask;
        scheduleBranch = true;
        break;
    case 0x09U: // ADDIU
        destination = rt;
        result = a + signExtend16(immediate);
        writeRegister = true;
        break;
    case 0x0CU: destination = rt; result = a & immediate; writeRegister = true; break;
    case 0x0DU: destination = rt; result = a | immediate; writeRegister = true; break;
    case 0x0EU: destination = rt; result = a ^ immediate; writeRegister = true; break;
    case 0x0FU: // LUI
        destination = rt;
        result = static_cast<std::uint32_t>(immediate) << 16U;
        writeRegister = true;
        break;
    case 0x20U: // LB
    case 0x21U: // LH
    case 0x23U: // LW
    case 0x24U: // LBU
    case 0x25U: // LHU
    case 0x28U: // SB
    case 0x29U: // SH
    case 0x2BU: { // SW
        const std::uint32_t address = (a + signExtend16(immediate)) & 0x0FFFU;
        const std::uint32_t physical = kDmemBase + address;
        destination = rt;
        if (opcode == 0x20U || opcode == 0x24U) {
            std::uint8_t value = 0;
            if (!bus.read8(physical, value)) { halted_ = true; return false; }
            result = opcode == 0x20U
                ? static_cast<std::uint32_t>(static_cast<std::int32_t>(
                    static_cast<std::int8_t>(value)))
                : static_cast<std::uint32_t>(value);
            writeRegister = true;
        } else if (opcode == 0x21U || opcode == 0x25U) {
            std::uint16_t value = 0;
            if ((address & 1U) != 0U || !bus.read16(physical, value)) {
                halted_ = true;
                return false;
            }
            result = opcode == 0x21U
                ? static_cast<std::uint32_t>(static_cast<std::int32_t>(
                    static_cast<std::int16_t>(value)))
                : static_cast<std::uint32_t>(value);
            writeRegister = true;
        } else if (opcode == 0x23U) {
            if ((address & 3U) != 0U || !bus.read32(physical, result)) {
                halted_ = true;
                return false;
            }
            writeRegister = true;
        } else if (opcode == 0x28U) {
            if (!bus.write8(physical, static_cast<std::uint8_t>(b))) {
                halted_ = true;
                return false;
            }
        } else if (opcode == 0x29U) {
            if ((address & 1U) != 0U ||
                !bus.write16(physical, static_cast<std::uint16_t>(b))) {
                halted_ = true;
                return false;
            }
        } else if ((address & 3U) != 0U || !bus.write32(physical, b)) {
            halted_ = true;
            return false;
        }
        break;
    }
    default:
        halted_ = true;
        return false;
    }

    if (writeRegister && destination != 0U) gpr_[destination] = result;
    gpr_[0] = 0U;
    ++executed_;

    if (inDelaySlot) {
        pendingBranch_ = false;
        pendingBranchTarget_ = 0U;
        pc_ = oldBranchTarget;
        if (breakAfterDelay) halted_ = true;
    } else {
        pc_ = nextPc;
        if (scheduleBranch) {
            pendingBranch_ = true;
            pendingBranchTarget_ = branchTarget;
        } else if (breakAfterDelay) {
            halted_ = true;
        }
    }
    return true;
}
} // namespace m64plus::n64
