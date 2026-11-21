#include <jni.h>
#include "n64/mips_cpu.h"
#include "n64/rdram.h"
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
    // Misaligned word access and out-of-RDRAM access must fail safely.
    Rdram badMemory;
    if(!badMemory.write32(0,0x8D080001U)) return JNI_FALSE; // LW t0,1(zero)
    MipsCpu badCpu;
    if(badCpu.step(badMemory)) return JNI_FALSE;
    return JNI_TRUE;
}
