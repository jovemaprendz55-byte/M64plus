#include "n64/machine.h"

#include <array>

#include <algorithm>
#include <cstdint>

namespace m64plus::n64 {

N64Machine::N64Machine() { reset(); }

bool N64Machine::bootCartridgeHle() noexcept {
    constexpr std::uint32_t kRomPayloadOffset = 0x1000U;
    constexpr std::size_t kMaxBootPayload = 0x100000U;
    constexpr std::uint32_t kBootDmemBase = Bus::kSpDmemBase;
    constexpr std::uint64_t kBootStatus = 0x34000000ULL;

    if (bus_.cartridgeSize() < kRomPayloadOffset + 4U) return false;
    std::uint32_t entryPoint = 0;
    if (!bus_.read32(Bus::kCartridgeBase + 8U, entryPoint)) return false;
    if (entryPoint < 0x80000000U || entryPoint >= 0xA0000000U ||
        (entryPoint & 3U) != 0U) return false;

    const std::size_t available = bus_.cartridgeSize() - kRomPayloadOffset;
    const std::size_t payloadSize = std::min(available, kMaxBootPayload);
    const std::uint32_t physicalEntry = entryPoint & 0x1FFFFFFFU;
    if (payloadSize == 0U ||
        static_cast<std::uint64_t>(physicalEntry) + payloadSize > Rdram::kSize)
        return false;

    // Identify the IPL3 CIC before reset; the cartridge remains inserted.
    std::array<std::uint8_t, 0x1000U> bootRom{};
    for (std::uint32_t i = 0; i < bootRom.size(); ++i) {
        if (!bus_.read8(Bus::kCartridgeBase + i, bootRom[i])) return false;
    }
    const CicInfo cic = detectCicFromIpl3(bootRom.data(), bootRom.size());
    const TvType tv = detectTvType(bootRom[0x3EU]);
    const std::uint32_t tvType = static_cast<std::uint32_t>(tv);

    // Reset hardware state while preserving the inserted cartridge.
    reset();

    // HLE the PIF/IPL2 handoff: make the cartridge header and IPL3 visible in DMEM.
    for (std::uint32_t i = 0; i < kRomPayloadOffset; ++i) {
        std::uint8_t byte = 0;
        if (!bus_.read8(Bus::kCartridgeBase + i, byte) ||
            !bus_.write8(kBootDmemBase + i, byte)) return false;
    }

    // HLE the common IPL3 action: copy up to 1 MiB from ROM offset 0x1000
    // to the entry point specified by the cartridge header.
    for (std::size_t i = 0; i < payloadSize; ++i) {
        std::uint8_t byte = 0;
        if (!bus_.read8(Bus::kCartridgeBase + kRomPayloadOffset +
                        static_cast<std::uint32_t>(i), byte) ||
            !bus_.rdram().write8(physicalEntry + static_cast<std::uint32_t>(i), byte))
            return false;
    }

    // Common NTSC, cartridge, cold-reset, CIC-6102-compatible boot parameters.
    const auto writeBootWord = [this](std::uint32_t address, std::uint32_t value) {
        return bus_.rdram().write32(address, value);
    };
    if (!writeBootWord(0x300U, tvType) ||    // osTvType from cartridge region
        !writeBootWord(0x304U, 0U) ||       // osRomType: cartridge
        !writeBootWord(0x308U, 0xB0000000U) || // osRomBase
        !writeBootWord(0x30CU, 0U) ||       // osResetType: cold
        !writeBootWord(0x314U, 0U) ||       // IPL2 version
        !writeBootWord(0x318U, static_cast<std::uint32_t>(Rdram::kSize)))
        return false;

    if (cic.osCicId != 0U && !writeBootWord(0x310U, cic.osCicId)) return false;

    const std::uint64_t canonicalEntry = static_cast<std::uint64_t>(
        static_cast<std::int64_t>(static_cast<std::int32_t>(entryPoint)));
    cpu_.reset(canonicalEntry);
    cpu_.setRegister(11U, 0xFFFFFFFFA4000040ULL); // t3: IPL3/DMEM handoff address
    cpu_.setRegister(19U, 0U);                    // s3: cartridge ROM type
    cpu_.setRegister(20U, tvType);                // s4: TV type from ROM region
    cpu_.setRegister(21U, 0U);                    // s5: cold reset
    cpu_.setRegister(22U, cic.seed);              // s6: detected CIC seed (6102 fallback)
    cpu_.setRegister(23U, 0U);                    // s7: IPL2 version
    cpu_.setRegister(29U, 0xFFFFFFFFA4001FF0ULL); // sp: boot stack in DMEM
    return cpu_.setCp0Register(Cp0::kStatus, kBootStatus);
}

void N64Machine::reset() noexcept {
    bus_.reset();
    cpu_.reset();
    rsp_.reset();
    rdp_ = RdpSyncState{};
}

bool N64Machine::stepCpu() noexcept {
    return cpu_.step(bus_);
}

std::size_t N64Machine::runCpuInstructions(std::size_t instructionBudget) noexcept {
    std::size_t executed = 0;
    while (executed < instructionBudget && stepCpu()) ++executed;
    return executed;
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
