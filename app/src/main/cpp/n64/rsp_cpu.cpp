#include "n64/rsp_cpu.h"

#include "n64/bus.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace m64plus::n64 {
namespace {
constexpr std::uint32_t kPcMask = 0x0FFCU;
constexpr std::uint32_t kDmemBase = Bus::kSpDmemBase;
constexpr std::uint32_t kImemBase = Bus::kSpImemBase;
constexpr std::array<std::array<std::uint8_t, 8>, 16> kElementMap{{
    {{0,1,2,3,4,5,6,7}}, {{0,1,2,3,4,5,6,7}},
    {{0,0,2,2,4,4,6,6}}, {{1,1,3,3,5,5,7,7}},
    {{0,0,0,0,4,4,4,4}}, {{1,1,1,1,5,5,5,5}},
    {{2,2,2,2,6,6,6,6}}, {{3,3,3,3,7,7,7,7}},
    {{0,0,0,0,0,0,0,0}}, {{1,1,1,1,1,1,1,1}},
    {{2,2,2,2,2,2,2,2}}, {{3,3,3,3,3,3,3,3}},
    {{4,4,4,4,4,4,4,4}}, {{5,5,5,5,5,5,5,5}},
    {{6,6,6,6,6,6,6,6}}, {{7,7,7,7,7,7,7,7}}
}};

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
    for (auto& vector : vr_) vector.fill(0);
    accumulator_.fill(0);
    vco_ = 0;
    vcc_ = 0;
    vce_ = 0;
    pendingBranch_ = false;
    halted_ = false;
    faulted_ = false;
}

std::uint32_t RspCpu::reg(unsigned index) const noexcept {
    return index < gpr_.size() ? (index == 0U ? 0U : gpr_[index]) : 0U;
}

std::uint32_t RspCpu::pc() const noexcept { return pc_; }
void RspCpu::setPc(std::uint32_t pc) noexcept {
    pc_ = pc & kPcMask;
    pendingBranch_ = false;
    pendingBranchTarget_ = 0U;
    halted_ = false;
    faulted_ = false;
}
bool RspCpu::halted() const noexcept { return halted_; }
bool RspCpu::faulted() const noexcept { return faulted_; }
std::uint64_t RspCpu::instructionsExecuted() const noexcept { return executed_; }
std::uint16_t RspCpu::vectorLane(unsigned r, unsigned lane) const noexcept {
    return r < vr_.size() && lane < 8U ? vr_[r][lane] : 0U;
}
void RspCpu::setVectorLane(unsigned r, unsigned lane, std::uint16_t value) noexcept {
    if (r < vr_.size() && lane < 8U) vr_[r][lane] = value;
}
std::uint16_t RspCpu::vectorCarryFlags() const noexcept { return vco_; }
std::uint16_t RspCpu::vectorCompareFlags() const noexcept { return vcc_; }
std::uint8_t RspCpu::vectorClipFlags() const noexcept { return vce_; }

