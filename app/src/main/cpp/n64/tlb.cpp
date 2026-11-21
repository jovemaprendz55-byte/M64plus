#include "n64/tlb.h"

namespace m64plus::n64 {

Tlb::Tlb() noexcept { reset(); }

void Tlb::reset() noexcept {
    entries_ = {};
}

bool Tlb::writeEntry(std::size_t index, std::uint32_t virtualPairBase,
                     std::uint8_t asid, std::uint32_t evenPfn,
                     std::uint32_t oddPfn, bool evenValid, bool oddValid,
                     bool evenDirty, bool oddDirty, bool global) noexcept {
    if (index >= entries_.size() || (virtualPairBase & kPairMask) != 0U)
        return false;
    Entry entry{};
    entry.vpn2 = virtualPairBase >> 13U;
    entry.asid = asid;
    entry.pfn[0] = evenPfn & 0xFFFFFU;
    entry.pfn[1] = oddPfn & 0xFFFFFU;
    entry.valid[0] = evenValid;
    entry.valid[1] = oddValid;
    entry.dirty[0] = evenDirty;
    entry.dirty[1] = oddDirty;
    entry.global = global;
    entry.occupied = true;
    entries_[index] = entry;
    return true;
}

TlbResult Tlb::translate(std::uint64_t virtualAddress, std::uint8_t asid,
                         bool store, std::uint32_t& physicalAddress) const noexcept {
    const std::uint32_t address = static_cast<std::uint32_t>(virtualAddress);
    const std::uint64_t canonical = static_cast<std::uint64_t>(
        static_cast<std::int64_t>(static_cast<std::int32_t>(address)));
    if (virtualAddress != address && virtualAddress != canonical)
        return TlbResult::Miss;

    // MIPS kernel unmapped segments: KSEG0 and KSEG1 share physical mapping.
    if (address >= 0x80000000U && address < 0xC0000000U) {
        physicalAddress = address & 0x1FFFFFFFU;
        return TlbResult::Success;
    }

    const std::uint32_t vpn2 = address >> 13U;
    const unsigned page = (address >> 12U) & 1U;
    for (const Entry& entry : entries_) {
        if (!entry.occupied || entry.vpn2 != vpn2 ||
            (!entry.global && entry.asid != asid))
            continue;
        if (!entry.valid[page]) return TlbResult::Invalid;
        if (store && !entry.dirty[page]) return TlbResult::Modified;
        physicalAddress = (entry.pfn[page] << 12U) | (address & kPageMask);
        return TlbResult::Success;
    }
    return TlbResult::Miss;
}

std::size_t Tlb::size() const noexcept { return entries_.size(); }

} // namespace m64plus::n64
