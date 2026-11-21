#include "n64/machine.h"

namespace m64plus::n64 {

N64Machine::N64Machine() { reset(); }

void N64Machine::reset() noexcept {
    bus_.reset();
    cpu_.reset();
    rsp_.reset();
}

bool N64Machine::stepCpu() noexcept {
    return cpu_.step(bus_);
}

bool N64Machine::stepRsp() noexcept {
    if (bus_.spHalted() || rsp_.halted()) return false;

    std::uint32_t instruction = 0;
    if (!bus_.read32(Bus::kSpImemBase + rsp_.pc(), instruction)) return false;

    const bool executed = rsp_.step(bus_);
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

Bus& N64Machine::bus() noexcept { return bus_; }
const Bus& N64Machine::bus() const noexcept { return bus_; }
MipsCpu& N64Machine::cpu() noexcept { return cpu_; }
const MipsCpu& N64Machine::cpu() const noexcept { return cpu_; }
RspCpu& N64Machine::rsp() noexcept { return rsp_; }
const RspCpu& N64Machine::rsp() const noexcept { return rsp_; }

} // namespace m64plus::n64