bool RspCpu::executeVector(unsigned element, unsigned vt, unsigned vs,
                           unsigned vd, unsigned function) noexcept {
    if (vt >= 32U || vs >= 32U || vd >= 32U || element >= 16U) return false;
    const bool add = function == 0x10U, sub = function == 0x11U;
    const bool addCarry = function == 0x14U, subCarry = function == 0x15U;
    const bool logical = function >= 0x28U && function <= 0x2DU;
    if (!add && !sub && !addCarry && !subCarry && !logical) return false;
    std::uint16_t nextVco = 0;
    for (unsigned lane = 0; lane < 8U; ++lane) {
        const std::uint16_t left = vr_[vs][lane];
        const std::uint16_t right = vr_[vt][kElementMap[element][lane]];
        std::uint16_t value = 0;
        if (add || sub) {
            const std::int32_t a = static_cast<std::int16_t>(left);
            const std::int32_t b = static_cast<std::int16_t>(right);
            const std::int32_t carry = (vco_ & (1U << lane)) != 0U ? 1 : 0;
            const std::int32_t sum = add ? a + b + carry : a - b - carry;
            accumulator_[lane] = sum;
            value = static_cast<std::uint16_t>(std::max(-32768, std::min(32767, sum)));
        } else if (addCarry) {
            const std::uint32_t sum = static_cast<std::uint32_t>(left) + right;
            accumulator_[lane] = sum;
            value = static_cast<std::uint16_t>(sum);
            if (sum > 0xFFFFU) nextVco |= static_cast<std::uint16_t>(1U << lane);
        } else if (subCarry) {
            const std::uint32_t diff = static_cast<std::uint32_t>(left) - right;
            accumulator_[lane] = static_cast<std::int16_t>(static_cast<std::uint16_t>(diff));
            value = static_cast<std::uint16_t>(diff);
            if (left < right) nextVco |= static_cast<std::uint16_t>(1U << lane);
            if (left != right) nextVco |= static_cast<std::uint16_t>(1U << (lane + 8U));
        } else {
            switch (function) {
            case 0x28U: value = left & right; break;
            case 0x29U: value = static_cast<std::uint16_t>(~(left & right)); break;
            case 0x2AU: value = left | right; break;
            case 0x2BU: value = static_cast<std::uint16_t>(~(left | right)); break;
            case 0x2CU: value = left ^ right; break;
            case 0x2DU: value = static_cast<std::uint16_t>(~(left ^ right)); break;
            default: return false;
            }
            accumulator_[lane] = static_cast<std::int16_t>(value);
        }
        vr_[vd][lane] = value;
    }
    if (add || sub || addCarry || subCarry) vco_ = nextVco;
    return true;
}

