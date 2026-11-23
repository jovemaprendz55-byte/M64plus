#include "n64/mips_cpu.h"
#include "n64/rdram.h"
#include "n64/bus.h"
#include "n64/cp0.h"
#include "n64/tlb.h"
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
    const auto takeException = [&](ExceptionCode code, std::uint64_t badAddress = 0, bool tlbRefill = false) {
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
        return takeException(ExceptionCode::BusInstruction, pc_);
    const std::uint32_t pc32 = static_cast<std::uint32_t>(pc_);
    const bool pcInDirectSegment = pc32 >= 0x80000000U && pc32 < 0xC0000000U;
    // KSEG0/KSEG1 are unmapped segments: their direct mapping applies even
    // while the software-managed TLB is disabled.
    if (tlbEnabled_ || pcInDirectSegment) {
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
    bool doExceptionReturn = false;
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
            if (inDelaySlot) return takeException(ExceptionCode::ReservedInstruction);
            scheduleBranch = true; branchTarget = a; break;
        case 0x09:
            if (inDelaySlot) return takeException(ExceptionCode::ReservedInstruction);
            if ((a & 3U) != 0) return takeException(ExceptionCode::AddressLoad, a);
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
        default: return takeException(ExceptionCode::ReservedInstruction);
        }
        writeRegister = writeRegister || (function != 0x08 && function != 0x09);
        break;
    case 0x01: {
        if (inDelaySlot) return takeException(ExceptionCode::ReservedInstruction);
        const auto signedA = static_cast<std::int64_t>(a);
        bool take = false;
        switch (rt) {
        case 0x00: take = signedA < 0; break;
        case 0x01: take = signedA >= 0; break;
        case 0x10: take = signedA < 0; destination = 31; result = sequentialPc + 4U; writeRegister = true; break;
        case 0x11: take = signedA >= 0; destination = 31; result = sequentialPc + 4U; writeRegister = true; break;
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
        if (inDelaySlot) return takeException(ExceptionCode::ReservedInstruction);
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
    // COP0: MFC0, MTC0 and ERET (register select 0 only).
    case 0x10:
        if (inDelaySlot)
            return takeException(ExceptionCode::ReservedInstruction);
        if (rs == 0x00) { // MFC0, select 0 only
            if ((instruction & 7U) != 0)
                return takeException(ExceptionCode::ReservedInstruction);
            destination = rt;
            result = signExtend32(static_cast<std::uint32_t>(cp0_.read(rd)));
            writeRegister = true;
        } else if (rs == 0x04) { // MTC0, select 0 only
            if ((instruction & 7U) != 0 || !cp0_.write(rd, gpr_[rt]))
                return takeException(ExceptionCode::ReservedInstruction);
        } else if (rs == 0x10) { // COP0 privileged operations
            if (function == 0x18) { // ERET
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
    // Integer loads use the selected physical memory interface; optional TLB
    // translation is applied before the bus or RDRAM access.
    case 0x20: case 0x21: case 0x23: case 0x24: case 0x25: case 0x27: case 0x37: {
        const std::uint64_t effective = a + signExtend16(immediate);
        std::uint32_t address = static_cast<std::uint32_t>(effective);
        if (effective > 0xFFFFFFFFULL &&
            effective != static_cast<std::uint64_t>(static_cast<std::int64_t>(
                static_cast<std::int32_t>(address))))
            return takeException(ExceptionCode::AddressLoad, effective);
        const bool inDirectSegment = address >= 0x80000000U && address < 0xC0000000U;
        if (tlbEnabled_ || inDirectSegment) {
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
        if ((opcode == 0x21 || opcode == 0x25) && (address & 1U)) return takeException(ExceptionCode::AddressLoad, effective);
        if ((opcode == 0x23 || opcode == 0x27) && (address & 3U)) return takeException(ExceptionCode::AddressLoad, effective);
        if (opcode == 0x37 && (address & 7U)) return takeException(ExceptionCode::AddressLoad, effective);
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
        } else if (opcode == 0x23 || opcode == 0x27) {
            std::uint32_t value = 0;
            if (!memory.read32(address, value)) return takeException(ExceptionCode::BusData, effective);
            result = opcode == 0x23 ? signExtend32(value) : value;
        } else {
            if (!memory.read64(address, result)) return takeException(ExceptionCode::BusData, effective);
        }
        destination = rt;
        writeRegister = true;
        break;
    }
    // Integer stores.
    case 0x28: case 0x29: case 0x2B: case 0x3F: {
        const std::uint64_t effective = a + signExtend16(immediate);
        std::uint32_t address = static_cast<std::uint32_t>(effective);
        if (effective > 0xFFFFFFFFULL &&
            effective != static_cast<std::uint64_t>(static_cast<std::int64_t>(
                static_cast<std::int32_t>(address))))
            return takeException(ExceptionCode::AddressStore, effective);
        const bool inDirectSegment = address >= 0x80000000U && address < 0xC0000000U;
        if (tlbEnabled_ || inDirectSegment) {
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
        if (opcode == 0x29 && (address & 1U)) return takeException(ExceptionCode::AddressStore, effective);
        if (opcode == 0x2B && (address & 3U)) return takeException(ExceptionCode::AddressStore, effective);
        if (opcode == 0x3F && (address & 7U)) return takeException(ExceptionCode::AddressStore, effective);
        bool stored = false;
        if (opcode == 0x28) stored = memory.write8(address, static_cast<std::uint8_t>(b));
        else if (opcode == 0x29) stored = memory.write16(address, static_cast<std::uint16_t>(b));
        else if (opcode == 0x2B) stored = memory.write32(address, static_cast<std::uint32_t>(b));
        else stored = memory.write64(address, b);
        if (!stored) return takeException(ExceptionCode::BusData, effective);
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
