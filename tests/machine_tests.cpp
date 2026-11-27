#include "n64/machine.h"
#include "n64/event_scheduler.h"
#include "n64/n64_clock.h"
#include "n64/cic.h"
#include "n64/cp0.h"
#include "n64/tlb.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

using m64plus::n64::Bus;
using m64plus::n64::N64Machine;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}


void testCp0EffectivePrivilegeControls64BitMode() {
    using m64plus::n64::Cp0;
    Cp0 cp0;

    require(!cp0.supports64BitMode(),
            "reset Kernel mode starts with 64-bit mode disabled when KX is clear");
    require(cp0.write(Cp0::kStatus, Cp0::kStatusKx),
            "enable 64-bit mode for Kernel privilege");
    require(cp0.kernelMode() && cp0.supports64BitMode(),
            "Kernel mode uses KX to enable 64-bit operations");

    require(cp0.write(Cp0::kStatus, (1ULL << 3U) | Cp0::kStatusSx),
            "select Supervisor privilege and enable SX");
    require(cp0.supervisorMode() && cp0.supports64BitMode(),
            "Supervisor mode uses SX to enable 64-bit operations");
    require(cp0.write(Cp0::kStatus, 1ULL << 3U),
            "clear SX while remaining in Supervisor privilege");
    require(!cp0.supports64BitMode(),
            "Supervisor mode remains 32-bit when SX is clear");

    require(cp0.write(Cp0::kStatus, (2ULL << 3U) | Cp0::kStatusUx),
            "select User privilege and enable UX");
    require(!cp0.kernelMode() && !cp0.supervisorMode() &&
            cp0.supports64BitMode(),
            "User mode uses UX to enable 64-bit operations");
    require(cp0.write(Cp0::kStatus, 2ULL << 3U),
            "clear UX while remaining in User privilege");
    require(!cp0.supports64BitMode(),
            "User mode remains 32-bit when UX is clear");

    require(cp0.write(Cp0::kStatus, (2ULL << 3U) | Cp0::kStatusUx |
                                      Cp0::kStatusExl),
            "set EXL while saved KSU selects User mode");
    require(cp0.kernelMode() && !cp0.supports64BitMode(),
            "EXL overrides User privilege and uses Kernel KX, not UX");
    require(cp0.write(Cp0::kStatus, (2ULL << 3U) | Cp0::kStatusKx |
                                      Cp0::kStatusErl),
            "set ERL while saved KSU selects User mode");
    require(cp0.kernelMode() && cp0.supports64BitMode(),
            "ERL overrides User privilege and uses Kernel KX");

    require(cp0.write(Cp0::kStatus, (3ULL << 3U) | Cp0::kStatusKx |
                                      Cp0::kStatusSx | Cp0::kStatusUx),
            "select reserved KSU encoding with all mode-enable bits set");
    require(!cp0.kernelMode() && !cp0.supervisorMode() &&
            !cp0.supports64BitMode(),
            "reserved KSU encoding does not grant a valid 64-bit mode");
}

void testCpuMips3InstructionModeRestrictions() {
    N64Machine machine;
    Bus& bus = machine.bus();
    constexpr std::uint32_t daddi = 0x61090001U; // DADDI t1,t0,1
    require(bus.write32(0U, daddi), "write DADDI mode-check instruction");

    auto& cpu = machine.cpu();
    cpu.reset(0xFFFFFFFF80000000ULL);
    cpu.setRegister(8U, 0x123456789ABCDEF0ULL);
    require(machine.stepCpu(), "32-bit Kernel mode executes MIPS III DADDI");
    require(!cpu.hasLastException() &&
            cpu.reg(9U) == 0x123456789ABCDEF1ULL,
            "Kernel mode permits MIPS III instructions even when KX is clear");

    cpu.reset(0U);
    require(cpu.setCp0Register(m64plus::n64::Cp0::kStatus, 1ULL << 3U),
            "select 32-bit Supervisor mode with SX clear");
    require(machine.stepCpu(), "32-bit Supervisor mode handles DADDI as Reserved Instruction");
    require(cpu.hasLastException() && cpu.lastExceptionCode() == 10U,
            "DADDI raises Reserved Instruction in 32-bit Supervisor mode");

    cpu.reset(0U);
    require(cpu.setCp0Register(m64plus::n64::Cp0::kStatus,
                               (1ULL << 3U) | m64plus::n64::Cp0::kStatusSx),
            "enable 64-bit Supervisor mode with SX");
    cpu.setRegister(8U, 0x123456789ABCDEF0ULL);
    require(machine.stepCpu(), "64-bit Supervisor mode executes DADDI");
    require(!cpu.hasLastException() &&
            cpu.reg(9U) == 0x123456789ABCDEF1ULL,
            "Supervisor mode executes MIPS III DADDI when SX is enabled");

    cpu.reset(0U);
    require(cpu.setCp0Register(m64plus::n64::Cp0::kStatus, 2ULL << 3U),
            "select 32-bit User mode with UX clear");
    cpu.setRegister(8U, 0x123456789ABCDEF0ULL);
    require(machine.stepCpu(), "32-bit User mode handles DADDI as Reserved Instruction");
    require(cpu.hasLastException() && cpu.lastExceptionCode() == 10U,
            "DADDI raises Reserved Instruction in 32-bit User mode");
    require(cpu.reg(9U) == 0U,
            "rejected User-mode DADDI does not modify its destination register");

    cpu.reset(0U);
    require(cpu.setCp0Register(m64plus::n64::Cp0::kStatus,
                               (2ULL << 3U) | m64plus::n64::Cp0::kStatusUx),
            "enable 64-bit User mode with UX");
    cpu.setRegister(8U, 0x123456789ABCDEF0ULL);
    require(machine.stepCpu(), "64-bit User mode executes DADDI");
    require(!cpu.hasLastException() &&
            cpu.reg(9U) == 0x123456789ABCDEF1ULL,
            "User mode executes MIPS III DADDI when UX is enabled");
}


void testCpuCop0DoublewordTransfers() {
    N64Machine machine;
    Bus& bus = machine.bus();
    auto& cpu = machine.cpu();
    constexpr std::uint64_t value = 0x123456789ABCDEF0ULL;
    const auto cop0 = [](unsigned move, unsigned rt, unsigned rd) {
        return (0x10U << 26U) | (move << 21U) | (rt << 16U) | (rd << 11U);
    };

    cpu.reset(0xFFFFFFFF80000000ULL);
    cpu.setRegister(8U, value);
    require(bus.write32(0U, cop0(5U, 8U, m64plus::n64::Cp0::kEpc)),
            "write DMTC0 instruction");
    require(bus.write32(4U, cop0(1U, 9U, m64plus::n64::Cp0::kEpc)),
            "write DMFC0 instruction");
    require(machine.stepCpu(), "Kernel mode executes DMTC0");
    require(machine.stepCpu(), "Kernel mode executes DMFC0");
    require(!cpu.hasLastException() && cpu.reg(9U) == value,
            "DMTC0 and DMFC0 preserve the full 64-bit CP0 value");

    cpu.reset(0U);
    require(cpu.setCp0Register(m64plus::n64::Cp0::kStatus, 1ULL << 3U),
            "select Supervisor mode for COP0 privilege test");
    require(bus.write32(0U, cop0(1U, 9U, m64plus::n64::Cp0::kEpc)),
            "write Supervisor DMFC0 instruction");
    require(machine.stepCpu(), "Supervisor DMFC0 raises a handled exception");
    require(cpu.hasLastException() && cpu.lastExceptionCode() == 11U,
            "DMFC0 remains privileged and raises Coprocessor Unusable outside Kernel mode");
}

void testN64ClockTimebaseUsesExactIntegerRatios() {
    using namespace m64plus::n64;

    require(kMasterTicksPerSecond == 187'500'000ULL,
            "master timebase represents 187.5 MHz");
    require(kCpuTicksPerCycle == 2ULL,
            "CPU cycle maps to two master ticks");
    require(kRcpTicksPerCycle == 3ULL && kRspTicksPerCycle == 3ULL,
            "RCP and RSP cycles map to three master ticks");
    require(kCp0CountTicksPerIncrement == 4ULL,
            "CP0 Count increment maps to four master ticks");
    require(kMasterTicksPerSecond / kCpuTicksPerCycle == 93'750'000ULL,
            "master timebase derives the 93.75 MHz CPU clock");
    require(kMasterTicksPerSecond / kRcpTicksPerCycle == 62'500'000ULL,
            "master timebase derives the 62.5 MHz RCP clock");
    require(kMasterTicksPerSecond / kCp0CountTicksPerIncrement ==
                46'875'000ULL,
            "master timebase derives the 46.875 MHz CP0 Count rate");
}

void testTlbPairedPagesAsidGlobalAndPermissions() {
    m64plus::n64::Tlb tlb;
    std::uint32_t physical = 0;

    require(tlb.writeEntry(0U, 0x00400000U, 0x12U, 0x100U, 0x101U,
                           true, true, true, true, false),
            "install non-global TLB pair for ASID 0x12");
    require(tlb.translate(0x00400123U, 0x12U, false, physical) ==
                m64plus::n64::TlbResult::Success && physical == 0x00100123U,
            "TLB translates even 4 KiB page with its offset");
    require(tlb.translate(0xFFFFFFFF80400123ULL, 0x12U, false, physical) ==
                m64plus::n64::TlbResult::Success && physical == 0x00400123U,
            "TLB accepts canonical sign-extended KSEG0 direct mapping");
    require(tlb.translate(0x0000000180400123ULL, 0x12U, false, physical) ==
                m64plus::n64::TlbResult::Miss,
            "TLB rejects noncanonical upper address bits");
    require(tlb.translate(0x00401123U, 0x12U, false, physical) ==
                m64plus::n64::TlbResult::Success && physical == 0x00101123U,
            "TLB translates odd 4 KiB page with its offset");
    require(tlb.translate(0x00400123U, 0x13U, false, physical) ==
                m64plus::n64::TlbResult::Miss,
            "non-global TLB entry rejects a different ASID");

    require(tlb.writeEntry(1U, 0x00600000U, 0x12U, 0x110U, 0x111U,
                           true, true, true, true, true),
            "install global TLB pair");
    require(tlb.translate(0x00601123U, 0x34U, false, physical) ==
                m64plus::n64::TlbResult::Success && physical == 0x00111123U,
            "global TLB entry matches independently of ASID");

    require(tlb.writeEntry(2U, 0x00800000U, 0x12U, 0x120U, 0x121U,
                           true, false, true, true, false),
            "install TLB pair with invalid odd page");
    require(tlb.translate(0x00801000U, 0x12U, false, physical) ==
                m64plus::n64::TlbResult::Invalid,
            "TLB reports invalid odd page");

    require(tlb.writeEntry(3U, 0x00A00000U, 0x12U, 0x130U, 0x131U,
                           true, true, true, false, false),
            "install TLB pair with clean odd page");
    require(tlb.translate(0x00A01000U, 0x12U, true, physical) ==
                m64plus::n64::TlbResult::Modified,
            "TLB rejects store to page without dirty bit");

    require(tlb.writeEntry(4U, 0xC0000000U, 0U, 0x140U, 0x141U,
                           true, true, true, true, false),
            "install SSEG TLB pair");
    require(tlb.translate(0xC0000123U, 0U, false, physical) ==
                m64plus::n64::TlbResult::Success && physical == 0x00140123U,
            "SSEG uses TLB translation to physical memory");
    require(tlb.writeEntry(5U, 0xE0000000U, 0U, 0x150U, 0x151U,
                           true, true, true, true, false),
            "install KSEG3 TLB pair");
    require(tlb.translate(0xE0001123U, 0U, false, physical) ==
                m64plus::n64::TlbResult::Success && physical == 0x00151123U,
            "KSEG3 uses TLB translation to physical memory");
}



void testCpuSupervisorSsegUsesTlbForFetchLoadAndStore() {
    N64Machine machine;
    Bus& bus = machine.bus();
    auto& cpu = machine.cpu();

    require(bus.write32(0U, 0x42000002U), "write TLBWI for SSEG supervisor mapping");
    require(bus.write32(4U, 0x00200008U), "write JR to supervisor SSEG code");
    require(bus.write32(8U, 0U), "write JR delay slot");
    require(bus.write32(0x3000U, 0x8C220000U), "write LW instruction in translated SSEG page");
    require(bus.write32(0x3004U, 0xAC230004U), "write SW instruction in translated SSEG page");
    require(bus.write32(0x2000U, 0x12345678U), "initialize SSEG mapped physical data");
    require(bus.write32(0x2004U, 0U), "initialize SSEG store destination");

    cpu.reset(0xFFFFFFFF80000000ULL);
    require(cpu.setCp0Register(m64plus::n64::Cp0::kIndex, 0U),
            "select TLB entry for SSEG supervisor mapping");
    require(cpu.setCp0Register(m64plus::n64::Cp0::kEntryHi, 0xC0000000U),
            "set SSEG virtual pair");
    require(cpu.setCp0Register(m64plus::n64::Cp0::kEntryLo0,
                               (2ULL << 6U) | 6ULL),
            "set valid dirty even SSEG page");
    require(cpu.setCp0Register(m64plus::n64::Cp0::kEntryLo1,
                               (3ULL << 6U) | 6ULL),
            "set valid dirty odd SSEG page");
    cpu.setRegister(1U, 0xFFFFFFFFC0001000ULL);
    cpu.setRegister(3U, 0xAABBCCDDU);

    require(machine.stepCpu(), "install SSEG mapping through TLBWI");
    cpu.setTlbEnabled(true);
    require(machine.stepCpu(), "execute JR to supervisor SSEG in kernel mode");
    require(machine.stepCpu(), "execute JR delay slot in kernel mode");
    require(cpu.pc() == 0xFFFFFFFFC0001000ULL,
            "JR transfers execution into supervisor SSEG");
    require(cpu.setCp0Register(m64plus::n64::Cp0::kStatus, 1ULL << 3U),
            "switch to supervisor mode after entering SSEG");
    cpu.setRegister(1U, 0xFFFFFFFFC0000000ULL);
    require(machine.stepCpu(), "supervisor fetches LW through SSEG TLB mapping");
    require(cpu.reg(2U) == 0x12345678U,
            "supervisor SSEG load reads translated physical memory");
    require(machine.stepCpu(), "supervisor fetches SW through SSEG TLB mapping");
    std::uint32_t stored = 0;
    require(bus.read32(0x2004U, stored) && stored == 0xAABBCCDDU,
            "supervisor SSEG store writes translated physical memory");
}

void testCpuKseg3LoadStoreUsesTlbTranslation() {
    N64Machine machine;
    Bus& bus = machine.bus();
    require(bus.write32(0U, 0x42000002U), "write TLBWI for KSEG3 mapping");
    require(bus.write32(4U, 0x8C220000U), "write LW from KSEG3");
    require(bus.write32(8U, 0xAC230004U), "write SW to KSEG3");
    require(bus.write32(0x2000U, 0x12345678U), "initialize KSEG3 mapped physical word");

    auto& cpu = machine.cpu();
    cpu.reset(0xFFFFFFFF80000000ULL);
    require(cpu.setCp0Register(m64plus::n64::Cp0::kIndex, 0U),
            "select TLB entry for KSEG3 mapping");
    require(cpu.setCp0Register(m64plus::n64::Cp0::kEntryHi, 0xE0000000U),
            "set KSEG3 virtual pair");
    require(cpu.setCp0Register(m64plus::n64::Cp0::kEntryLo0,
                               (2ULL << 6U) | 6ULL),
            "set valid dirty even KSEG3 page");
    require(cpu.setCp0Register(m64plus::n64::Cp0::kEntryLo1,
                               (3ULL << 6U) | 6ULL),
            "set valid dirty odd KSEG3 page");
    cpu.setRegister(1U, 0xFFFFFFFFE0000000ULL);
    cpu.setRegister(3U, 0xAABBCCDDU);
    require(machine.stepCpu(), "install KSEG3 mapping through TLBWI");
    cpu.setTlbEnabled(true);
    require(machine.stepCpu(), "load from KSEG3 translated physical page");
    require(cpu.reg(2U) == 0x12345678U,
            "KSEG3 load reads data from translated physical address");
    require(machine.stepCpu(), "store to KSEG3 translated physical page");
    std::uint32_t stored = 0;
    require(bus.read32(0x2004U, stored) && stored == 0xAABBCCDDU,
            "KSEG3 store writes data to translated physical address");
}

