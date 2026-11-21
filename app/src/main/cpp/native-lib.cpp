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
    cp0.exceptionReturn();
    if (cp0.raiseException(ExceptionCode::TlbLoad, 0x00401234ULL, false,
                           0x00401234ULL, true) != 0xFFFFFFFFBFC00200ULL)
        return JNI_FALSE;
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

extern "C" JNIEXPORT jboolean JNICALL
Java_com_jsr_m64plus_MainActivity_nativeCpuTlbInstructionSelfTest(JNIEnv*, jclass) {
    using m64plus::n64::MipsCpu; using m64plus::n64::Rdram; using m64plus::n64::Cp0;
    Rdram memory;
    const std::uint32_t program[] = {
        0x3C090040U, 0x35290007U, 0x40895000U,
        0x240A0487U, 0x408A1000U, 0x240B0D03U, 0x408B1800U,
        0x240C0003U, 0x408C0000U, 0x42000002U,
        0x3C0D0060U, 0x35AD0009U, 0x408D5000U, 0x42000008U,
        0x3C0D0040U, 0x35AD0007U, 0x408D5000U, 0x42000008U,
        0x42000001U, 0x400E5000U, 0x400F1000U
    };
    for (std::uint32_t i=0;i<sizeof(program)/sizeof(program[0]);++i)
        if (!memory.write32(i*4U, program[i])) return JNI_FALSE;
    MipsCpu cpu;
    for (unsigned i=0;i<sizeof(program)/sizeof(program[0]);++i)
        if (!cpu.step(memory)) return JNI_FALSE;
    if (cpu.reg(14) != 0x00400007ULL || cpu.reg(15) != 0x00000487ULL ||
        cpu.cp0().read(Cp0::kIndex) != 3ULL) return JNI_FALSE;

    Rdram randomMemory;
    const std::uint32_t randomProgram[] = {
        0x2408001FU, 0x40883000U, 0x24090002U,
        0x3C0A0080U, 0x354A0002U, 0x408A5000U,
        0x240B0003U, 0x408B1000U, 0x408B1800U,
        0x42000006U, 0x42000008U, 0x400C0000U
    };
    for (std::uint32_t i=0;i<sizeof(randomProgram)/sizeof(randomProgram[0]);++i)
        if (!randomMemory.write32(i*4U, randomProgram[i])) return JNI_FALSE;
    MipsCpu randomCpu;
    for (unsigned i=0;i<sizeof(randomProgram)/sizeof(randomProgram[0]);++i)
        if (!randomCpu.step(randomMemory)) return JNI_FALSE;
    if (randomCpu.reg(12) != 31ULL || randomCpu.cp0().read(Cp0::kIndex) != 31ULL)
        return JNI_FALSE;
    return JNI_TRUE;
}


