#pragma once
#include <array>
#include <cstdint>
#include "n64/cp0.h"
#include "n64/tlb.h"
namespace m64plus::n64 {
class Rdram;
class Bus;
class MipsCpu final {
public:
    MipsCpu();
    void reset(std::uint64_t pc = 0);
    bool step(Rdram& memory);
    bool step(Bus& memory);
    void setTlbEnabled(bool enabled) noexcept;
    void setRegister(unsigned index, std::uint64_t value) noexcept;
    bool setCp0Register(unsigned index, std::uint64_t value) noexcept;
    std::uint64_t reg(unsigned index) const noexcept;
    std::uint64_t pc() const noexcept;
    std::uint64_t instructionsExecuted() const noexcept;
    const Cp0& cp0() const noexcept;
private:
    template <typename Memory> bool stepMemory(Memory& memory);
    std::array<std::uint64_t, 32> gpr_{};
    std::uint64_t pc_ = 0;
    std::uint64_t executed_ = 0;
    bool pendingBranch_ = false;
    std::uint64_t pendingBranchTarget_ = 0;
    Cp0 cp0_{};
    Tlb tlb_{};
    bool tlbEnabled_ = false;
};
}
