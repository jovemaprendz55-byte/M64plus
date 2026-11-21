#include "n64/cp0.h"

namespace m64plus::n64 {
namespace {
constexpr std::uint64_t kCauseBd = 1ULL << 31U;
constexpr std::uint64_t kCauseExcCodeMask = 0x1FULL << 2U;
constexpr std::uint64_t kCauseSoftwareInterruptMask = 0x3ULL << 8U;
constexpr std::uint64_t kCauseTimerInterrupt = 1ULL << 15U;
constexpr std::uint64_t kStatusWritableMask = 0xFFFFFFFFULL;
}

Cp0::Cp0() { reset(); }

void Cp0::reset() noexcept {
    registers_.fill(0);
}

std::uint64_t Cp0::read(unsigned index) const noexcept {
    return index < registers_.size() ? registers_[index] : 0;
}

bool Cp0::write(unsigned index, std::uint64_t value) noexcept {
    if (index >= registers_.size()) return false;
    switch (index) {
    case kBadVAddr:
    case kEpc:
        registers_[index] = value;
        return true;
    case kCount:
        registers_[index] = static_cast<std::uint32_t>(value);
        return true;
    case kCompare:
        registers_[index] = static_cast<std::uint32_t>(value);
        registers_[kCause] &= ~kCauseTimerInterrupt;
        return true;
    case kStatus:
        registers_[index] = value & kStatusWritableMask;
        return true;
    case kCause:
        // Software may change only the two software interrupt request bits.
        registers_[index] = (registers_[index] & ~kCauseSoftwareInterruptMask) |
                            (value & kCauseSoftwareInterruptMask);
        return true;
    default:
        return false;
    }
}

void Cp0::tick() noexcept {
    const std::uint32_t next = static_cast<std::uint32_t>(registers_[kCount]) + 1U;
    registers_[kCount] = next;
    if (next == static_cast<std::uint32_t>(registers_[kCompare])) {
        registers_[kCause] |= kCauseTimerInterrupt;
    }
}

std::uint64_t Cp0::raiseException(ExceptionCode code, std::uint64_t instructionPc,
                                  bool inDelaySlot, std::uint64_t badAddress) noexcept {
    if ((registers_[kStatus] & kStatusExl) == 0) {
        registers_[kEpc] = inDelaySlot ? instructionPc - 4U : instructionPc;
        if (inDelaySlot) registers_[kCause] |= kCauseBd;
        else registers_[kCause] &= ~kCauseBd;
    }
    registers_[kCause] = (registers_[kCause] & ~kCauseExcCodeMask) |
        (static_cast<std::uint64_t>(code) << 2U);
    if (code == ExceptionCode::AddressLoad || code == ExceptionCode::AddressStore ||
        code == ExceptionCode::TlbLoad || code == ExceptionCode::TlbStore ||
        code == ExceptionCode::TlbModification) {
        registers_[kBadVAddr] = badAddress;
    }
    registers_[kStatus] |= kStatusExl;
    return (registers_[kStatus] & kStatusBev) != 0 ? 0xFFFFFFFFBFC00380ULL
                                                   : 0xFFFFFFFF80000180ULL;
}

std::uint64_t Cp0::exceptionReturn() noexcept {
    const std::uint64_t target = registers_[kEpc];
    registers_[kStatus] &= ~kStatusExl;
    return target;
}

bool Cp0::exceptionLevel() const noexcept {
    return (registers_[kStatus] & kStatusExl) != 0;
}

} // namespace m64plus::n64