bool RspCpu::step(Bus& bus) noexcept {
    if (halted_) return false;

    const bool inDelaySlot = pendingBranch_;
    const std::uint32_t oldBranchTarget = pendingBranchTarget_;
    std::uint32_t instruction = 0;
    if (!bus.read32(kImemBase + pc_, instruction)) {
        halted_ = true; faulted_ = true;
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

    const auto readVectorByte = [&](unsigned vectorReg, unsigned byteIndex) noexcept {
        const std::uint16_t lane = vr_[vectorReg][byteIndex >> 1U];
        return static_cast<std::uint8_t>((byteIndex & 1U) == 0U
            ? lane >> 8U : lane);
    };
    const auto writeVectorByte = [&](unsigned vectorReg, unsigned byteIndex,
                                     std::uint8_t value) noexcept {
        std::uint16_t& lane = vr_[vectorReg][byteIndex >> 1U];
        if ((byteIndex & 1U) == 0U) {
            lane = static_cast<std::uint16_t>((lane & 0x00FFU) |
                (static_cast<std::uint16_t>(value) << 8U));
        } else {
            lane = static_cast<std::uint16_t>((lane & 0xFF00U) | value);
        }
    };

    if (opcode == 0x12U) {
        if (rs >= 0x10U) {
            if (!executeVector(rs & 0x0FU, rt, rd,
                               (instruction >> 6U) & 31U, function)) {
                halted_ = true; faulted_ = true;
                return false;
            }
        } else {
            const unsigned byteElement = (instruction >> 7U) & 0x0FU;
            switch (rs) {
            case 0x00U: { // MFC2: read a 16-bit byte window, sign extended.
                const std::uint16_t value = static_cast<std::uint16_t>(
                    (static_cast<std::uint16_t>(readVectorByte(rd, byteElement)) << 8U) |
                    readVectorByte(rd, (byteElement + 1U) & 0x0FU));
                destination = rt;
                result = signExtend16(value);
                writeRegister = true;
                break;
            }
            case 0x02U: // CFC2: read VCO, VCC, or VCE.
                if (rd == 0U) result = signExtend16(vco_);
                else if (rd == 1U) result = signExtend16(vcc_);
                else if (rd == 2U) {
                    result = static_cast<std::uint32_t>(static_cast<std::int32_t>(
                        static_cast<std::int8_t>(vce_)));
                } else { halted_ = true; faulted_ = true; return false; }
                destination = rt;
                writeRegister = true;
                break;
            case 0x04U: { // MTC2: write a 16-bit byte window; byte 15 does not wrap.
                const std::uint16_t value = static_cast<std::uint16_t>(gpr_[rt]);
                writeVectorByte(rd, byteElement, static_cast<std::uint8_t>(value >> 8U));
                if (byteElement < 15U) {
                    writeVectorByte(rd, byteElement + 1U, static_cast<std::uint8_t>(value));
                }
                break;
            }
            case 0x06U: // CTC2: write VCO, VCC, or VCE.
                if (rd == 0U) vco_ = static_cast<std::uint16_t>(gpr_[rt]);
                else if (rd == 1U) vcc_ = static_cast<std::uint16_t>(gpr_[rt]);
                else if (rd == 2U) vce_ = static_cast<std::uint8_t>(gpr_[rt]);
                else { halted_ = true; faulted_ = true; return false; }
                break;
            default:
                halted_ = true; faulted_ = true;
                return false;
            }
        }
    } else switch (opcode) {
    case 0x00U:
        destination = rd;
        switch (function) {
        case 0x00U: result = b << shift; break; // SLL
        case 0x02U: result = b >> shift; break; // SRL
        case 0x08U: // JR
            if (inDelaySlot) { halted_ = true; faulted_ = true; return false; }
            scheduleBranch = true;
            branchTarget = a & kPcMask;
            writeRegister = false;
            break;
        case 0x0DU: // BREAK
            if (inDelaySlot) { halted_ = true; faulted_ = true; return false; }
            breakAfterDelay = true;
            writeRegister = false;
            break;
        case 0x21U: result = a + b; break; // ADDU
        case 0x23U: result = a - b; break; // SUBU
        case 0x24U: result = a & b; break;
        case 0x25U: result = a | b; break;
        case 0x26U: result = a ^ b; break;
        case 0x27U: result = ~(a | b); break;
        default: halted_ = true; faulted_ = true; return false;
        }
        if (function != 0x08U && function != 0x0DU) writeRegister = true;
        break;
    case 0x01U: { // REGIMM: BLTZ / BGEZ
        if (inDelaySlot || (rt != 0U && rt != 1U)) {
            halted_ = true; faulted_ = true;
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
        if (inDelaySlot) { halted_ = true; faulted_ = true; return false; }
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
        if (inDelaySlot) { halted_ = true; faulted_ = true; return false; }
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
            if (!bus.read8(physical, value)) { halted_ = true; faulted_ = true; return false; }
            result = opcode == 0x20U
                ? static_cast<std::uint32_t>(static_cast<std::int32_t>(
                    static_cast<std::int8_t>(value)))
                : static_cast<std::uint32_t>(value);
            writeRegister = true;
        } else if (opcode == 0x21U || opcode == 0x25U) {
            std::uint16_t value = 0;
            if ((address & 1U) != 0U || !bus.read16(physical, value)) {
                halted_ = true; faulted_ = true;
                return false;
            }
            result = opcode == 0x21U
                ? static_cast<std::uint32_t>(static_cast<std::int32_t>(
                    static_cast<std::int16_t>(value)))
                : static_cast<std::uint32_t>(value);
            writeRegister = true;
        } else if (opcode == 0x23U) {
            if ((address & 3U) != 0U || !bus.read32(physical, result)) {
                halted_ = true; faulted_ = true;
                return false;
            }
            writeRegister = true;
        } else if (opcode == 0x28U) {
            if (!bus.write8(physical, static_cast<std::uint8_t>(b))) {
                halted_ = true; faulted_ = true;
                return false;
            }
        } else if (opcode == 0x29U) {
            if ((address & 1U) != 0U ||
                !bus.write16(physical, static_cast<std::uint16_t>(b))) {
                halted_ = true; faulted_ = true;
                return false;
            }
        } else if ((address & 3U) != 0U || !bus.write32(physical, b)) {
            halted_ = true; faulted_ = true;
            return false;
        }
        break;
    }
    default:
        halted_ = true; faulted_ = true;
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
