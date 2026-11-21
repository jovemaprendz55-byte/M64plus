#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace m64plus::n64 {

enum class TlbResult : std::uint8_t { Success, Miss, Invalid, Modified };

class Tlb final {
public:
    static constexpr std::size_t kEntryCount = 32;
    static constexpr std::uint32_t kPageSize = 4096;
    static constexpr std::uint32_t kPageMask = kPageSize - 1U;
    static constexpr std::uint32_t kPairMask = (kPageSize * 2U) - 1U;

    Tlb() noexcept;
    void reset() noexcept;
    bool writeEntry(std::size_t index, std::uint32_t virtualPairBase,
                    std::uint8_t asid, std::uint32_t evenPfn,
                    std::uint32_t oddPfn, bool evenValid, bool oddValid,
                    bool evenDirty, bool oddDirty, bool global) noexcept;
    TlbResult translate(std::uint64_t virtualAddress, std::uint8_t asid,
                        bool store, std::uint32_t& physicalAddress) const noexcept;
    std::size_t size() const noexcept;

private:
    struct Entry {
        std::uint32_t vpn2 = 0;
        std::uint8_t asid = 0;
        std::uint32_t pfn[2]{};
        bool valid[2]{};
        bool dirty[2]{};
        bool global = false;
        bool occupied = false;
    };
    std::array<Entry, kEntryCount> entries_{};
};

} // namespace m64plus::n64
