#pragma once
#include <array>
#include <cstdint>

namespace m64plus::n64 {

enum class ExceptionCode : std::uint8_t {
    Interrupt = 0, TlbModification = 1, TlbLoad = 2, TlbStore = 3,
    AddressLoad = 4, AddressStore = 5, BusInstruction = 6, BusData = 7,
    Syscall = 8, Breakpoint = 9, ReservedInstruction = 10,
    CoprocessorUnusable = 11, Overflow = 12, Trap = 13, FloatingPoint = 15
};

class Cp0 final {
public:
    static constexpr unsigned kBadVAddr = 8;
    static constexpr unsigned kCount = 9;
    static constexpr unsigned kCompare = 11;
    static constexpr unsigned kStatus = 12;
    static constexpr unsigned kCause = 13;
    static constexpr unsigned kEpc = 14;
    static constexpr std::uint64_t kStatusExl = 1ULL << 1U;
    static constexpr std::uint64_t kStatusBev = 1ULL << 22U;

    Cp0();
    void reset() noexcept;
    std::uint64_t read(unsigned index) const noexcept;
    bool write(unsigned index, std::uint64_t value) noexcept;
    void tick() noexcept;
    std::uint64_t raiseException(ExceptionCode code, std::uint64_t instructionPc,
                                 bool inDelaySlot = false,
                                 std::uint64_t badAddress = 0) noexcept;
    std::uint64_t exceptionReturn() noexcept;
    bool exceptionLevel() const noexcept;
private:
    std::array<std::uint64_t, 32> registers_{};
};

} // namespace m64plus::n64