extern "C" JNIEXPORT jboolean JNICALL
Java_com_jsr_m64plus_MainActivity_nativeCpuTlbTranslationSelfTest(JNIEnv*, jclass) {
    using m64plus::n64::MipsCpu;
    using m64plus::n64::Rdram;
    using m64plus::n64::Cp0;
    Rdram memory;

    // Bootstrap through physical RDRAM, install an 8 KiB TLB pair, then
    // jump into the mapped virtual page before enabling address translation.
    const std::uint32_t bootstrap[] = {
        0x3C080040U, // LUI t0,0x0040 -> EntryHi VPN2 0x00400000, ASID 0
        0x40885000U, // MTC0 t0,EntryHi
        0x24090087U, // ADDIU t1,zero, PFN=2, G/V/D = 1
        0x40891000U, // MTC0 t1,EntryLo0
        0x240900C7U, // ADDIU t1,zero, PFN=3, G/V/D = 1
        0x40891800U, // MTC0 t1,EntryLo1
        0x240A0000U, // ADDIU t2,zero,0
        0x40820000U, // MTC0 t2,Index
        0x42000002U, // TLBWI
        0x08100010U, // J 0x00400040
        0x00000000U  // delay slot
    };
    for (std::uint32_t i = 0; i < sizeof(bootstrap) / sizeof(bootstrap[0]); ++i)
        if (!memory.write32(i * 4U, bootstrap[i])) return JNI_FALSE;

    const std::uint32_t virtualProgram[] = {
        0x24081234U, // ADDIU t0,zero,0x1234
        0x3C090040U, // LUI t1,0x0040
        0x35290080U, // ORI t1,t1,0x0080
        0xAD280000U, // SW t0,0(t1) through TLB
        0x8D2A0000U  // LW t2,0(t1) through TLB
    };
    for (std::uint32_t i = 0; i < sizeof(virtualProgram) / sizeof(virtualProgram[0]); ++i)
        if (!memory.write32(0x2040U + i * 4U, virtualProgram[i])) return JNI_FALSE;

    MipsCpu cpu;
    for (unsigned i = 0; i < sizeof(bootstrap) / sizeof(bootstrap[0]); ++i)
        if (!cpu.step(memory)) return JNI_FALSE;
    if (cpu.pc() != 0x00400040ULL) return JNI_FALSE;
    cpu.setTlbEnabled(true);
    for (unsigned i = 0; i < sizeof(virtualProgram) / sizeof(virtualProgram[0]); ++i)
        if (!cpu.step(memory)) return JNI_FALSE;

    std::uint32_t stored = 0;
    if (!memory.read32(0x2080U, stored) || stored != 0x1234U) return JNI_FALSE;
    if (cpu.reg(10) != 0x1234ULL || cpu.pc() != 0x00400054ULL) return JNI_FALSE;

    // An unmapped virtual instruction fetch must enter the TLB-load vector.
    MipsCpu missCpu;
    missCpu.reset(0x00500000ULL);
    missCpu.setTlbEnabled(true);
    if (!missCpu.step(memory)) return JNI_FALSE;
    if (missCpu.pc() != 0xFFFFFFFF80000000ULL) return JNI_FALSE;
    if (((missCpu.cp0().read(Cp0::kCause) >> 2U) & 0x1FULL) != 2U) return JNI_FALSE;
    if (missCpu.cp0().read(Cp0::kBadVAddr) != 0x00500000ULL) return JNI_FALSE;
    if (missCpu.cp0().read(Cp0::kEntryHi) != 0x00500000ULL) return JNI_FALSE;
    if ((missCpu.cp0().read(Cp0::kContext) & 0x7FFFF0ULL) !=
        ((0x00500000ULL >> 9U) & 0x7FFFF0ULL)) return JNI_FALSE;
    return JNI_TRUE;
}


