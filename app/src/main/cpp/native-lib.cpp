#include <jni.h>
#include "n64/mips_cpu.h"
#include "n64/cp0.h"
#include "n64/rdram.h"
#include "n64/tlb.h"
extern "C" JNIEXPORT jboolean JNICALL
Java_com_jsr_m64plus_MainActivity_nativeRdramSelfTest(JNIEnv*, jclass) {
    m64plus::n64::Rdram memory;
    if (memory.size() != m64plus::n64::Rdram::kSize) return JNI_FALSE;
    constexpr std::uint32_t address = 0x100;
    constexpr std::uint32_t expected = 0x80371240;
    std::uint32_t actual = 0;
    if (!memory.write32(address, expected) || !memory.read32(address, actual)) return JNI_FALSE;
    if (actual != expected) return JNI_FALSE;
    std::uint8_t first = 0;
    if (!memory.read8(address, first) || first != 0x80) return JNI_FALSE;
    if (memory.write32(static_cast<std::uint32_t>(memory.size() - 2U), expected)) return JNI_FALSE;
    return JNI_TRUE;
}
extern "C" JNIEXPORT jboolean JNICALL
Java_com_jsr_m64plus_MainActivity_nativeCpuSelfTest(JNIEnv*, jclass) {
    using m64plus::n64::MipsCpu; using m64plus::n64::Rdram;
    Rdram memory;
    constexpr std::uint32_t program[] = {0x3C081234U,0x35085678U,0x2409FFFFU,0x0109502DU,0x654B0010U,0x000B6040U};
    for (std::uint32_t i=0;i<6;++i) if(!memory.write32(i*4U,program[i])) return JNI_FALSE;
    MipsCpu cpu;
    for(unsigned i=0;i<6;++i) if(!cpu.step(memory)) return JNI_FALSE;
    if(cpu.reg(8)!=0x12345678ULL || cpu.reg(9)!=0xFFFFFFFFFFFFFFFFULL ||
       cpu.reg(10)!=0x12345677ULL || cpu.reg(11)!=0x12345687ULL ||
       cpu.reg(12)!=0x2468AD0EULL || cpu.reg(0)!=0 || cpu.pc()!=24 ||
       cpu.instructionsExecuted()!=6) return JNI_FALSE;
    return JNI_TRUE;
}
extern "C" JNIEXPORT jboolean JNICALL
Java_com_jsr_m64plus_MainActivity_nativeCpuBranchSelfTest(JNIEnv*, jclass) {
    using m64plus::n64::MipsCpu; using m64plus::n64::Rdram;
    Rdram memory;
    // BEQ taken: execute the delay slot, skip address 8, continue at 12.
    const std::uint32_t branchProgram[] = {0x24080001U,0x11080002U,0x24090007U,0x24090009U,0x240A000BU};
    for(std::uint32_t i=0;i<5;++i) if(!memory.write32(i*4U,branchProgram[i])) return JNI_FALSE;
    MipsCpu branchCpu;
    for(unsigned i=0;i<4;++i) if(!branchCpu.step(memory)) return JNI_FALSE;
    if(branchCpu.pc()!=12 || branchCpu.reg(9)!=7 || branchCpu.reg(10)!=0) return JNI_FALSE;
    if(!branchCpu.step(memory) || branchCpu.reg(10)!=11) return JNI_FALSE;
    // JAL to 16, execute delay slot, JR ra back to 8, execute JR delay slot.
    Rdram jumpMemory;
    const std::uint32_t jumpProgram[] = {0x0C000004U,0x24080001U,0x24080002U,0x00000000U,
                                         0x03E00008U,0x24090003U};
    for(std::uint32_t i=0;i<6;++i) if(!jumpMemory.write32(i*4U,jumpProgram[i])) return JNI_FALSE;
    MipsCpu jumpCpu;
    for(unsigned i=0;i<4;++i) if(!jumpCpu.step(jumpMemory)) return JNI_FALSE;
    if(jumpCpu.pc()!=8 || jumpCpu.reg(31)!=8 || jumpCpu.reg(8)!=1 || jumpCpu.reg(9)!=3) return JNI_FALSE;
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_jsr_m64plus_MainActivity_nativeCpuMemorySelfTest(JNIEnv*, jclass) {
    using m64plus::n64::MipsCpu; using m64plus::n64::Rdram;
    Rdram memory;
    // t0=0x100; t1=0x81234567; SW; signed/unsigned byte and word loads.
    const std::uint32_t program[] = {
        0x24080100U, 0x3C098123U, 0x35294567U, 0xAD090000U,
        0x810A0000U, 0x910B0000U, 0x8D0C0000U, 0x9D0D0000U,
        0x240E1234U, 0xA50E0004U, 0x950F0004U,
        0xAC090008U, 0x8C100008U
    };
    for (std::uint32_t i=0;i<sizeof(program)/sizeof(program[0]);++i)
        if(!memory.write32(i*4U,program[i])) return JNI_FALSE;
    MipsCpu cpu;
    for(unsigned i=0;i<sizeof(program)/sizeof(program[0]);++i)
        if(!cpu.step(memory)) return JNI_FALSE;
    if(cpu.reg(10)!=0xFFFFFFFFFFFFFF81ULL) return JNI_FALSE;
    if(cpu.reg(11)!=0x81ULL) return JNI_FALSE;
    if(cpu.reg(12)!=0xFFFFFFFF81234567ULL) return JNI_FALSE;
    if(cpu.reg(13)!=0x81234567ULL) return JNI_FALSE;
    if(cpu.reg(15)!=0x1234ULL) return JNI_FALSE;
    if(cpu.reg(16)!=0xFFFFFFFF81234567ULL) return JNI_FALSE;
    // Misaligned word access must fail safely.
    Rdram badMemory;
    if(!badMemory.write32(0,0x8D080001U)) return JNI_FALSE; // LW t0,1(zero)
    MipsCpu badCpu;
    if(!badCpu.step(badMemory)) return JNI_FALSE;
    if(badCpu.pc()!=0xFFFFFFFF80000180ULL) return JNI_FALSE;
    if(((badCpu.cp0().read(m64plus::n64::Cp0::kCause)>>2U)&0x1FULL)!=4U) return JNI_FALSE;
    if(badCpu.cp0().read(m64plus::n64::Cp0::kBadVAddr)!=1U) return JNI_FALSE;

    // Exercise 64-bit LD/SD and byte/halfword stores with big-endian data.
    Rdram wideMemory;
    constexpr std::uint64_t wideValue = 0x1122334455667788ULL;
    if(!wideMemory.write64(0x200, wideValue)) return JNI_FALSE;
    const std::uint32_t wideProgram[] = {
        0x24080200U, // ADDIU t0,zero,0x200
        0xDD090000U, // LD t1,0(t0)
        0xFD090008U, // SD t1,8(t0)
        0xDD0A0008U, // LD t2,8(t0)
        0x3C0BAABBU, // LUI t3,0xAABB
        0x356BCCDDU, // ORI t3,t3,0xCCDD
        0xA10B0010U, // SB t3,16(t0)
        0xA50B0012U, // SH t3,18(t0)
        0x910C0010U, // LBU t4,16(t0)
        0x950D0012U  // LHU t5,18(t0)
    };
    for(std::uint32_t i=0;i<sizeof(wideProgram)/sizeof(wideProgram[0]);++i)
        if(!wideMemory.write32(i*4U,wideProgram[i])) return JNI_FALSE;
    MipsCpu wideCpu;
    for(unsigned i=0;i<sizeof(wideProgram)/sizeof(wideProgram[0]);++i)
        if(!wideCpu.step(wideMemory)) return JNI_FALSE;
    if(wideCpu.reg(9)!=wideValue || wideCpu.reg(10)!=wideValue) return JNI_FALSE;
    if(wideCpu.reg(12)!=0xDDULL || wideCpu.reg(13)!=0xCCDDULL) return JNI_FALSE;
    std::uint64_t copiedValue = 0;
    if(!wideMemory.read64(0x208,copiedValue) || copiedValue!=wideValue) return JNI_FALSE;
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_jsr_m64plus_MainActivity_nativeCp0SelfTest(JNIEnv*, jclass) {
    using m64plus::n64::Cp0;
    using m64plus::n64::ExceptionCode;
    Cp0 cp0;
    if (cp0.read(Cp0::kStatus) != 0 || cp0.exceptionLevel()) return JNI_FALSE;
    // Address error in a branch delay slot: EPC points to the branch.
    const std::uint64_t vector = cp0.raiseException(ExceptionCode::AddressLoad,
                                                     0xFFFFFFFF80001004ULL, true,
                                                     0xFFFFFFFFA0000001ULL);
    if (vector != 0xFFFFFFFF80000180ULL) return JNI_FALSE;
    if (cp0.read(Cp0::kEpc) != 0xFFFFFFFF80001000ULL) return JNI_FALSE;
    if (cp0.read(Cp0::kBadVAddr) != 0xFFFFFFFFA0000001ULL) return JNI_FALSE;
    if ((cp0.read(Cp0::kCause) & (1ULL << 31U)) == 0) return JNI_FALSE;
    if (((cp0.read(Cp0::kCause) >> 2U) & 0x1FULL) != 4U) return JNI_FALSE;
    if (!cp0.exceptionLevel()) return JNI_FALSE;
    // Nested exception must preserve the original EPC.
    cp0.raiseException(ExceptionCode::Breakpoint, 0xFFFFFFFF80002000ULL);
    if (cp0.read(Cp0::kEpc) != 0xFFFFFFFF80001000ULL) return JNI_FALSE;
    if (cp0.exceptionReturn() != 0xFFFFFFFF80001000ULL || cp0.exceptionLevel()) return JNI_FALSE;
    // Count/Compare timer interrupt; writing Compare acknowledges it.
    if (!cp0.write(Cp0::kCount, 4) || !cp0.write(Cp0::kCompare, 6)) return JNI_FALSE;
    cp0.tick();
    cp0.tick();
    if ((cp0.read(Cp0::kCause) & (1ULL << 15U)) == 0) return JNI_FALSE;
    if (!cp0.write(Cp0::kCompare, 10)) return JNI_FALSE;
    if ((cp0.read(Cp0::kCause) & (1ULL << 15U)) != 0) return JNI_FALSE;
    // BEV selects the boot exception vector.
    if (!cp0.write(Cp0::kStatus, Cp0::kStatusBev)) return JNI_FALSE;
    if (cp0.raiseException(ExceptionCode::Syscall, 0xFFFFFFFF80003000ULL) !=
        0xFFFFFFFFBFC00380ULL) return JNI_FALSE;
    // TLB-related CP0 register masks and read-only Random behavior.
    if (!cp0.write(Cp0::kIndex, 0xFFFFFFFFULL) || cp0.read(Cp0::kIndex) != 0x8000001FULL)
        return JNI_FALSE;
    if (!cp0.write(Cp0::kEntryLo0, 0xFFFFFFFFULL) ||
        cp0.read(Cp0::kEntryLo0) != 0x03FFFFFFULL) return JNI_FALSE;
    if (!cp0.write(Cp0::kEntryHi, 0xFFFFFFFFULL) ||
        cp0.read(Cp0::kEntryHi) != 0xFFFFE0FFULL) return JNI_FALSE;
    if (!cp0.write(Cp0::kWired, 4) || cp0.read(Cp0::kWired) != 4 ||
        cp0.read(Cp0::kRandom) != 31) return JNI_FALSE;
    if (cp0.write(Cp0::kRandom, 2)) return JNI_FALSE;
    if (!cp0.write(Cp0::kPageMask, 0x01FFE000ULL) || cp0.read(Cp0::kPageMask) != 0)
        return JNI_FALSE;
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_jsr_m64plus_MainActivity_nativeCpuExceptionIntegrationSelfTest(JNIEnv*, jclass) {
    using m64plus::n64::MipsCpu;
    using m64plus::n64::Rdram;
    using m64plus::n64::Cp0;
    Rdram memory;
    // Reserved opcode is delivered to the general exception vector.
    if (!memory.write32(0, 0xFC000000U)) return JNI_FALSE;
    MipsCpu cpu;
    if (!cpu.step(memory)) return JNI_FALSE;
    if (cpu.pc() != 0xFFFFFFFF80000180ULL || !cpu.cp0().exceptionLevel()) return JNI_FALSE;
    if (((cpu.cp0().read(Cp0::kCause) >> 2U) & 0x1FULL) != 10U) return JNI_FALSE;
    if (cpu.cp0().read(Cp0::kEpc) != 0U) return JNI_FALSE;

    // Misaligned fetch raises AdEL and records the bad virtual address.
    MipsCpu fetchCpu;
    fetchCpu.reset(2);
    if (!fetchCpu.step(memory)) return JNI_FALSE;
    if (fetchCpu.pc() != 0xFFFFFFFF80000180ULL) return JNI_FALSE;
    if (fetchCpu.cp0().read(Cp0::kBadVAddr) != 2U) return JNI_FALSE;

    // Fault in a branch delay slot records the branch PC and BD flag.
    Rdram delayMemory;
    const std::uint32_t delayProgram[] = {0x10000001U, 0xFC000000U};
    if (!delayMemory.write32(0, delayProgram[0]) || !delayMemory.write32(4, delayProgram[1]))
        return JNI_FALSE;
    MipsCpu delayCpu;
    if (!delayCpu.step(delayMemory) || !delayCpu.step(delayMemory)) return JNI_FALSE;
    if (delayCpu.cp0().read(Cp0::kEpc) != 0U) return JNI_FALSE;
    if ((delayCpu.cp0().read(Cp0::kCause) & (1ULL << 31U)) == 0) return JNI_FALSE;
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_jsr_m64plus_MainActivity_nativeCpuCp0InstructionSelfTest(JNIEnv*, jclass) {
    using m64plus::n64::MipsCpu;
    using m64plus::n64::Rdram;
    using m64plus::n64::Cp0;
    Rdram memory;
    // Set EPC=0x20, read Count, then set EXL and execute ERET.
    const std::uint32_t program[] = {
        0x24090020U, // ADDIU t1,zero,0x20
        0x40897000U, // MTC0 t1,EPC
        0x40084800U, // MFC0 t0,Count
        0x240A0002U, // ADDIU t2,zero,2 (EXL)
        0x408A6000U, // MTC0 t2,Status
        0x42000018U  // ERET
    };
    for (std::uint32_t i=0;i<sizeof(program)/sizeof(program[0]);++i)
        if (!memory.write32(i*4U, program[i])) return JNI_FALSE;
    MipsCpu cpu;
    for (unsigned i=0;i<6;++i) if (!cpu.step(memory)) return JNI_FALSE;
    if (cpu.reg(8) != 2U) return JNI_FALSE; // Count advanced once per completed instruction
    if (cpu.pc() != 0x20U || cpu.cp0().exceptionLevel()) return JNI_FALSE;
    if (cpu.cp0().read(Cp0::kEpc) != 0x20U) return JNI_FALSE;
    // ERET in a delay slot is architecturally reserved.
    Rdram delayMemory;
    if (!delayMemory.write32(0,0x10000001U) || !delayMemory.write32(4,0x42000018U))
        return JNI_FALSE;
    MipsCpu delayCpu;
    if (!delayCpu.step(delayMemory) || !delayCpu.step(delayMemory)) return JNI_FALSE;
    if (((delayCpu.cp0().read(Cp0::kCause)>>2U)&0x1FULL) != 10U) return JNI_FALSE;
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_jsr_m64plus_MainActivity_nativeTlbSelfTest(JNIEnv*, jclass) {
    using m64plus::n64::Tlb;
    using m64plus::n64::TlbResult;
    Tlb tlb;
    if (tlb.size() != 32U) return JNI_FALSE;
    std::uint32_t physical = 0;
    // Unmapped kernel segments bypass the TLB.
    if (tlb.translate(0x80001234ULL, 0, false, physical) != TlbResult::Success ||
        physical != 0x00001234U) return JNI_FALSE;
    if (tlb.translate(0xA0005678ULL, 0, true, physical) != TlbResult::Success ||
        physical != 0x00005678U) return JNI_FALSE;
    // Two 4 KiB pages form one 8 KiB TLB pair.
    if (!tlb.writeEntry(3, 0x00400000U, 7, 0x00012U, 0x00034U,
                        true, true, true, false, false)) return JNI_FALSE;
    if (tlb.translate(0x00400123ULL, 7, false, physical) != TlbResult::Success ||
        physical != 0x00012123U) return JNI_FALSE;
    if (tlb.translate(0x00401123ULL, 7, false, physical) != TlbResult::Success ||
        physical != 0x00034123U) return JNI_FALSE;
    if (tlb.translate(0x00400123ULL, 8, false, physical) != TlbResult::Miss)
        return JNI_FALSE;
    if (tlb.translate(0x00401123ULL, 7, true, physical) != TlbResult::Modified)
        return JNI_FALSE;
    if (!tlb.writeEntry(4, 0x00600000U, 9, 0x00056U, 0x00078U,
                        true, false, true, false, false)) return JNI_FALSE;
    if (tlb.translate(0x00601000ULL, 9, false, physical) != TlbResult::Invalid)
        return JNI_FALSE;
    // Global entries ignore ASID.
    if (!tlb.writeEntry(5, 0x00800000U, 1, 0x0009AU, 0x0009BU,
                        true, true, true, true, true)) return JNI_FALSE;
    if (tlb.translate(0x00800200ULL, 99, false, physical) != TlbResult::Success ||
        physical != 0x0009A200U) return JNI_FALSE;
    // TLBP matches VPN2 and ASID/global, independent of V/D bits.
    if (tlb.probe(0x00401123ULL, 7) != 3 ||
        tlb.probe(0x00400123ULL, 8) != -1 ||
        tlb.probe(0x00800200ULL, 99) != 5) return JNI_FALSE;
    // TLBR-style readback returns the exact EntryLo state.
    m64plus::n64::TlbEntryData entry{};
    if (!tlb.readEntry(3, entry) || entry.virtualPairBase != 0x00400000U ||
        entry.asid != 7 || entry.evenPfn != 0x12U || entry.oddPfn != 0x34U ||
        !entry.evenValid || !entry.oddValid || !entry.evenDirty || entry.oddDirty ||
        entry.global) return JNI_FALSE;
    if (tlb.readEntry(31, entry)) return JNI_FALSE;
    tlb.reset();
    if (tlb.translate(0x00400123ULL, 7, false, physical) != TlbResult::Miss)
        return JNI_FALSE;
    return JNI_TRUE;
}
