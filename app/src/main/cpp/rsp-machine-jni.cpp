#include <jni.h>

#include "n64/machine.h"

extern "C" JNIEXPORT jboolean JNICALL
Java_com_jsr_m64plus_MainActivity_nativeRspMachineSelfTest(JNIEnv*, jclass) {
    using m64plus::n64::Bus;
    using m64plus::n64::N64Machine;

    N64Machine machine;
    Bus& bus = machine.bus();
    if (!bus.spHalted() || machine.stepRsp()) return JNI_FALSE;

    const std::uint32_t rspProgram[] = {
        0x24011234U, // ADDIU r1,zero,0x1234
        0xAC010000U, // SW r1,0(zero) -> SP DMEM
        0x0000000DU  // BREAK
    };
    for (std::uint32_t i = 0; i < 3U; ++i) {
        if (!bus.write32(Bus::kSpImemBase + 0x20U + i * 4U, rspProgram[i]))
            return JNI_FALSE;
    }
    if (!bus.write32(Bus::kSpPc, 0x20U) ||
        !bus.write32(Bus::kMiInterruptMask, 1U << 1U) ||
        !bus.write32(Bus::kSpStatus, Bus::kSpInterruptOnBreak | 1U))
        return JNI_FALSE;
    if (machine.runRsp(8U) != 3U || bus.spProgramCounter() != 0x2CU) return JNI_FALSE;
    if (machine.rsp().reg(1) != 0x1234U ||
        !bus.spHalted() || !bus.spBroke() || !bus.miInterruptPending())
        return JNI_FALSE;

    if (!bus.write32(0x00000000U, 0x3C080400U) ||
        !bus.write32(0x00000004U, 0x8D090000U))
        return JNI_FALSE;
    if (!machine.stepCpu() || !machine.stepCpu()) return JNI_FALSE;
    if (machine.cpu().reg(9) != 0x1234U) return JNI_FALSE;

    const std::uint32_t startRsp[] = {
        0x3C080408U, 0x24090040U, 0xAD090000U,
        0x3C0A0404U, 0x240B0005U, 0xAD4B0010U
    };
    for (std::uint32_t i = 0; i < 6U; ++i)
        if (!bus.write32(8U + i * 4U, startRsp[i])) return JNI_FALSE;
    for (unsigned i = 0; i < 6U; ++i)
        if (!machine.stepCpu()) return JNI_FALSE;
    if (bus.spProgramCounter() != 0x40U || bus.spHalted()) return JNI_FALSE;

    const std::uint32_t vectorTask[] = {
        0x24017530U, 0x48810800U, 0x24012710U,
        0x48811000U, 0x4A0208D0U, 0x0000000DU
    };
    for (std::uint32_t i = 0; i < 6U; ++i)
        if (!bus.write32(Bus::kSpImemBase + 0x40U + i * 4U, vectorTask[i]))
            return JNI_FALSE;
    if (machine.runRsp(8U) != 6U ||
        machine.rsp().vectorLane(3, 0) != 32767U ||
        !bus.spHalted() || !bus.spBroke()) return JNI_FALSE;
    return JNI_TRUE;
}
