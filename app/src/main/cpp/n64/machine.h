#pragma once

#include <cstddef>

#include "n64/bus.h"
#include "n64/mips_cpu.h"
#include "n64/rsp_cpu.h"

namespace m64plus::n64 {

// Shared machine state for the main MIPS CPU and the RSP.
// Scheduling is instruction-budget based for now; hardware cycle timing,
// cartridge boot, and RDP execution are not implemented yet.
class N64Machine final {
public:
    N64Machine();

    void reset() noexcept;
    bool stepCpu() noexcept;
    bool stepRsp() noexcept;
    std::size_t runRsp(std::size_t instructionBudget) noexcept;

    Bus& bus() noexcept;
    const Bus& bus() const noexcept;
    MipsCpu& cpu() noexcept;
    const MipsCpu& cpu() const noexcept;
    RspCpu& rsp() noexcept;
    const RspCpu& rsp() const noexcept;

private:
    Bus bus_{};
    MipsCpu cpu_{};
    RspCpu rsp_{};
};

} // namespace m64plus::n64