void testMachineRdpRunnerHonorsCommandBudget() {
    N64Machine machine;
    Bus& bus = machine.bus();
    const std::uint64_t commands[] = {
        (0x3FULL << 56U) | (2ULL << 51U) | (7ULL << 32U) | 0x6000U,
        (0x37ULL << 56U) | 0xA1B2C3D4ULL
    };
    for (std::size_t i = 0; i < 2U; ++i) {
        require(bus.write64(0x780U + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "write budgeted machine RDP command");
    }
    require(bus.write32(Bus::kDpStart, 0x780U) &&
            bus.write32(Bus::kDpEnd, 0x790U),
            "publish budgeted machine RDP command stream");

    require(machine.runRdp(1U) == 1U,
            "machine RDP runner processes no more than one command per budget");
    std::uint32_t current = 0U;
    require(bus.read32(Bus::kDpCurrent, current) && current == 0x788U,
            "RDP current advances by exactly one command after a one-command budget");
    require(machine.rdp().hasColorImage && !machine.rdp().hasFillColor,
            "one-command budget applies only the first RDP state update");

    require(machine.runRdp(1U) == 1U,
            "a subsequent RDP slice processes the next command");
    require(bus.read32(Bus::kDpCurrent, current) && current == 0x790U,
            "second RDP slice advances current to the published end");
    require(machine.rdp().hasFillColor && machine.rdp().fillColor == 0xA1B2C3D4U,
            "second RDP slice applies the fill-color state update");
    require(machine.runRdp(1U) == 0U,
            "RDP runner reports no work after the command stream is exhausted");
}

void testMachineRdpRunnerConsumesUnsupportedCommands() {
    N64Machine machine;
    Bus& bus = machine.bus();
    const std::uint64_t commands[] = {
        (0x08ULL << 56U), // Triangle command: four-word base, not yet rasterized.
        0ULL,
        0ULL,
        0ULL,
        (0x37ULL << 56U) | 0x55667788ULL
    };
    for (std::size_t i = 0; i < 5U; ++i) {
        require(bus.write64(0x980U + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "write unsupported and supported RDP commands");
    }
    require(bus.write32(Bus::kDpStart, 0x980U) &&
            bus.write32(Bus::kDpEnd, 0x9A8U),
            "publish mixed RDP command stream");

    require(machine.runRdp(2U) == 2U,
            "machine RDP runner consumes unsupported command without retrying forever");
    std::uint32_t current = 0U;
    require(bus.read32(Bus::kDpCurrent, current) && current == 0x9A8U,
            "unsupported triangle command advances DPC to following command");
    require(machine.rdp().hasFillColor &&
            machine.rdp().fillColor == 0x55667788U,
            "runner continues and processes supported command after unsupported triangle");
    require(machine.runRdp(1U) == 0U,
            "mixed RDP stream is exhausted after unsupported command was consumed");
}

void testMachineRunsRdpTextureLoadIntoTmem() {
    N64Machine machine;
    Bus& bus = machine.bus();
    for (std::uint32_t i = 0; i < 12U; ++i) {
        require(bus.write8(0x1000U + i, static_cast<std::uint8_t>(i + 1U)),
                "seed machine RDP source texture");
    }

    const std::uint64_t commands[] = {
        (0x3DULL << 56U) | (1ULL << 51U) | (3ULL << 32U) | 0x1000ULL,
        (0x35ULL << 56U) | (1ULL << 51U) | (1ULL << 41U),
        (0x34ULL << 56U) | (4ULL << 44U) | (4ULL << 32U) |
            (8ULL << 12U) | 8ULL
    };
    for (std::size_t i = 0; i < 3U; ++i) {
        require(bus.write64(0x900U + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "write machine RDP texture-load command");
    }
    require(bus.write32(Bus::kDpStart, 0x900U) &&
            bus.write32(Bus::kDpEnd, 0x918U),
            "publish machine RDP texture-load stream");

    require(machine.runRdp(3U) == 3U,
            "machine RDP runner processes texture setup and LOAD_TILE");
    require(machine.rdp().hasTextureLoad &&
            machine.rdp().lastTextureLoad.kind ==
                m64plus::n64::RdpTextureLoadKind::Tile,
            "machine RDP state records the executed tile load");

    std::uint8_t value = 0U;
    require(machine.rdp().tmem.readByte(0U, value) && value == 6U,
            "machine LOAD_TILE copies first selected texel into TMEM");
    require(machine.rdp().tmem.readByte(1U, value) && value == 7U,
            "machine LOAD_TILE copies adjacent texel into TMEM");
    require(machine.rdp().tmem.readByte(12U, value) && value == 10U,
            "machine LOAD_TILE applies odd-row TMEM swizzle");
    require(machine.rdp().tmem.readByte(13U, value) && value == 11U,
            "machine LOAD_TILE preserves odd-row texel after swizzle");
    require(machine.runRdp(1U) == 0U,
            "machine RDP runner stops after texture-load stream is exhausted");
}

void testMachineRunsRdpFillIntoFramebuffer() {
    N64Machine machine;
    Bus& bus = machine.bus();
    constexpr std::uint32_t base = 0x5000U;
    constexpr std::uint32_t sentinel = 0xAABBCCDDU;
    constexpr std::uint32_t color = 0x11223344U;

    for (std::uint32_t i = 0; i < 16U; ++i) {
        require(bus.write32(base + i * 4U, sentinel),
                "seed machine RDP framebuffer sentinels");
    }

    const std::uint64_t commands[] = {
        (0x3FULL << 56U) | (3ULL << 51U) | (3ULL << 32U) | base,
        (0x37ULL << 56U) | color,
        (0x2FULL << 56U) | (3ULL << 52U),
        (0x36ULL << 56U) | (8ULL << 44U) | (8ULL << 32U)
    };
    for (std::size_t i = 0; i < 4U; ++i) {
        require(bus.write64(0x700U + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "write machine RDP fill command stream");
    }
    require(bus.write32(Bus::kDpStart, 0x700U) &&
            bus.write32(Bus::kDpEnd, 0x720U),
            "publish machine RDP fill command stream");

    require(machine.runRdp(4U) == 4U,
            "machine RDP runner consumes setup and fill commands");
    require(machine.runRdp(4U) == 0U,
            "machine RDP runner stops at the end of the command stream");

    for (std::uint32_t y = 0; y < 4U; ++y) {
        for (std::uint32_t x = 0; x < 4U; ++x) {
            std::uint32_t pixel = 0U;
            require(bus.read32(base + (y * 4U + x) * 4U, pixel),
                    "read machine-produced framebuffer pixel");
            const bool inside = x < 3U && y < 3U;
            require(pixel == (inside ? color : sentinel),
                    "machine RDP runner writes only the requested fill rectangle");
        }
    }
}

void testMachineOwnsAndProcessesRdpState() {
    N64Machine machine;
    Bus& bus = machine.bus();
    // SET_COLOR_IMAGE: RGBA16, width 320, address 0x2000.
    const std::uint64_t setColorImage = (0x3FULL << 56U) |
        (2ULL << 51U) | (319ULL << 32U) | 0x2000ULL;
    require(bus.write64(0x100U, setColorImage), "write RDP SET_COLOR_IMAGE command");
    require(bus.write32(Bus::kDpStart, 0x100U), "set DPC_START");
    require(bus.write32(Bus::kDpEnd, 0x108U), "set DPC_END");
    require(machine.runRdp(4U) == 1U, "machine consumes one RDP command");
    const auto& image = machine.rdp().colorImage;
    require(machine.rdp().hasColorImage, "machine retains RDP color image state");
    require(image.format == 0U && image.size == 2U && image.width == 320U &&
            image.address == 0x2000U, "machine exposes decoded color image descriptor");
    require(machine.runRdp(4U) == 0U, "RDP runner stops when command stream is empty");

    // Seed additional RDP-local state so reset covers TMEM and sync bookkeeping.
    require(machine.rdp().tmem.writeByte(0x10U, 0xA5U),
            "seed RDP TMEM before machine reset");
    machine.rdp().hasTextureLoad = true;
    machine.rdp().syncFullCount = 9U;

    // Reset must clear RDP state as well as CPU/RSP and bus state.
    machine.reset();
    require(!machine.rdp().hasColorImage, "machine reset clears RDP image state");
    require(!machine.rdp().hasTextureLoad,
            "machine reset clears pending texture-load state");
    require(machine.rdp().syncFullCount == 0U,
            "machine reset clears RDP synchronization counters");
    std::uint8_t tmemByte = 0xFFU;
    require(machine.rdp().tmem.readByte(0x10U, tmemByte) && tmemByte == 0U,
            "machine reset clears RDP texture memory");
}

void testCpuKsegDirectMappingWithoutTlb() {
    N64Machine machine;
    Bus& bus = machine.bus();

    // Execute from KSEG0 while the software-managed TLB is disabled.
    machine.cpu().reset(0xFFFFFFFF80000000ULL);
    const std::uint32_t program[] = {
        0x3C08A000U, // LUI t0,0xA000 -> KSEG1 base
        0x35080100U, // ORI t0,t0,0x0100 -> KSEG1 + 0x100
        0x240955AAU, // ADDIU t1,zero,0x55AA
        0xAD090000U  // SW t1,0(t0) -> physical 0x100
    };
    for (std::uint32_t i = 0; i < 4U; ++i)
        require(bus.write32(i * 4U, program[i]), "load KSEG direct-map test program");

    for (unsigned i = 0; i < 4U; ++i)
        require(machine.stepCpu(), "CPU executes through KSEG0 with TLB disabled");

    std::uint32_t stored = 0;
    require(bus.read32(0x100U, stored), "read physical destination after KSEG1 store");
    require(stored == 0x000055AAU, "KSEG1 store maps to the corresponding physical address");
    require(machine.cpu().pc() == 0xFFFFFFFF80000010ULL,
            "CPU PC remains in canonical KSEG0 after instruction fetches");
}

void testCpuFetchesAndExecutesFromCartridgeKseg1() {
    N64Machine machine;
    std::uint8_t rom[0x40U + 8U]{};
    rom[0] = 0x80U;
    rom[1] = 0x37U;
    rom[2] = 0x12U;
    rom[3] = 0x40U;
    // IPL3 entry point in a cartridge image: two simple MIPS instructions.
    const std::uint32_t program[] = {0x24081234U, 0x25090001U};
    for (unsigned i = 0; i < 2U; ++i) {
        const std::uint32_t instruction = program[i];
        const std::size_t offset = 0x40U + i * 4U;
        rom[offset] = static_cast<std::uint8_t>(instruction >> 24U);
        rom[offset + 1U] = static_cast<std::uint8_t>(instruction >> 16U);
        rom[offset + 2U] = static_cast<std::uint8_t>(instruction >> 8U);
        rom[offset + 3U] = static_cast<std::uint8_t>(instruction);
    }
    require(machine.bus().loadCartridge(rom, sizeof(rom)),
            "load test cartridge for CPU execution");

    machine.cpu().reset(0xFFFFFFFFB0000040ULL);
    require(machine.stepCpu(), "CPU fetches first instruction through KSEG1 cartridge mapping");
    require(machine.stepCpu(), "CPU fetches second instruction through KSEG1 cartridge mapping");
    require(machine.cpu().reg(8) == 0x1234U,
            "CPU executes instruction fetched from cartridge IPL3 offset");
    require(machine.cpu().reg(9) == 0x1235U,
            "CPU continues executing sequential cartridge instruction");
    require(machine.cpu().pc() == 0xFFFFFFFFB0000048ULL,
            "CPU PC advances inside KSEG1 cartridge window");
}


void testCpuRunnerStopsWhenInstructionRaisesException() {
    N64Machine machine;
    require(machine.bus().write32(0U, 0x7000003FU),
            "write unsupported instruction for CPU runner");
    machine.cpu().reset(0xFFFFFFFF80000000ULL);
    require(machine.runCpuInstructions(64U) == 0U,
            "CPU runner stops when step handles an exception without retiring an instruction");
    require(machine.cpu().instructionsExecuted() == 0U,
            "exception does not count as a retired instruction in CPU runner");
    require(machine.cpu().hasLastException(), "CPU records the last exception");
    require(machine.cpu().lastExceptionPc() == 0xFFFFFFFF80000000ULL,
            "CPU records the address of the faulting instruction");
    require(machine.cpu().lastExceptionInstruction() == 0x7000003FU,
            "CPU records the faulting instruction word");
    require(machine.cpu().lastExceptionCode() == 10U,
            "CPU records Reserved Instruction exception code");
}


void testCpuHiLoMultiplyDivideAndTransfers() {
    N64Machine machine;
    Bus& bus = machine.bus();
    const std::uint32_t program[] = {
        0x2408FFF9U, // ADDIU t0,zero,-7
        0x24090003U, // ADDIU t1,zero,3
        0x01090018U, // MULT t0,t1 -> -21
        0x00005012U, // MFLO t2
        0x00005810U, // MFHI t3
        0x0109001AU, // DIV t0,t1 -> quotient -2, remainder -1
        0x00006012U, // MFLO t4
        0x00006810U, // MFHI t5
        0x240E1234U, // ADDIU t6,zero,0x1234
        0x01C00011U, // MTHI t6
        0x00007810U, // MFHI t7
        0x01C00013U, // MTLO t6
        0x00008012U, // MFLO s0
        0x01090019U, // MULTU t0,t1 -> unsigned product 0x00000002FFFFFFEB
        0x00008812U, // MFLO s1
        0x00009010U, // MFHI s2
        0x0109001BU, // DIVU t0,t1 -> quotient 0x55555553, remainder 0
        0x00009812U, // MFLO s3
        0x0000A010U, // MFHI s4
        0x0008A938U, // DSLL s5,t0,4
        0x0015B13AU, // DSRL s6,s5,4
        0x0015B93BU, // DSRA s7,s5,4
        0x0128C014U  // DSLLV t8,t0,t1
    };
    for (std::uint32_t i = 0; i < 23U; ++i)
        require(bus.write32(i * 4U, program[i]), "load HI/LO and shift program");
    machine.cpu().reset(0xFFFFFFFF80000000ULL);
    require(machine.cpu().setCp0Register(m64plus::n64::Cp0::kStatus,
                                           m64plus::n64::Cp0::kStatusKx),
            "enable Kernel 64-bit mode for MIPS III instruction test");
    for (unsigned i = 0; i < 23U; ++i)
        require(machine.stepCpu(), "execute HI/LO and shift instruction");
    require(machine.cpu().reg(10U) == 0xFFFFFFFFFFFFFFEBULL,
            "MULT writes sign-extended product to LO and MFLO reads it");
    require(machine.cpu().reg(11U) == 0xFFFFFFFFFFFFFFFFULL,
            "MULT writes sign-extended high word and MFHI reads it");
    require(machine.cpu().reg(12U) == 0xFFFFFFFFFFFFFFFEULL,
            "DIV writes signed quotient to LO");
    require(machine.cpu().reg(13U) == 0xFFFFFFFFFFFFFFFFULL,
            "DIV writes signed remainder to HI");
    require(machine.cpu().reg(15U) == 0x1234U,
            "MTHI writes HI and MFHI transfers it to a GPR");
    require(machine.cpu().reg(16U) == 0x1234U,
            "MTLO writes LO and MFLO transfers it to a GPR");
    require(machine.cpu().reg(17U) == 0xFFFFFFFFFFFFFFEBULL,
            "MULTU writes the low unsigned product word to LO");
    require(machine.cpu().reg(18U) == 2U,
            "MULTU writes the high unsigned product word to HI");
    require(machine.cpu().reg(19U) == 0x55555553U,
            "DIVU writes the unsigned quotient to LO");
    require(machine.cpu().reg(20U) == 0U,
            "DIVU writes the unsigned remainder to HI");
    require(machine.cpu().reg(21U) == 0xFFFFFFFFFFFFFF90ULL,
            "DSLL shifts a full 64-bit register");
    require(machine.cpu().reg(22U) == 0x0FFFFFFFFFFFFFF9ULL,
            "DSRL performs a logical 64-bit right shift");
    require(machine.cpu().reg(23U) == 0xFFFFFFFFFFFFFFF9ULL,
            "DSRA performs an arithmetic 64-bit right shift");
    require(machine.cpu().reg(24U) == 0xFFFFFFFFFFFFFFC8ULL,
            "DSLLV uses the variable 64-bit shift amount");
}

void testCpuDoublewordMultiplyDivide() {
    N64Machine machine;
    Bus& bus = machine.bus();
    const std::uint32_t program[] = {
        0x0109001CU, // DMULT t0,t1
        0x00006012U, // MFLO t4
        0x00006810U, // MFHI t5
        0x0109001DU, // DMULTU t0,t1
        0x00007012U, // MFLO t6
        0x00007810U, // MFHI t7
        0x0109001EU, // DDIV t0,t1
        0x00008012U, // MFLO s0
        0x00008810U, // MFHI s1
        0x0109001FU, // DDIVU t0,t1
        0x00009012U, // MFLO s2
        0x00009810U  // MFHI s3
    };
    for (std::uint32_t i = 0; i < 12U; ++i)
        require(bus.write32(i * 4U, program[i]), "load 64-bit multiply/divide program");

    machine.cpu().reset(0xFFFFFFFF80000000ULL);
        require(machine.cpu().setCp0Register(m64plus::n64::Cp0::kStatus,
                                                m64plus::n64::Cp0::kStatusKx),
                "enable Kernel 64-bit mode for doubleword instruction test");
    machine.cpu().setRegister(8U, 0xFFFFFFFFFFFFFFFEULL);
    machine.cpu().setRegister(9U, 3U);
    for (unsigned i = 0; i < 3U; ++i)
        require(machine.stepCpu(), "execute signed 64-bit multiply and HI/LO reads");
    require(machine.cpu().reg(12U) == 0xFFFFFFFFFFFFFFFAULL,
            "DMULT stores low 64 bits of signed product in LO");
    require(machine.cpu().reg(13U) == 0xFFFFFFFFFFFFFFFFULL,
            "DMULT stores signed high 64 bits in HI");

    machine.cpu().reset(0xFFFFFFFF8000000CU);
        require(machine.cpu().setCp0Register(m64plus::n64::Cp0::kStatus,
                                                m64plus::n64::Cp0::kStatusKx),
                "enable Kernel 64-bit mode for doubleword instruction test");
    machine.cpu().setRegister(8U, UINT64_MAX);
    machine.cpu().setRegister(9U, 2U);
    for (unsigned i = 0; i < 3U; ++i)
        require(machine.stepCpu(), "execute unsigned 64-bit multiply and HI/LO reads");
    require(machine.cpu().reg(14U) == 0xFFFFFFFFFFFFFFFEULL,
            "DMULTU stores low 64 bits of unsigned product in LO");
    require(machine.cpu().reg(15U) == 1U,
            "DMULTU stores high 64 bits of unsigned product in HI");

    machine.cpu().reset(0xFFFFFFFF80000018ULL);
        require(machine.cpu().setCp0Register(m64plus::n64::Cp0::kStatus,
                                                m64plus::n64::Cp0::kStatusKx),
                "enable Kernel 64-bit mode for doubleword instruction test");
    machine.cpu().setRegister(8U, UINT64_MAX - 6U); // -7
    machine.cpu().setRegister(9U, 3U);
    for (unsigned i = 0; i < 3U; ++i)
        require(machine.stepCpu(), "execute signed 64-bit divide and HI/LO reads");
    require(machine.cpu().reg(16U) == UINT64_MAX - 1U,
            "DDIV stores signed quotient -2 in LO");
    require(machine.cpu().reg(17U) == UINT64_MAX,
            "DDIV stores signed remainder -1 in HI");

    machine.cpu().reset(0xFFFFFFFF80000024ULL);
        require(machine.cpu().setCp0Register(m64plus::n64::Cp0::kStatus,
                                                m64plus::n64::Cp0::kStatusKx),
                "enable Kernel 64-bit mode for doubleword instruction test");
    machine.cpu().setRegister(8U, UINT64_MAX);
    machine.cpu().setRegister(9U, 3U);
    for (unsigned i = 0; i < 3U; ++i)
        require(machine.stepCpu(), "execute unsigned 64-bit divide and HI/LO reads");
    require(machine.cpu().reg(18U) == 0x5555555555555555ULL,
            "DDIVU stores unsigned quotient in LO");
    require(machine.cpu().reg(19U) == 0U,
            "DDIVU stores unsigned remainder in HI");
}


void testCpuDoublewordMultiplyDivideEdgeCases() {
    N64Machine machine;
    Bus& bus = machine.bus();
    const std::uint32_t readHiLo[] = {
        0x00006012U, // MFLO t4
        0x00006810U  // MFHI t5
    };
    auto run = [&](std::uint32_t operation, std::uint64_t left, std::uint64_t right,
                   std::uint64_t expectedLo, std::uint64_t expectedHi,
                   const char* description) {
        require(bus.write32(0x1000U, operation), "write edge-case multiply/divide instruction");
        require(bus.write32(0x1004U, readHiLo[0]), "write edge-case MFLO");
        require(bus.write32(0x1008U, readHiLo[1]), "write edge-case MFHI");
        machine.cpu().reset(0xFFFFFFFF80001000ULL);
        require(machine.cpu().setCp0Register(m64plus::n64::Cp0::kStatus,
                                                m64plus::n64::Cp0::kStatusKx),
                "enable Kernel 64-bit mode for doubleword instruction test");
        machine.cpu().setRegister(8U, left);
        machine.cpu().setRegister(9U, right);
        require(machine.stepCpu() && machine.stepCpu() && machine.stepCpu(),
                "execute edge-case operation and read HI/LO");
        require(machine.cpu().reg(12U) == expectedLo, description);
        require(machine.cpu().reg(13U) == expectedHi, description);
    };

    run(0x0109001CU, 0x8000000000000000ULL, UINT64_MAX,
        0x8000000000000000ULL, 0ULL,
        "DMULT handles INT64_MIN multiplied by -1 without signed overflow");
    run(0x0109001CU, 0x8000000000000000ULL, 2ULL,
        0ULL, UINT64_MAX,
        "DMULT produces the correct high half for INT64_MIN multiplied by 2");
    run(0x0109001DU, UINT64_MAX, UINT64_MAX,
        1ULL, 0xFFFFFFFFFFFFFFFEULL,
        "DMULTU produces the full 128-bit UINT64_MAX squared product");
    run(0x0109001EU, 0x8000000000000000ULL, UINT64_MAX,
        0x8000000000000000ULL, 0ULL,
        "DDIV handles INT64_MIN divided by -1 deterministically");
    run(0x0109001EU, UINT64_MAX, 0ULL,
        1ULL, UINT64_MAX,
        "DDIV uses the documented deterministic quotient for negative divide by zero");
    run(0x0109001EU, 7ULL, 0ULL,
        UINT64_MAX, 7ULL,
        "DDIV uses the documented deterministic quotient for nonnegative divide by zero");
    run(0x0109001FU, 0xFEDCBA9876543210ULL, 0ULL,
        UINT64_MAX, 0xFEDCBA9876543210ULL,
        "DDIVU returns all-one quotient and original dividend remainder on divide by zero");
}

void testCpuLoadLinkedStoreConditional() {
    N64Machine machine;
    Bus& bus = machine.bus();
    constexpr std::uint32_t dataAddress = 0x200U;
    constexpr std::uint32_t initial = 0x11223344U;
    const std::uint32_t program[] = {
        0xC1090000U, // LL t1,0(t0)
        0x25290001U, // ADDIU t1,t1,1
        0xE1090000U, // SC t1,0(t0)
        0xC10A0000U, // LL t2,0(t0)
        0x240B0055U, // ADDIU t3,zero,0x55
        0xAD0B0000U, // SW t3,0(t0), invalidates reservation
        0xE10A0000U  // SC t2,0(t0), must fail
    };
    for (std::uint32_t i = 0; i < sizeof(program) / sizeof(program[0]); ++i)
        require(bus.write32(i * 4U, program[i]), "write LL/SC test program");
    require(bus.write32(dataAddress, initial), "initialize LL/SC target word");
    machine.cpu().reset(0xFFFFFFFF80000000ULL);
    machine.cpu().setRegister(8U, dataAddress);
    require(machine.stepCpu(), "LL loads linked word");
    require(machine.cpu().reg(9U) == initial, "LL sign-extends loaded 32-bit word");
    require(machine.stepCpu(), "modify loaded value before SC");
    require(machine.stepCpu(), "SC succeeds with matching reservation");
    std::uint32_t value = 0;
    require(bus.read32(dataAddress, value) && value == initial + 1U, "successful SC writes modified value");
    require(machine.cpu().reg(9U) == 1U, "successful SC writes one to rt");
    require(machine.stepCpu(), "second LL establishes a new reservation");
    require(machine.stepCpu(), "prepare value before intervening store");
    require(machine.stepCpu(), "ordinary SW invalidates reservation");
    require(machine.stepCpu(), "SC fails after intervening store");
    require(bus.read32(dataAddress, value) && value == 0x55U, "failed SC leaves intervening store intact");
    require(machine.cpu().reg(10U) == 0U, "failed SC writes zero to rt");
}



void testCpuAddressStoreFaultInvalidatesLoadLinkedReservation() {
    N64Machine machine;
    Bus& bus = machine.bus();
    constexpr std::uint32_t dataAddress = 0x330U;
    constexpr std::uint32_t initial = 0x13579BDFU;
    require(bus.write32(0x000U, 0xC1090000U), "write LL before address-store fault");
    require(bus.write32(0x004U, 0xAD0B0001U), "write misaligned SW to raise address-store exception");
    require(bus.write32(0x180U, 0xE1090000U), "write SC at address-store exception vector");
    require(bus.write32(dataAddress, initial), "initialize address-store LL/SC target");
    machine.cpu().reset(0xFFFFFFFF80000000ULL);
    machine.cpu().setRegister(8U, dataAddress);
    machine.cpu().setRegister(11U, 0xA5A5A5A5U);
    require(machine.stepCpu(), "LL establishes reservation before address-store fault");
    require(machine.stepCpu(), "misaligned SW raises address-store exception");
    require(machine.cpu().hasLastException(), "CPU records address-store exception after LL");
    require(machine.cpu().lastExceptionCode() == 5U,
            "CPU records Address Store exception code");
    require(machine.cpu().pc() == 0xFFFFFFFF80000180ULL,
            "CPU transfers control to address-store exception vector");
    require(machine.stepCpu(), "SC executes at address-store vector without reservation");
    std::uint32_t value = 0;
    require(bus.read32(dataAddress, value) && value == initial,
            "SC after address-store fault does not modify reserved memory");
    require(machine.cpu().reg(9U) == 0U,
            "SC reports failure because address-store fault invalidated reservation");
}


void testCpuBusStoreFaultInvalidatesLoadLinkedReservation() {
    N64Machine machine;
    Bus& bus = machine.bus();
    constexpr std::uint32_t dataAddress = 0x340U;
    constexpr std::uint32_t unmappedAddress = 0x05000000U;
    constexpr std::uint32_t initial = 0x89ABCDEFU;
    require(bus.write32(0x000U, 0xC1090000U), "write LL before bus-store fault");
    require(bus.write32(0x004U, 0xAD4B0000U), "write SW to unmapped address");
    require(bus.write32(0x180U, 0xE1090000U), "write SC at bus-store exception vector");
    require(bus.write32(dataAddress, initial), "initialize bus-store LL/SC target");
    machine.cpu().reset(0xFFFFFFFF80000000ULL);
    machine.cpu().setRegister(8U, dataAddress);
    machine.cpu().setRegister(10U, unmappedAddress);
    machine.cpu().setRegister(11U, 0xA5A5A5A5U);
    require(machine.stepCpu(), "LL establishes reservation before bus-store fault");
    require(machine.stepCpu(), "SW to unmapped address raises bus-data exception");
    require(machine.cpu().hasLastException(), "CPU records bus-store exception after LL");
    require(machine.cpu().lastExceptionCode() == 7U,
            "CPU records Bus Data exception code for failed store");
    require(machine.cpu().pc() == 0xFFFFFFFF80000180ULL,
            "CPU transfers control to bus-store exception vector");
    require(machine.stepCpu(), "SC executes at bus-store exception vector without reservation");
    std::uint32_t value = 0;
    require(bus.read32(dataAddress, value) && value == initial,
            "SC after bus-store fault does not modify reserved memory");
    require(machine.cpu().reg(9U) == 0U,
            "SC reports failure because bus-store fault invalidated reservation");
}



void testCpuTlbStoreFaultInvalidatesLoadLinkedReservation() {
    N64Machine machine;
    Bus& bus = machine.bus();
    constexpr std::uint32_t dataAddress = 0x350U;
    constexpr std::uint64_t unmappedVirtualAddress = 0x00400000ULL;
    constexpr std::uint32_t initial = 0x76543210U;
    require(bus.write32(0x1000U, 0xC1090000U), "write LL before TLB store fault");
    require(bus.write32(0x1004U, 0xAD4B0000U), "write SW through unmapped TLB address");
    require(bus.write32(0x000U, 0xE1090000U), "write SC at TLB exception vector");
    require(bus.write32(dataAddress, initial), "initialize TLB-fault LL/SC target");
    machine.cpu().reset(0xFFFFFFFF80001000ULL);
    machine.cpu().setRegister(8U, dataAddress);
    machine.cpu().setRegister(10U, unmappedVirtualAddress);
    machine.cpu().setRegister(11U, 0xA5A5A5A5U);
    require(machine.stepCpu(), "LL establishes reservation before TLB fault");
    machine.cpu().setTlbEnabled(true);
    require(machine.stepCpu(), "SW through unmapped virtual address raises TLB exception");
    require(machine.cpu().hasLastException(), "CPU records TLB store exception after LL");
    require(machine.cpu().lastExceptionCode() == 3U,
            "CPU records TLB Store exception code");
    require(machine.cpu().pc() == 0xFFFFFFFF80000000ULL,
            "CPU transfers control to TLB refill vector");
    machine.cpu().setTlbEnabled(false);
    require(machine.stepCpu(), "SC executes at TLB exception vector without reservation");
    std::uint32_t value = 0;
    require(bus.read32(dataAddress, value) && value == initial,
            "SC after TLB store fault does not modify reserved memory");
    require(machine.cpu().reg(9U) == 0U,
            "SC reports failure because TLB store fault invalidated reservation");
}


void testCpuTlbInvalidStoreFaultInvalidatesLoadLinkedReservation() {
    N64Machine machine;
    Bus& bus = machine.bus();
    constexpr std::uint32_t dataAddress = 0x360U;
    constexpr std::uint32_t initial = 0x0BADF00DU;
    constexpr std::uint64_t invalidVirtualAddress = 0x00400000ULL;

    require(bus.write32(0x1000U, 0xC1090000U), "write LL before invalid TLB store");
    require(bus.write32(0x1004U, 0x42000002U), "write TLBWI for invalid mapping");
    require(bus.write32(0x1008U, 0xAD4B0000U), "write SW through invalid TLB mapping");
    require(bus.write32(0x180U, 0xE1090000U), "write SC at general exception vector");
    require(bus.write32(dataAddress, initial), "initialize invalid-TLB LL/SC target");

    machine.cpu().reset(0xFFFFFFFF80001000ULL);
    machine.cpu().setRegister(8U, dataAddress);
    machine.cpu().setRegister(10U, invalidVirtualAddress);
    machine.cpu().setRegister(11U, 0xA5A5A5A5U);
    require(machine.cpu().setCp0Register(m64plus::n64::Cp0::kIndex, 0U),
            "select TLB entry for invalid mapping");
    require(machine.cpu().setCp0Register(m64plus::n64::Cp0::kEntryHi,
                                         invalidVirtualAddress),
            "set virtual page for invalid TLB mapping");
    require(machine.cpu().setCp0Register(m64plus::n64::Cp0::kEntryLo0, 0U),
            "clear even-page valid bit in TLB mapping");
    require(machine.cpu().setCp0Register(m64plus::n64::Cp0::kEntryLo1, 0U),
            "clear odd-page valid bit in TLB mapping");
    require(machine.stepCpu(), "LL establishes reservation before invalid TLB store");
    machine.cpu().setTlbEnabled(true);
    require(machine.stepCpu(), "TLBWI installs invalid translation");
    require(machine.stepCpu(), "SW through invalid TLB entry raises TLB store exception");
    require(machine.cpu().hasLastException(), "CPU records invalid TLB store exception");
    require(machine.cpu().lastExceptionCode() == 3U,
            "CPU records TLB Store exception code for invalid entry");
    require(machine.cpu().pc() == 0xFFFFFFFF80000180ULL,
            "CPU uses general exception vector for invalid TLB entry");
    machine.cpu().setTlbEnabled(false);
    require(machine.stepCpu(), "SC executes at general exception vector without reservation");

    std::uint32_t value = 0;
    require(bus.read32(dataAddress, value) && value == initial,
            "SC after invalid TLB store does not modify reserved memory");
    require(machine.cpu().reg(9U) == 0U,
            "SC reports failure because invalid TLB store fault invalidated reservation");
}


void testCpuTlbInvalidLoadFaultInvalidatesLoadLinkedReservation() {
    N64Machine machine;
    Bus& bus = machine.bus();
    constexpr std::uint32_t dataAddress = 0x368U;
    constexpr std::uint32_t initial = 0x55AA33CCU;
    constexpr std::uint64_t virtualAddress = 0x00400000ULL;

    require(bus.write32(0x1000U, 0xC1090000U), "write LL before invalid TLB load");
    require(bus.write32(0x1004U, 0x42000002U), "write TLBWI for invalid mapping");
    require(bus.write32(0x1008U, 0x8D490000U), "write LW through invalid TLB mapping");
    require(bus.write32(0x180U, 0xE1090000U), "write SC at TLB invalid vector");
    require(bus.write32(dataAddress, initial), "initialize invalid-TLB LL/SC target");

    machine.cpu().reset(0xFFFFFFFF80001000ULL);
    machine.cpu().setRegister(8U, dataAddress);
    machine.cpu().setRegister(10U, virtualAddress);
    machine.cpu().setRegister(11U, 0xA5A5A5A5U);
    require(machine.cpu().setCp0Register(m64plus::n64::Cp0::kIndex, 0U),
            "select TLB entry for invalid mapping");
    require(machine.cpu().setCp0Register(m64plus::n64::Cp0::kEntryHi, virtualAddress),
            "set virtual page for invalid TLB mapping");
    require(machine.cpu().setCp0Register(m64plus::n64::Cp0::kEntryLo0, 0U),
            "install invalid even-page TLB entry");
    require(machine.cpu().setCp0Register(m64plus::n64::Cp0::kEntryLo1, 0U),
            "clear odd-page TLB entry");

    require(machine.stepCpu(), "LL establishes reservation before invalid TLB load");
    machine.cpu().setTlbEnabled(true);
    require(machine.stepCpu(), "TLBWI installs invalid translation");
    require(machine.stepCpu(), "LW through invalid TLB entry raises invalid exception");
    require(machine.cpu().hasLastException(), "CPU records TLB invalid load exception");
    require(machine.cpu().lastExceptionCode() == 2U,
            "CPU records TLB Load exception code");
    require(machine.cpu().pc() == 0xFFFFFFFF80000180ULL,
            "CPU uses general exception vector for TLB invalid load");
    machine.cpu().setTlbEnabled(false);
    require(machine.stepCpu(), "SC executes at exception vector without reservation");

    std::uint32_t value = 0;
    require(bus.read32(dataAddress, value) && value == initial,
            "SC after TLB invalid load fault does not modify reserved memory");
    require(machine.cpu().reg(9U) == 0U,
            "SC reports failure because TLB invalid load fault invalidated reservation");
}




void testCpuTlbLoadMissInBranchDelaySlotInvalidatesLoadLinkedReservation() {
    N64Machine machine;
    Bus& bus = machine.bus();
    constexpr std::uint32_t dataAddress = 0x378U;
    constexpr std::uint32_t initial = 0x1A2B3C4DU;
    constexpr std::uint64_t branchTarget = 0xFFFFFFFF80002000ULL;
    constexpr std::uint64_t unmappedAddress = 0x00400000ULL;

    require(bus.write32(0x1000U, 0xC1090000U), "write LL before delay-slot TLB load miss");
    require(bus.write32(0x1004U, 0x02000008U), "write JR r16 before delay-slot TLB load miss");
    require(bus.write32(0x1008U, 0x8D490000U), "write LW in JR delay slot");
    require(bus.write32(0x000U, 0xE1090000U), "write SC at TLB refill vector");
    require(bus.write32(dataAddress, initial), "initialize delay-slot TLB LL/SC target");

    machine.cpu().reset(0xFFFFFFFF80001000ULL);
    machine.cpu().setRegister(8U, dataAddress);
    machine.cpu().setRegister(10U, unmappedAddress);
    machine.cpu().setRegister(16U, branchTarget);
    require(machine.stepCpu(), "LL establishes reservation before delay-slot TLB fault");
    machine.cpu().setTlbEnabled(true);
    require(machine.stepCpu(), "JR schedules branch before delay-slot TLB fault");
    require(machine.stepCpu(), "LW in branch delay slot raises TLB load miss");
    require(machine.cpu().hasLastException(), "CPU records delay-slot TLB load exception");
    require(machine.cpu().lastExceptionCode() == 2U,
            "CPU records TLB Load exception code in branch delay slot");
    require(machine.cpu().pc() == 0xFFFFFFFF80000000ULL,
            "CPU transfers to TLB refill vector after delay-slot load miss");
    require(machine.cpu().cp0().read(m64plus::n64::Cp0::kEpc) == 0xFFFFFFFF80001004ULL,
            "CP0 EPC points to branch instruction for delay-slot TLB exception");
    require((machine.cpu().cp0().read(m64plus::n64::Cp0::kCause) & (1ULL << 31U)) != 0,
            "CP0 Cause sets branch-delay bit for delay-slot TLB exception");
    require(machine.cpu().cp0().read(m64plus::n64::Cp0::kBadVAddr) == unmappedAddress,
            "CP0 BadVAddr records the unmapped delay-slot load address");
    require(machine.cpu().cp0().read(m64plus::n64::Cp0::kEntryHi) == unmappedAddress,
            "CP0 EntryHi records the unmapped delay-slot virtual page");

    machine.cpu().setTlbEnabled(false);
    require(machine.stepCpu(), "SC executes at refill vector without reservation");
    std::uint32_t value = 0;
    require(bus.read32(dataAddress, value) && value == initial,
            "SC after delay-slot TLB miss leaves reserved memory unchanged");
    require(machine.cpu().reg(9U) == 0U,
            "delay-slot TLB load miss invalidates LL reservation");
}

void testCpuTlbLoadMissInvalidatesLoadLinkedReservation() {
    N64Machine machine;
    Bus& bus = machine.bus();
    constexpr std::uint32_t dataAddress = 0x374U;
    constexpr std::uint32_t initial = 0x3C5A7E91U;
    constexpr std::uint64_t virtualAddress = 0x00400000ULL;

    require(bus.write32(0x1000U, 0xC1090000U), "write LL before TLB load miss");
    require(bus.write32(0x1004U, 0x8D490000U), "write LW through unmapped TLB address");
    require(bus.write32(0x000U, 0xE1090000U), "write SC at TLB refill vector");
    require(bus.write32(dataAddress, initial), "initialize TLB-load-miss LL/SC target");

    machine.cpu().reset(0xFFFFFFFF80001000ULL);
    machine.cpu().setRegister(8U, dataAddress);
    machine.cpu().setRegister(10U, virtualAddress);
    require(machine.stepCpu(), "LL establishes reservation before TLB load miss");
    machine.cpu().setTlbEnabled(true);
    require(machine.stepCpu(), "LW through unmapped TLB entry raises refill exception");
    require(machine.cpu().hasLastException(), "CPU records TLB load-miss exception");
    require(machine.cpu().lastExceptionCode() == 2U,
            "CPU records TLB Load exception code for data access");
    require(machine.cpu().pc() == 0xFFFFFFFF80000000ULL,
            "CPU transfers to TLB refill vector after data load miss");

    machine.cpu().setTlbEnabled(false);
    require(machine.stepCpu(), "SC executes at refill vector without reservation");
    std::uint32_t value = 0;
    require(bus.read32(dataAddress, value) && value == initial,
            "SC after TLB data load miss leaves reserved memory unchanged");
    require(machine.cpu().reg(9U) == 0U,
            "TLB data load miss invalidates LL reservation");
}

void testCpuTlbFetchMissInvalidatesLoadLinkedReservation() {
    N64Machine machine;
    Bus& bus = machine.bus();
    constexpr std::uint32_t dataAddress = 0x36CU;
    constexpr std::uint32_t initial = 0xA55A3CC3U;
    constexpr std::uint64_t virtualAddress = 0x00400000ULL;

    require(bus.write32(0x1000U, 0xC1090000U), "write LL before TLB instruction-fetch miss");
    require(bus.write32(0x1004U, 0x02000008U), "write JR r16 to unmapped virtual address");
    require(bus.write32(0x1008U, 0x00000000U), "write JR delay-slot NOP");
    require(bus.write32(0x000U, 0xE1090000U), "write SC at TLB refill vector");
    require(bus.write32(dataAddress, initial), "initialize TLB-fetch-miss LL/SC target");

    machine.cpu().reset(0xFFFFFFFF80001000ULL);
    machine.cpu().setRegister(8U, dataAddress);
    machine.cpu().setRegister(16U, virtualAddress);
    require(machine.stepCpu(), "LL establishes reservation before TLB instruction-fetch miss");
    require(machine.stepCpu(), "JR schedules branch to unmapped virtual address");
    require(machine.stepCpu(), "JR delay slot completes before TLB fetch");
    require(machine.cpu().pc() == virtualAddress,
            "CPU reaches unmapped virtual address after JR delay slot");
    machine.cpu().setTlbEnabled(true);
    require(machine.stepCpu(), "instruction fetch through unmapped TLB address raises refill exception");
    require(machine.cpu().hasLastException(), "CPU records TLB instruction-fetch exception");
    require(machine.cpu().lastExceptionCode() == 2U,
            "CPU records TLB Load exception code for instruction fetch");
    require(machine.cpu().pc() == 0xFFFFFFFF80000000ULL,
            "CPU transfers to TLB refill vector after instruction-fetch miss");

    machine.cpu().setTlbEnabled(false);
    require(machine.stepCpu(), "SC executes at refill vector without reservation");
    std::uint32_t value = 0;
    require(bus.read32(dataAddress, value) && value == initial,
            "SC after TLB instruction-fetch miss leaves reserved memory unchanged");
    require(machine.cpu().reg(9U) == 0U,
            "TLB instruction-fetch exception invalidates LL reservation");
}


void testCpuTlbInvalidFetchFaultInvalidatesLoadLinkedReservation() {
    N64Machine machine;
    Bus& bus = machine.bus();
    constexpr std::uint32_t dataAddress = 0x370U;
    constexpr std::uint32_t initial = 0x6A5B4C3DU;
    constexpr std::uint64_t virtualAddress = 0x00400000ULL;

    require(bus.write32(0x1000U, 0xC1090000U), "write LL before invalid TLB fetch");
    require(bus.write32(0x1004U, 0x42000002U), "write TLBWI for invalid fetch mapping");
    require(bus.write32(0x1008U, 0x02000008U), "write JR r16 to invalid TLB page");
    require(bus.write32(0x100CU, 0x00000000U), "write JR delay-slot NOP");
    require(bus.write32(0x180U, 0xE1090000U), "write SC at TLB invalid vector");
    require(bus.write32(dataAddress, initial), "initialize invalid-TLB-fetch LL/SC target");

    machine.cpu().reset(0xFFFFFFFF80001000ULL);
    machine.cpu().setRegister(8U, dataAddress);
    machine.cpu().setRegister(16U, virtualAddress);
    require(machine.cpu().setCp0Register(m64plus::n64::Cp0::kIndex, 0U),
            "select TLB entry for invalid fetch mapping");
    require(machine.cpu().setCp0Register(m64plus::n64::Cp0::kEntryHi, virtualAddress),
            "set virtual page for invalid fetch mapping");
    require(machine.cpu().setCp0Register(m64plus::n64::Cp0::kEntryLo0, 0U),
            "install invalid even-page fetch mapping");
    require(machine.cpu().setCp0Register(m64plus::n64::Cp0::kEntryLo1, 0U),
            "clear odd-page fetch mapping");

    require(machine.stepCpu(), "LL establishes reservation before invalid TLB fetch");
    machine.cpu().setTlbEnabled(true);
    require(machine.stepCpu(), "TLBWI installs invalid instruction mapping");
    require(machine.stepCpu(), "JR schedules branch to invalid TLB page");
    require(machine.stepCpu(), "JR delay slot completes before invalid TLB fetch");
    require(machine.cpu().pc() == virtualAddress,
            "CPU reaches invalid virtual address after JR delay slot");
    require(machine.stepCpu(), "instruction fetch through invalid TLB entry raises invalid exception");
    require(machine.cpu().hasLastException(), "CPU records TLB invalid instruction-fetch exception");
    require(machine.cpu().lastExceptionCode() == 2U,
            "CPU records TLB Load exception code for invalid instruction fetch");
    require(machine.cpu().pc() == 0xFFFFFFFF80000180ULL,
            "CPU transfers to general vector after invalid TLB instruction fetch");

    machine.cpu().setTlbEnabled(false);
    require(machine.stepCpu(), "SC executes at general vector without reservation");
    std::uint32_t value = 0;
    require(bus.read32(dataAddress, value) && value == initial,
            "SC after invalid TLB instruction fetch leaves reserved memory unchanged");
    require(machine.cpu().reg(9U) == 0U,
            "TLB invalid instruction-fetch exception invalidates LL reservation");
}

void testCpuTlbModifiedStoreFaultInvalidatesLoadLinkedReservation() {
    N64Machine machine;
    Bus& bus = machine.bus();
    constexpr std::uint32_t dataAddress = 0x368U;
    constexpr std::uint32_t initial = 0x55AA33CCU;
    constexpr std::uint64_t virtualAddress = 0x00400000ULL;

    require(bus.write32(0x1000U, 0xC1090000U), "write LL before modified TLB store");
    require(bus.write32(0x1004U, 0x42000002U), "write TLBWI for clean mapping");
    require(bus.write32(0x1008U, 0xAD4B0000U), "write SW through clean TLB mapping");
    require(bus.write32(0x180U, 0xE1090000U), "write SC at TLB modification vector");
    require(bus.write32(dataAddress, initial), "initialize modified-TLB LL/SC target");

    machine.cpu().reset(0xFFFFFFFF80001000ULL);
    machine.cpu().setRegister(8U, dataAddress);
    machine.cpu().setRegister(10U, virtualAddress);
    machine.cpu().setRegister(11U, 0xA5A5A5A5U);
    require(machine.cpu().setCp0Register(m64plus::n64::Cp0::kIndex, 0U),
            "select TLB entry for clean mapping");
    require(machine.cpu().setCp0Register(m64plus::n64::Cp0::kEntryHi, virtualAddress),
            "set virtual page for clean TLB mapping");
    require(machine.cpu().setCp0Register(m64plus::n64::Cp0::kEntryLo0, 2U),
            "set valid but clean even-page TLB entry");
    require(machine.cpu().setCp0Register(m64plus::n64::Cp0::kEntryLo1, 0U),
            "clear odd-page TLB entry");

    require(machine.stepCpu(), "LL establishes reservation before modified TLB store");
    machine.cpu().setTlbEnabled(true);
    require(machine.stepCpu(), "TLBWI installs valid clean translation");
    require(machine.stepCpu(), "SW through clean TLB entry raises modification exception");
    require(machine.cpu().hasLastException(), "CPU records TLB modification exception");
    require(machine.cpu().lastExceptionCode() == 1U,
            "CPU records TLB Modification exception code");
    require(machine.cpu().pc() == 0xFFFFFFFF80000180ULL,
            "CPU uses general exception vector for TLB modification");
    machine.cpu().setTlbEnabled(false);
    require(machine.stepCpu(), "SC executes at exception vector without reservation");

    std::uint32_t value = 0;
    require(bus.read32(dataAddress, value) && value == initial,
            "SC after TLB modification fault does not modify reserved memory");
    require(machine.cpu().reg(9U) == 0U,
            "SC reports failure because TLB modification fault invalidated reservation");
}


void testCpuDoublewordLoadLinkedStoreConditional() {
    N64Machine machine;
    Bus& bus = machine.bus();
    constexpr std::uint32_t dataAddress = 0x280U;
    constexpr std::uint64_t initial = 0x1122334455667788ULL;
    const std::uint32_t program[] = {
        0xD1090000U, // LLD t1,0(t0)
        0xF1090000U, // SCD t1,0(t0)
        0xD10A0000U, // LLD t2,0(t0)
        0x240B0055U, // ADDIU t3,zero,0x55
        0xAD0B0000U, // SW t3,0(t0), invalidates reservation
        0xF10A0000U  // SCD t2,0(t0), must fail
    };
    for (std::uint32_t i = 0; i < sizeof(program) / sizeof(program[0]); ++i)
        require(bus.write32(i * 4U, program[i]), "write LLD/SCD test program");
    require(bus.write64(dataAddress, initial), "initialize LLD/SCD target doubleword");
    machine.cpu().reset(0xFFFFFFFF80000000ULL);
        require(machine.cpu().setCp0Register(m64plus::n64::Cp0::kStatus,
                                                m64plus::n64::Cp0::kStatusKx),
                "enable Kernel 64-bit mode for doubleword instruction test");
    machine.cpu().setRegister(8U, dataAddress);
    require(machine.stepCpu(), "LLD loads linked doubleword");
    require(machine.cpu().reg(9U) == initial, "LLD loads full 64-bit value");
    require(machine.stepCpu(), "SCD succeeds with matching 64-bit reservation");
    std::uint64_t value = 0;
    require(bus.read64(dataAddress, value) && value == initial,
            "successful SCD writes the reserved doubleword");
    require(machine.cpu().reg(9U) == 1U, "successful SCD writes one to rt");
    require(machine.stepCpu(), "second LLD establishes a reservation");
    require(machine.stepCpu(), "prepare value before intervening store");
    require(machine.stepCpu(), "ordinary SW invalidates 64-bit reservation");
    require(machine.stepCpu(), "SCD fails after intervening store");
    require(bus.read64(dataAddress, value) && value == 0x0000005555667788ULL,
            "failed SCD leaves intervening word store intact");
    require(machine.cpu().reg(10U) == 0U, "failed SCD writes zero to rt");
}


void testCpuLoadLinkedStoreConditionalIgnoresUnrelatedBusWrites() {
    N64Machine machine;
    Bus& bus = machine.bus();
    constexpr std::uint32_t dataAddress = 0x2C0U;
    constexpr std::uint32_t unrelatedAddress = 0x400U;
    const std::uint32_t program[] = {0xC1090000U, 0xE1090000U}; // LL / SC
    for (std::uint32_t i = 0; i < 2U; ++i)
        require(bus.write32(i * 4U, program[i]), "write unrelated-write LL/SC program");
    require(bus.write32(dataAddress, 0xAABBCCDDU), "initialize LL/SC target");
    machine.cpu().reset(0xFFFFFFFF80000000ULL);
    machine.cpu().setRegister(8U, dataAddress);
    require(machine.stepCpu(), "LL establishes reservation");
    require(bus.write32(unrelatedAddress, 0x55667788U),
            "external bus agent writes outside the reserved 16-byte granule");
    require(machine.stepCpu(), "SC executes after unrelated external write");
    std::uint32_t value = 0U;
    require(bus.read32(dataAddress, value) && value == 0xAABBCCDDU,
            "unrelated write does not alter reserved word");
    require(machine.cpu().reg(9U) == 1U,
            "SC succeeds when only an unrelated reservation granule changed");
}

void testCpuLoadLinkedStoreConditionalDetectsWritesInSameGranule() {
    N64Machine machine;
    Bus& bus = machine.bus();
    constexpr std::uint32_t dataAddress = 0x2C0U;
    const std::uint32_t program[] = {0xC1090000U, 0xE1090000U}; // LL / SC
    for (std::uint32_t i = 0; i < 2U; ++i)
        require(bus.write32(i * 4U, program[i]), "write same-granule LL/SC program");
    require(bus.write32(dataAddress, 0xAABBCCDDU), "initialize same-granule LL/SC target");
    machine.cpu().reset(0xFFFFFFFF80000000ULL);
    machine.cpu().setRegister(8U, dataAddress);
    require(machine.stepCpu(), "LL establishes same-granule reservation");
    require(bus.write32(dataAddress + 8U, 0x55667788U),
            "external agent writes a different word in the reserved 16-byte granule");
    require(machine.stepCpu(), "SC executes after same-granule external write");
    std::uint32_t value = 0U;
    require(bus.read32(dataAddress, value) && value == 0xAABBCCDDU,
            "failed SC leaves reserved word unchanged");
    require(machine.cpu().reg(9U) == 0U,
            "SC fails when another word in the reservation granule changes");
}

void testCpuLoadLinkedStoreConditionalDetectsBusWrites() {
    N64Machine machine;
    Bus& bus = machine.bus();
    constexpr std::uint32_t dataAddress = 0x2C0U;
    constexpr std::uint32_t initial = 0xAABBCCDDU;
    const std::uint32_t program[] = {
        0xC1090000U, // LL t1,0(t0)
        0xE1090000U  // SC t1,0(t0)
    };
    for (std::uint32_t i = 0; i < 2U; ++i)
        require(bus.write32(i * 4U, program[i]), "write external bus LL/SC program");
    require(bus.write32(dataAddress, initial), "initialize external bus LL/SC word");
    machine.cpu().reset(0xFFFFFFFF80000000ULL);
    machine.cpu().setRegister(8U, dataAddress);
    require(machine.stepCpu(), "LL establishes reservation before external bus write");
    require(bus.write32(dataAddress, 0x55667788U),
            "external bus agent changes reserved RDRAM word");
    require(machine.stepCpu(), "SC executes after external bus write");
    std::uint32_t value = 0;
    require(bus.read32(dataAddress, value) && value == 0x55667788U,
            "SC does not overwrite data changed by external bus agent");
    require(machine.cpu().reg(9U) == 0U,
            "SC reports failure after external RDRAM write epoch changes");
}

void testCpuExceptionInvalidatesLoadLinkedReservation() {
    N64Machine machine;
    Bus& bus = machine.bus();
    constexpr std::uint32_t dataAddress = 0x2E0U;
    constexpr std::uint32_t initial = 0x12345678U;
    require(bus.write32(0x000U, 0xC1090000U), "write LL before exception");
    require(bus.write32(0x004U, 0x7000003FU), "write reserved instruction to raise exception");
    require(bus.write32(0x180U, 0xE1090000U), "write SC at general exception vector");
    require(bus.write32(dataAddress, initial), "initialize exception LL/SC target");
    machine.cpu().reset(0xFFFFFFFF80000000ULL);
    machine.cpu().setRegister(8U, dataAddress);
    require(machine.stepCpu(), "LL establishes reservation before exception");
    require(machine.stepCpu(), "reserved instruction raises and records exception");
    require(machine.cpu().hasLastException(), "CPU records exception after LL");
    require(machine.cpu().pc() == 0xFFFFFFFF80000180ULL,
            "CPU transfers control to general exception vector");
    require(machine.stepCpu(), "SC executes at exception vector without reservation");
    std::uint32_t value = 0;
    require(bus.read32(dataAddress, value) && value == initial,
            "SC after exception does not modify reserved memory");
    require(machine.cpu().reg(9U) == 0U,
            "SC reports failure because exception invalidated reservation");
}

void testCpuInterruptInvalidatesLoadLinkedReservation() {
    N64Machine machine;
    Bus& bus = machine.bus();
    constexpr std::uint32_t dataAddress = 0x2F0U;
    constexpr std::uint32_t initial = 0x87654321U;
    require(bus.write32(0x000U, 0xC1090000U), "write LL before interrupt");
    require(bus.write32(0x004U, 0xE1090000U), "write SC after interrupt");
    require(bus.write32(0x180U, 0xE1090000U), "write SC at interrupt exception vector");
    require(bus.write32(dataAddress, initial), "initialize interrupt LL/SC target");
    machine.cpu().reset(0xFFFFFFFF80000000ULL);
    machine.cpu().setRegister(8U, dataAddress);
    require(machine.stepCpu(), "LL establishes reservation before interrupt");
    require(machine.cpu().setCp0Register(m64plus::n64::Cp0::kStatus, 0x101ULL),
            "enable global and software interrupt mask in CP0 Status");
    require(machine.cpu().setCp0Register(m64plus::n64::Cp0::kCause, 0x100ULL),
            "assert software interrupt pending in CP0 Cause");
    require(machine.stepCpu(), "CPU accepts pending interrupt before SC");
    require(machine.cpu().lastExceptionCode() == 0U,
            "CPU records Interrupt exception code");
    require(machine.cpu().pc() == 0xFFFFFFFF80000180ULL,
            "CPU transfers control to general interrupt vector");
    require(machine.stepCpu(), "SC executes at interrupt vector without reservation");
    std::uint32_t value = 0;
    require(bus.read32(dataAddress, value) && value == initial,
            "SC after interrupt does not modify reserved memory");
    require(machine.cpu().reg(9U) == 0U,
            "SC reports failure because interrupt invalidated reservation");
}

void testCpuMemoryFaultInvalidatesLoadLinkedReservation() {
    N64Machine machine;
    Bus& bus = machine.bus();
    constexpr std::uint32_t dataAddress = 0x2F8U;
    constexpr std::uint32_t initial = 0xCAFEBABEU;
    require(bus.write32(0x000U, 0xC10A0000U), "write LL before memory fault");
    require(bus.write32(0x004U, 0x8D2A0000U), "write misaligned LW to raise address exception");
    require(bus.write32(0x180U, 0xE10A0000U), "write SC at memory-fault exception vector");
    require(bus.write32(dataAddress, initial), "initialize memory-fault LL/SC target");
    machine.cpu().reset(0xFFFFFFFF80000000ULL);
    machine.cpu().setRegister(8U, dataAddress);
    machine.cpu().setRegister(9U, dataAddress + 1U);
    require(machine.stepCpu(), "LL establishes reservation before memory fault");
    require(machine.stepCpu(), "misaligned LW raises and records address exception");
    require(machine.cpu().hasLastException(), "CPU records memory access exception after LL");
    require(machine.cpu().lastExceptionCode() == 4U,
            "CPU records Address Load exception code");
    require(machine.cpu().pc() == 0xFFFFFFFF80000180ULL,
            "CPU transfers control to memory-fault exception vector");
    require(machine.stepCpu(), "SC executes at memory-fault vector without reservation");
    std::uint32_t value = 0;
    require(bus.read32(dataAddress, value) && value == initial,
            "SC after memory fault does not modify reserved memory");
    require(machine.cpu().reg(10U) == 0U,
            "SC reports failure because memory fault invalidated reservation");
}


void testCpuBusDataFaultInvalidatesLoadLinkedReservation() {
    N64Machine machine;
    Bus& bus = machine.bus();
    constexpr std::uint32_t dataAddress = 0x300U;
    constexpr std::uint32_t unmappedAddress = 0x05000000U;
    constexpr std::uint32_t initial = 0x13579BDFU;
    require(bus.write32(0x000U, 0xC10A0000U), "write LL before bus data fault");
    require(bus.write32(0x004U, 0x8D2B0000U), "write LW from unmapped address");
    require(bus.write32(0x180U, 0xE10A0000U), "write SC at bus data exception vector");
    require(bus.write32(dataAddress, initial), "initialize bus-fault LL/SC target");
    machine.cpu().reset(0xFFFFFFFF80000000ULL);
    machine.cpu().setRegister(8U, dataAddress);
    machine.cpu().setRegister(9U, unmappedAddress);
    require(machine.stepCpu(), "LL establishes reservation before bus data fault");
    require(machine.stepCpu(), "unmapped LW raises bus data exception");
    require(machine.cpu().hasLastException(), "CPU records bus data exception after LL");
    require(machine.cpu().lastExceptionCode() == 7U,
            "CPU records Bus Data exception code");
    require(machine.cpu().pc() == 0xFFFFFFFF80000180ULL,
            "CPU transfers control to bus data exception vector");
    require(machine.stepCpu(), "SC executes at bus data vector without reservation");
    std::uint32_t value = 0;
    require(bus.read32(dataAddress, value) && value == initial,
            "SC after bus data fault does not modify reserved memory");
    require(machine.cpu().reg(10U) == 0U,
            "SC reports failure because bus data fault invalidated reservation");
}

void testCpuDataAddressErrorPrecedesTlbFault() {
    {
        N64Machine machine;
        Bus& bus = machine.bus();
        constexpr std::uint64_t badAddress = 0x00400001ULL;
        require(bus.write32(0x1000U, 0x8D490001U), "write unaligned LW to unmapped virtual page");
        machine.cpu().reset(0xFFFFFFFF80001000ULL);
        machine.cpu().setRegister(10U, 0x00400000U);
        machine.cpu().setTlbEnabled(true);
        require(machine.stepCpu(), "unaligned LW raises address error before TLB lookup");
        require(machine.cpu().lastExceptionCode() == 4U,
                "unaligned LW reports Address Load instead of TLB Load");
        require(machine.cpu().cp0().read(m64plus::n64::Cp0::kBadVAddr) == badAddress,
                "unaligned LW records its effective virtual address");
    }
    {
        N64Machine machine;
        Bus& bus = machine.bus();
        constexpr std::uint64_t badAddress = 0x00400001ULL;
        require(bus.write32(0x1000U, 0xAD4B0001U), "write unaligned SW to unmapped virtual page");
        machine.cpu().reset(0xFFFFFFFF80001000ULL);
        machine.cpu().setRegister(10U, 0x00400000U);
        machine.cpu().setRegister(11U, 0xA5A5A5A5U);
        machine.cpu().setTlbEnabled(true);
        require(machine.stepCpu(), "unaligned SW raises address error before TLB lookup");
        require(machine.cpu().lastExceptionCode() == 5U,
                "unaligned SW reports Address Store instead of TLB Store");
        require(machine.cpu().cp0().read(m64plus::n64::Cp0::kBadVAddr) == badAddress,
                "unaligned SW records its effective virtual address");
    }
}


void testCpuJalrUnalignedTargetFaultsAfterDelaySlot() {
    N64Machine machine;
    Bus& bus = machine.bus();
    constexpr std::uint64_t target = 0xFFFFFFFF80000022ULL;
    require(bus.write32(0x000U, 0x0200F809U), "write JALR r31,r16 with unaligned target");
    require(bus.write32(0x004U, 0x24091234U), "write ADDIU in JALR delay slot");
    machine.cpu().reset(0xFFFFFFFF80000000ULL);
    machine.cpu().setRegister(16U, target);

    require(machine.stepCpu(), "JALR schedules its unaligned target");
    require(!machine.cpu().hasLastException(), "JALR does not fault before its delay slot");
    require(machine.cpu().pc() == 0xFFFFFFFF80000004ULL,
            "JALR enters its delay slot before transferring to the target");
    require(machine.cpu().reg(31U) == 0xFFFFFFFF80000008ULL,
            "JALR writes the return address before its delay slot");

    require(machine.stepCpu(), "JALR delay slot executes before target fetch");
    require(machine.cpu().reg(9U) == 0x1234U,
            "JALR delay-slot instruction completes before the address error");
    require(machine.cpu().pc() == target,
            "JALR transfers to the original unaligned target after the delay slot");
    require(machine.stepCpu(), "unaligned JALR target raises fetch Address Load");
    require(machine.cpu().lastExceptionCode() == 4U,
            "unaligned JALR target raises Address Load");
    require(machine.cpu().cp0().read(m64plus::n64::Cp0::kBadVAddr) == target,
            "BadVAddr records the unaligned JALR target");
    require(machine.cpu().cp0().read(m64plus::n64::Cp0::kEpc) == target,
            "EPC identifies the target fetch, not the JALR instruction");
    require((machine.cpu().cp0().read(m64plus::n64::Cp0::kCause) & (1ULL << 31U)) == 0U,
            "target-fetch Address Load does not set Cause.BD");
}

void testCpuNonCanonicalInstructionAddressRaisesAddressLoad() {
    N64Machine machine;
    constexpr std::uint64_t invalidPc = 0x0000000180000000ULL;
    machine.cpu().reset(invalidPc);

    require(machine.stepCpu(), "non-canonical instruction address raises an exception");
    require(machine.cpu().hasLastException(), "CPU records instruction address exception");
    require(machine.cpu().lastExceptionCode() == 4U,
            "non-canonical instruction fetch raises Address Load, not Bus Instruction");
    require(machine.cpu().cp0().read(m64plus::n64::Cp0::kBadVAddr) == invalidPc,
            "BadVAddr records the non-canonical instruction address");
}

void testCpuBusInstructionFaultInvalidatesLoadLinkedReservation() {
    N64Machine machine;
    Bus& bus = machine.bus();
    constexpr std::uint32_t dataAddress = 0x320U;
    constexpr std::uint32_t initial = 0x2468ACE0U;
    require(bus.write32(0x000U, 0xC1080000U), "write LL before bus instruction fault");
    require(bus.write32(0x004U, 0x01200008U), "write JR to unmapped instruction address");
    require(bus.write32(0x008U, 0x00000000U), "write JR delay-slot NOP");
    require(bus.write32(0x180U, 0xE1080000U), "write SC at bus instruction exception vector");
    require(bus.write32(dataAddress, initial), "initialize bus-instruction LL/SC target");
    machine.cpu().reset(0xFFFFFFFF80000000ULL);
    machine.cpu().setRegister(8U, dataAddress);
    machine.cpu().setRegister(9U, 0xFFFFFFFF85000000ULL);
    require(machine.stepCpu(), "LL establishes reservation before bus instruction fault");
    require(machine.stepCpu(), "JR schedules unmapped instruction address");
    require(machine.stepCpu(), "JR delay slot executes before instruction fetch fault");
    require(machine.stepCpu(), "unmapped instruction fetch raises bus instruction exception");
    require(machine.cpu().hasLastException(), "CPU records bus instruction exception after LL");
    require(machine.cpu().lastExceptionCode() == 6U,
            "CPU records Bus Instruction exception code");
    require(machine.cpu().pc() == 0xFFFFFFFF80000180ULL,
            "CPU transfers control to bus instruction exception vector");
    require(machine.stepCpu(), "SC executes at bus instruction vector without reservation");
    std::uint32_t value = 0;
    require(bus.read32(dataAddress, value) && value == initial,
            "SC after bus instruction fault does not modify reserved memory");
    require(machine.cpu().reg(10U) == 0U,
            "SC reports failure because bus instruction fault invalidated reservation");
}

void testCpuSignedArithmeticAndImmediateComparisons() {
    N64Machine machine;
    Bus& bus = machine.bus();
    const std::uint32_t program[] = {
        0x20080007U, // ADDI t0,zero,7
        0x29090008U, // SLTI t1,t0,8
        0x2D0A0008U, // SLTIU t2,t0,8
        0x610BFFFEU, // DADDI t3,t0,-2
        0x010B602CU, // DADD t4,t0,t3
        0x010B682EU  // DSUB t5,t0,t3
    };
    for (std::uint32_t i = 0; i < 6U; ++i)
        require(bus.write32(i * 4U, program[i]), "load signed arithmetic program");
    machine.cpu().reset(0xFFFFFFFF80000000ULL);
    require(machine.cpu().setCp0Register(m64plus::n64::Cp0::kStatus,
                                           m64plus::n64::Cp0::kStatusKx),
            "enable Kernel 64-bit mode for MIPS III instruction test");
    for (unsigned i = 0; i < 6U; ++i)
        require(machine.stepCpu(), "execute signed arithmetic instruction");
    require(machine.cpu().reg(8U) == 7U, "ADDI writes a signed 32-bit result");
    require(machine.cpu().reg(9U) == 1U, "SLTI compares signed operands");
    require(machine.cpu().reg(10U) == 1U, "SLTIU compares unsigned operands");
    require(machine.cpu().reg(11U) == 5U, "DADDI sign-extends its immediate");
    require(machine.cpu().reg(12U) == 12U, "DADD computes a 64-bit signed sum");
    require(machine.cpu().reg(13U) == 2U, "DSUB computes a 64-bit signed difference");

    machine.cpu().reset(0xFFFFFFFF80000000ULL);
    machine.cpu().setRegister(8U, 0x7FFFFFFFULL);
    require(bus.write32(0U, 0x21090001U), "write overflowing ADDI instruction");
    require(machine.stepCpu(), "ADDI overflow is handled by exception path");
    require(machine.cpu().hasLastException() &&
            machine.cpu().lastExceptionCode() == 12U,
            "ADDI overflow raises Integer Overflow exception");

    machine.cpu().reset(0xFFFFFFFF80000000ULL);
    require(machine.cpu().setCp0Register(m64plus::n64::Cp0::kStatus,
                                           m64plus::n64::Cp0::kStatusKx),
            "enable Kernel 64-bit mode for MIPS III instruction test");
    machine.cpu().setRegister(8U, 0x7FFFFFFFFFFFFFFFULL);
    require(bus.write32(0U, 0x61090001U), "write overflowing DADDI instruction");
    require(machine.stepCpu(), "DADDI overflow is handled by exception path");
    require(machine.cpu().hasLastException() &&
            machine.cpu().lastExceptionCode() == 12U,
            "DADDI overflow raises Integer Overflow exception");
}

void testCpuUnalignedWordMemoryOperations() {
    N64Machine machine;
    Bus& bus = machine.bus();
    const std::uint32_t initialMemory = 0x11223344U;
    const std::uint64_t initialRegister = 0xFFFFFFFFAABBCCDDULL;
    const std::uint32_t offsets[] = {0U, 1U, 2U, 3U};
    const std::uint64_t expectedLwl[] = {
        0x0000000011223344ULL, 0x00000000223344DDULL,
        0x000000003344CCDDULL, 0x0000000044BBCCDDULL
    };
    const std::uint64_t expectedLwr[] = {
        0xFFFFFFFFAABBCC11ULL, 0xFFFFFFFFAABB1122ULL,
        0xFFFFFFFFAA112233ULL, 0x0000000011223344ULL
    };
    const std::uint32_t expectedSwl[] = {
        0xAABBCCDDU, 0x11AABBCCU, 0x1122AABBU, 0x112233AAU
    };
    const std::uint32_t expectedSwr[] = {
        0xDD223344U, 0xCCDD3344U, 0xBBCCDD44U, 0xAABBCCDDU
    };

    for (std::uint32_t offset : offsets) {
        const std::uint32_t immediate = offset;
        require(bus.write32(0U, initialMemory), "initialize unaligned load memory");
        require(bus.write32(4U, 0x88090000U | immediate), "write LWL instruction");
        machine.cpu().reset(0xFFFFFFFF80000004ULL);
        machine.cpu().setRegister(9U, initialRegister);
        require(machine.stepCpu(), "execute LWL for each byte offset");
        require(machine.cpu().reg(9U) == expectedLwl[offset],
                "LWL produces expected big-endian merge at every byte offset");

        require(bus.write32(4U, 0x98090000U | immediate), "write LWR instruction");
        machine.cpu().reset(0xFFFFFFFF80000004ULL);
        machine.cpu().setRegister(9U, initialRegister);
        require(machine.stepCpu(), "execute LWR for each byte offset");
        require(machine.cpu().reg(9U) == expectedLwr[offset],
                "LWR produces expected big-endian merge at every byte offset");

        std::uint32_t stored = 0;
        require(bus.write32(0U, initialMemory), "reset SWL memory");
        require(bus.write32(4U, 0xA8090000U | immediate), "write SWL instruction");
        machine.cpu().reset(0xFFFFFFFF80000004ULL);
        machine.cpu().setRegister(9U, initialRegister);
        require(machine.stepCpu(), "execute SWL for each byte offset");
        require(bus.read32(0U, stored) && stored == expectedSwl[offset],
                "SWL preserves untouched bytes at every byte offset");

        require(bus.write32(0U, initialMemory), "reset SWR memory");
        require(bus.write32(4U, 0xB8090000U | immediate), "write SWR instruction");
        machine.cpu().reset(0xFFFFFFFF80000004ULL);
        machine.cpu().setRegister(9U, initialRegister);
        require(machine.stepCpu(), "execute SWR for each byte offset");
        require(bus.read32(0U, stored) && stored == expectedSwr[offset],
                "SWR preserves untouched bytes at every byte offset");
    }
}

 
void testCpuUnalignedDoublewordMemoryOperations() {
    N64Machine machine;
    Bus& bus = machine.bus();
    const std::uint64_t initialMemory = 0x1122334455667788ULL;
    const std::uint64_t initialRegister = 0xA1A2A3A4A5A6A7A8ULL;
    const std::uint32_t offsets[] = {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U};
    const std::uint64_t expectedLdl[] = {
        0x1122334455667788ULL, 0x22334455667788A8ULL,
        0x334455667788A7A8ULL, 0x4455667788A6A7A8ULL,
        0x55667788A5A6A7A8ULL, 0x667788A4A5A6A7A8ULL,
        0x7788A3A4A5A6A7A8ULL, 0x88A2A3A4A5A6A7A8ULL
    };
    const std::uint64_t expectedLdr[] = {
        0xA1A2A3A4A5A6A711ULL, 0xA1A2A3A4A5A61122ULL,
        0xA1A2A3A4A5112233ULL, 0xA1A2A3A411223344ULL,
        0xA1A2A31122334455ULL, 0xA1A2112233445566ULL,
        0xA111223344556677ULL, 0x1122334455667788ULL
    };
    const std::uint64_t expectedSdl[] = {
        0xA1A2A3A4A5A6A7A8ULL, 0x11A1A2A3A4A5A6A7ULL,
        0x1122A1A2A3A4A5A6ULL, 0x112233A1A2A3A4A5ULL,
        0x11223344A1A2A3A4ULL, 0x1122334455A1A2A3ULL,
        0x112233445566A1A2ULL, 0x11223344556677A1ULL
    };
    const std::uint64_t expectedSdr[] = {
        0xA822334455667788ULL, 0xA7A8334455667788ULL,
        0xA6A7A84455667788ULL, 0xA5A6A7A855667788ULL,
        0xA4A5A6A7A8667788ULL, 0xA3A4A5A6A7A87788ULL,
        0xA2A3A4A5A6A7A888ULL, 0xA1A2A3A4A5A6A7A8ULL
    };

    for (std::uint32_t offset : offsets) {
        std::uint64_t loaded = 0;
        require(bus.write64(0U, initialMemory), "initialize LDL/LDR memory");
        require(bus.write32(0x1000U, 0x68090000U | offset), "write LDL instruction");
        machine.cpu().reset(0xFFFFFFFF80001000ULL);
        require(machine.cpu().setCp0Register(m64plus::n64::Cp0::kStatus,
                                                m64plus::n64::Cp0::kStatusKx),
                "enable Kernel 64-bit mode for doubleword instruction test");
        machine.cpu().setRegister(9U, initialRegister);
        require(machine.stepCpu(), "execute LDL at each byte offset");
        require(machine.cpu().reg(9U) == expectedLdl[offset],
                "LDL merges expected big-endian bytes at every offset");

        require(bus.write32(0x1000U, 0x6C090000U | offset), "write LDR instruction");
        machine.cpu().reset(0xFFFFFFFF80001000ULL);
        require(machine.cpu().setCp0Register(m64plus::n64::Cp0::kStatus,
                                                m64plus::n64::Cp0::kStatusKx),
                "enable Kernel 64-bit mode for doubleword instruction test");
        machine.cpu().setRegister(9U, initialRegister);
        require(machine.stepCpu(), "execute LDR at each byte offset");
        require(machine.cpu().reg(9U) == expectedLdr[offset],
                "LDR merges expected big-endian bytes at every offset");

        require(bus.write64(0U, initialMemory), "reset SDL memory");
        require(bus.write32(0x1000U, 0xB0090000U | offset), "write SDL instruction");
        machine.cpu().reset(0xFFFFFFFF80001000ULL);
        require(machine.cpu().setCp0Register(m64plus::n64::Cp0::kStatus,
                                                m64plus::n64::Cp0::kStatusKx),
                "enable Kernel 64-bit mode for doubleword instruction test");
        machine.cpu().setRegister(9U, initialRegister);
        require(machine.stepCpu(), "execute SDL at each byte offset");
        require(bus.read64(0U, loaded) && loaded == expectedSdl[offset],
                "SDL preserves untouched bytes at every offset");

        require(bus.write64(0U, initialMemory), "reset SDR memory");
        require(bus.write32(0x1000U, 0xB4090000U | offset), "write SDR instruction");
        machine.cpu().reset(0xFFFFFFFF80001000ULL);
        require(machine.cpu().setCp0Register(m64plus::n64::Cp0::kStatus,
                                                m64plus::n64::Cp0::kStatusKx),
                "enable Kernel 64-bit mode for doubleword instruction test");
        machine.cpu().setRegister(9U, initialRegister);
        require(machine.stepCpu(), "execute SDR at each byte offset");
        require(bus.read64(0U, loaded) && loaded == expectedSdr[offset],
                "SDR preserves untouched bytes at every offset");
    }
}

void testCpuUnalignedDoublewordMemoryBoundaryFaults() {
    N64Machine machine;
    Bus& bus = machine.bus();
    constexpr std::uint64_t sentinel = 0xA1A2A3A4A5A6A7A8ULL;
    const std::uint32_t opcodes[] = {
        0x69090000U, // LDL base=$t0
        0x6D090000U, // LDR base=$t0
        0xB1090000U, // SDL base=$t0
        0xB5090000U  // SDR base=$t0
    };
    std::uint64_t lastValidWord = 0;
    require(bus.write64(0x007FFFF8U, 0x1122334455667788ULL),
            "initialize final valid RDRAM doubleword");
    for (std::uint32_t opcode : opcodes) {
        require(bus.write32(0x1000U, opcode), "write boundary access instruction");
        machine.cpu().reset(0xFFFFFFFF80001000ULL);
        require(machine.cpu().setCp0Register(m64plus::n64::Cp0::kStatus,
                                                m64plus::n64::Cp0::kStatusKx),
                "enable Kernel 64-bit mode for doubleword instruction test");
        machine.cpu().setRegister(8U, 0x00800000ULL);
        machine.cpu().setRegister(9U, sentinel);
        require(machine.stepCpu(), "boundary access raises a handled CPU exception");
        require(machine.cpu().hasLastException() &&
                machine.cpu().lastExceptionCode() == 7U,
                "unaligned doubleword access beyond RDRAM reports BusData");
        require(machine.cpu().reg(9U) == sentinel,
                "failed doubleword access does not modify target register");
        require(bus.read64(0x007FFFF8U, lastValidWord) &&
                lastValidWord == 0x1122334455667788ULL,
                "failed doubleword access does not corrupt final valid RDRAM word");
    }
}

void testCpuRunnerHonorsInstructionBudgetAndStopsAtBudget() {
    N64Machine machine;
    Bus& bus = machine.bus();
    const std::uint32_t program[] = {
        0x24080001U, // ADDIU t0,zero,1
        0x25080001U, // ADDIU t0,t0,1
        0x25080001U, // ADDIU t0,t0,1
        0x25080001U  // ADDIU t0,t0,1
    };
    for (std::uint32_t i = 0; i < 4U; ++i)
        require(bus.write32(i * 4U, program[i]), "load bounded CPU runner program");
    machine.cpu().reset(0xFFFFFFFF80000000ULL);
    require(machine.runCpuInstructions(0U) == 0U,
            "zero CPU instruction budget executes nothing");
    require(machine.cpu().instructionsExecuted() == 0U,
            "zero budget leaves CPU instruction counter unchanged");
    require(machine.runCpuInstructions(2U) == 2U,
            "CPU runner executes exactly the requested first slice");
    require(machine.cpu().reg(8U) == 2U &&
            machine.cpu().pc() == 0xFFFFFFFF80000008ULL,
            "first CPU slice advances state by two instructions");
    require(machine.runCpuInstructions(1U) == 1U,
            "CPU runner executes the next bounded slice");
    require(machine.cpu().reg(8U) == 3U &&
            machine.cpu().pc() == 0xFFFFFFFF8000000CULL,
            "second CPU slice continues from prior PC");
    require(machine.runCpuInstructions(1U) == 1U,
            "CPU runner can execute final instruction in a slice");
    require(machine.cpu().reg(8U) == 4U,
            "CPU runner executes final instruction without exceeding budget");
}


void testCpuRejectsInvalidCop0ReservedFields() {
    const std::uint32_t invalidInstructions[] = {
        0x40086008U, // MFC0 with reserved bit 3 set
        0x40886008U, // MTC0 with reserved bit 3 set
        0x40086009U, // MFC0 with unsupported register select 1
        0x40886009U, // MTC0 with unsupported register select 1
        0x40886001U, // MTC0 with register select 1 and no reserved bits
        0x42000048U, // TLBP with reserved bit 6 set
        0x42000058U  // ERET with reserved bit 6 set
    };
    for (const std::uint32_t instruction : invalidInstructions) {
        N64Machine machine;
        require(machine.bus().write32(0U, instruction),
                "write COP0 instruction with reserved bits set");
        machine.cpu().reset(0xFFFFFFFF80000000ULL);
        require(machine.runCpuInstructions(1U) == 0U,
                "invalid COP0 reserved field raises an exception");
        require(machine.cpu().hasLastException() &&
                machine.cpu().lastExceptionCode() == 10U,
                "invalid COP0 reserved field raises Reserved Instruction");
        require(machine.cpu().lastExceptionInstruction() == instruction,
                "CPU records invalid COP0 encoding");
        require(machine.cpu().pc() == 0xFFFFFFFF80000180ULL,
                "invalid COP0 encoding enters general exception vector");
    }
}

void testCpuCop1UnusableExceptionSetsCauseCoprocessorNumber() {
    N64Machine machine;
    require(machine.bus().write32(0U, 0x46000000U),
            "write a COP1 instruction while CU1 is disabled");
    auto& cpu = machine.cpu();
    cpu.reset(0U);
    require(machine.stepCpu(), "COP1 instruction raises Coprocessor Unusable");
    require(cpu.hasLastException() &&
                cpu.lastExceptionCode() ==
                    static_cast<std::uint8_t>(m64plus::n64::ExceptionCode::CoprocessorUnusable),
            "COP1 with CU1 disabled raises Coprocessor Unusable");
    const std::uint64_t cause = cpu.cp0().read(m64plus::n64::Cp0::kCause);
    require(((cause >> 2U) & 0x1FULL) == 11U,
            "COP1 unusable exception records ExcCode 11");
    require(((cause >> 28U) & 3ULL) == 1ULL,
            "COP1 unusable exception records coprocessor number 1 in Cause.CE");
    require(cpu.pc() == 0xFFFFFFFF80000180ULL,
            "COP1 unusable exception enters the general exception vector");
}


void testCpuCop1WithCu1EnabledIsStillUnimplemented() {
    N64Machine machine;
    require(machine.bus().write32(0U, 0x46000000U),
            "write COP1 instruction while CU1 is enabled");
    auto& cpu = machine.cpu();
    cpu.reset(0U);
    require(cpu.setCp0Register(m64plus::n64::Cp0::kStatus,
                               m64plus::n64::Cp0::kStatusCu1),
            "enable CU1 for unimplemented COP1 test");
    require(machine.stepCpu(), "unimplemented COP1 instruction raises Reserved Instruction");
    require(cpu.hasLastException() &&
                cpu.lastExceptionCode() ==
                    static_cast<std::uint8_t>(m64plus::n64::ExceptionCode::ReservedInstruction),
            "COP1 with CU1 enabled raises Reserved Instruction until FPU exists");
    const std::uint64_t cause = cpu.cp0().read(m64plus::n64::Cp0::kCause);
    require(((cause >> 2U) & 0x1FULL) == 10U,
            "unimplemented COP1 instruction records ExcCode 10");
    require(((cause >> 28U) & 3ULL) == 0ULL,
            "Reserved Instruction does not report a false coprocessor number");
}

void testCpuCop1WordRegisterTransfers() {
    N64Machine machine;
    Bus& bus = machine.bus();
    auto& cpu = machine.cpu();
    const auto cop1 = [](unsigned move, unsigned rt, unsigned fs) {
        return (0x11U << 26U) | (move << 21U) |
               (rt << 16U) | (fs << 11U);
    };

    cpu.reset(0U);
    require(cpu.setCp0Register(m64plus::n64::Cp0::kStatus,
                               m64plus::n64::Cp0::kStatusCu1),
            "enable CU1 for MTC1/MFC1 transfer test");
    cpu.setRegister(8U, 0xFEDCBA9876543210ULL);
    require(bus.write32(0U, cop1(4U, 8U, 3U)),
            "write MTC1 instruction");
    require(bus.write32(4U, cop1(0U, 9U, 3U)),
            "write MFC1 instruction");
    require(machine.stepCpu(), "MTC1 transfers a GPR word into an FPR");
    require(machine.stepCpu(), "MFC1 transfers an FPR word into a GPR");
    require(!cpu.hasLastException() &&
                cpu.reg(9U) == 0x0000000076543210ULL,
            "MTC1 stores the low word and MFC1 sign-extends only its 32-bit value");

    cpu.reset(0U);
    require(bus.write32(0U, cop1(0U, 9U, 3U)),
            "write MFC1 instruction for CU1-disabled check");
    require(machine.stepCpu(), "MFC1 with CU1 clear raises Coprocessor Unusable");
    require(cpu.hasLastException() &&
                cpu.lastExceptionCode() ==
                    static_cast<std::uint8_t>(
                        m64plus::n64::ExceptionCode::CoprocessorUnusable),
            "MFC1 checks CU1 before accessing FPR state");
    require(((cpu.cp0().read(m64plus::n64::Cp0::kCause) >> 28U) & 3ULL) == 1ULL,
            "MFC1 CU1-disabled exception reports CE=1");

    // FR=1 exposes each FPR as a full 64-bit register for DMFC1/DMTC1.
    cpu.reset(0U);
    require(cpu.setCp0Register(m64plus::n64::Cp0::kStatus,
                               m64plus::n64::Cp0::kStatusCu1 |
                               m64plus::n64::Cp0::kStatusFr),
            "enable CU1 and FR=1 for doubleword FPR transfers");
    cpu.setRegister(8U, 0xFEDCBA9876543210ULL);
    require(bus.write32(0U, cop1(5U, 8U, 6U)), "write DMTC1 instruction");
    require(bus.write32(4U, cop1(1U, 9U, 6U)), "write DMFC1 instruction");
    require(machine.stepCpu(), "DMTC1 transfers a full GPR doubleword into an FPR");
    require(machine.stepCpu(), "DMFC1 transfers a full FPR doubleword into a GPR");
    require(!cpu.hasLastException() && cpu.reg(9U) == 0xFEDCBA9876543210ULL,
            "DMTC1 and DMFC1 preserve all 64 FPR bits");

    // FR=0 does not permit the 64-bit transfer forms.
    cpu.reset(0U);
    require(cpu.setCp0Register(m64plus::n64::Cp0::kStatus,
                               m64plus::n64::Cp0::kStatusCu1),
            "enable CU1 with FR=0");
    require(bus.write32(0U, cop1(1U, 9U, 6U)), "write DMFC1 with FR=0");
    require(machine.stepCpu(), "DMFC1 with FR=0 raises Reserved Instruction");
    require(cpu.hasLastException() &&
                cpu.lastExceptionCode() ==
                    static_cast<std::uint8_t>(
                        m64plus::n64::ExceptionCode::ReservedInstruction),
            "DMFC1 is rejected when FR=0");

    // Verify the word transfer updates only the low half of an FPR.
    cpu.reset(0U);
    require(cpu.setCp0Register(m64plus::n64::Cp0::kStatus,
                               m64plus::n64::Cp0::kStatusCu1 |
                               m64plus::n64::Cp0::kStatusFr),
            "enable FR=1 to seed a doubleword FPR value");
    cpu.setRegister(8U, 0x1122334455667788ULL);
    require(bus.write32(0U, cop1(5U, 8U, 6U)), "write DMTC1 seed instruction");
    require(machine.stepCpu(), "seed FPR upper and lower words with DMTC1");
    require(cpu.setCp0Register(m64plus::n64::Cp0::kStatus,
                               m64plus::n64::Cp0::kStatusCu1),
            "switch to FR=0 for MTC1 preservation check");
    cpu.setRegister(8U, 0xAABBCCDDEEFF0011ULL);
    require(bus.write32(4U, cop1(4U, 8U, 6U)), "write MTC1 low-word update");
    require(machine.stepCpu(), "MTC1 updates FPR low word in FR=0");
    require(cpu.setCp0Register(m64plus::n64::Cp0::kStatus,
                               m64plus::n64::Cp0::kStatusCu1 |
                               m64plus::n64::Cp0::kStatusFr),
            "switch back to FR=1 to inspect full FPR");
    require(bus.write32(8U, cop1(1U, 9U, 6U)), "write DMFC1 verification");
    require(machine.stepCpu(), "read updated FPR with DMFC1");
    require(!cpu.hasLastException() && cpu.reg(9U) == 0x11223344EEFF0011ULL,
            "MTC1 preserves the FPR upper word while replacing its lower word");
}

void testCpuCop1InstructionClassesHaveCorrectExceptionBehavior() {
    N64Machine machine;
    Bus& bus = machine.bus();
    auto& cpu = machine.cpu();
    const std::vector<unsigned> cop1Formats = {
        0U, 1U, 2U, 4U, 5U, 6U, 8U, 16U, 17U, 20U, 21U, 22U
    };

    for (const unsigned format : cop1Formats) {
        const std::uint32_t instruction =
            (0x11U << 26U) | (format << 21U);
        require(bus.write32(0U, instruction),
                "write COP1 instruction class for CU1-disabled test");
        cpu.reset(0U);
        require(machine.stepCpu(),
                "COP1 instruction with CU1 disabled raises an exception");
        require(cpu.hasLastException() &&
                    cpu.lastExceptionCode() ==
                        static_cast<std::uint8_t>(
                            m64plus::n64::ExceptionCode::CoprocessorUnusable),
                "all COP1 instruction classes trap as Coprocessor Unusable when CU1 is clear");
        const std::uint64_t cause = cpu.cp0().read(m64plus::n64::Cp0::kCause);
        require(((cause >> 28U) & 3ULL) == 1ULL,
                "all CU1-disabled COP1 classes report CE=1");

        cpu.reset(0U);
        require(cpu.setCp0Register(m64plus::n64::Cp0::kStatus,
                                   m64plus::n64::Cp0::kStatusCu1),
                "enable CU1 for COP1 instruction class test");
        const bool stepResult = machine.stepCpu();
        if (format == 0U || format == 4U) {
            require(stepResult && !cpu.hasLastException(),
                    "implemented MFC1/MTC1 transfers execute when CU1 is set");
        } else {
            require(stepResult &&
                        cpu.hasLastException() &&
                        cpu.lastExceptionCode() ==
                            static_cast<std::uint8_t>(
                                m64plus::n64::ExceptionCode::ReservedInstruction),
                    "unimplemented COP1 instruction classes raise Reserved Instruction when CU1 is set");
        }
    }
}

void testCpuCop0RequiresKernelPrivilege() {
    const auto runCase = [](std::uint64_t status, bool shouldExecute,
                            const char* message) {
        N64Machine machine;
        require(machine.bus().write32(0U, 0x40086000U),
                "write MFC0 instruction for privilege test");
        auto& cpu = machine.cpu();
        cpu.reset(0U);
        require(cpu.setCp0Register(m64plus::n64::Cp0::kStatus, status),
                "set CP0 privilege mode for test");
        const std::uint64_t executed = machine.runCpuInstructions(1U);
        if (shouldExecute) {
            require(executed == 1U && !cpu.hasLastException(),
                    message);
            require(cpu.pc() == 4U,
                    "privileged override continues after MFC0");
        } else {
            require(executed == 0U && cpu.hasLastException() &&
                    cpu.lastExceptionCode() == 11U,
                    message);
            require(cpu.pc() == 0xFFFFFFFF80000180ULL,
                    "user-mode COP0 access enters general exception vector");
            require(((cpu.cp0().read(m64plus::n64::Cp0::kCause) >> 2U) & 0x1FULL) == 11U,
                    "user-mode COP0 access records Coprocessor Unusable");
        }
    };

    runCase(2ULL << 3U, false,
            "user mode cannot execute MFC0");
    runCase(1ULL << 3U, false,
            "supervisor mode cannot execute MFC0");
    runCase((2ULL << 3U) | m64plus::n64::Cp0::kStatusExl, true,
            "EXL forces kernel privilege for MFC0");
    runCase((2ULL << 3U) | m64plus::n64::Cp0::kStatusErl, true,
            "ERL forces kernel privilege for MFC0");
}

void testCpuCop0PrivilegedOperationsRequireKernelPrivilege() {
    const std::uint32_t instructions[] = {
        0x42000008U, // TLBP
        0x42000001U, // TLBR
        0x42000002U, // TLBWI
        0x42000006U, // TLBWR
        0x42000018U  // ERET
    };
    const std::uint64_t userAndSupervisorModes[] = {
        2ULL << 3U,
        1ULL << 3U
    };
    for (const std::uint32_t instruction : instructions) {
        for (const std::uint64_t status : userAndSupervisorModes) {
            N64Machine machine;
            require(machine.bus().write32(0U, instruction),
                    "write privileged COP0 operation");
            auto& cpu = machine.cpu();
            cpu.reset(0U);
            require(cpu.setCp0Register(m64plus::n64::Cp0::kStatus, status),
                    "set non-kernel mode for privileged COP0 operation");
            require(machine.runCpuInstructions(1U) == 0U,
                    "non-kernel COP0 operation is not executed");
            require(cpu.hasLastException() && cpu.lastExceptionCode() == 11U,
                    "non-kernel COP0 operation raises Coprocessor Unusable");
            require(cpu.pc() == 0xFFFFFFFF80000180ULL,
                    "non-kernel COP0 operation enters general exception vector");
            require(((cpu.cp0().read(m64plus::n64::Cp0::kCause) >> 2U) & 0x1FULL) == 11U,
                    "non-kernel COP0 operation records exception code 11");
        }
    }
}

void testCpuUserModeCannotFetchFromKseg0OrKseg1() {
    const std::uint64_t addresses[] = {
        0xFFFFFFFF80000000ULL,
        0xFFFFFFFFA0000000ULL
    };
    for (const std::uint64_t address : addresses) {
        N64Machine machine;
        auto& cpu = machine.cpu();
        cpu.reset(address);
        require(cpu.setCp0Register(m64plus::n64::Cp0::kStatus, 2ULL << 3U),
                "set user mode before KSEG instruction fetch");
        require(machine.stepCpu(), "user KSEG instruction fetch raises exception");
        require(cpu.hasLastException() && cpu.lastExceptionCode() == 4U,
                "user KSEG instruction fetch raises Address Load");
        require(cpu.pc() == 0xFFFFFFFF80000180ULL,
                "user KSEG instruction fetch enters general exception vector");
    }
}

void testCpuUserModeCannotLoadOrStoreKseg0OrKseg1() {
    const std::uint64_t addresses[] = {
        0xFFFFFFFF80000000ULL,
        0xFFFFFFFFA0000000ULL
    };
    for (const std::uint64_t address : addresses) {
        {
            N64Machine machine;
            Bus& bus = machine.bus();
            require(bus.write32(0U, 0x8C220000U),
                    "write LW from KSEG test instruction");
            auto& cpu = machine.cpu();
            cpu.reset(0U);
            cpu.setRegister(1U, address);
            require(cpu.setCp0Register(m64plus::n64::Cp0::kStatus, 2ULL << 3U),
                    "set user mode before KSEG load");
            require(machine.stepCpu(), "user KSEG load raises exception");
            require(cpu.hasLastException() && cpu.lastExceptionCode() == 4U,
                    "user KSEG load raises Address Load");
            require(cpu.pc() == 0xFFFFFFFF80000180ULL,
                    "user KSEG load enters general exception vector");
        }
        {
            N64Machine machine;
            Bus& bus = machine.bus();
            require(bus.write32(0U, 0xAC220000U),
                    "write SW to KSEG test instruction");
            auto& cpu = machine.cpu();
            cpu.reset(0U);
            cpu.setRegister(1U, address);
            cpu.setRegister(2U, 0x12345678U);
            require(cpu.setCp0Register(m64plus::n64::Cp0::kStatus, 2ULL << 3U),
                    "set user mode before KSEG store");
            require(machine.stepCpu(), "user KSEG store raises exception");
            require(cpu.hasLastException() && cpu.lastExceptionCode() == 5U,
                    "user KSEG store raises Address Store");
            require(cpu.pc() == 0xFFFFFFFF80000180ULL,
                    "user KSEG store enters general exception vector");
        }
    }
}


void testCpuSupervisorModeCannotAccessKseg0OrKseg1() {
    const std::uint64_t addresses[] = {
        0xFFFFFFFF80000000ULL,
        0xFFFFFFFFA0000000ULL
    };
    for (const std::uint64_t address : addresses) {
        {
            N64Machine machine;
            auto& cpu = machine.cpu();
            cpu.reset(address);
            require(cpu.setCp0Register(m64plus::n64::Cp0::kStatus, 1ULL << 3U),
                    "set supervisor mode before KSEG instruction fetch");
            require(machine.stepCpu(), "supervisor KSEG instruction fetch raises exception");
            require(cpu.hasLastException() && cpu.lastExceptionCode() == 4U,
                    "supervisor KSEG0/KSEG1 instruction fetch raises Address Load");
            require(cpu.pc() == 0xFFFFFFFF80000180ULL,
                    "supervisor KSEG instruction fetch enters general exception vector");
        }
        {
            N64Machine machine;
            Bus& bus = machine.bus();
            require(bus.write32(0U, 0x8C220000U),
                    "write LW from supervisor KSEG test instruction");
            auto& cpu = machine.cpu();
            cpu.reset(0U);
            cpu.setRegister(1U, address);
            require(cpu.setCp0Register(m64plus::n64::Cp0::kStatus, 1ULL << 3U),
                    "set supervisor mode before KSEG load");
            require(machine.stepCpu(), "supervisor KSEG load raises exception");
            require(cpu.hasLastException() && cpu.lastExceptionCode() == 4U,
                    "supervisor KSEG0/KSEG1 load raises Address Load");
            require(cpu.pc() == 0xFFFFFFFF80000180ULL,
                    "supervisor KSEG load enters general exception vector");
        }
        {
            N64Machine machine;
            Bus& bus = machine.bus();
            require(bus.write32(0U, 0xAC220000U),
                    "write SW to supervisor KSEG test instruction");
            auto& cpu = machine.cpu();
            cpu.reset(0U);
            cpu.setRegister(1U, address);
            cpu.setRegister(2U, 0x12345678U);
            require(cpu.setCp0Register(m64plus::n64::Cp0::kStatus, 1ULL << 3U),
                    "set supervisor mode before KSEG store");
            require(machine.stepCpu(), "supervisor KSEG store raises exception");
            require(cpu.hasLastException() && cpu.lastExceptionCode() == 5U,
                    "supervisor KSEG0/KSEG1 store raises Address Store");
            require(cpu.pc() == 0xFFFFFFFF80000180ULL,
                    "supervisor KSEG store enters general exception vector");
        }
    }
}


void testCpuPrivilegeChecksForSsegAndKseg3() {
    const std::uint64_t sseg = 0xFFFFFFFFC0000000ULL;
    const std::uint64_t kseg3 = 0xFFFFFFFFE0000000ULL;
    const std::uint32_t loadInstruction = 0x8C220000U; // LW v0,0(at)
    const std::uint32_t storeInstruction = 0xAC220000U; // SW v0,0(at)

    // User mode cannot fetch, load, or store in SSEG or KSEG3.
    for (const std::uint64_t address : {sseg, kseg3}) {
        {
            N64Machine machine;
            auto& cpu = machine.cpu();
            cpu.reset(address);
            require(cpu.setCp0Register(m64plus::n64::Cp0::kStatus, 2ULL << 3U),
                    "set user mode for privileged segment fetch");
            require(machine.stepCpu(), "user privileged segment fetch faults");
            require(cpu.hasLastException() && cpu.lastExceptionCode() == 4U,
                    "user privileged segment fetch raises Address Load");
        }
        {
            N64Machine machine;
            Bus& bus = machine.bus();
            require(bus.write32(0U, loadInstruction), "write user privileged segment LW");
            auto& cpu = machine.cpu();
            cpu.reset(0U);
            cpu.setRegister(1U, address);
            require(cpu.setCp0Register(m64plus::n64::Cp0::kStatus, 2ULL << 3U),
                    "set user mode for privileged segment load");
            require(machine.stepCpu(), "user privileged segment load faults");
            require(cpu.hasLastException() && cpu.lastExceptionCode() == 4U,
                    "user privileged segment load raises Address Load");
        }
        {
            N64Machine machine;
            Bus& bus = machine.bus();
            require(bus.write32(0U, storeInstruction), "write user privileged segment SW");
            auto& cpu = machine.cpu();
            cpu.reset(0U);
            cpu.setRegister(1U, address);
            cpu.setRegister(2U, 0x12345678U);
            require(cpu.setCp0Register(m64plus::n64::Cp0::kStatus, 2ULL << 3U),
                    "set user mode for privileged segment store");
            require(machine.stepCpu(), "user privileged segment store faults");
            require(cpu.hasLastException() && cpu.lastExceptionCode() == 5U,
                    "user privileged segment store raises Address Store");
        }
    }

    // Supervisor mode may access SSEG, but not KSEG3.
    {
        N64Machine machine;
        auto& cpu = machine.cpu();
        cpu.reset(sseg);
        require(cpu.setCp0Register(m64plus::n64::Cp0::kStatus, 1ULL << 3U),
                "set supervisor mode for SSEG fetch");
        require(machine.stepCpu(), "supervisor SSEG fetch proceeds to memory");
        require(!cpu.hasLastException() || cpu.lastExceptionCode() != 4U,
                "supervisor SSEG fetch is not rejected by privilege checks");
    }
    {
        N64Machine machine;
        auto& cpu = machine.cpu();
        cpu.reset(kseg3);
        require(cpu.setCp0Register(m64plus::n64::Cp0::kStatus, 1ULL << 3U),
                "set supervisor mode for KSEG3 fetch");
        require(machine.stepCpu(), "supervisor KSEG3 fetch raises exception");
        require(cpu.hasLastException() && cpu.lastExceptionCode() == 4U,
                "supervisor KSEG3 fetch raises Address Load");
    }
}

void testCpuEretPrioritizesErrorEpcWhenErlAndExlAreSet() {
    N64Machine machine;
    Bus& bus = machine.bus();
    require(bus.write32(0U, 0x40886000U),
            "write MTC0 Status with ERL and EXL set");
    require(bus.write32(4U, 0x408AF000U),
            "write MTC0 ErrorEPC");
    require(bus.write32(8U, 0x408B7000U),
            "write MTC0 EPC");
    require(bus.write32(12U, 0x42000018U),
            "write ERET with ERL and EXL set");

    auto& cpu = machine.cpu();
    cpu.reset(0xFFFFFFFF80000000ULL);
    cpu.setRegister(8U, (2ULL << 3U) |
                         m64plus::n64::Cp0::kStatusErl |
                         m64plus::n64::Cp0::kStatusExl);
    cpu.setRegister(10U, 0x20U);
    cpu.setRegister(11U, 0x40U);

    require(machine.stepCpu(), "MTC0 sets both ERL and EXL");
    require(machine.stepCpu(), "MTC0 writes ErrorEPC");
    require(machine.stepCpu(), "MTC0 writes distinct EPC");
    require(machine.stepCpu(), "ERET executes with ERL and EXL both set");
    require(cpu.pc() == 0x20U,
            "ERET uses ErrorEPC when ERL and EXL are both set");
    const std::uint64_t status = cpu.cp0().read(m64plus::n64::Cp0::kStatus);
    require((status & m64plus::n64::Cp0::kStatusErl) == 0,
            "ERET clears ERL when both level bits are set");
    require((status & m64plus::n64::Cp0::kStatusExl) != 0,
            "ERET leaves EXL set when returning through ErrorEPC");
    require((status & (3ULL << 3U)) == (2ULL << 3U),
            "ERET preserves KSU while clearing only ERL");
}
void testCpuEretUsesErrorEpcWhenErlIsSet() {
    N64Machine machine;
    Bus& bus = machine.bus();
    require(bus.write32(0U, 0x40886000U),
            "write MTC0 Status with ERL set");
    require(bus.write32(4U, 0x408AF000U),
            "write MTC0 ErrorEPC");
    require(bus.write32(8U, 0x408B7000U),
            "write MTC0 EPC");
    require(bus.write32(12U, 0x42000018U),
            "write ERET for ERL return");
    require(bus.write32(0x20U, 0x40096000U),
            "write user-mode MFC0 at ErrorEPC target");

    auto& cpu = machine.cpu();
    cpu.reset(0xFFFFFFFF80000000ULL);
    cpu.setRegister(8U, (2ULL << 3U) | m64plus::n64::Cp0::kStatusErl);
    cpu.setRegister(10U, 0x20U);
    cpu.setRegister(11U, 0x40U);

    require(machine.stepCpu(), "MTC0 sets ERL and user KSU");
    require(machine.stepCpu(), "MTC0 writes ErrorEPC");
    require(machine.stepCpu(), "MTC0 writes distinct EPC");
    require(machine.stepCpu(), "ERET executes while ERL is set");
    require(cpu.pc() == 0x20U,
            "ERET selects ErrorEPC instead of EPC when ERL is set");
    const std::uint64_t status = cpu.cp0().read(m64plus::n64::Cp0::kStatus);
    require((status & m64plus::n64::Cp0::kStatusErl) == 0,
            "ERET clears ERL on ErrorEPC return");
    require((status & m64plus::n64::Cp0::kStatusExl) == 0,
            "ERET does not set EXL on ErrorEPC return");
    require((status & (3ULL << 3U)) == (2ULL << 3U),
            "ERET restores user privilege after clearing ERL");

    require(machine.runCpuInstructions(1U) == 0U,
            "user-mode MFC0 remains blocked after ErrorEPC return");
    require(cpu.hasLastException() && cpu.lastExceptionCode() == 11U,
            "MFC0 after ErrorEPC return raises Coprocessor Unusable");
    require(cpu.pc() == 0xFFFFFFFF80000180ULL,
            "post-ERL-return MFC0 enters general exception vector");
}
void testCpuEretRestoresUserPrivilegeFromStatus() {
    N64Machine machine;
    Bus& bus = machine.bus();
    // Enter user mode, raise SYSCALL, then return from the handler to user code.
    require(bus.write32(0U, 0x40886000U),
            "write MTC0 Status to enter user mode");
    require(bus.write32(4U, 0x0000000CU),
            "write SYSCALL in user mode");
    require(bus.write32(8U, 0x40096000U),
            "write MFC0 Status after ERET");
    require(bus.write32(0x180U, 0x408A7000U),
            "write MTC0 EPC in exception handler");
    require(bus.write32(0x184U, 0x42000018U),
            "write ERET in exception handler");

    auto& cpu = machine.cpu();
    cpu.reset(0U);
    cpu.setRegister(8U, 2ULL << 3U); // KSU=user.
    cpu.setRegister(10U, 8U); // Resume after the SYSCALL instruction.

    require(machine.stepCpu(), "kernel enters user mode through MTC0 Status");
    require(machine.stepCpu(), "user-mode SYSCALL raises an exception");
    require(cpu.hasLastException() && cpu.lastExceptionCode() == 8U,
            "user-mode SYSCALL records Syscall exception");
    require(cpu.pc() == 0xFFFFFFFF80000180ULL,
            "SYSCALL enters the general exception handler");
    require((cpu.cp0().read(m64plus::n64::Cp0::kStatus) &
             (3ULL << 3U)) == (2ULL << 3U),
            "exception entry preserves the previous user KSU field");

    require(machine.stepCpu(), "exception handler updates EPC");
    require(machine.stepCpu(), "exception handler executes ERET");
    require(cpu.pc() == 8U,
            "ERET returns to the requested user instruction in kuseg");
    require(!cpu.cp0().exceptionLevel(),
            "ERET clears EXL after exception return");
    require((cpu.cp0().read(m64plus::n64::Cp0::kStatus) &
             (3ULL << 3U)) == (2ULL << 3U),
            "ERET preserves user KSU privilege mode");

    require(machine.runCpuInstructions(1U) == 0U,
            "user-mode MFC0 remains blocked after ERET");
    require(cpu.hasLastException() && cpu.lastExceptionCode() == 11U,
            "MFC0 after ERET raises Coprocessor Unusable");
    require(cpu.pc() == 0xFFFFFFFF80000180ULL,
            "post-ERET user-mode MFC0 enters general exception vector");
}

void testCpuMtc0StatusChangesPrivilegeForNextInstruction() {
    N64Machine machine;
    Bus& bus = machine.bus();
    // MTC0 r8, Status; followed by MFC0 r9, Status.
    require(bus.write32(0U, 0x40886000U),
            "write MTC0 Status instruction");
    require(bus.write32(4U, 0x40096000U),
            "write MFC0 Status instruction after privilege change");
    auto& cpu = machine.cpu();
    cpu.reset(0U);
    cpu.setRegister(8U, 2ULL << 3U); // User mode.
    require(machine.stepCpu(), "kernel MTC0 changes Status to user mode");
    require(!cpu.hasLastException(),
            "kernel MTC0 Status completes without exception");
    require((cpu.cp0().read(m64plus::n64::Cp0::kStatus) &
             (3ULL << 3U)) == (2ULL << 3U),
            "MTC0 Status changes KSU to user mode");
    require(machine.runCpuInstructions(1U) == 0U,
            "following MFC0 is blocked after entering user mode");
    require(cpu.hasLastException() && cpu.lastExceptionCode() == 11U,
            "following MFC0 raises Coprocessor Unusable");
    require(cpu.pc() == 0xFFFFFFFF80000180ULL,
            "following MFC0 enters general exception vector");
}

void testCpuMtc0WorksInKernelMode() {
    // MTC0 r8, Wired: kernel code may update CP0 state.
    N64Machine machine;
    require(machine.bus().write32(0U, 0x40883000U),
            "write kernel MTC0 Wired instruction");
    auto& cpu = machine.cpu();
    cpu.reset(0xFFFFFFFF80000000ULL);
    cpu.setRegister(8U, 17U);
    require(cpu.cp0().read(m64plus::n64::Cp0::kWired) == 0U,
            "Wired starts at zero before kernel MTC0");
    require(machine.stepCpu(), "kernel MTC0 Wired executes");
    require(!cpu.hasLastException(),
            "kernel MTC0 does not raise an exception");
    require(cpu.cp0().read(m64plus::n64::Cp0::kWired) == 17U,
            "kernel MTC0 writes the value into Wired");
    require(cpu.cp0().read(m64plus::n64::Cp0::kRandom) == 30U,
            "Wired write resets Random and CP0 advances it after the instruction");
}

void testCpuMtc0RequiresKernelPrivilege() {
    // MTC0 r8, Wired: non-kernel code must not modify CP0 state.
    constexpr std::uint32_t mtc0Wired = 0x40883000U;
    const std::uint64_t modes[] = {2ULL << 3U, 1ULL << 3U};
    for (const std::uint64_t status : modes) {
        N64Machine machine;
        require(machine.bus().write32(0U, mtc0Wired),
                "write MTC0 Wired instruction");
        auto& cpu = machine.cpu();
        cpu.reset(0U);
        cpu.setRegister(8U, 17U);
        require(cpu.setCp0Register(m64plus::n64::Cp0::kStatus, status),
                "set non-kernel privilege for MTC0");
        require(cpu.cp0().read(m64plus::n64::Cp0::kWired) == 0U,
                "Wired starts at zero before non-kernel MTC0");
        require(machine.runCpuInstructions(1U) == 0U,
                "non-kernel MTC0 is not executed");
        require(cpu.hasLastException() && cpu.lastExceptionCode() == 11U,
                "non-kernel MTC0 raises Coprocessor Unusable");
        require(cpu.pc() == 0xFFFFFFFF80000180ULL,
                "non-kernel MTC0 enters general exception vector");
        require(cpu.cp0().read(m64plus::n64::Cp0::kWired) == 0U,
                "non-kernel MTC0 leaves Wired unchanged");
        require(((cpu.cp0().read(m64plus::n64::Cp0::kCause) >> 2U) & 0x1FULL) == 11U,
                "non-kernel MTC0 records exception code 11");
    }
}


void testCpuNestedTlbExceptionUpdatesFaultAddressRegisters() {
    m64plus::n64::Cp0 cp0;
    constexpr std::uint64_t initialAsid = 0x5AU;
    constexpr std::uint64_t firstPc = 0xFFFFFFFF80001000ULL;
    constexpr std::uint64_t nestedPc = 0xFFFFFFFF80002000ULL;
    constexpr std::uint64_t badAddress = 0xC000123456789ABCULL;
    require(cp0.write(m64plus::n64::Cp0::kEntryHi, initialAsid),
            "set initial EntryHi ASID before nested exception");
    require(cp0.write(m64plus::n64::Cp0::kXContext, 0xFFFFFFFFFFFFFFFFULL),
            "seed XContext PTEBase before nested exception");
    cp0.raiseException(m64plus::n64::ExceptionCode::Syscall, firstPc, true);
    const std::uint64_t firstEpc = cp0.read(m64plus::n64::Cp0::kEpc);
    const std::uint64_t firstCause = cp0.read(m64plus::n64::Cp0::kCause);
    const std::uint64_t expectedContext = (badAddress >> 9U) & 0x7FFFF0ULL;
    const std::uint64_t expectedXContext = 0xFFFFFFFE00000000ULL |
        ((badAddress >> 9U) & 0x7FFFFFF0ULL) |
        ((badAddress >> 31U) & 0x180000000ULL);

    cp0.raiseException(m64plus::n64::ExceptionCode::TlbLoad, nestedPc,
                       false, badAddress, true);
    require(cp0.read(m64plus::n64::Cp0::kContext) == expectedContext,
            "nested TLB exception updates Context despite EXL already being set");
    require(cp0.read(m64plus::n64::Cp0::kXContext) == expectedXContext,
            "nested TLB exception updates XContext despite EXL already being set");
    require(cp0.read(m64plus::n64::Cp0::kEntryHi) ==
                ((badAddress & 0xFFFFE000ULL) | initialAsid),
            "nested TLB exception updates EntryHi and preserves ASID");
    require(cp0.read(m64plus::n64::Cp0::kBadVAddr) == badAddress,
            "nested TLB exception updates BadVAddr");
    require(cp0.read(m64plus::n64::Cp0::kEpc) == firstEpc,
            "nested TLB exception preserves the first EPC");
    require((cp0.read(m64plus::n64::Cp0::kCause) & (1ULL << 31U)) ==
                (firstCause & (1ULL << 31U)),
            "nested TLB exception preserves the first Cause.BD state");
    require(((cp0.read(m64plus::n64::Cp0::kCause) >> 2U) & 0x1FULL) ==
                static_cast<std::uint64_t>(m64plus::n64::ExceptionCode::TlbLoad),
            "nested TLB exception updates Cause.ExcCode while preserving first EPC and BD");
}

void testCpuXContextWritesOnlyPteBaseField() {
    m64plus::n64::Cp0 cp0;
    require(cp0.write(m64plus::n64::Cp0::kXContext, 0xFFFFFFFFFFFFFFFFULL),
            "write XContext PTEBase");
    require(cp0.read(m64plus::n64::Cp0::kXContext) == 0xFFFFFFFE00000000ULL,
            "XContext writes only the PTEBase field and preserves reserved/BadVPN2 fields");

    constexpr std::uint64_t badAddress = 0xC000123456789ABCULL;
    const std::uint64_t expectedFaultFields =
        ((badAddress >> 9U) & 0x7FFFFFF0ULL) |
        ((badAddress >> 31U) & 0x180000000ULL);
    cp0.raiseException(m64plus::n64::ExceptionCode::TlbLoad, 0xFFFFFFFF80001000ULL,
                       false, badAddress, true);
    require(cp0.read(m64plus::n64::Cp0::kXContext) ==
                (0xFFFFFFFE00000000ULL | expectedFaultFields),
            "TLB exception updates XContext R and BadVPN2 while preserving PTEBase");
}

void testCpuCop0TlbInstructionsWriteProbeAndReadEntry() {
    N64Machine machine;
    Bus& bus = machine.bus();
    const std::uint32_t program[] = {
        0x42000002U, // TLBWI
        0x42000008U, // TLBP (ASID mismatch)
        0x42000008U, // TLBP (ASID match)
        0x42000001U  // TLBR
    };
    for (std::uint32_t i = 0; i < 4U; ++i)
        require(bus.write32(i * 4U, program[i]), "write COP0 TLB instruction program");

    auto& cpu = machine.cpu();
    require(cpu.setCp0Register(m64plus::n64::Cp0::kIndex, 7U),
            "set Index before TLBWI");
    require(cpu.setCp0Register(m64plus::n64::Cp0::kEntryHi, 0x00400012U),
            "set EntryHi before TLBWI");
    require(cpu.setCp0Register(m64plus::n64::Cp0::kEntryLo0,
                               (0x123ULL << 6U) | 6ULL),
            "set even EntryLo before TLBWI");
    require(cpu.setCp0Register(m64plus::n64::Cp0::kEntryLo1,
                               (0x124ULL << 6U) | 6ULL),
            "set odd EntryLo before TLBWI");
    cpu.reset(0xFFFFFFFF80000000ULL);
    // Reset the CPU first, then restore the CP0 values used by the instruction.
    require(cpu.setCp0Register(m64plus::n64::Cp0::kIndex, 7U),
            "restore Index after CPU reset");
    require(cpu.setCp0Register(m64plus::n64::Cp0::kEntryHi, 0x00400012U),
            "restore EntryHi after CPU reset");
    require(cpu.setCp0Register(m64plus::n64::Cp0::kEntryLo0,
                               (0x123ULL << 6U) | 6ULL),
            "restore even EntryLo after CPU reset");
    require(cpu.setCp0Register(m64plus::n64::Cp0::kEntryLo1,
                               (0x124ULL << 6U) | 6ULL),
            "restore odd EntryLo after CPU reset");
    require(machine.stepCpu(), "CPU executes TLBWI");
    require(cpu.setCp0Register(m64plus::n64::Cp0::kEntryHi, 0x00400013U),
            "change ASID before TLBP mismatch");
    require(machine.stepCpu(), "CPU executes TLBP for mismatched ASID");
    require((cpu.cp0().read(m64plus::n64::Cp0::kIndex) & 0x80000000ULL) != 0U,
            "TLBP sets Index probe-failure bit for non-global ASID mismatch");

    require(cpu.setCp0Register(m64plus::n64::Cp0::kEntryHi, 0x00400012U),
            "restore matching ASID before TLBP");
    require(machine.stepCpu(), "CPU executes TLBP for matching ASID");
    require(cpu.cp0().read(m64plus::n64::Cp0::kIndex) == 7U,
            "TLBP returns the matching TLB entry index");

    require(cpu.setCp0Register(m64plus::n64::Cp0::kEntryHi, 0U),
            "clear EntryHi before TLBR");
    require(cpu.setCp0Register(m64plus::n64::Cp0::kEntryLo0, 0U),
            "clear even EntryLo before TLBR");
    require(cpu.setCp0Register(m64plus::n64::Cp0::kEntryLo1, 0U),
            "clear odd EntryLo before TLBR");
    require(machine.stepCpu(), "CPU executes TLBR");
    require(cpu.cp0().read(m64plus::n64::Cp0::kEntryHi) == 0x00400012U,
            "TLBR restores EntryHi and ASID from selected entry");
    require(cpu.cp0().read(m64plus::n64::Cp0::kEntryLo0) ==
                ((0x123ULL << 6U) | 6ULL) &&
            cpu.cp0().read(m64plus::n64::Cp0::kEntryLo1) ==
                ((0x124ULL << 6U) | 6ULL),
            "TLBR restores both EntryLo PFNs and permission bits");
}


void testCpuCop0TlbWriteRandomHonorsWiredBoundary() {
    N64Machine machine;
    Bus& bus = machine.bus();
    const std::uint32_t program[] = {
        0x42000006U, // TLBWR
        0x42000008U  // TLBP
    };
    for (std::uint32_t i = 0; i < 2U; ++i)
        require(bus.write32(i * 4U, program[i]), "write TLBWR program");

    auto& cpu = machine.cpu();
    cpu.reset(0xFFFFFFFF80000000ULL);
    require(cpu.setCp0Register(m64plus::n64::Cp0::kWired, 30U),
            "set Wired boundary to reserve entries 0 through 30");
    require(cpu.cp0().read(m64plus::n64::Cp0::kRandom) == 31U,
            "writing Wired resets Random to the first non-wired entry");
    require(cpu.setCp0Register(m64plus::n64::Cp0::kEntryHi, 0x0123402AU),
            "configure EntryHi for TLBWR");
    require(cpu.setCp0Register(m64plus::n64::Cp0::kEntryLo0,
                               (0x155ULL << 6U) | 6ULL),
            "configure even EntryLo for TLBWR");
    require(cpu.setCp0Register(m64plus::n64::Cp0::kEntryLo1,
                               (0x156ULL << 6U) | 6ULL),
            "configure odd EntryLo for TLBWR");

    require(machine.stepCpu(), "CPU executes TLBWR");
    require(cpu.cp0().read(m64plus::n64::Cp0::kRandom) == 30U,
            "Random advances but never enters the Wired range");

    require(cpu.setCp0Register(m64plus::n64::Cp0::kEntryHi, 0x0123402AU),
            "restore matching EntryHi before probing TLBWR result");
    require(machine.stepCpu(), "CPU probes the entry installed by TLBWR");
    require(cpu.cp0().read(m64plus::n64::Cp0::kIndex) == 31U,
            "TLBWR installs its entry at Random index 31 above Wired");
}

void testMachineDoesNotRetryUnsupportedRspInstruction() {
    N64Machine machine;
    Bus& bus = machine.bus();
    require(bus.write32(Bus::kSpImemBase, 0xFFFFFFFFU),
            "write unsupported RSP instruction");
    require(bus.write32(Bus::kSpPc, 0U), "set faulting RSP PC");
    require(bus.write32(Bus::kSpStatus, 1U), "release RSP HALT");
    require(!machine.stepRsp(), "unsupported RSP instruction fails");
    require(machine.rsp().faulted(), "machine preserves RSP fault state");
    require(!machine.stepRsp(), "machine does not restart faulting instruction");
    require(machine.rsp().instructionsExecuted() == 0U,
            "faulting instruction is not repeatedly executed");
    require(bus.write32(Bus::kSpPc, 4U), "move SP_PC to a new task location");
    require(bus.write32(Bus::kSpImemBase + 4U, 0x0000000DU),
            "write BREAK at new task location");
    require(machine.stepRsp(), "new SP_PC permits a new RSP task");
    require(bus.spHalted() && bus.spBroke(), "new task BREAK halts normally");
}

void testSharedCpuRspBusAndSpBreak() {
    N64Machine machine;
    Bus& bus = machine.bus();

    // The RSP starts halted until software clears SP_STATUS.HALT.
    require(bus.spHalted(), "SP starts halted after reset");
    require(!machine.stepRsp(), "RSP does not run while SP is halted");

    const std::uint32_t rspProgram[] = {
        0x24011234U, // ADDIU r1,zero,0x1234
        0xAC010000U, // SW r1,0(zero) -> SP DMEM
        0x0000000DU  // BREAK
    };
    for (std::uint32_t i = 0; i < 3U; ++i) {
        require(bus.write32(Bus::kSpImemBase + 0x20U + i * 4U, rspProgram[i]),
                "load RSP program into IMEM");
    }

    // Enable the SP interrupt mask and request an interrupt on RSP BREAK.
    require(bus.write32(Bus::kSpPc, 0x20U), "set RSP start PC through SP_PC");
    require(bus.spProgramCounter() == 0x20U, "SP_PC register stores aligned PC");
    require(bus.write32(Bus::kMiInterruptMask, 1U << 1U),
            "enable MI SP interrupt");
    require(bus.write32(Bus::kSpStatus, Bus::kSpSetInterruptOnBreak | 1U),
            "configure SP interrupt-on-break and clear HALT");
    require(!bus.spHalted(), "SP HALT clears through MMIO command");

    require(machine.runRsp(8U) == 3U, "RSP executes task within instruction budget");
    require(bus.spProgramCounter() == 0x2CU, "SP_PC tracks RSP instruction progress");
    require(machine.rsp().reg(1) == 0x1234U, "RSP scalar result");
    require(bus.spHalted() && bus.spBroke(), "BREAK sets SP HALT and BROKE");
    require(bus.miInterruptPending(), "BREAK asserts masked MI SP interrupt");

    // The main CPU observes data written by the RSP through the same physical bus.
    require(bus.write32(0x00000000U, 0x3C080400U), "write CPU LUI instruction");
    require(bus.write32(0x00000004U, 0x8D090000U), "write CPU LW instruction");
    require(machine.stepCpu(), "main CPU executes LUI");
    require(machine.stepCpu(), "main CPU reads SP DMEM");
    require(machine.cpu().reg(9) == 0x1234U,
            "main CPU sees the RSP result in shared SP DMEM");

    // The main CPU programs SP_PC and releases HALT through MMIO.
    const std::uint32_t startRsp[] = {
        0x3C080408U, // LUI t0,0x0408 -> SP_PC
        0x24090040U, // ADDIU t1,zero,0x40
        0xAD090000U, // SW t1,0(t0) -> SP_PC
        0x3C0A0404U, // LUI t2,0x0404 -> SP registers
        0x240B0005U, // ADDIU t3,zero,clear HALT + clear BROKE
        0xAD4B0010U  // SW t3,0x10(t2) -> SP_STATUS
    };
    for (std::uint32_t i = 0; i < 6U; ++i)
        require(bus.write32(8U + i * 4U, startRsp[i]), "write CPU RSP-start program");
    for (unsigned i = 0; i < 6U; ++i)
        require(machine.stepCpu(), "CPU programs SP_PC and SP_STATUS");
    require(bus.spProgramCounter() == 0x40U && !bus.spHalted(),
            "CPU starts RSP task through MMIO");

    const std::uint32_t vectorTask[] = {
        0x24017530U, // ADDIU r1,zero,30000
        0x48810800U, // MTC2 r1,v1[e0]
        0x24012710U, // ADDIU r1,zero,10000
        0x48811000U, // MTC2 r1,v2[e0]
        0x4A0208D0U, // VADD v3,v1,v2
        0x0000000DU  // BREAK
    };
    for (std::uint32_t i = 0; i < 6U; ++i)
        require(bus.write32(Bus::kSpImemBase + 0x40U + i * 4U, vectorTask[i]),
                "load vector RSP task");
    require(machine.runRsp(8U) == 6U, "RSP runs COP2 vector task");
    require(machine.rsp().vectorLane(3, 0) == 32767U,
            "integrated RSP executes saturated VADD");
    require(bus.spHalted() && bus.spBroke(), "second task also signals BREAK");
}

void testCpuProgramsPiDmaAndExecutesCartridgePayload() {
    N64Machine machine;
    Bus& bus = machine.bus();

    std::uint8_t rom[0x48U]{};
    rom[0] = 0x80U; rom[1] = 0x37U; rom[2] = 0x12U; rom[3] = 0x40U;
    const std::uint32_t payload[] = {0x24081234U, 0x25090001U};
    for (unsigned i = 0; i < 2U; ++i) {
        const std::uint32_t instruction = payload[i];
        const std::size_t offset = 0x40U + i * 4U;
        rom[offset] = static_cast<std::uint8_t>(instruction >> 24U);
        rom[offset + 1U] = static_cast<std::uint8_t>(instruction >> 16U);
        rom[offset + 2U] = static_cast<std::uint8_t>(instruction >> 8U);
        rom[offset + 3U] = static_cast<std::uint8_t>(instruction);
    }
    require(bus.loadCartridge(rom, sizeof(rom)),
            "load cartridge for CPU-driven PI DMA integration");

    // The emulated CPU programs PI through its KSEG1 MMIO address.
    const std::uint32_t dmaProgram[] = {
        0x3C08A460U, // LUI t0,0xA460 -> PI registers
        0x24090200U, // ADDIU t1,zero,0x200 -> RDRAM destination
        0xAD090000U, // SW t1,PI_DRAM_ADDR(t0)
        0x3C091000U, // LUI t1,0x1000
        0x35290040U, // ORI t1,t1,0x40 -> cartridge source
        0xAD090004U, // SW t1,PI_CART_ADDR(t0)
        0x24090007U, // ADDIU t1,zero,7 -> eight bytes
        0xAD09000CU  // SW t1,PI_WR_LEN(t0), starts cart-to-RDRAM DMA
    };
    for (std::uint32_t i = 0; i < 8U; ++i)
        require(bus.write32(i * 4U, dmaProgram[i]),
                "write CPU program that configures PI DMA");
    for (unsigned i = 0; i < 8U; ++i)
        require(machine.stepCpu(), "CPU executes PI DMA setup instruction");

    std::uint32_t copied = 0;
    require(bus.read32(0x200U, copied) && copied == payload[0],
            "CPU-triggered PI DMA copies first instruction into RDRAM");
    require(bus.read32(0x204U, copied) && copied == payload[1],
            "CPU-triggered PI DMA copies second instruction into RDRAM");
    require(bus.read32(Bus::kPiStatus, copied) &&
            (copied & Bus::kPiStatusInterrupt) != 0U,
            "CPU-triggered PI DMA completes and raises PI interrupt");

    machine.cpu().reset(0xFFFFFFFF80000200ULL);
    require(machine.stepCpu(), "CPU fetches first cartridge payload instruction from RDRAM");
    require(machine.stepCpu(), "CPU fetches second cartridge payload instruction from RDRAM");
    require(machine.cpu().reg(8) == 0x1234U,
            "CPU executes first instruction copied from cartridge");
    require(machine.cpu().reg(9) == 0x1235U,
            "CPU executes second instruction copied from cartridge");
}


void testHleBootRejectsInvalidEntryWithoutResettingMachine() {
    N64Machine machine;
    std::vector<std::uint8_t> rom(0x1008U, 0U);
    rom[0] = 0x80U; rom[1] = 0x37U; rom[2] = 0x12U; rom[3] = 0x40U;
    // The cartridge header entry point is deliberately unaligned.
    rom[8] = 0x80U; rom[9] = 0x00U; rom[10] = 0x04U; rom[11] = 0x02U;
    require(machine.bus().loadCartridge(rom.data(), rom.size()),
            "load cartridge with invalid HLE entry point");
    require(machine.bus().rdram().write32(0x200U, 0xA1B2C3D4U),
            "seed RDRAM before rejected HLE boot");
    machine.cpu().reset(0xFFFFFFFF80001234ULL);

    require(!machine.bootCartridgeHle(),
            "HLE rejects an unaligned cartridge entry point");

    std::uint32_t value = 0U;
    require(machine.bus().rdram().read32(0x200U, value) && value == 0xA1B2C3D4U,
            "rejected HLE boot leaves existing RDRAM untouched");
    require(machine.cpu().pc() == 0xFFFFFFFF80001234ULL,
            "rejected HLE boot leaves CPU execution state untouched");
    require(machine.bus().read32(Bus::kCartridgeBase + 8U, value) &&
                value == 0x80000402U,
            "rejected HLE boot preserves the inserted cartridge header");
}


void testHleBootRejectsPayloadBeyondRdramWithoutResettingMachine() {
    N64Machine machine;
    // The HLE copy starts at 0x1FF000; a 0x2000-byte payload crosses 8 MiB.
    std::vector<std::uint8_t> rom(0x1000U + 0x2000U, 0U);
    rom[0] = 0x80U; rom[1] = 0x37U; rom[2] = 0x12U; rom[3] = 0x40U;
    rom[8] = 0x81U; rom[9] = 0xFFU; rom[10] = 0xF0U; rom[11] = 0x00U;
    require(machine.bus().loadCartridge(rom.data(), rom.size()),
            "load cartridge whose HLE payload exceeds RDRAM");
    require(machine.bus().rdram().write32(0x200U, 0x5A6B7C8DU),
            "seed RDRAM before oversized HLE boot");
    machine.cpu().reset(0xFFFFFFFF80005678ULL);

    require(!machine.bootCartridgeHle(),
            "HLE rejects payload extending beyond physical RDRAM");

    std::uint32_t value = 0U;
    require(machine.bus().rdram().read32(0x200U, value) && value == 0x5A6B7C8DU,
            "oversized HLE payload leaves existing RDRAM untouched");
    require(machine.cpu().pc() == 0xFFFFFFFF80005678ULL,
            "oversized HLE payload leaves CPU execution state untouched");
    require(machine.bus().cartridgeSize() == rom.size(),
            "oversized HLE rejection preserves the inserted cartridge");
}


void testHleBootAcceptsPayloadEndingExactlyAtRdramBoundary() {
    N64Machine machine;
    constexpr std::size_t kPayloadSize = 0x100000U;
    constexpr std::uint32_t kPhysicalEntry = 0x00700000U;
    std::vector<std::uint8_t> rom(0x1000U + kPayloadSize, 0U);
    rom[0] = 0x80U; rom[1] = 0x37U; rom[2] = 0x12U; rom[3] = 0x40U;
    // KSEG0 virtual entry maps to physical 0x700000; 1 MiB ends at 8 MiB.
    rom[8] = 0x80U; rom[9] = 0x70U; rom[10] = 0x00U; rom[11] = 0x00U;
    rom[0x1000U] = 0x12U;
    rom[0x1001U] = 0x34U;
    rom[0x1002U] = 0x56U;
    rom[0x1003U] = 0x78U;
    rom.back() = 0xA5U;
    require(machine.bus().loadCartridge(rom.data(), rom.size()),
            "load cartridge whose HLE payload ends exactly at RDRAM boundary");
    require(machine.bootCartridgeHle(),
            "HLE accepts a payload whose final byte is exactly at the RDRAM limit");

    std::uint32_t value = 0U;
    require(machine.bus().rdram().read32(kPhysicalEntry, value) &&
                value == 0x12345678U,
            "exact-boundary HLE boot copies the first payload word");
    std::uint8_t lastByte = 0U;
    require(machine.bus().rdram().read8(0x007FFFFFU, lastByte) && lastByte == 0xA5U,
            "exact-boundary HLE boot copies the final byte at the 8 MiB limit");
    require(machine.cpu().pc() == 0xFFFFFFFF80700000ULL,
            "HLE preserves the KSEG0 virtual entry point while copying to physical RDRAM");
}

void testHleBootMapsKseg1EntryToPhysicalRdram() {
    N64Machine machine;
    std::vector<std::uint8_t> rom(0x1010U, 0U);
    rom[0] = 0x80U; rom[1] = 0x37U; rom[2] = 0x12U; rom[3] = 0x40U;
    // KSEG1 entry 0xA0000400 aliases physical RDRAM address 0x400.
    rom[8] = 0xA0U; rom[9] = 0x00U; rom[10] = 0x04U; rom[11] = 0x00U;
    // addiu t0, zero, 1
    rom[0x1000U] = 0x24U; rom[0x1001U] = 0x08U;
    rom[0x1002U] = 0x00U; rom[0x1003U] = 0x01U;
    // beq t0, t0, +1: branch target follows the delay slot.
    rom[0x1004U] = 0x11U; rom[0x1005U] = 0x08U;
    rom[0x1006U] = 0x00U; rom[0x1007U] = 0x01U;
    // addiu t1, zero, 2: executes in the branch delay slot.
    rom[0x1008U] = 0x24U; rom[0x1009U] = 0x09U;
    rom[0x100AU] = 0x00U; rom[0x100BU] = 0x02U;
    // addiu t2, zero, 3: branch target.
    rom[0x100CU] = 0x24U; rom[0x100DU] = 0x0AU;
    rom[0x100EU] = 0x00U; rom[0x100FU] = 0x03U;
    require(machine.bus().loadCartridge(rom.data(), rom.size()),
            "load cartridge with KSEG1 entry point");
    require(machine.bootCartridgeHle(), "HLE accepts a valid KSEG1 entry point");

    std::uint32_t value = 0U;
    require(machine.bus().rdram().read32(0x400U, value) && value == 0x24080001U,
            "KSEG1 entry payload is copied to its physical RDRAM alias");
    require(machine.cpu().pc() == 0xFFFFFFFFA0000400ULL,
            "HLE retains the canonical KSEG1 virtual entry point");
    require(machine.stepCpu() && !machine.cpu().hasLastException(),
            "HLE-booted CPU fetches and executes its first instruction through KSEG1");
    require(machine.cpu().reg(8U) == 1U,
            "KSEG1 boot entry executes its first cartridge instruction");
    require(machine.stepCpu() && !machine.cpu().hasLastException(),
            "KSEG1 CPU executes a conditional branch without exception");
    require(machine.stepCpu() && !machine.cpu().hasLastException(),
            "KSEG1 CPU executes the branch delay-slot instruction");
    require(machine.cpu().reg(9U) == 2U,
            "KSEG1 branch delay slot executes before taking the branch");
    require(machine.stepCpu() && !machine.cpu().hasLastException(),
            "KSEG1 CPU fetches the branch target after the delay slot");
    require(machine.cpu().reg(10U) == 3U,
            "KSEG1 branch target executes after the delay slot");

    // A not-taken ordinary branch must still execute its delay slot and
    // continue with the sequential instruction rather than the target.
    N64Machine fallthroughMachine;
    std::vector<std::uint8_t> fallthroughRom(0x1014U, 0U);
    fallthroughRom[0] = 0x80U; fallthroughRom[1] = 0x37U;
    fallthroughRom[2] = 0x12U; fallthroughRom[3] = 0x40U;
    fallthroughRom[8] = 0xA0U; fallthroughRom[9] = 0x00U;
    fallthroughRom[10] = 0x04U; fallthroughRom[11] = 0x00U;
    const std::uint32_t fallthroughProgram[] = {
        0x24080001U, // addiu t0, zero, 1
        0x11090002U, // beq t0, t1, +2: not taken because t1 is zero
        0x240A0002U, // delay slot
        0x240B0003U, // sequential instruction (not the branch target)
        0x240C0004U  // branch target
    };
    for (std::size_t i = 0; i < 5U; ++i) {
        const std::uint32_t instruction = fallthroughProgram[i];
        const std::size_t offset = 0x1000U + i * 4U;
        fallthroughRom[offset] = static_cast<std::uint8_t>(instruction >> 24U);
        fallthroughRom[offset + 1U] = static_cast<std::uint8_t>(instruction >> 16U);
        fallthroughRom[offset + 2U] = static_cast<std::uint8_t>(instruction >> 8U);
        fallthroughRom[offset + 3U] = static_cast<std::uint8_t>(instruction);
    }
    require(fallthroughMachine.bus().loadCartridge(
                fallthroughRom.data(), fallthroughRom.size()),
            "load KSEG1 cartridge for branch fallthrough test");
    require(fallthroughMachine.bootCartridgeHle(),
            "HLE boots KSEG1 cartridge for branch fallthrough test");
    require(fallthroughMachine.stepCpu() && fallthroughMachine.stepCpu(),
            "KSEG1 program executes setup and BEQ branch");
    require(fallthroughMachine.stepCpu() &&
            fallthroughMachine.cpu().reg(10U) == 2U,
            "KSEG1 BEQ delay slot executes");
    require(fallthroughMachine.stepCpu() &&
            fallthroughMachine.cpu().reg(11U) == 3U &&
            fallthroughMachine.cpu().reg(12U) == 0U,
            "KSEG1 BEQ falls through after its delay slot");
    require(fallthroughMachine.stepCpu() &&
            !fallthroughMachine.cpu().hasLastException() &&
            fallthroughMachine.cpu().reg(12U) == 4U,
            "KSEG1 fallthrough continues into the following instruction");
}

void testCartridgeHleBootInitializesRdramDmemAndCpuState() {
    N64Machine machine;
    std::vector<std::uint8_t> rom(0x1008U, 0U);
    rom[0] = 0x80U; rom[1] = 0x37U; rom[2] = 0x12U; rom[3] = 0x40U;
    rom[8] = 0x80U; rom[9] = 0x00U; rom[10] = 0x04U; rom[11] = 0x00U;
    const std::uint32_t program[] = {0x24081234U, 0x25090001U};
    for (unsigned i = 0; i < 2U; ++i) {
        const std::uint32_t instruction = program[i];
        const std::size_t offset = 0x1000U + i * 4U;
        rom[offset] = static_cast<std::uint8_t>(instruction >> 24U);
        rom[offset + 1U] = static_cast<std::uint8_t>(instruction >> 16U);
        rom[offset + 2U] = static_cast<std::uint8_t>(instruction >> 8U);
        rom[offset + 3U] = static_cast<std::uint8_t>(instruction);
    }
    require(machine.bus().loadCartridge(rom.data(), rom.size()),
            "load synthetic cartridge for HLE boot");
    require(machine.bootCartridgeHle(), "HLE boot accepts valid cartridge entry point");

    std::uint32_t value = 0;
    require(machine.bus().read32(0x400U, value) && value == program[0],
            "HLE copies cartridge payload to header entry point in RDRAM");
    require(machine.bus().read32(0x404U, value) && value == program[1],
            "HLE copies following payload instruction to RDRAM");
    require(machine.bus().read32(Bus::kSpDmemBase, value) && value == 0x80371240U,
            "HLE copies cartridge header into SP DMEM");
    require(machine.bus().read32(0x300U, value) && value == 1U,
            "HLE publishes NTSC TV boot parameter");
    require(machine.bus().read32(0x308U, value) && value == 0xB0000000U,
            "HLE publishes cartridge ROM base");
    require(machine.bus().read32(0x318U, value) && value == 0x00800000U,
            "HLE publishes 8 MiB RDRAM size");
    require(machine.cpu().pc() == 0xFFFFFFFF80000400ULL,
            "HLE sets CPU PC to ROM header entry point");
    require(machine.cpu().reg(20U) == 1U && machine.cpu().reg(22U) == 0x3FU,
            "HLE sets NTSC and CIC-6102 boot arguments");
    require(machine.cpu().reg(29U) == 0xFFFFFFFFA4001FF0ULL,
            "HLE initializes boot stack in SP DMEM");
    require(machine.cpu().cp0().read(m64plus::n64::Cp0::kStatus) == 0x34000000ULL,
            "HLE initializes CPU CP0 status");
    require(machine.stepCpu() && machine.stepCpu(),
            "CPU executes instructions loaded by HLE boot");
    require(machine.cpu().reg(8U) == 0x1234U && machine.cpu().reg(9U) == 0x1235U,
            "HLE-booted CPU executes cartridge entry code");
}


void testHleBootDetectsCicAndPalRegionFromRom() {
    N64Machine machine;
    std::vector<std::uint8_t> rom(0x1008U, 0U);
    rom[0] = 0x80U; rom[1] = 0x37U; rom[2] = 0x12U; rom[3] = 0x40U;
    rom[8] = 0x80U; rom[9] = 0x00U; rom[10] = 0x04U; rom[11] = 0x00U;
    rom[0x3EU] = static_cast<std::uint8_t>('P'); // PAL region
    // Build an IPL3 whose 1008 little-endian 32-bit words sum to CIC-X103.
    std::uint64_t remaining = 0x000000D6497E414BULL;
    for (std::size_t i = 0; i < 0xFC0U / 4U && remaining != 0U; ++i) {
        const std::uint32_t word = remaining > 0xFFFFFFFFULL
            ? 0xFFFFFFFFU : static_cast<std::uint32_t>(remaining);
        remaining -= word;
        const std::size_t offset = 0x40U + i * 4U;
        rom[offset] = static_cast<std::uint8_t>(word);
        rom[offset + 1U] = static_cast<std::uint8_t>(word >> 8U);
        rom[offset + 2U] = static_cast<std::uint8_t>(word >> 16U);
        rom[offset + 3U] = static_cast<std::uint8_t>(word >> 24U);
    }
    require(remaining == 0U, "synthetic IPL3 checksum fits the bootcode area");
    const std::uint32_t program[] = {0x24081234U, 0x25090001U};
    for (unsigned i = 0; i < 2U; ++i) {
        const std::uint32_t instruction = program[i];
        const std::size_t offset = 0x1000U + i * 4U;
        rom[offset] = static_cast<std::uint8_t>(instruction >> 24U);
        rom[offset + 1U] = static_cast<std::uint8_t>(instruction >> 16U);
        rom[offset + 2U] = static_cast<std::uint8_t>(instruction >> 8U);
        rom[offset + 3U] = static_cast<std::uint8_t>(instruction);
    }
    require(machine.bus().loadCartridge(rom.data(), rom.size()),
            "load synthetic CIC-X103 PAL cartridge");
    require(machine.bootCartridgeHle(), "HLE boots with detected CIC-X103 and PAL");
    std::uint32_t value = 0;
    require(machine.cpu().reg(20U) == 0U,
            "HLE sets s4 to PAL from cartridge country code");
    require(machine.cpu().reg(22U) == 0x78U,
            "HLE sets s6 to CIC-X103 seed");
    require(machine.bus().read32(0x300U, value) && value == 0U,
            "HLE publishes PAL TV type in low-memory boot parameters");
    require(machine.bus().read32(0x310U, value) && value == 6103U,
            "HLE publishes CIC-6103 identifier for X103");
}


void testCp0RandomCyclesWithinWiredBoundary() {
    m64plus::n64::Cp0 cp0;
    require(cp0.read(m64plus::n64::Cp0::kRandom) == 31U,
            "CP0 reset initializes Random to entry 31");

    require(cp0.write(m64plus::n64::Cp0::kWired, 29U),
            "set Wired to reserve entries 0 through 29");
    require(cp0.read(m64plus::n64::Cp0::kRandom) == 31U,
            "Wired write restarts Random at entry 31");
    cp0.tick();
    require(cp0.read(m64plus::n64::Cp0::kRandom) == 30U,
            "Random decrements from 31 to 30");
    cp0.tick();
    require(cp0.read(m64plus::n64::Cp0::kRandom) == 29U,
            "Random reaches the Wired boundary before wrapping");
    cp0.tick();
    require(cp0.read(m64plus::n64::Cp0::kRandom) == 31U,
            "Random wraps to 31 on the tick after reaching Wired");

    require(cp0.write(m64plus::n64::Cp0::kWired, 0U),
            "set Wired to zero");
    for (unsigned i = 0; i < 32U; ++i) cp0.tick();
    require(cp0.read(m64plus::n64::Cp0::kRandom) == 31U,
            "Random cycles through entries 31 to 1 when Wired is zero");

    require(cp0.write(m64plus::n64::Cp0::kWired, 31U),
            "set Wired to the final TLB entry");
    cp0.tick();
    require(cp0.read(m64plus::n64::Cp0::kRandom) == 31U,
            "Random remains at 31 when all lower entries are wired");
}


void testCpuExceptionAdvancesCp0TimersWithoutRetiringInstruction() {
    N64Machine machine;
    require(machine.bus().write32(0U, 0x7000003FU),
            "write reserved instruction for CP0 timer exception test");
    require(machine.cpu().setCp0Register(m64plus::n64::Cp0::kCount, 40U),
            "initialize Count before exception");
    require(machine.cpu().setCp0Register(m64plus::n64::Cp0::kCompare, 100U),
            "set Compare away from exception test Count");

    require(machine.stepCpu(), "CPU handles reserved instruction exception");
    require(machine.cpu().instructionsExecuted() == 0U,
            "exception does not retire an instruction");
    require(machine.cpu().cp0().read(m64plus::n64::Cp0::kCount) == 41U,
            "exception advances CP0 Count exactly once");
    require(machine.cpu().cp0().read(m64plus::n64::Cp0::kRandom) == 30U,
            "legacy exception tick advances CP0 Random exactly once");
}

void testCp0CountAndRandomCanAdvanceIndependently() {
    m64plus::n64::Cp0 cp0;
    require(cp0.write(m64plus::n64::Cp0::kCount, 10U), "initialize Count");
    require(cp0.write(m64plus::n64::Cp0::kCompare, 11U), "set Compare");
    cp0.tickRandom();
    require(cp0.read(m64plus::n64::Cp0::kCount) == 10U,
            "Random-only tick leaves Count unchanged");
    require(cp0.read(m64plus::n64::Cp0::kRandom) == 30U,
            "Random-only tick advances Random");
    cp0.tickCount();
    require(cp0.read(m64plus::n64::Cp0::kCount) == 11U,
            "Count-only tick advances Count");
    require((cp0.read(m64plus::n64::Cp0::kCause) & (1ULL << 15U)) != 0,
            "Count-only tick raises timer pending at Compare");
    require(cp0.read(m64plus::n64::Cp0::kRandom) == 30U,
            "Count-only tick leaves Random unchanged");
}

void testCp0ErrorLevelMasksInterrupts() {
    m64plus::n64::Cp0 cp0;
    cp0.setExternalInterrupt(true);
    constexpr std::uint64_t interruptEnable = 1ULL;
    constexpr std::uint64_t errorLevel = 1ULL << 2U;
    constexpr std::uint64_t externalInterruptMask = 1ULL << 10U;
    require(cp0.write(m64plus::n64::Cp0::kStatus,
                      interruptEnable | errorLevel | externalInterruptMask),
            "enable external interrupt while ERL is set");
    require(!cp0.interruptRequested(),
            "ERL masks interrupts even when IE and the interrupt mask are set");
    require(cp0.write(m64plus::n64::Cp0::kStatus,
                      interruptEnable | externalInterruptMask),
            "clear ERL while retaining interrupt enables");
    require(cp0.interruptRequested(),
            "pending external interrupt is accepted after ERL is cleared");
}

void testCp0CountCompareTimerInterrupt() {
    using m64plus::n64::Cp0;
    Cp0 cp0;

    constexpr std::uint64_t timerMask = 1ULL << 15U;
    require(cp0.write(Cp0::kStatus, Cp0::kStatusIe | timerMask),
            "enable global and timer interrupts");
    require(cp0.write(Cp0::kCount, 0xFFFFFFFEULL),
            "initialize Count before timer match");
    require(cp0.write(Cp0::kCompare, 0xFFFFFFFFULL),
            "set Compare to next Count value");

    cp0.tick();
    require(cp0.read(Cp0::kCount) == 0xFFFFFFFFULL,
            "Count increments to Compare");
    require((cp0.read(Cp0::kCause) & timerMask) != 0U,
            "Count reaching Compare asserts timer pending bit");
    require(cp0.interruptRequested(),
            "enabled timer pending bit requests an interrupt");

    require(cp0.write(Cp0::kCompare, 0U),
            "writing Compare clears the timer pending bit");
    require((cp0.read(Cp0::kCause) & timerMask) == 0U,
            "Compare write clears timer pending state");
    require(!cp0.interruptRequested(),
            "cleared timer pending bit no longer requests interrupt");

    require(cp0.write(Cp0::kCount, 0xFFFFFFFFULL),
            "set Count to its maximum value");
    require(cp0.write(Cp0::kCompare, 0U),
            "set Compare to wrapped Count value");
    cp0.tick();
    require(cp0.read(Cp0::kCount) == 0U,
            "Count wraps from 32-bit maximum to zero");
    require((cp0.read(Cp0::kCause) & timerMask) != 0U,
            "timer interrupt triggers when Count wraps to Compare");

    require(cp0.write(Cp0::kStatus, Cp0::kStatusIe),
            "mask the timer interrupt source");
    require(!cp0.interruptRequested(),
            "masked timer pending bit does not request an interrupt");
}


void testCpuAcceptsCp0TimerInterruptBetweenInstructions() {
    N64Machine machine;
    Bus& bus = machine.bus();
    require(bus.write32(0x000U, 0x25080001U), "write first ADDIU before timer interrupt");
    require(bus.write32(0x004U, 0x25080001U), "write second ADDIU before timer interrupt");
    require(bus.write32(0x008U, 0x25080001U), "write instruction interrupted by timer");
    require(bus.write32(0x180U, 0x42000018U), "write ERET at timer interrupt vector");

    auto& cpu = machine.cpu();
    cpu.reset(0xFFFFFFFF80000000ULL);
    constexpr std::uint64_t timerMask = 1ULL << 15U;
    require(cpu.setCp0Register(m64plus::n64::Cp0::kCount, 0U),
            "initialize Count before CPU timer interrupt");
    require(cpu.setCp0Register(m64plus::n64::Cp0::kCompare, 2U),
            "schedule timer interrupt after two CPU instructions");
    require(cpu.setCp0Register(m64plus::n64::Cp0::kStatus,
                               m64plus::n64::Cp0::kStatusIe | timerMask),
            "enable global and timer interrupt in CPU Status");

    require(machine.stepCpu(), "CPU executes first instruction before timer match");
    require(machine.stepCpu(), "CPU executes second instruction and timer becomes pending");
    require(cpu.pc() == 0xFFFFFFFF80000008ULL,
            "CPU reaches next instruction after timer match");
    require((cpu.cp0().read(m64plus::n64::Cp0::kCause) & timerMask) != 0U,
            "timer match sets Cause.IP7");

    require(machine.stepCpu(), "CPU accepts timer interrupt before next instruction");
    require(cpu.pc() == 0xFFFFFFFF80000180ULL,
            "timer interrupt transfers control to general exception vector");
    require(cpu.cp0().read(m64plus::n64::Cp0::kEpc) == 0xFFFFFFFF80000008ULL,
            "timer interrupt saves the next instruction address in EPC");
    require((cpu.cp0().read(m64plus::n64::Cp0::kCause) & 0x7CU) == 0U,
            "timer interrupt records Interrupt exception code");
    require(cpu.cp0().exceptionLevel(),
            "timer interrupt sets CP0 EXL before entering handler");
    require(cpu.reg(8U) == 2U,
            "interrupted instruction does not execute before handler entry");
}

void testCpuDefersTimerInterruptUntilAfterBranchDelaySlot() {
    N64Machine machine;
    Bus& bus = machine.bus();
    require(bus.write32(0x000U, 0x10000001U), "write taken BEQ before timer interrupt");
    require(bus.write32(0x004U, 0x25080001U), "write ADDIU in branch delay slot");
    require(bus.write32(0x008U, 0x25290001U), "write target instruction after delay slot");
    require(bus.write32(0x180U, 0x42000018U), "write ERET at deferred timer handler");

    auto& cpu = machine.cpu();
    cpu.reset(0xFFFFFFFF80000000ULL);
    constexpr std::uint64_t timerMask = 1ULL << 15U;
    require(cpu.setCp0Register(m64plus::n64::Cp0::kCount, 0U),
            "initialize Count before branch-delay timer test");
    require(cpu.setCp0Register(m64plus::n64::Cp0::kCompare, 1U),
            "schedule timer interrupt immediately after branch");
    require(cpu.setCp0Register(m64plus::n64::Cp0::kStatus,
                               m64plus::n64::Cp0::kStatusIe | timerMask),
            "enable timer interrupt before branch");

    require(machine.stepCpu(), "CPU executes branch while timer is not yet pending");
    require((cpu.cp0().read(m64plus::n64::Cp0::kCause) & timerMask) != 0U,
            "timer becomes pending after branch instruction");
    require(cpu.pc() == 0xFFFFFFFF80000004ULL,
            "CPU points to branch delay slot while interrupt is pending");

    require(machine.stepCpu(), "CPU executes delay slot instead of taking interrupt");
    require(cpu.reg(8U) == 1U,
            "branch delay-slot instruction executes before timer interrupt");
    require(cpu.pc() == 0xFFFFFFFF80000008ULL,
            "taken branch reaches its target after delay slot");

    require(machine.stepCpu(), "CPU accepts pending timer interrupt after delay slot");
    require(cpu.pc() == 0xFFFFFFFF80000180ULL,
            "deferred timer interrupt enters general exception vector");
    require(cpu.cp0().read(m64plus::n64::Cp0::kEpc) == 0xFFFFFFFF80000008ULL,
            "deferred interrupt saves branch target as EPC");
    require((cpu.cp0().read(m64plus::n64::Cp0::kCause) & (1ULL << 31U)) == 0U,
            "deferred interrupt does not incorrectly set Cause.BD");
    require(cpu.reg(9U) == 0U,
            "target instruction does not execute before deferred interrupt handler");
}

void testCpuSyscallAndBreakpointExceptions() {
    const struct ExceptionCase {
        std::uint32_t instruction;
        std::uint8_t code;
        const char* description;
    } cases[] = {
        {0x0000000CU, 8U, "SYSCALL"},
        {0x0000000DU, 9U, "BREAK"},
        {0x0001234CU, 8U, "SYSCALL with code field"},
        {0x0001234DU, 9U, "BREAK with code field"}
    };
    for (const auto& item : cases) {
        N64Machine machine;
        require(machine.bus().write32(0U, item.instruction),
                "write syscall/break instruction");
        machine.cpu().reset(0xFFFFFFFF80000000ULL);
        require(machine.runCpuInstructions(1U) == 0U,
                "syscall/break stops normal instruction execution");
        require(machine.cpu().hasLastException() &&
                machine.cpu().lastExceptionCode() == item.code,
                item.description);
        require(machine.cpu().lastExceptionInstruction() == item.instruction,
                "syscall/break records the triggering instruction");
        require(machine.cpu().cp0().read(m64plus::n64::Cp0::kEpc) ==
                0xFFFFFFFF80000000ULL,
                "syscall/break stores instruction address in EPC");
        require((machine.cpu().cp0().read(m64plus::n64::Cp0::kCause) & 0x7CU) ==
                (static_cast<std::uint64_t>(item.code) << 2U),
                "syscall/break stores the correct Cause exception code");
        require(machine.cpu().pc() == 0xFFFFFFFF80000180ULL,
                "syscall/break enters general exception vector");
    }
}

void testCpuSyscallAndBreakpointInBranchDelaySlot() {
    const struct DelayCase {
        std::uint32_t instruction;
        std::uint8_t code;
    } cases[] = {
        {0x0000000CU, 8U},
        {0x0000000DU, 9U}
    };
    for (const auto& item : cases) {
        N64Machine machine;
        require(machine.bus().write32(0U, 0x10000001U),
                "write branch before syscall/break delay-slot test");
        require(machine.bus().write32(4U, item.instruction),
                "write syscall/break in branch delay slot");
        machine.cpu().reset(0xFFFFFFFF80000000ULL);
        require(machine.stepCpu(), "execute branch before syscall/break");
        require(machine.stepCpu(), "take syscall/break from branch delay slot");
        require(machine.cpu().lastExceptionCode() == item.code,
                "delay-slot syscall/break raises the correct exception");
        require(machine.cpu().cp0().read(m64plus::n64::Cp0::kEpc) ==
                0xFFFFFFFF80000000ULL,
                "delay-slot syscall/break records branch address in EPC");
        require((machine.cpu().cp0().read(m64plus::n64::Cp0::kCause) &
                 (1ULL << 31U)) != 0U,
                "delay-slot syscall/break sets Cause.BD");
        require(((machine.cpu().cp0().read(m64plus::n64::Cp0::kCause) >> 2U) &
                 0x1FU) == item.code,
                "delay-slot syscall/break records the exception code in Cause.ExcCode");
    }
}

void testCpuSpecialTrapInstructions() {
    const struct TrapCase {
        std::uint32_t instruction;
        std::uint64_t left;
        std::uint64_t right;
        bool traps;
        const char* description;
    } cases[] = {
        {0x01090030U, 5U, 5U, true, "TGE traps when signed left >= right"},
        {0x01090031U, 4U, 5U, false, "TGEU does not trap when left < right"},
        {0x01090032U, 4U, 5U, true, "TLT traps when signed left < right"},
        {0x01090033U, 5U, 4U, false, "TLTU does not trap when left >= right"},
        {0x01090034U, 7U, 7U, true, "TEQ traps when operands are equal"},
        {0x01090036U, 7U, 8U, true, "TNE traps when operands differ"},
        {0x01090030U, 4U, 5U, false, "TGE does not trap when left < right"},
        {0x01090031U, 5U, 4U, true, "TGEU traps when left >= right"},
        {0x01090032U, 5U, 4U, false, "TLT does not trap when left >= right"},
        {0x01090033U, 4U, 5U, true, "TLTU traps when left < right"},
        {0x01090034U, 7U, 8U, false, "TEQ does not trap when operands differ"},
        {0x01090036U, 7U, 7U, false, "TNE does not trap when operands are equal"},
        {0x01090030U, 0xFFFFFFFFFFFFFFFFULL, 0U, false,
         "TGE uses signed comparison for negative values"},
        {0x01090032U, 0xFFFFFFFFFFFFFFFFULL, 0U, true,
         "TLT uses signed comparison for negative values"}
    };
    for (const auto& item : cases) {
        N64Machine machine;
        require(machine.bus().write32(0U, item.instruction),
                "write SPECIAL trap instruction");
        machine.cpu().reset(0xFFFFFFFF80000000ULL);
        machine.cpu().setRegister(8U, item.left);
        machine.cpu().setRegister(9U, item.right);
        require(machine.stepCpu(), item.description);
        if (item.traps) {
            require(machine.cpu().hasLastException() &&
                    machine.cpu().lastExceptionCode() == 13U,
                    "taken SPECIAL trap raises Trap exception");
            require(machine.cpu().pc() == 0xFFFFFFFF80000180ULL,
                    "taken SPECIAL trap enters general exception vector");
            require(machine.cpu().cp0().read(m64plus::n64::Cp0::kEpc) ==
                    0xFFFFFFFF80000000ULL,
                    "taken SPECIAL trap records its instruction address");
        } else {
            require(!machine.cpu().hasLastException(),
                    "untaken SPECIAL trap does not raise an exception");
            require(machine.cpu().pc() == 0xFFFFFFFF80000004ULL,
                    "untaken SPECIAL trap continues to next instruction");
        }
    }
}

void testCpuSyncInstructionAndReservedFields() {
    const std::uint32_t validSync[] = {
        0x0000000FU, // SYNC stype 0
        0x0000004FU, // SYNC stype 1
        0x000007CFU  // SYNC stype 31
    };
    for (const std::uint32_t instruction : validSync) {
        N64Machine machine;
        require(machine.bus().write32(0U, instruction), "write valid SYNC instruction");
        machine.cpu().reset(0xFFFFFFFF80000000ULL);
        require(machine.stepCpu(), "execute valid SYNC instruction");
        require(!machine.cpu().hasLastException() &&
                machine.cpu().pc() == 0xFFFFFFFF80000004ULL,
                "valid SYNC completes without exception");
    }

    {
        N64Machine machine;
        Bus& bus = machine.bus();
        require(bus.write32(0x000U, 0x10000001U), "write branch before SYNC delay slot");
        require(bus.write32(0x004U, 0x0000000FU), "write SYNC in branch delay slot");
        require(bus.write32(0x008U, 0x240A0001U), "write branch target after SYNC");
        machine.cpu().reset(0xFFFFFFFF80000000ULL);
        require(machine.stepCpu(), "execute branch before SYNC delay slot");
        require(machine.stepCpu(), "execute SYNC in branch delay slot");
        require(!machine.cpu().hasLastException() &&
                machine.cpu().pc() == 0xFFFFFFFF80000008ULL,
                "SYNC is legal in a branch delay slot and preserves branch target");
        require(machine.stepCpu() && machine.cpu().reg(10U) == 1U,
                "execution continues at branch target after delay-slot SYNC");
    }

    const std::uint32_t invalidSync[] = {
        0x0020000FU, // rs != 0
        0x0001000FU, // rt != 0
        0x0000080FU  // rd != 0
    };
    for (const std::uint32_t instruction : invalidSync) {
        N64Machine machine;
        require(machine.bus().write32(0U, instruction), "write invalid SYNC encoding");
        machine.cpu().reset(0xFFFFFFFF80000000ULL);
        require(machine.runCpuInstructions(1U) == 0U,
                "invalid SYNC encoding raises an exception");
        require(machine.cpu().hasLastException() &&
                machine.cpu().lastExceptionCode() == 10U &&
                machine.cpu().lastExceptionInstruction() == instruction,
                "invalid SYNC encoding raises Reserved Instruction");
    }
}

void testCpuMoveConditionalInstructions() {
    const struct MoveCase {
        std::uint32_t instruction;
        std::uint64_t source;
        std::uint64_t condition;
        std::uint64_t expected;
        const char* description;
    } cases[] = {
        {0x0109500AU, 0x123456789ABCDEF0ULL, 0U,
         0x123456789ABCDEF0ULL, "MOVZ copies source when condition is zero"},
        {0x0109500AU, 0x123456789ABCDEF0ULL, 1U,
         0xA5A5A5A5A5A5A5A5ULL, "MOVZ preserves destination when condition is nonzero"},
        {0x0109500BU, 0xFEDCBA9876543210ULL, 1U,
         0xFEDCBA9876543210ULL, "MOVN copies source when condition is nonzero"},
        {0x0109500BU, 0xFEDCBA9876543210ULL, 0U,
         0xA5A5A5A5A5A5A5A5ULL, "MOVN preserves destination when condition is zero"}
    };
    for (const auto& item : cases) {
        N64Machine machine;
        require(machine.bus().write32(0U, item.instruction),
                "write conditional move instruction");
        machine.cpu().reset(0xFFFFFFFF80000000ULL);
        machine.cpu().setRegister(8U, item.source);
        machine.cpu().setRegister(9U, item.condition);
        machine.cpu().setRegister(10U, 0xA5A5A5A5A5A5A5A5ULL);
        require(machine.stepCpu(), item.description);
        require(machine.cpu().reg(10U) == item.expected,
                "conditional move updates destination only when condition matches");
        require(!machine.cpu().hasLastException(),
                "valid conditional move does not raise an exception");
    }

    const struct DelaySlotCase {
        std::uint32_t moveInstruction;
        std::uint64_t condition;
        std::uint64_t expected;
        const char* description;
    } delaySlotCases[] = {
        {0x0109500AU, 0U, 0x123456789ABCDEF0ULL,
         "MOVZ executes in a branch delay slot"},
        {0x0109500BU, 1U, 0xFEDCBA9876543210ULL,
         "MOVN executes in a branch delay slot"}
    };
    for (const auto& item : delaySlotCases) {
        N64Machine machine;
        Bus& bus = machine.bus();
        require(bus.write32(0x000U, 0x10000002U),
                "write branch before conditional move delay slot");
        require(bus.write32(0x004U, item.moveInstruction),
                "write conditional move in branch delay slot");
        require(bus.write32(0x00CU, 0x240B0001U),
                "write branch target instruction");
        auto& cpu = machine.cpu();
        cpu.reset(0xFFFFFFFF80000000ULL);
        cpu.setRegister(8U, item.expected);
        cpu.setRegister(9U, item.condition);
        cpu.setRegister(10U, 0xA5A5A5A5A5A5A5A5ULL);
        require(machine.stepCpu(), "execute branch before conditional move");
        require(machine.stepCpu(), item.description);
        require(cpu.reg(10U) == item.expected,
                "conditional move takes effect in branch delay slot");
        require(cpu.pc() == 0xFFFFFFFF8000000CU,
                "conditional move delay slot preserves branch target");
        require(!cpu.hasLastException(),
                "conditional move in delay slot does not raise an exception");
    }
}

void testCpuRegimmTrapImmediateInstructions() {
    const struct TrapImmediateCase {
        unsigned rt;
        std::uint64_t value;
        std::uint16_t immediate;
        bool traps;
        const char* description;
    } cases[] = {
        {0x08U, 5U, 5U, true, "TGEI traps when signed value >= immediate"},
        {0x08U, 4U, 5U, false, "TGEI does not trap below immediate"},
        {0x09U, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFU, true,
         "TGEIU compares against sign-extended unsigned immediate"},
        {0x09U, 0x7FFFFFFFFFFFFFFFULL, 0xFFFFU, false,
         "TGEIU does not trap below sign-extended unsigned immediate"},
        {0x0AU, 4U, 5U, true, "TLTI traps when signed value < immediate"},
        {0x0AU, 5U, 5U, false, "TLTI does not trap at equality"},
        {0x0BU, 0x7FFFFFFFFFFFFFFFULL, 0xFFFFU, true,
         "TLTIU uses sign-extended immediate as unsigned value"},
        {0x0BU, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFU, false,
         "TLTIU does not trap when unsigned value equals immediate"},
        {0x0CU, 0xFFFFFFFFFFFFFFFBU, 0xFFFBU, true,
         "TEQI compares with sign-extended immediate"},
        {0x0CU, 5U, 6U, false, "TEQI does not trap for unequal values"},
        {0x0EU, 5U, 5U, false, "TNEI does not trap for equal values"},
        {0x0EU, 6U, 5U, true, "TNEI traps for unequal values"}
    };
    for (const auto& item : cases) {
        N64Machine machine;
        const std::uint32_t instruction = (0x01U << 26U) | (8U << 21U) |
            (item.rt << 16U) | item.immediate;
        require(machine.bus().write32(0U, instruction),
                "write REGIMM trap-immediate instruction");
        machine.cpu().reset(0xFFFFFFFF80000000ULL);
        machine.cpu().setRegister(8U, item.value);
        require(machine.stepCpu(), item.description);
        if (item.traps) {
            require(machine.cpu().hasLastException() &&
                    machine.cpu().lastExceptionCode() == 13U,
                    "REGIMM trap-immediate raises Trap exception");
            require(machine.cpu().pc() == 0xFFFFFFFF80000180ULL,
                    "REGIMM trap-immediate enters general exception vector");
            require(machine.cpu().cp0().read(m64plus::n64::Cp0::kEpc) ==
                    0xFFFFFFFF80000000ULL,
                    "REGIMM trap-immediate records instruction address");
        } else {
            require(!machine.cpu().hasLastException() &&
                    machine.cpu().pc() == 0xFFFFFFFF80000004ULL,
                    "untaken REGIMM trap-immediate continues normally");
        }
    }

    N64Machine machine;
    Bus& bus = machine.bus();
    require(bus.write32(0U, 0x10000001U), "write branch before trap-immediate");
    require(bus.write32(4U, 0x05080005U), "write TGEI in branch delay slot");
    machine.cpu().reset(0xFFFFFFFF80000000ULL);
    machine.cpu().setRegister(8U, 5U);
    require(machine.stepCpu(), "execute branch before delay-slot TGEI");
    require(machine.stepCpu(), "execute legal TGEI in branch delay slot");
    require(machine.cpu().hasLastException() &&
            machine.cpu().lastExceptionCode() == 13U,
            "trap-immediate in delay slot raises Trap exception");
    require(machine.cpu().cp0().read(m64plus::n64::Cp0::kEpc) ==
            0xFFFFFFFF80000000ULL,
            "delay-slot trap records branch address in EPC");
    require((machine.cpu().cp0().read(m64plus::n64::Cp0::kCause) &
            (1ULL << 31U)) != 0U,
            "delay-slot trap sets Cause.BD");
}

void testCpuRejectsInvalidBlezBgtzEncodings() {
    const std::uint32_t invalidBranches[] = {
        0x19080001U, // BLEZ with rt != 0
        0x1D080001U, // BGTZ with rt != 0
        0x59080001U, // BLEZL with rt != 0
        0x5D080001U  // BGTZL with rt != 0
    };
    for (const std::uint32_t instruction : invalidBranches) {
        N64Machine machine;
        require(machine.bus().write32(0U, instruction),
                "write invalid BLEZ/BGTZ encoding");
        machine.cpu().reset(0xFFFFFFFF80000000ULL);
        require(machine.runCpuInstructions(1U) == 0U,
                "invalid BLEZ/BGTZ encoding raises an exception");
        require(machine.cpu().hasLastException() &&
                machine.cpu().lastExceptionCode() == 10U,
                "invalid BLEZ/BGTZ encoding raises Reserved Instruction");
        require(machine.cpu().lastExceptionInstruction() == instruction,
                "CPU records the invalid BLEZ/BGTZ instruction");
        require(machine.cpu().pc() == 0xFFFFFFFF80000180ULL,
                "invalid BLEZ/BGTZ encoding enters general exception vector");
    }
}

void testCpuRejectsInvalidSpecialShiftAndJumpEncodings() {
    const std::uint32_t invalidInstructions[] = {
        0x00221800U, // SLL with rs != 0
        0x00221842U, // SRL with rs != 0
        0x002218C3U, // SRA with rs != 0
        0x00221844U, // SLLV with shift != 0
        0x002218D4U, // DSLLV with shift != 0
        0x00200808U, // JR with rt != 0
        0x00201808U, // JR with rd != 0
        0x00210809U, // JALR with rt != 0
        0x00200849U, // JALR with shift != 0
        0x00200010U, // MFHI with rs != 0
        0x00210011U, // MTHI with rt != 0
        0x00220818U, // MULT with rd != 0
        0x00221860U  // ADD with shift != 0
    };
    for (const std::uint32_t instruction : invalidInstructions) {
        N64Machine machine;
        require(machine.bus().write32(0U, instruction),
                "write invalid SPECIAL shift/jump encoding");
        machine.cpu().reset(0xFFFFFFFF80000000ULL);
        require(machine.runCpuInstructions(1U) == 0U,
                "invalid SPECIAL shift/jump encoding raises an exception");
        require(machine.cpu().hasLastException() &&
                machine.cpu().lastExceptionCode() == 10U,
                "invalid SPECIAL shift/jump encoding raises Reserved Instruction");
        require(machine.cpu().lastExceptionInstruction() == instruction,
                "CPU records invalid SPECIAL shift/jump instruction");
        require(machine.cpu().pc() == 0xFFFFFFFF80000180ULL,
                "invalid SPECIAL shift/jump enters general exception vector");
    }
}

void testCpuBranchLikelyDelaySlotSemantics() {
    auto runCase = [](std::uint32_t branch, std::uint64_t left, std::uint64_t right,
                      bool taken, const char* description) {
        N64Machine machine;
        Bus& bus = machine.bus();
        require(bus.write32(0x000U, branch), "write branch-likely instruction");
        require(bus.write32(0x004U, 0x240A0001U),
                "write instruction in branch-likely delay slot");
        require(bus.write32(0x008U, 0x240B0001U),
                "write sequential instruction after branch-likely");
        require(bus.write32(0x00CU, 0x240C0001U),
                "write taken branch-likely target instruction");

        auto& cpu = machine.cpu();
        cpu.reset(0xFFFFFFFF80000000ULL);
        cpu.setRegister(8U, left);
        cpu.setRegister(9U, right);
        require(machine.stepCpu(), description);
        if (taken) {
            require(cpu.pc() == 0xFFFFFFFF80000004ULL,
                    "taken branch-likely schedules its delay slot");
            require(machine.stepCpu(), "execute taken branch-likely delay slot");
            require(cpu.reg(10U) == 1U && cpu.pc() == 0xFFFFFFFF8000000CU,
                    "taken branch-likely executes delay slot and reaches target");
            require(machine.stepCpu(), "execute taken branch-likely target");
            require(cpu.reg(12U) == 1U && cpu.reg(11U) == 0U,
                    "taken branch-likely skips sequential instruction");
        } else {
            require(cpu.pc() == 0xFFFFFFFF80000008ULL,
                    "untaken branch-likely annuls its delay slot");
            require(cpu.reg(10U) == 0U,
                    "annulled branch-likely delay-slot instruction has no side effect");
            require(machine.stepCpu(), "execute instruction after annulled delay slot");
            require(cpu.reg(11U) == 1U && cpu.reg(10U) == 0U,
                    "untaken branch-likely resumes after the annulled delay slot");
        }
    };

    runCase(0x51090002U, 7U, 7U, true,
            "execute taken BEQL branch-likely");
    runCase(0x55090002U, 7U, 7U, false,
            "execute untaken BNEL branch-likely");
    runCase(0x05020002U, 0U, 0U, false,
            "execute untaken BLTZL branch-likely");
    runCase(0x05030002U, 7U, 0U, true,
            "execute taken BGEZL branch-likely");
    runCase(0x59000002U, 0U, 0U, true,
            "execute taken BLEZL branch-likely");
    runCase(0x5D000002U, 1U, 0U, true,
            "execute taken BGTZL branch-likely");
    runCase(0x59000002U, 1U, 0U, false,
            "execute untaken BLEZL branch-likely");
    runCase(0x5D000002U, 0U, 0U, false,
            "execute untaken BGTZL branch-likely");

    auto runLinkLikelyCase = [](std::uint32_t branch, std::uint64_t value,
                                bool taken, const char* description) {
        N64Machine machine;
        Bus& bus = machine.bus();
        require(bus.write32(0x000U, branch), "write REGIMM link-likely instruction");
        require(bus.write32(0x004U, 0x240A0001U),
                "write REGIMM link-likely delay-slot instruction");
        require(bus.write32(0x008U, 0x240B0001U),
                "write instruction after REGIMM link-likely");
        require(bus.write32(0x00CU, 0x240C0001U),
                "write REGIMM link-likely target instruction");
        auto& cpu = machine.cpu();
        cpu.reset(0xFFFFFFFF80000000ULL);
        cpu.setRegister(8U, value);
        require(machine.stepCpu(), description);
        require(cpu.reg(31U) == (taken ? 0xFFFFFFFF80000008ULL : 0U),
                "REGIMM link-likely writes the return address only when taken");
        if (taken) {
            require(cpu.pc() == 0xFFFFFFFF80000004ULL,
                    "taken REGIMM link-likely schedules its delay slot");
            require(machine.stepCpu(), "execute taken REGIMM link-likely delay slot");
            require(cpu.reg(10U) == 1U && cpu.pc() == 0xFFFFFFFF8000000CU,
                    "taken REGIMM link-likely executes delay slot and branches");
        } else {
            require(cpu.pc() == 0xFFFFFFFF80000008ULL && cpu.reg(10U) == 0U,
                    "untaken REGIMM link-likely annuls its delay slot");
            require(machine.stepCpu(), "execute instruction after annulled REGIMM delay slot");
            require(cpu.reg(11U) == 1U,
                    "untaken REGIMM link-likely continues after the annulled slot");
        }
    };
    runLinkLikelyCase(0x05120002U, 0xFFFFFFFFFFFFFFFFULL, true,
                      "execute taken BLTZALL branch-likely");
    runLinkLikelyCase(0x05130002U, 0U, true,
                      "execute taken BGEZALL branch-likely");
    runLinkLikelyCase(0x05120002U, 0U, false,
                      "execute untaken BLTZALL branch-likely");
    runLinkLikelyCase(0x05130002U, 0xFFFFFFFFFFFFFFFFULL, false,
                      "execute untaken BGEZALL branch-likely");

    auto runLinkCase = [](std::uint32_t branch, std::uint64_t value,
                          bool taken, const char* description) {
        N64Machine machine;
        Bus& bus = machine.bus();
        require(bus.write32(0x000U, branch), "write REGIMM link instruction");
        require(bus.write32(0x004U, 0x240A0001U),
                "write REGIMM link delay-slot instruction");
        require(bus.write32(0x008U, 0x240B0001U),
                "write instruction after REGIMM link");
        require(bus.write32(0x00CU, 0x240C0001U),
                "write REGIMM link target instruction");
        auto& cpu = machine.cpu();
        cpu.reset(0xFFFFFFFF80000000ULL);
        cpu.setRegister(8U, value);
        require(machine.stepCpu(), description);
        require(cpu.reg(31U) == (taken ? 0xFFFFFFFF80000008ULL : 0U),
                "REGIMM branch-and-link writes return address only when taken");
        if (taken) {
            require(cpu.pc() == 0xFFFFFFFF80000004ULL,
                    "taken REGIMM branch-and-link schedules delay slot");
            require(machine.stepCpu(), "execute taken REGIMM branch-and-link delay slot");
            require(cpu.reg(10U) == 1U && cpu.pc() == 0xFFFFFFFF8000000CU,
                    "taken REGIMM branch-and-link reaches target");
        } else {
            require(cpu.pc() == 0xFFFFFFFF80000004ULL,
                    "untaken ordinary REGIMM branch still schedules its delay slot");
            require(machine.stepCpu(), "execute untaken REGIMM branch delay slot");
            require(cpu.reg(10U) == 1U && cpu.pc() == 0xFFFFFFFF80000008ULL,
                    "untaken ordinary REGIMM branch retires its delay slot before continuing");
            require(machine.stepCpu(), "execute instruction after untaken REGIMM branch");
            require(cpu.reg(11U) == 1U,
                    "untaken REGIMM branch continues sequentially after its delay slot");
        }
    };
    runLinkCase(0x05100002U, 0xFFFFFFFFFFFFFFFFULL, true,
                "execute taken BLTZAL branch-and-link");
    runLinkCase(0x05110002U, 0U, true,
                "execute taken BGEZAL branch-and-link");
    runLinkCase(0x05100002U, 0U, false,
                "execute untaken BLTZAL branch-and-link");
    runLinkCase(0x05110002U, 0xFFFFFFFFFFFFFFFFULL, false,
                "execute untaken BGEZAL branch-and-link");
}

void testCpuEretClearsLoadLinkedReservation() {
    N64Machine machine;
    Bus& bus = machine.bus();
    constexpr std::uint32_t dataAddress = 0x240U;
    constexpr std::uint32_t initial = 0x12345678U;
    require(bus.write32(0x000U, 0xC1090000U), "write LL before ERET");
    require(bus.write32(0x004U, 0x42000018U), "write ERET after LL");
    require(bus.write32(0x020U, 0xE1090000U), "write SC at ERET target");
    require(bus.write32(dataAddress, initial), "initialize ERET LL/SC target");

    auto& cpu = machine.cpu();
    cpu.reset(0xFFFFFFFF80000000ULL);
    cpu.setRegister(8U, dataAddress);
    require(cpu.setCp0Register(m64plus::n64::Cp0::kEpc,
                               0xFFFFFFFF80000020ULL),
            "set ERET return address after LL");
    require(machine.stepCpu(), "LL establishes reservation before ERET");
    require(machine.stepCpu(), "ERET returns and clears reservation");
    require(cpu.pc() == 0xFFFFFFFF80000020ULL,
            "ERET transfers to EPC after clearing reservation");
    require(machine.stepCpu(), "SC executes after ERET");
    std::uint32_t value = 0;
    require(bus.read32(dataAddress, value) && value == initial,
            "SC after ERET does not modify reserved memory");
    require(cpu.reg(9U) == 0U,
            "SC reports failure because ERET cleared the LLbit");
}

void testCpuEretRestoresExceptionReturnAddress() {
    N64Machine machine;
    Bus& bus = machine.bus();
    constexpr std::uint64_t initialPc = 0xFFFFFFFF80001000ULL;
    require(bus.write32(0x1000U, 0x7000003FU),
            "place reserved instruction at exception return address");
    require(bus.write32(0x0180U, 0x42000018U),
            "place ERET at general exception vector");

    machine.cpu().reset(initialPc);
    require(machine.stepCpu(), "CPU takes reserved-instruction exception");
    require(machine.cpu().pc() == 0xFFFFFFFF80000180ULL,
            "reserved instruction enters general exception vector");
    require(machine.cpu().cp0().read(m64plus::n64::Cp0::kEpc) == initialPc,
            "exception handler receives original instruction address in EPC");
    require(machine.cpu().cp0().exceptionLevel(),
            "exception entry sets CP0 EXL");

    require(machine.stepCpu(), "exception handler executes ERET");
    require(machine.cpu().pc() == initialPc,
            "ERET restores execution to EPC");
    require(!machine.cpu().cp0().exceptionLevel(),
            "ERET clears CP0 EXL");
}


void testCpuEretReturnsToBranchAfterDelaySlotException() {
    N64Machine machine;
    Bus& bus = machine.bus();
    constexpr std::uint64_t branchPc = 0xFFFFFFFF80001000ULL;
    constexpr std::uint64_t causeBd = 1ULL << 31U;

    require(bus.write32(0x1000U, 0x10000001U),
            "write BEQ with a delay slot at the test address");
    require(bus.write32(0x1004U, 0x42000018U),
            "write ERET into the branch delay slot");
    require(bus.write32(0x0180U, 0x42000018U),
            "write ERET into the general exception handler");

    machine.cpu().reset(branchPc);
    require(machine.stepCpu(), "CPU executes branch before delay-slot exception");
    require(machine.cpu().pc() == branchPc + 4U,
            "branch schedules its delay-slot instruction");
    require(machine.stepCpu(), "ERET in delay slot raises Reserved Instruction");
    require(machine.cpu().pc() == 0xFFFFFFFF80000180ULL,
            "delay-slot exception enters general exception vector");
    require(machine.cpu().cp0().read(m64plus::n64::Cp0::kEpc) == branchPc,
            "delay-slot exception records branch address in EPC");
    require((machine.cpu().cp0().read(m64plus::n64::Cp0::kCause) & causeBd) != 0U,
            "delay-slot exception sets Cause.BD");

    require(machine.stepCpu(), "exception handler ERET executes");
    require(machine.cpu().pc() == branchPc,
            "ERET resumes at branch instruction recorded in EPC");
    require(!machine.cpu().cp0().exceptionLevel(),
            "ERET clears EXL after a delay-slot exception");
}


void testMiExternalInterruptReachesMainCpu() {
    N64Machine machine;
    Bus& bus = machine.bus();
    auto& cpu = machine.cpu();

    require(bus.write32(Bus::kMiInterruptMask, 1U << 1U),
            "enable MI SP interrupt mask");
    bus.setMiInterrupt(0U, true);
    require(bus.miInterruptPending(), "asserted SP source becomes pending when masked");

    cpu.reset(0xFFFFFFFF80000000ULL);
    constexpr std::uint64_t statusIeAndIp2Mask = (1ULL << 0U) | (1ULL << 10U);
    require(cpu.setCp0Register(m64plus::n64::Cp0::kStatus, statusIeAndIp2Mask),
            "enable CPU interrupts and external IP2 mask");

    require(machine.stepCpu(), "CPU accepts pending MI external interrupt");
    require(cpu.lastExceptionCode() ==
                static_cast<std::uint8_t>(m64plus::n64::ExceptionCode::Interrupt),
            "MI interrupt enters the CPU Interrupt exception");
    require((cpu.cp0().read(m64plus::n64::Cp0::kCause) & (1ULL << 10U)) != 0U,
            "MI interrupt is reflected in CP0 Cause.IP2");
    require(cpu.cp0().exceptionLevel(), "MI interrupt sets CP0 EXL");
}


void testMiExternalInterruptDeassertionClearsCpuPendingBit() {
    N64Machine machine;
    Bus& bus = machine.bus();
    auto& cpu = machine.cpu();

    require(bus.write32(Bus::kMiInterruptMask, 1U << 1U),
            "enable MI SP interrupt mask for deassertion test");
    bus.setMiInterrupt(0U, true);
    cpu.reset(0xFFFFFFFF80000000ULL);
    constexpr std::uint64_t statusIeAndIp2Mask = (1ULL << 0U) | (1ULL << 10U);
    require(cpu.setCp0Register(m64plus::n64::Cp0::kStatus, statusIeAndIp2Mask),
            "enable CPU external interrupt input");

    bus.setMiInterrupt(0U, false);
    require(!bus.miInterruptPending(), "clearing MI SP source removes pending MI interrupt");
    require(machine.stepCpu(), "CPU executes normally after MI source is deasserted");
    require(!cpu.hasLastException(), "deasserted MI source does not cause an interrupt exception");
    require((cpu.cp0().read(m64plus::n64::Cp0::kCause) & (1ULL << 10U)) == 0U,
            "deasserted MI source clears CP0 Cause.IP2 before CPU execution");
}


void testMiMaskCommandsDriveCpuExternalPendingBit() {
    N64Machine machine;
    Bus& bus = machine.bus();
    auto& cpu = machine.cpu();

    // Keep CPU interrupts globally disabled so each step can observe IP2
    // without taking an exception.
    cpu.reset(0xFFFFFFFF80000000ULL);
    require(cpu.setCp0Register(m64plus::n64::Cp0::kStatus, 1ULL << 10U),
            "enable the CPU IP2 mask while global interrupts remain disabled");
    require(bus.write32(Bus::kMiInterruptMask, 1U << 1U),
            "enable MI SP interrupt source");
    bus.setMiInterrupt(0U, true);
    require(machine.stepCpu(), "CPU samples asserted MI SP source");
    require((cpu.cp0().read(m64plus::n64::Cp0::kCause) & (1ULL << 10U)) != 0U,
            "asserted masked MI source raises CPU Cause.IP2");

    require(bus.write32(Bus::kMiInterruptMask, 1U),
            "clear MI SP interrupt mask with clear command");
    require(!bus.miInterruptPending(),
            "clearing MI mask suppresses still-asserted SP source");
    require(machine.stepCpu(), "CPU samples cleared MI interrupt mask");
    require((cpu.cp0().read(m64plus::n64::Cp0::kCause) & (1ULL << 10U)) == 0U,
            "cleared MI mask deasserts CPU Cause.IP2");

    require(bus.write32(Bus::kMiInterruptMask, 1U << 1U),
            "set MI SP interrupt mask again");
    require(bus.miInterruptPending(),
            "re-enabling MI mask exposes the still-asserted SP source");
    require(machine.stepCpu(), "CPU samples re-enabled MI interrupt mask");
    require((cpu.cp0().read(m64plus::n64::Cp0::kCause) & (1ULL << 10U)) != 0U,
            "re-enabled MI mask reasserts CPU Cause.IP2");
    require(!cpu.hasLastException(),
            "masked IP2 observation does not raise an exception while CPU IE is disabled");
}


void testMiInterruptSourcesRemainAggregatedWhenOneMaskIsCleared() {
    N64Machine machine;
    Bus& bus = machine.bus();
    auto& cpu = machine.cpu();

    cpu.reset(0xFFFFFFFF80000000ULL);
    require(cpu.setCp0Register(m64plus::n64::Cp0::kStatus, 1ULL << 10U),
            "enable CPU IP2 mask without global interrupt enable");
    require(bus.write32(Bus::kMiInterruptMask, (1U << 1U) | (1U << 9U)),
            "enable MI SP and PI interrupt masks");
    bus.setMiInterrupt(0U, true);
    bus.setMiInterrupt(4U, true);
    require(bus.miInterruptPending(), "SP and PI sources are both pending");

    require(machine.stepCpu(), "CPU samples simultaneous SP and PI interrupts");
    constexpr std::uint64_t ip2 = 1ULL << 10U;
    require((cpu.cp0().read(m64plus::n64::Cp0::kCause) & ip2) != 0U,
            "simultaneous MI sources assert CPU Cause.IP2");

    require(bus.write32(Bus::kMiInterruptMask, 1U),
            "clear only the MI SP interrupt mask");
    require(bus.miInterruptPending(),
            "pending PI source keeps aggregate MI interrupt asserted");
    require(machine.stepCpu(), "CPU samples remaining PI interrupt source");
    require((cpu.cp0().read(m64plus::n64::Cp0::kCause) & ip2) != 0U,
            "clearing SP mask does not clear IP2 while PI remains enabled and pending");

    require(bus.write32(Bus::kMiInterruptMask, 1U << 8U),
            "clear the remaining MI PI interrupt mask");
    require(!bus.miInterruptPending(),
            "aggregate MI interrupt clears after all pending sources are masked");
    require(machine.stepCpu(), "CPU samples that no enabled MI sources remain");
    require((cpu.cp0().read(m64plus::n64::Cp0::kCause) & ip2) == 0U,
            "CPU Cause.IP2 clears after all enabled MI sources are masked");
    require(!cpu.hasLastException(),
            "aggregated pending-bit test does not take an interrupt exception");
}



void testMachineResetPreservesInsertedCartridgeAndClearsRdram() {
    N64Machine machine;
    std::vector<std::uint8_t> rom(64U, 0U);
    rom[0] = 0x80U;
    rom[1] = 0x37U;
    rom[2] = 0x12U;
    rom[3] = 0x40U;
    rom[4] = 0x12U;
    rom[5] = 0x34U;
    rom[6] = 0x56U;
    rom[7] = 0x78U;

    require(machine.bus().loadCartridge(rom.data(), rom.size()),
            "load cartridge before machine reset");
    require(machine.bus().rdram().write32(0x200U, 0xAABBCCDDU),
            "write RDRAM before machine reset");

    machine.reset();

    std::uint32_t value = 0U;
    require(machine.bus().read32(Bus::kCartridgeBase, value) &&
                value == 0x80371240U,
            "machine reset preserves the inserted cartridge header");
    require(machine.bus().read32(Bus::kCartridgeBase + 4U, value) &&
                value == 0x12345678U,
            "machine reset preserves cartridge contents");
    require(machine.bus().rdram().read32(0x200U, value) && value == 0U,
            "machine reset clears RDRAM contents");
    require(machine.cpu().pc() == 0U,
            "machine reset restores default CPU reset address");
}


void testMachineResetClearsCpuAndMiInterruptState() {
    N64Machine machine;
    constexpr std::uint32_t kMiSpSource = 1U << 0U;
    constexpr std::uint32_t kMiSpMaskSet = 1U << 1U;
    constexpr std::uint64_t kCauseIp2 = 1ULL << 10U;

    require(machine.bus().write32(Bus::kMiInterruptMask, kMiSpMaskSet),
            "enable MI SP interrupt before machine reset");
    machine.bus().setMiInterrupt(0U, true);
    require(machine.cpu().setCp0Register(m64plus::n64::Cp0::kStatus,
                                         1ULL | kCauseIp2),
            "enable CPU global and external interrupt masks");
    require(machine.stepCpu(),
            "CPU accepts pending MI interrupt before reset");
    require((machine.cpu().cp0().read(m64plus::n64::Cp0::kCause) & kCauseIp2) != 0U,
            "CPU Cause records external interrupt before reset");
    require(machine.cpu().cp0().exceptionLevel(),
            "CPU enters exception level for MI interrupt");

    machine.reset();

    require((machine.cpu().cp0().read(m64plus::n64::Cp0::kCause) & kCauseIp2) == 0U,
            "machine reset clears CPU external interrupt pending bit");
    require(!machine.cpu().cp0().exceptionLevel(),
            "machine reset clears CPU exception level");
    std::uint32_t value = 0U;
    require(machine.bus().read32(Bus::kMiInterrupt, value) && value == 0U,
            "machine reset clears MI source register");
    require(machine.bus().read32(Bus::kMiInterruptMask, value) && value == 0U,
            "machine reset clears MI interrupt mask");
    require((kMiSpSource & value) == 0U,
            "machine reset leaves SP interrupt mask disabled");
    require(machine.cpu().pc() == 0U,
            "machine reset restores the default CPU reset PC");
}


void testMachineResetClearsRspAndSpExecutionState() {
    N64Machine machine;
    Bus& bus = machine.bus();

    const std::uint32_t rspProgram[] = {
        0x24011234U, // ADDIU r1,zero,0x1234
        0x0000000DU  // BREAK
    };
    for (std::uint32_t i = 0; i < 2U; ++i) {
        require(bus.write32(Bus::kSpImemBase + 0x20U + i * 4U, rspProgram[i]),
                "load RSP reset-test program into IMEM");
    }
    require(bus.write32(Bus::kSpPc, 0x20U), "set RSP reset-test PC");
    require(bus.write32(Bus::kSpStatus, 1U), "release SP HALT for reset test");
    require(machine.runRsp(4U) == 2U, "execute RSP task before machine reset");
    require(machine.rsp().reg(1U) == 0x1234U,
            "RSP has nonzero scalar state before machine reset");
    require(bus.spBroke() && bus.spHalted(),
            "RSP BREAK leaves SP halted and broke before machine reset");

    machine.reset();

    require(machine.rsp().pc() == 0U && machine.rsp().instructionsExecuted() == 0U,
            "machine reset clears RSP PC and instruction counter");
    require(machine.rsp().reg(1U) == 0U,
            "machine reset clears RSP scalar registers");
    require(!machine.rsp().halted() && !machine.rsp().faulted(),
            "machine reset clears RSP internal halt and fault state");
    require(bus.spProgramCounter() == 0U && bus.spHalted() && !bus.spBroke(),
            "machine reset restores SP PC and halted status");
    std::uint32_t value = 0U;
    require(bus.read32(Bus::kSpDmemBase, value) && value == 0U,
            "machine reset clears SP DMEM");
    require(bus.read32(Bus::kSpImemBase + 0x20U, value) && value == 0U,
            "machine reset clears SP IMEM");
}


void testMachineResetClearsPiAndSpDmaRegisters() {
    N64Machine machine;
    Bus& bus = machine.bus();

    std::vector<std::uint8_t> rom(72U, 0U);
    rom[0] = 0x80U;
    rom[1] = 0x37U;
    rom[2] = 0x12U;
    rom[3] = 0x40U;
    rom[0x40U] = 0x11U;
    rom[0x41U] = 0x22U;
    rom[0x42U] = 0x33U;
    rom[0x43U] = 0x44U;
    require(bus.loadCartridge(rom.data(), rom.size()),
            "load cartridge for PI DMA reset test");

    require(bus.write32(Bus::kPiDramAddr, 0x300U),
            "set PI DMA destination before machine reset");
    require(bus.write32(Bus::kPiCartAddr, Bus::kCartridgeBase + 0x40U),
            "set PI DMA source before machine reset");
    require(bus.write32(Bus::kPiWriteLength, 3U),
            "complete PI DMA before machine reset");

    require(bus.write8(Bus::kSpDmemBase + 0x80U, 0x5AU),
            "seed SP DMEM for SP DMA reset test");
    require(bus.write32(Bus::kSpMemAddr, 0x80U),
            "set SP DMA source before machine reset");
    require(bus.write32(Bus::kSpDramAddr, 0x400U),
            "set SP DMA destination before machine reset");
    require(bus.write32(Bus::kSpWriteLength, 0U),
            "complete SP DMA before machine reset");

    std::uint32_t value = 0U;
    require(bus.read32(Bus::kPiStatus, value) &&
                (value & Bus::kPiStatusInterrupt) != 0U,
            "PI DMA completion interrupt is set before reset");
    require(bus.read32(0x300U, value) && value == 0x11223344U,
            "PI DMA wrote cartridge data before reset");
    require(bus.read32(0x400U, value) && value == 0x5A000000U,
            "SP DMA wrote DMEM data before reset");

    machine.reset();

    require(bus.read32(Bus::kPiStatus, value) && value == 0U,
            "machine reset clears PI status and completion interrupt");
    require(bus.read32(Bus::kPiDramAddr, value) && value == 0U,
            "machine reset clears PI DRAM address register");
    require(bus.read32(Bus::kPiCartAddr, value) && value == Bus::kCartridgeBase,
            "machine reset restores PI cartridge address register");
    require(bus.read32(Bus::kSpMemAddr, value) && value == 0U,
            "machine reset clears SP memory address register");
    require(bus.read32(Bus::kSpDramAddr, value) && value == 0U,
            "machine reset clears SP DRAM address register");
    require(bus.read32(Bus::kSpDmaBusy, value) && value == 0U,
            "machine reset clears SP DMA busy register");
    require(bus.read32(0x300U, value) && value == 0U,
            "machine reset clears RDRAM data written by PI DMA");
    require(bus.read32(Bus::kSpDmemBase + 0x80U, value) && value == 0U,
            "machine reset clears SP DMEM data written before reset");
    require(bus.read32(Bus::kCartridgeBase + 0x40U, value) && value == 0x11223344U,
            "machine reset preserves cartridge contents after DMA reset");
}


void testMachineOwnsAndResetsEventScheduler() {
    N64Machine machine;
    bool fired = false;
    require(machine.eventScheduler().scheduleAt(7U, [&fired] { fired = true; }) != 0U,
            "machine exposes its deterministic event scheduler");
    require(machine.eventScheduler().hasPendingEvents(),
            "machine scheduler tracks a pending device event");
    machine.reset();
    require(machine.eventScheduler().now() == 0U &&
            !machine.eventScheduler().hasPendingEvents(),
            "machine reset clears scheduler time and queued events");
    require(machine.eventScheduler().scheduleAt(0U, [&fired] { fired = true; }) != 0U,
            "machine scheduler accepts a fresh event after reset");
    require(machine.eventScheduler().advanceTo(0U) && fired,
            "machine scheduler executes fresh events after reset");
}

void testEventSchedulerOrdersCancelsAndAdvancesDeterministically() {
    m64plus::n64::EventScheduler scheduler;
    std::vector<int> order;
    const auto later = scheduler.scheduleAt(12U, [&order] { order.push_back(3); });
    require(later != 0U, "schedule event at later deadline");
    require(!scheduler.cancel(999U), "cannot cancel an unknown event identifier");
    require(scheduler.scheduleAt(5U, [&order] { order.push_back(1); }) != 0U,
            "schedule first event at shared deadline");
    require(scheduler.scheduleAt(5U, [&order] { order.push_back(2); }) != 0U,
            "schedule second event at shared deadline");
    require(scheduler.cancel(later), "cancel pending event");
    require(!scheduler.cancel(later), "cancellation is accepted only once");
    m64plus::n64::EventScheduler cancelledOnly;
    const auto cancelledId = cancelledOnly.scheduleAt(99U, [] {});
    require(cancelledOnly.cancel(cancelledId),
            "cancel the only scheduled event");
    require(!cancelledOnly.hasPendingEvents(),
            "cancelled events are excluded from pending event state");
    require(scheduler.advanceTo(5U), "advance scheduler to first deadline");
    require(order.size() == 2U && order[0] == 1 && order[1] == 2,
            "same-deadline events execute once in insertion order");
    require(scheduler.now() == 5U, "scheduler clock reaches requested target");
    require(!scheduler.advanceTo(4U), "scheduler rejects backwards time");
    require(scheduler.now() == 5U, "backwards advance leaves current time unchanged");
    require(scheduler.advanceTo(12U), "advance across cancelled deadline");
    require(order.size() == 2U && !scheduler.hasPendingEvents(),
            "cancelled event is not executed and queue becomes empty");
    require(!scheduler.cancel(later), "cannot cancel an event after its deadline");

    require(scheduler.scheduleAt(13U, [&scheduler, &order] {
        order.push_back(4);
        scheduler.scheduleAt(13U, [&order] { order.push_back(5); });
    }) != 0U, "schedule callback that creates a same-time event");
    require(scheduler.advanceTo(13U), "drain events scheduled by callbacks");
    require(order.size() == 4U && order[2] == 4 && order[3] == 5,
            "new due events are drained deterministically in the same advance");

    scheduler.reset();
    require(scheduler.now() == 0U && !scheduler.hasPendingEvents(),
            "reset clears scheduler time and pending events");
    require(scheduler.scheduleAt(0U, [&order] { order.push_back(6); }) == 1U,
            "reset restarts event identifiers");
    require(scheduler.advanceTo(0U) && order.back() == 6,
            "zero-deadline event executes after reset");
}

} // namespace

int main() {
    testCp0EffectivePrivilegeControls64BitMode();
    testCpuMips3InstructionModeRestrictions();
    testCpuCop0DoublewordTransfers();
    testN64ClockTimebaseUsesExactIntegerRatios();
    testMachineOwnsAndResetsEventScheduler();
    testEventSchedulerOrdersCancelsAndAdvancesDeterministically();
    testMachineResetClearsPiAndSpDmaRegisters();
    testMachineResetClearsRspAndSpExecutionState();
    testMachineResetPreservesInsertedCartridgeAndClearsRdram();
    testMachineResetClearsCpuAndMiInterruptState();
    testTlbPairedPagesAsidGlobalAndPermissions();
    testCp0RandomCyclesWithinWiredBoundary();
    testCpuExceptionAdvancesCp0TimersWithoutRetiringInstruction();
    testCp0CountAndRandomCanAdvanceIndependently();
    testCp0ErrorLevelMasksInterrupts();
    testCp0CountCompareTimerInterrupt();
    testMiExternalInterruptReachesMainCpu();
    testMiExternalInterruptDeassertionClearsCpuPendingBit();
    testMiMaskCommandsDriveCpuExternalPendingBit();
    testMiInterruptSourcesRemainAggregatedWhenOneMaskIsCleared();
    testCpuAcceptsCp0TimerInterruptBetweenInstructions();
    testCpuDefersTimerInterruptUntilAfterBranchDelaySlot();
    testCpuSyscallAndBreakpointExceptions();
    testCpuSyscallAndBreakpointInBranchDelaySlot();
    testCpuSpecialTrapInstructions();
    testCpuRegimmTrapImmediateInstructions();
    testCpuSyncInstructionAndReservedFields();
    testCpuMoveConditionalInstructions();
    testCpuRejectsInvalidBlezBgtzEncodings();
    testCpuRejectsInvalidSpecialShiftAndJumpEncodings();
    testCpuBranchLikelyDelaySlotSemantics();
    testCpuEretClearsLoadLinkedReservation();
    testCpuEretRestoresExceptionReturnAddress();
    testCpuEretReturnsToBranchAfterDelaySlotException();
    testCpuRejectsInvalidCop0ReservedFields();
    testCpuCop1UnusableExceptionSetsCauseCoprocessorNumber();
    testCpuCop1WithCu1EnabledIsStillUnimplemented();
    testCpuCop1InstructionClassesHaveCorrectExceptionBehavior();
    testCpuCop1WordRegisterTransfers();
    testCpuCop0RequiresKernelPrivilege();
    testCpuCop0PrivilegedOperationsRequireKernelPrivilege();
    testCpuUserModeCannotFetchFromKseg0OrKseg1();
    testCpuUserModeCannotLoadOrStoreKseg0OrKseg1();
    testCpuSupervisorModeCannotAccessKseg0OrKseg1();
    testCpuPrivilegeChecksForSsegAndKseg3();
    testCpuSupervisorSsegUsesTlbForFetchLoadAndStore();
    testCpuKseg3LoadStoreUsesTlbTranslation();
    testCpuEretPrioritizesErrorEpcWhenErlAndExlAreSet();
    testCpuEretUsesErrorEpcWhenErlIsSet();
    testCpuEretRestoresUserPrivilegeFromStatus();
    testCpuMtc0StatusChangesPrivilegeForNextInstruction();
    testCpuMtc0WorksInKernelMode();
    testCpuMtc0RequiresKernelPrivilege();
    testCpuNestedTlbExceptionUpdatesFaultAddressRegisters();
    testCpuXContextWritesOnlyPteBaseField();
    testCpuCop0TlbInstructionsWriteProbeAndReadEntry();
    testCpuCop0TlbWriteRandomHonorsWiredBoundary();
    testMachineRdpRunnerHonorsCommandBudget();
    testMachineRunsRdpFillIntoFramebuffer();
    testMachineRunsRdpTextureLoadIntoTmem();
    testMachineRdpRunnerConsumesUnsupportedCommands();
    testMachineOwnsAndProcessesRdpState();
    testCpuKsegDirectMappingWithoutTlb();
    testCpuFetchesAndExecutesFromCartridgeKseg1();
    testCpuProgramsPiDmaAndExecutesCartridgePayload();
    testCpuHiLoMultiplyDivideAndTransfers();
    testCpuDoublewordMultiplyDivide();
    testCpuDoublewordMultiplyDivideEdgeCases();
    testCpuLoadLinkedStoreConditional();
    testCpuLoadLinkedStoreConditionalIgnoresUnrelatedBusWrites();
    testCpuLoadLinkedStoreConditionalDetectsWritesInSameGranule();
    testCpuLoadLinkedStoreConditionalDetectsBusWrites();
    testCpuExceptionInvalidatesLoadLinkedReservation();
    testCpuInterruptInvalidatesLoadLinkedReservation();
    testCpuMemoryFaultInvalidatesLoadLinkedReservation();
    testCpuBusDataFaultInvalidatesLoadLinkedReservation();
    testCpuDataAddressErrorPrecedesTlbFault();
    testCpuJalrUnalignedTargetFaultsAfterDelaySlot();
    testCpuNonCanonicalInstructionAddressRaisesAddressLoad();
    testCpuBusInstructionFaultInvalidatesLoadLinkedReservation();
    testCpuAddressStoreFaultInvalidatesLoadLinkedReservation();
    testCpuBusStoreFaultInvalidatesLoadLinkedReservation();
    testCpuTlbStoreFaultInvalidatesLoadLinkedReservation();
    testCpuTlbInvalidStoreFaultInvalidatesLoadLinkedReservation();
    testCpuTlbInvalidLoadFaultInvalidatesLoadLinkedReservation();
    testCpuTlbLoadMissInvalidatesLoadLinkedReservation();
    testCpuTlbLoadMissInBranchDelaySlotInvalidatesLoadLinkedReservation();
    testCpuTlbFetchMissInvalidatesLoadLinkedReservation();
    testCpuTlbInvalidFetchFaultInvalidatesLoadLinkedReservation();
    testCpuTlbModifiedStoreFaultInvalidatesLoadLinkedReservation();
    testCpuDoublewordLoadLinkedStoreConditional();
    testCpuSignedArithmeticAndImmediateComparisons();
    testCpuUnalignedWordMemoryOperations();
    testCpuUnalignedDoublewordMemoryOperations();
    testCpuUnalignedDoublewordMemoryBoundaryFaults();
    testCpuRunnerHonorsInstructionBudgetAndStopsAtBudget();
    testCpuRunnerStopsWhenInstructionRaisesException();
    testHleBootRejectsInvalidEntryWithoutResettingMachine();
    testHleBootRejectsPayloadBeyondRdramWithoutResettingMachine();
    testHleBootAcceptsPayloadEndingExactlyAtRdramBoundary();
    testHleBootMapsKseg1EntryToPhysicalRdram();
    testCartridgeHleBootInitializesRdramDmemAndCpuState();
    testHleBootDetectsCicAndPalRegionFromRom();
    testMachineDoesNotRetryUnsupportedRspInstruction();
    testSharedCpuRspBusAndSpBreak();
    std::cout << "N64 machine CPU/RSP integration tests passed.\n";
    return 0;
}
