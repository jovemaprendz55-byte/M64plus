#pragma once

#include <cstddef>

#include "n64/bus.h"
#include "n64/mips_cpu.h"
#include "n64/rsp_cpu.h"
#include "n64/rdp.h"

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
    // Consume up to commandBudget RDP commands; unsupported commands are consumed but not rendered.
    std::size_t runRdp(std::size_t commandBudget) noexcept;

    Bus& bus() noexcept;
    const Bus& bus() const noexcept;
    MipsCpu& cpu() noexcept;
    const MipsCpu& cpu() const noexcept;
    RspCpu& rsp() noexcept;
    const RspCpu& rsp() const noexcept;
    RdpSyncState& rdp() noexcept;
    const RdpSyncState& rdp() const noexcept;

private:
    Bus bus_{};
    MipsCpu cpu_{};
    RspCpu rsp_{};
    RdpSyncState rdp_{};
};

} // namespace m64plus::n64
