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

private:
    std::array<std::uint32_t, 32> gpr_{};
    std::uint32_t pc_ = 0;
    std::uint32_t pendingBranchTarget_ = 0;
    std::uint64_t executed_ = 0;
    bool pendingBranch_ = false;
    bool halted_ = false;
};
} // namespace m64plus::n64
