#pragma once

#include <cstdint>

namespace m64plus::n64 {

// Integer representation of the documented N64 clock relationships.
// One master tick represents 1 / 187.5 MHz seconds. This is a timebase,
// not a claim that every CPU instruction consumes one CPU cycle.
using MasterTick = std::uint64_t;

inline constexpr MasterTick kMasterTicksPerSecond = 187'500'000ULL;
inline constexpr MasterTick kCpuTicksPerCycle = 2ULL;
inline constexpr MasterTick kRcpTicksPerCycle = 3ULL;
inline constexpr MasterTick kRspTicksPerCycle = kRcpTicksPerCycle;
inline constexpr MasterTick kCp0CountTicksPerIncrement = 4ULL;

static_assert(kMasterTicksPerSecond % kCpuTicksPerCycle == 0ULL);
static_assert(kMasterTicksPerSecond % kRcpTicksPerCycle == 0ULL);
static_assert(kMasterTicksPerSecond % kCp0CountTicksPerIncrement == 0ULL);
static_assert(kCpuTicksPerCycle * 3ULL == kRcpTicksPerCycle * 2ULL);

} // namespace m64plus::n64
