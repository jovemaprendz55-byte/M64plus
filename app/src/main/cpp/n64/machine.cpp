#include "n64/machine.h"

namespace m64plus::n64 {

N64Machine::N64Machine() { reset(); }

void N64Machine::reset() noexcept {
    bus_.reset();
    cpu_.reset();
    rsp_.reset();
    rdp_ = RdpSyncState{};
}

bool N64Machine::stepCpu() noexcept {
    return cpu_.step(bus_);
}

bool N64Machine::stepRsp() noexcept {
    if (bus_.spHalted()) return false;
    if (rsp_.pc() != bus_.spProgramCounter()) {
        rsp_.setPc(bus_.spProgramCounter());
    } else if (rsp_.halted()) {
        // A fault is latched until software changes SP_PC or resets the RSP.
        // A normal BREAK may start a new task after software clears SP HALT.
        if (rsp_.faulted()) return false;
        rsp_.setPc(bus_.spProgramCounter());
    }

    std::uint32_t instruction = 0;
    if (!bus_.read32(Bus::kSpImemBase + rsp_.pc(), instruction)) return false;

    const bool executed = rsp_.step(bus_);
    if (executed) bus_.setSpProgramCounter(rsp_.pc());
    if (executed && rsp_.halted() &&
        (instruction >> 26U) == 0U && (instruction & 0x3FU) == 0x0DU) {
        bus_.signalSpBreak();
    }
    return executed;
}

std::size_t N64Machine::runRsp(std::size_t instructionBudget) noexcept {
    std::size_t executed = 0;
    while (executed < instructionBudget && stepRsp()) ++executed;
    return executed;
}

std::size_t N64Machine::runRdp(std::size_t commandBudget) noexcept {
    std::size_t consumed = 0;
    while (consumed < commandBudget) {
        const RdpProcessResult result = RdpCommandProcessor::processNext(bus_, rdp_);
        if (result == RdpProcessResult::NoCommand) break;
        ++consumed;
    }
    return consumed;
}

Bus& N64Machine::bus() noexcept { return bus_; }
const Bus& N64Machine::bus() const noexcept { return bus_; }
MipsCpu& N64Machine::cpu() noexcept { return cpu_; }
const MipsCpu& N64Machine::cpu() const noexcept { return cpu_; }
RspCpu& N64Machine::rsp() noexcept { return rsp_; }
const RspCpu& N64Machine::rsp() const noexcept { return rsp_; }
RdpSyncState& N64Machine::rdp() noexcept { return rdp_; }
const RdpSyncState& N64Machine::rdp() const noexcept { return rdp_; }

} // namespace m64plus::n64
