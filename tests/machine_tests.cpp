#include "n64/machine.h"
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

void testTlbPairedPagesAsidGlobalAndPermissions() {
    m64plus::n64::Tlb tlb;
    std::uint32_t physical = 0;

    require(tlb.writeEntry(0U, 0x00400000U, 0x12U, 0x100U, 0x101U,
                           true, true, true, true, false),
            "install non-global TLB pair for ASID 0x12");
    require(tlb.translate(0x00400123U, 0x12U, false, physical) ==
                m64plus::n64::TlbResult::Success && physical == 0x00100123U,
            "TLB translates even 4 KiB page with its offset");
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

    // Reset must clear RDP state as well as CPU/RSP and bus state.
    machine.reset();
    require(!machine.rdp().hasColorImage, "machine reset clears RDP image state");
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
    machine.cpu().setRegister(8U, 0xFFFFFFFFFFFFFFFEULL);
    machine.cpu().setRegister(9U, 3U);
    for (unsigned i = 0; i < 3U; ++i)
        require(machine.stepCpu(), "execute signed 64-bit multiply and HI/LO reads");
    require(machine.cpu().reg(12U) == 0xFFFFFFFFFFFFFFFAULL,
            "DMULT stores low 64 bits of signed product in LO");
    require(machine.cpu().reg(13U) == 0xFFFFFFFFFFFFFFFFULL,
            "DMULT stores signed high 64 bits in HI");

    machine.cpu().reset(0xFFFFFFFF8000000CU);
    machine.cpu().setRegister(8U, UINT64_MAX);
    machine.cpu().setRegister(9U, 2U);
    for (unsigned i = 0; i < 3U; ++i)
        require(machine.stepCpu(), "execute unsigned 64-bit multiply and HI/LO reads");
    require(machine.cpu().reg(14U) == 0xFFFFFFFFFFFFFFFEULL,
            "DMULTU stores low 64 bits of unsigned product in LO");
    require(machine.cpu().reg(15U) == 1U,
            "DMULTU stores high 64 bits of unsigned product in HI");

    machine.cpu().reset(0xFFFFFFFF80000018ULL);
    machine.cpu().setRegister(8U, UINT64_MAX - 6U); // -7
    machine.cpu().setRegister(9U, 3U);
    for (unsigned i = 0; i < 3U; ++i)
        require(machine.stepCpu(), "execute signed 64-bit divide and HI/LO reads");
    require(machine.cpu().reg(16U) == UINT64_MAX - 1U,
            "DDIV stores signed quotient -2 in LO");
    require(machine.cpu().reg(17U) == UINT64_MAX,
            "DDIV stores signed remainder -1 in HI");

    machine.cpu().reset(0xFFFFFFFF80000024ULL);
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
        machine.cpu().setRegister(9U, initialRegister);
        require(machine.stepCpu(), "execute LDL at each byte offset");
        require(machine.cpu().reg(9U) == expectedLdl[offset],
                "LDL merges expected big-endian bytes at every offset");

        require(bus.write32(0x1000U, 0x6C090000U | offset), "write LDR instruction");
        machine.cpu().reset(0xFFFFFFFF80001000ULL);
        machine.cpu().setRegister(9U, initialRegister);
        require(machine.stepCpu(), "execute LDR at each byte offset");
        require(machine.cpu().reg(9U) == expectedLdr[offset],
                "LDR merges expected big-endian bytes at every offset");

        require(bus.write64(0U, initialMemory), "reset SDL memory");
        require(bus.write32(0x1000U, 0xB0090000U | offset), "write SDL instruction");
        machine.cpu().reset(0xFFFFFFFF80001000ULL);
        machine.cpu().setRegister(9U, initialRegister);
        require(machine.stepCpu(), "execute SDL at each byte offset");
        require(bus.read64(0U, loaded) && loaded == expectedSdl[offset],
                "SDL preserves untouched bytes at every offset");

        require(bus.write64(0U, initialMemory), "reset SDR memory");
        require(bus.write32(0x1000U, 0xB4090000U | offset), "write SDR instruction");
        machine.cpu().reset(0xFFFFFFFF80001000ULL);
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

} // namespace

int main() {
    testTlbPairedPagesAsidGlobalAndPermissions();
    testCpuCop0TlbInstructionsWriteProbeAndReadEntry();
    testCpuCop0TlbWriteRandomHonorsWiredBoundary();
    testMachineOwnsAndProcessesRdpState();
    testCpuKsegDirectMappingWithoutTlb();
    testCpuFetchesAndExecutesFromCartridgeKseg1();
    testCpuProgramsPiDmaAndExecutesCartridgePayload();
    testCpuHiLoMultiplyDivideAndTransfers();
    testCpuDoublewordMultiplyDivide();
    testCpuDoublewordMultiplyDivideEdgeCases();
    testCpuLoadLinkedStoreConditional();
    testCpuLoadLinkedStoreConditionalDetectsBusWrites();
    testCpuExceptionInvalidatesLoadLinkedReservation();
    testCpuInterruptInvalidatesLoadLinkedReservation();
    testCpuMemoryFaultInvalidatesLoadLinkedReservation();
    testCpuBusDataFaultInvalidatesLoadLinkedReservation();
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
    testCartridgeHleBootInitializesRdramDmemAndCpuState();
    testHleBootDetectsCicAndPalRegionFromRom();
    testMachineDoesNotRetryUnsupportedRspInstruction();
    testSharedCpuRspBusAndSpBreak();
    std::cout << "N64 machine CPU/RSP integration tests passed.\n";
    return 0;
}
