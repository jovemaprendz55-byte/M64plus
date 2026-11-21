#pragma once

#include <array>
#include <cstdint>

namespace m64plus::n64 {
class Bus;

// Initial scalar RSP interpreter. Vector operations and COP0 DMA scheduling
// are intentionally implemented in later stages.
class RspCpu final {
public:
    RspCpu();
    void reset() noexcept;
    bool step(Bus& bus) noexcept;
    std::uint32_t reg(unsigned index) const noexcept;
    std::uint32_t pc() const noexcept;
    bool halted() const noexcept;
    std::uint64_t instructionsExecuted() const noexcept;
    std::uint16_t vectorLane(unsigned reg, unsigned lane) const noexcept;
    void setVectorLane(unsigned reg, unsigned lane, std::uint16_t value) noexcept;
    std::uint16_t vectorCarryFlags() const noexcept;
    std::uint16_t vectorCompareFlags() const noexcept;
    std::uint8_t vectorClipFlags() const noexcept;

private:
    bool executeVector(unsigned element, unsigned vt, unsigned vs, unsigned vd, unsigned function) noexcept;
    std::array<std::uint32_t, 32> gpr_{};
    std::array<std::array<std::uint16_t, 8>, 32> vr_{};
    std::array<std::int64_t, 8> accumulator_{};
    std::uint16_t vco_ = 0;
    std::uint16_t vcc_ = 0;
    std::uint8_t vce_ = 0;
    std::uint32_t pc_ = 0;
    std::uint32_t pendingBranchTarget_ = 0;
    std::uint64_t executed_ = 0;
    bool pendingBranch_ = false;
    bool halted_ = false;
};
} // namespace m64plus::n64
