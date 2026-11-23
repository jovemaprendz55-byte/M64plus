#pragma once

#include <cstddef>
#include <cstdint>

namespace m64plus::n64 {

enum class CicType : std::uint8_t {
    Unknown, Cic5101, X101, X102, X103, X105, X106, Cic5167, Cic8303, Cic8401, Cic8501
};

enum class TvType : std::uint8_t { Pal = 0, Ntsc = 1, Mpal = 2 };

struct CicInfo final {
    CicType type = CicType::Unknown;
    std::uint8_t seed = 0x3FU; // documented HLE fallback: CIC-6102
    std::uint32_t osCicId = 0U;
    bool recognized = false;
};

CicInfo detectCicFromIpl3(const std::uint8_t* rom, std::size_t size) noexcept;
TvType detectTvType(std::uint8_t countryCode) noexcept;

} // namespace m64plus::n64
