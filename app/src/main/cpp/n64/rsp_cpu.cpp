#include "n64/rsp_cpu.h"

#include "n64/bus.h"
#include "n64/rsp_divide_tables.h"

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

std::uint32_t countLeadingZeros(std::uint32_t value) noexcept {
    std::uint32_t count = 0;
    for (std::uint32_t mask = 0x80000000U; mask != 0U && (value & mask) == 0U; mask >>= 1U)
        ++count;
    return count;
}

std::uint32_t normalizedMagnitude(std::uint32_t raw, std::int64_t& signedValue) noexcept {
    signedValue = (raw & 0x80000000U) != 0U
        ? static_cast<std::int64_t>(raw) - 0x100000000LL
        : static_cast<std::int64_t>(raw);
    const std::uint32_t mask = signedValue < 0 ? 0xFFFFFFFFU : 0U;
    std::uint32_t magnitude = raw ^ mask;
    if (signedValue > -32768) magnitude -= mask;
    return magnitude;
}

std::uint32_t reciprocalLookup(std::uint32_t raw) noexcept {
    std::int64_t input = 0;
    const std::uint32_t magnitude = normalizedMagnitude(raw, input);
    const std::uint32_t mask = input < 0 ? 0xFFFFFFFFU : 0U;
    if (magnitude == 0U) return 0x7FFFFFFFU;
    if (input == -32768) return 0xFFFF0000U;
    const std::uint32_t shift = countLeadingZeros(magnitude);
    const std::uint32_t index = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(magnitude) << shift & 0x7FC00000ULL) >> 22U);
    const std::uint32_t mantissa = 0x10000U | detail::kReciprocalTable[index];
    return ((mantissa << 14U) >> (31U - shift)) ^ mask;
}

std::uint32_t inverseSquareRootLookup(std::uint32_t raw) noexcept {
    std::int64_t input = 0;
    const std::uint32_t magnitude = normalizedMagnitude(raw, input);
    const std::uint32_t mask = input < 0 ? 0xFFFFFFFFU : 0U;
    if (magnitude == 0U) return 0x7FFFFFFFU;
    if (input == -32768) return 0xFFFF0000U;
    const std::uint32_t shift = countLeadingZeros(magnitude);
    const std::uint32_t index = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(magnitude) << shift & 0x7FC00000ULL) >> 22U);
    const std::uint32_t tableIndex = (index & 0x1FEU) | (shift & 1U);
    const std::uint32_t mantissa = 0x10000U | detail::kInverseSquareRootTable[tableIndex];
    return ((mantissa << 14U) >> ((31U - shift) >> 1U)) ^ mask;
}
} // namespace

RspCpu::RspCpu() { reset(); }

