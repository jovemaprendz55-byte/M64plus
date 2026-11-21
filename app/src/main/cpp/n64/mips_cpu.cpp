#include "n64/mips_cpu.h"
#include "n64/rdram.h"
#include <cstdint>
namespace m64plus::n64 {
namespace {
std::uint64_t signExtend32(std::uint32_t value) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(value)));
}
std::uint64_t signExtend16(std::uint16_t value) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int16_t>(value)));
}
}
MipsCpu::MipsCpu() { reset(); }
void MipsCpu::reset(std::uint64_t pc) {
    gpr_.fill(0);
    pc_ = pc;
    executed_ = 0;
}
std::uint64_t MipsCpu::reg(unsigned index) const noexcept {
    return index < gpr_.size() ? (index == 0 ? 0 : gpr_[index]) : 0;
}
std::uint64_t MipsCpu::pc() const noexcept { return pc_; }
std::uint64_t MipsCpu::instructionsExecuted() const noexcept { return executed_; }
bool MipsCpu::step(const Rdram& memory) {
    if ((pc_ & 3U) != 0 || pc_ > 0xFFFFFFFFULL) return false;
    std::uint32_t instruction = 0;
    if (!memory.read32(static_cast<std::uint32_t>(pc_), instruction)) return false;
    const unsigned opcode = instruction >> 26U;
    const unsigned rs = (instruction >> 21U) & 31U;
    const unsigned rt = (instruction >> 16U) & 31U;
    const unsigned rd = (instruction >> 11U) & 31U;
    const unsigned shift = (instruction >> 6U) & 31U;
    const unsigned function = instruction & 63U;
    const std::uint16_t immediate = static_cast<std::uint16_t>(instruction);
    const std::uint64_t a = gpr_[rs];
    const std::uint64_t b = gpr_[rt];
    std::uint64_t result = 0;
    unsigned destination = 0;
    bool writeRegister = false;
    switch (opcode) {
    case 0x00:
        destination = rd;
        switch (function) {
        case 0x00: result = signExtend32(static_cast<std::uint32_t>(b) << shift); break;
        case 0x02: result = signExtend32(static_cast<std::uint32_t>(b) >> shift); break;
        case 0x08: return false;
        case 0x21: result = signExtend32(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b)); break;
        case 0x23: result = signExtend32(static_cast<std::uint32_t>(a) - static_cast<std::uint32_t>(b)); break;
        case 0x24: result = a & b; break;
        case 0x25: result = a | b; break;
        case 0x26: result = a ^ b; break;
        case 0x27: result = ~(a | b); break;
        case 0x2A: result = static_cast<std::int64_t>(a) < static_cast<std::int64_t>(b); break;
        case 0x2B: result = a < b; break;
        case 0x2D: result = a + b; break;
        case 0x2F: result = a - b; break;
        default: return false;
        }
        writeRegister = true;
        break;
    case 0x09:
        destination = rt;
        result = signExtend32(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(signExtend16(immediate)));
        writeRegister = true;
        break;
    case 0x19:
        destination = rt;
        result = a + signExtend16(immediate);
        writeRegister = true;
        break;
    case 0x0C: destination = rt; result = a & immediate; writeRegister = true; break;
    case 0x0D: destination = rt; result = a | immediate; writeRegister = true; break;
    case 0x0E: destination = rt; result = a ^ immediate; writeRegister = true; break;
    case 0x0F: destination = rt; result = signExtend32(static_cast<std::uint32_t>(immediate) << 16U); writeRegister = true; break;
    default: return false;
    }
    if (writeRegister && destination != 0) gpr_[destination] = result;
    gpr_[0] = 0;
    pc_ += 4U;
    ++executed_;
    return true;
}
}
