#pragma once

#include <cstddef>

#include "n64/bus.h"
#include "n64/cic.h"
#include "n64/mips_cpu.h"
#include "n64/rsp_cpu.h"
#include "n64/rdp.h"
#include "n64/event_scheduler.h"

namespace m64plus::n64 {

// Shared machine state for the main MIPS CPU and the RSP.
// Execution is bounded by instruction/command budgets. Cartridge startup uses
// an HLE handoff profile, and the RDP command processor handles a limited subset;
// cycle-accurate scheduling and complete device emulation are still pending.
class N64Machine final {
public:
    N64Machine();

    void reset() noexcept;
    // Start a cartridge through a documented CIC-6102/NTSC-style HLE boot profile.
    bool bootCartridgeHle() noexcept;
    bool stepCpu() noexcept;
    // Execute at most instructionBudget CPU steps; never runs without a bound.
    std::size_t runCpuInstructions(std::size_t instructionBudget) noexcept;
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
    EventScheduler& eventScheduler() noexcept;
    const EventScheduler& eventScheduler() const noexcept;

private:
    Bus bus_{};
    MipsCpu cpu_{};
    RspCpu rsp_{};
    RdpSyncState rdp_{};
    EventScheduler eventScheduler_{};
};

} // namespace m64plus::n64