extern "C" JNIEXPORT jboolean JNICALL
Java_com_jsr_m64plus_MainActivity_nativeCpuTlbProtectionSelfTest(JNIEnv*, jclass) {
    using m64plus::n64::MipsCpu;
    using m64plus::n64::Rdram;
    using m64plus::n64::Cp0;

    const auto runFault = [](bool store, std::uint32_t evenEntryLo,
                             std::uint32_t expectedException) -> bool {
        Rdram memory;
        const std::uint32_t bootstrap[] = {
            0x3C080040U, // LUI t0,0x0040: EntryHi VPN2
            0x40885000U, // MTC0 t0,EntryHi
            evenEntryLo, // ADDIU t1,zero,EntryLo0 value
            0x40891000U, // MTC0 t1,EntryLo0
            0x240900C7U, // ADDIU t1,zero, valid/dirty/global odd page
            0x40891800U, // MTC0 t1,EntryLo1
            0x240A0000U, // ADDIU t2,zero,0
            0x40820000U, // MTC0 t2,Index
            0x42000002U, // TLBWI
            0x3C0B8000U, // LUI t3,0x8000
            0x256B0040U, // ADDIU t3,t3,0x40
            0x01600008U, // JR t3 -> sign-extended KSEG0
            0x00000000U  // delay slot
        };
        for (std::uint32_t i = 0; i < sizeof(bootstrap) / sizeof(bootstrap[0]); ++i)
            if (!memory.write32(i * 4U, bootstrap[i])) return false;

        const std::uint32_t code[] = {
            0x3C090040U,                         // LUI t1,0x0040
            store ? 0x240A0055U : 0x8D2A0000U,   // ADDIU t2,zero,0x55 / LW t2,0(t1)
            0xAD2A0000U                          // SW t2,0(t1)
        };
        // For the invalid-page case, the second instruction faults on LW.
        // For the modified-page case, ADDIU completes and SW faults.
        if (!memory.write32(0x40U, code[0]) ||
            !memory.write32(0x44U, code[1]) ||
            !memory.write32(0x48U, code[2])) return false;

        MipsCpu cpu;
        for (unsigned i = 0; i < sizeof(bootstrap) / sizeof(bootstrap[0]); ++i)
            if (!cpu.step(memory)) return false;
        if (cpu.pc() != 0xFFFFFFFF80000040ULL) return false;
        cpu.setTlbEnabled(true);

        const unsigned maxSteps = store ? 3U : 2U;
        for (unsigned i = 0; i < maxSteps; ++i)
            if (!cpu.step(memory)) return false;
        const std::uint64_t cause = cpu.cp0().read(Cp0::kCause);
        return cpu.pc() == 0xFFFFFFFF80000180ULL &&
               ((cause >> 2U) & 0x1FULL) == expectedException &&
               cpu.cp0().read(Cp0::kBadVAddr) == 0x00400000ULL &&
               cpu.cp0().exceptionLevel();
    };

    // V=0 on the even page must raise TLBL (ExcCode 2).
    if (!runFault(false, 0x00000085U, 2U)) return JNI_FALSE;
    // V=1,D=0 on the even page must raise TLB Modification (ExcCode 1).
    if (!runFault(true, 0x00000083U, 1U)) return JNI_FALSE;
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_jsr_m64plus_MainActivity_nativeBusSelfTest(JNIEnv*, jclass) {
    using m64plus::n64::Bus;
    using m64plus::n64::BusRegion;
    Bus bus;
    if (bus.region(0x00001000U) != BusRegion::Rdram ||
        bus.region(0x04000000U) != BusRegion::SpDmem ||
        bus.region(0x04001000U) != BusRegion::SpImem ||
        bus.region(0x04002000U) != BusRegion::Unmapped)
        return JNI_FALSE;

    constexpr std::uint32_t rdramAddress = 0x00000100U;
    constexpr std::uint64_t pattern = 0x1122334455667788ULL;
    std::uint64_t actual = 0;
    if (!bus.write64(rdramAddress, pattern) ||
        !bus.read64(rdramAddress, actual) || actual != pattern)
        return JNI_FALSE;

    constexpr std::uint32_t dmemAddress = Bus::kSpDmemBase + 0x20U;
    constexpr std::uint32_t imemAddress = Bus::kSpImemBase + 0x20U;
    if (!bus.write32(dmemAddress, 0xA1B2C3D4U) ||
        !bus.write32(imemAddress, 0x10203040U))
        return JNI_FALSE;
    std::uint32_t dmem = 0;
    std::uint32_t imem = 0;
    if (!bus.read32(dmemAddress, dmem) || dmem != 0xA1B2C3D4U ||
        !bus.read32(imemAddress, imem) || imem != 0x10203040U)
        return JNI_FALSE;

    // Adjacent SP memories must remain independent.
    if (!bus.read32(dmemAddress, dmem) || dmem == imem) return JNI_FALSE;
    // Accesses crossing a region boundary and unmapped MMIO must fail safely.
    if (bus.read32(Bus::kSpDmemBase + 0x0FFFU, dmem) ||
        bus.write32(0x04300000U, 0xFFFFFFFFU))
        return JNI_FALSE;
    bus.reset();
    if (!bus.read64(rdramAddress, actual) || actual != 0 ||
        !bus.read32(dmemAddress, dmem) || dmem != 0)
        return JNI_FALSE;
    return JNI_TRUE;
}
