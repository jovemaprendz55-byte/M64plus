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
    using m64plus::n64::MipsCpu;
    using m64plus::n64::Rdram;
    Rdram memory;
    // LUI t0,0x1234; ORI t0,t0,0x5678; ADDIU t1,zero,-1;
    // DADDU t2,t0,t1; DADDIU t3,t2,0x10; SLL t4,t3,1.
    constexpr std::uint32_t program[] = {
        0x3C081234U, 0x35085678U, 0x2409FFFFU,
        0x0109502DU, 0x654B0010U, 0x000B6040U
    };
    for (std::uint32_t i = 0; i < sizeof(program) / sizeof(program[0]); ++i) {
        if (!memory.write32(i * 4U, program[i])) return JNI_FALSE;
    }
    MipsCpu cpu;
    for (unsigned i = 0; i < sizeof(program) / sizeof(program[0]); ++i) {
        if (!cpu.step(memory)) return JNI_FALSE;
    }
    if (cpu.reg(8) != 0x12345678ULL) return JNI_FALSE;
    if (cpu.reg(9) != 0xFFFFFFFFFFFFFFFFULL) return JNI_FALSE;
    if (cpu.reg(10) != 0x12345677ULL) return JNI_FALSE;
    if (cpu.reg(11) != 0x12345687ULL) return JNI_FALSE;
    if (cpu.reg(12) != 0x2468AD0EULL) return JNI_FALSE;
    if (cpu.reg(0) != 0 || cpu.pc() != 24 || cpu.instructionsExecuted() != 6) return JNI_FALSE;
    return JNI_TRUE;
}
