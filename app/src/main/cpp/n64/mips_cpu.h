#pragma once
#include <array>
#include <cstdint>
namespace m64plus::n64 {
class Rdram;
class MipsCpu final {
public:
    MipsCpu();
    void reset(std::uint64_t pc = 0);
    bool step(const Rdram& memory);
    std::uint64_t reg(unsigned index) const noexcept;
    std::uint64_t pc() const noexcept;
    std::uint64_t instructionsExecuted() const noexcept;
private:
    std::array<std::uint64_t, 32> gpr_{};
    std::uint64_t pc_ = 0;
    std::uint64_t executed_ = 0;
    bool pendingBranch_ = false;
    std::uint64_t pendingBranchTarget_ = 0;
};
}