void RspCpu::reset() noexcept {
    gpr_.fill(0);
    pc_ = 0;
    pendingBranchTarget_ = 0;
    executed_ = 0;
    for (auto& vector : vr_) vector.fill(0);
    accumulator_.fill({});
    vco_ = 0;
    vcc_ = 0;
    vce_ = 0;
    divIn_ = 0;
    divOut_ = 0;
    divInputPending_ = false;
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
    if (function == 0x1DU) { // VSAR: read one 16-bit accumulator slice.
        if (element > 2U) return false;
        for (unsigned lane = 0; lane < 8U; ++lane) {
            const AccumulatorLane& acc = accumulator_[lane];
            vr_[vd][lane] = element == 0U ? acc.lo
                : (element == 1U ? acc.md : acc.hi);
        }
        return true;
    }
    const bool multiplyFraction = function == 0x00U;
    const bool multiplyUnsigned = function == 0x01U;
    const bool multiplyLow = function == 0x04U;
    const bool multiplyMidSigned = function == 0x05U;
    const bool multiplyMidUnsigned = function == 0x06U;
    const bool multiplyHigh = function == 0x07U;
    const bool multiplyAccumulateFraction = function == 0x08U;
    const bool multiplyAccumulateUnsigned = function == 0x09U;
    const bool multiplyAccumulateLow = function == 0x0CU;
    const bool multiplyAccumulateMidSigned = function == 0x0DU;
    const bool multiplyAccumulateMidUnsigned = function == 0x0EU;
    const bool multiplyAccumulateHigh = function == 0x0FU;
    const bool multiplyFamily = multiplyFraction || multiplyUnsigned || multiplyLow ||
        multiplyMidSigned || multiplyMidUnsigned || multiplyHigh ||
        multiplyAccumulateFraction || multiplyAccumulateUnsigned ||
        multiplyAccumulateLow || multiplyAccumulateMidSigned ||
        multiplyAccumulateMidUnsigned || multiplyAccumulateHigh;
    const bool add = function == 0x10U, sub = function == 0x11U;
    const bool absolute = function == 0x13U;
    const bool addCarry = function == 0x14U, subCarry = function == 0x15U;
    const bool compareLess = function == 0x20U;
    const bool compareEqual = function == 0x21U;
    const bool compareNotEqual = function == 0x22U;
    const bool compareGreaterEqual = function == 0x23U;
    const bool clipLow = function == 0x24U;
    const bool clipHigh = function == 0x25U;
    const bool clipReverse = function == 0x26U;
    const bool merge = function == 0x27U;
    const bool comparison = compareLess || compareEqual || compareNotEqual ||
        compareGreaterEqual;
    const bool logical = function >= 0x28U && function <= 0x2DU;
    if (!multiplyFamily && !add && !sub && !absolute && !addCarry && !subCarry &&
        !comparison && !clipLow && !clipHigh && !clipReverse && !merge && !logical) return false;
    std::uint16_t nextVco = 0;
    std::uint16_t nextVcc = clipLow ? vcc_ : 0U;
    std::uint8_t nextVce = 0;
    for (unsigned lane = 0; lane < 8U; ++lane) {
        const std::uint16_t left = vr_[vs][lane];
        const std::uint16_t right = vr_[vt][kElementMap[element][lane]];
        std::uint16_t value = 0;
        if (multiplyFamily) {
            std::int64_t product = 0;
            std::uint64_t previous =
                (static_cast<std::uint64_t>(accumulator_[lane].hi) << 32U) |
                (static_cast<std::uint64_t>(accumulator_[lane].md) << 16U) |
                accumulator_[lane].lo;
            bool accumulate = multiplyAccumulateFraction || multiplyAccumulateUnsigned ||
                multiplyAccumulateLow || multiplyAccumulateMidSigned ||
                multiplyAccumulateMidUnsigned || multiplyAccumulateHigh;
            bool unsignedClamp = false;
            std::int64_t accumulatorValue = 0;
            if (multiplyLow || multiplyAccumulateLow) {
                product = (static_cast<std::int64_t>(left) * right) >> 16U;
                accumulatorValue = product;
                value = static_cast<std::uint16_t>(product);
            } else if (multiplyMidSigned || multiplyAccumulateMidSigned) {
                product = static_cast<std::int64_t>(static_cast<std::int16_t>(left)) * right;
                accumulatorValue = product;
            } else if (multiplyMidUnsigned || multiplyAccumulateMidUnsigned) {
                product = static_cast<std::int64_t>(left) *
                    static_cast<std::int16_t>(right);
                accumulatorValue = product;
                unsignedClamp = true;
            } else if (multiplyHigh || multiplyAccumulateHigh) {
                product = static_cast<std::int64_t>(static_cast<std::int16_t>(left)) *
                    static_cast<std::int16_t>(right) * 65536;
                accumulatorValue = product;
            } else {
                product = static_cast<std::int64_t>(static_cast<std::int16_t>(left)) *
                    static_cast<std::int16_t>(right) * 2 +
                    ((multiplyFraction || multiplyUnsigned) ? 0x8000 : 0);
                accumulate = multiplyAccumulateFraction || multiplyAccumulateUnsigned;
                accumulatorValue = product;
                unsignedClamp = multiplyUnsigned || multiplyAccumulateUnsigned;
            }
            if (multiplyLow || multiplyMidSigned || multiplyMidUnsigned || multiplyHigh) {
                // VMUD* clears the accumulator before storing its partial product.
                accumulate = false;
            }
            const std::uint64_t raw = accumulate
                ? (previous + static_cast<std::uint64_t>(accumulatorValue)) & 0x0000FFFFFFFFFFFFULL
                : static_cast<std::uint64_t>(accumulatorValue) & 0x0000FFFFFFFFFFFFULL;
            accumulator_[lane].lo = static_cast<std::uint16_t>(raw);
            accumulator_[lane].md = static_cast<std::uint16_t>(raw >> 16U);
            accumulator_[lane].hi = static_cast<std::uint16_t>(raw >> 32U);
            const std::int64_t signedAccumulator = (raw & (1ULL << 47U)) != 0U
                ? static_cast<std::int64_t>(raw) - (1LL << 48U)
                : static_cast<std::int64_t>(raw);
            const std::int64_t shifted = signedAccumulator >= 0
                ? signedAccumulator / 65536
                : -((-signedAccumulator + 65535) / 65536);
            if (multiplyLow) {
                value = static_cast<std::uint16_t>(raw);
            } else if (multiplyAccumulateLow || multiplyMidUnsigned ||
                       multiplyAccumulateMidUnsigned) {
                if (signedAccumulator < 0) value = 0U;
                else if (signedAccumulator > 65535) value = 0xFFFFU;
                else value = static_cast<std::uint16_t>(signedAccumulator);
            } else if (multiplyMidSigned || multiplyAccumulateMidSigned) {
                if (shifted < -32768) value = 0x8000U;
                else if (shifted > 32767) value = 0x7FFFU;
                else value = static_cast<std::uint16_t>(shifted);
            } else if (multiplyHigh || multiplyAccumulateHigh) {
                if (shifted < -32768) value = 0x8000U;
                else if (shifted > 32767) value = 0x7FFFU;
                else value = static_cast<std::uint16_t>(shifted);
            } else if (unsignedClamp) {
                if (shifted < 0) value = 0U;
                else if (shifted > 32767) value = 0xFFFFU;
                else value = static_cast<std::uint16_t>(shifted);
            } else {
                if (shifted < -32768) value = 0x8000U;
                else if (shifted > 32767) value = 0x7FFFU;
                else value = static_cast<std::uint16_t>(shifted);
            }
        } else if (comparison) {
            const std::int32_t a = static_cast<std::int16_t>(left);
            const std::int32_t b = static_cast<std::int16_t>(right);
            const bool equal = a == b;
            const bool vcoLow = (vco_ & (1U << lane)) != 0U;
            const bool vcoHigh = (vco_ & (1U << (lane + 8U))) != 0U;
            bool selected = false;
            if (compareLess) selected = a < b || (equal && vcoLow && vcoHigh);
            else if (compareEqual) selected = equal && !vcoHigh;
            else if (compareNotEqual) selected = !equal || vcoHigh;
            else selected = a > b || (equal && !(vcoLow && vcoHigh));
            if (selected) nextVcc |= static_cast<std::uint16_t>(1U << lane);
            value = selected ? left : right;
            accumulator_[lane].lo = value;
        } else if (clipLow) {
            const bool vcoLow = (vco_ & (1U << lane)) != 0U;
            const bool vcoHigh = (vco_ & (1U << (lane + 8U))) != 0U;
            const bool vce = (vce_ & (1U << lane)) != 0U;
            const std::uint16_t negatedRight = static_cast<std::uint16_t>(0U - right);
            if (!vcoLow && !vcoHigh) {
                const std::uint16_t bit = static_cast<std::uint16_t>(1U << (lane + 8U));
                if (left >= right) nextVcc |= bit;
                else nextVcc &= static_cast<std::uint16_t>(~bit);
            }
            if (vcoLow && !vcoHigh) {
                const bool lessEqual = left <= negatedRight;
                const bool equal = left == negatedRight;
                const bool selected = vce ? lessEqual : equal;
                const std::uint16_t bit = static_cast<std::uint16_t>(1U << lane);
                if (selected) nextVcc |= bit;
                else nextVcc &= static_cast<std::uint16_t>(~bit);
            }
            const bool selected = vcoLow
                ? (nextVcc & (1U << lane)) != 0U
                : (nextVcc & (1U << (lane + 8U))) != 0U;
            const std::uint16_t clipBound = vcoLow ? negatedRight : right;
            value = selected ? clipBound : left;
            accumulator_[lane].lo = value;
        } else if (clipHigh || clipReverse) {
            const bool oppositeSigns = ((left ^ right) & 0x8000U) != 0U;
            const std::uint16_t negatedRight = clipReverse
                ? static_cast<std::uint16_t>(~right)
                : static_cast<std::uint16_t>(0U - right);
            const std::uint16_t clipBound = oppositeSigns ? negatedRight : right;
            const bool extensionEqual = clipHigh && oppositeSigns &&
                left == static_cast<std::uint16_t>(negatedRight - 1U);
            const bool notEqual = !extensionEqual && left != clipBound;
            const bool lessEqualNegative = static_cast<std::int16_t>(left) <=
                static_cast<std::int16_t>(negatedRight);
            const bool greaterEqualPositive = static_cast<std::int16_t>(left) >=
                static_cast<std::int16_t>(right);
            if (oppositeSigns) nextVco |= static_cast<std::uint16_t>(1U << lane);
            if (notEqual) nextVco |= static_cast<std::uint16_t>(1U << (lane + 8U));
            if (extensionEqual) nextVce |= static_cast<std::uint8_t>(1U << lane);
            if (lessEqualNegative) nextVcc |= static_cast<std::uint16_t>(1U << lane);
            if (greaterEqualPositive) nextVcc |= static_cast<std::uint16_t>(1U << (lane + 8U));
            const bool selected = oppositeSigns ? lessEqualNegative : greaterEqualPositive;
            value = selected ? clipBound : left;
            accumulator_[lane].lo = value;
        } else if (merge) {
            const bool selected = (vcc_ & (1U << lane)) != 0U;
            value = selected ? left : right;
            accumulator_[lane].lo = value;
        } else if (add || sub) {
            const std::int32_t a = static_cast<std::int16_t>(left);
            const std::int32_t b = static_cast<std::int16_t>(right);
            const std::int32_t carry = (vco_ & (1U << lane)) != 0U ? 1 : 0;
            const std::int32_t sum = add ? a + b + carry : a - b - carry;
            accumulator_[lane].lo = static_cast<std::uint16_t>(sum);
            value = static_cast<std::uint16_t>(std::max(-32768, std::min(32767, sum)));
        } else if (absolute) {
            const std::int16_t sign = static_cast<std::int16_t>(left);
            if (sign > 0) {
                value = right;
                accumulator_[lane].lo = value;
            } else if (sign < 0) {
                if (right == 0x8000U) {
                    value = 0x7FFFU;
                    accumulator_[lane].lo = 0x8000U;
                } else {
                    value = static_cast<std::uint16_t>(
                        -static_cast<std::int16_t>(right));
                    accumulator_[lane].lo = value;
                }
            } else {
                value = 0U;
                accumulator_[lane].lo = 0U;
            }
        } else if (addCarry) {
            const std::uint32_t sum = static_cast<std::uint32_t>(left) + right;
            accumulator_[lane].lo = static_cast<std::uint16_t>(sum);
            value = static_cast<std::uint16_t>(sum);
            if (sum > 0xFFFFU) nextVco |= static_cast<std::uint16_t>(1U << lane);
        } else if (subCarry) {
            const std::uint32_t diff = static_cast<std::uint32_t>(left) - right;
            accumulator_[lane].lo = static_cast<std::uint16_t>(diff);
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
            accumulator_[lane].lo = value;
        }
        vr_[vd][lane] = value;
    }
    if (add || sub || addCarry || subCarry || comparison || clipHigh || clipReverse)
        vco_ = nextVco;
    if (comparison || clipLow || clipHigh || clipReverse) vcc_ = nextVcc;
    if (clipHigh || clipReverse) vce_ = nextVce;
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
            const unsigned element = rs & 0x0FU;
            const unsigned vd = (instruction >> 6U) & 31U;
            if (function == 0x37U) { // VNOP has no architectural side effects.
                // Deliberately do nothing.
            } else if (function >= 0x30U && function <= 0x36U) {
                const unsigned destinationLane = rd & 7U;
                const auto sourceVector = vr_[rt];
                for (unsigned lane = 0; lane < 8U; ++lane)
                    accumulator_[lane].lo = sourceVector[kElementMap[element][lane]];

                if (function == 0x33U) { // VMOV
                    const unsigned sourceLane = kElementMap[element][destinationLane];
                    vr_[vd][destinationLane] = sourceVector[sourceLane];
                } else if (function == 0x32U || function == 0x36U) { // VRCPH / VRSQH
                    divIn_ = sourceVector[kElementMap[element][destinationLane]];
                    divInputPending_ = true;
                    vr_[vd][destinationLane] = divOut_;
                } else {
                    const unsigned sourceLane = kElementMap[element][destinationLane];
                    const bool low = function == 0x31U || function == 0x35U;
                    const bool squareRoot = function == 0x34U || function == 0x35U;
                    const std::uint32_t input = low && divInputPending_
                        ? (static_cast<std::uint32_t>(divIn_) << 16U) | sourceVector[sourceLane]
                        : signExtend16(sourceVector[sourceLane]);
                    const std::uint32_t result = squareRoot
                        ? inverseSquareRootLookup(input) : reciprocalLookup(input);
                    divOut_ = static_cast<std::uint16_t>(result >> 16U);
                    divInputPending_ = false;
                    vr_[vd][destinationLane] = static_cast<std::uint16_t>(result);
                }
            } else if (!executeVector(element, rt, rd, vd, function)) {
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
    } else if (opcode == 0x32U || opcode == 0x3AU) {
        // Initial RSP vector memory transfers: byte, short, word, and doubleword.
        const unsigned subOpcode = (instruction >> 11U) & 31U;
        if (subOpcode > 3U) {
            halted_ = true; faulted_ = true;
            return false;
        }
        const unsigned element = (instruction >> 7U) & 0x0FU;
        const unsigned byteCount = 1U << subOpcode;
        std::int32_t offset = static_cast<std::int32_t>(instruction & 0x7FU);
        if ((offset & 0x40) != 0) offset -= 0x80;
        const std::uint32_t address = static_cast<std::uint32_t>(
            static_cast<std::int64_t>(gpr_[rs]) +
            static_cast<std::int64_t>(offset) * byteCount) & 0x0FFFU;
        const bool load = opcode == 0x32U;
        for (unsigned byte = 0; byte < byteCount; ++byte) {
            const unsigned vectorByte = load ? element + byte : (element + byte) & 0x0FU;
            if (load && vectorByte >= 16U) break;
            const std::uint32_t dmemAddress = Bus::kSpDmemBase +
                ((address + byte) & 0x0FFFU);
            if (load) {
                std::uint8_t value = 0;
                if (!bus.read8(dmemAddress, value)) {
                    halted_ = true; faulted_ = true;
                    return false;
                }
                writeVectorByte(rt, vectorByte, value);
            } else {
                if (!bus.write8(dmemAddress, readVectorByte(rt, vectorByte))) {
                    halted_ = true; faulted_ = true;
                    return false;
                }
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
