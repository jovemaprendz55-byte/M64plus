#include "n64/cic.h"

namespace m64plus::n64 {
namespace {
constexpr std::size_t kIpl3Offset = 0x40U;
constexpr std::size_t kIpl3Size = 0xFC0U;

std::uint32_t readLittleEndianWord(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8U) |
           (static_cast<std::uint32_t>(p[2]) << 16U) |
           (static_cast<std::uint32_t>(p[3]) << 24U);
}
} // namespace

CicInfo detectCicFromIpl3(const std::uint8_t* rom, std::size_t size) noexcept {
    CicInfo result{};
    if (rom == nullptr || size < kIpl3Offset + kIpl3Size) return result;

    std::uint64_t sum = 0U;
    for (std::size_t offset = kIpl3Offset; offset < kIpl3Offset + kIpl3Size;
         offset += 4U) {
        sum += readLittleEndianWord(rom + offset);
    }

    switch (sum) {
        case 0x000000A5F80BF620ULL:
            result.type = CicType::Cic5101; result.seed = 0xACU; break;
        case 0x000000D0027FDF31ULL:
        case 0x000000CFFB631223ULL:
            result.type = CicType::X101; result.seed = 0x3FU; break;
        case 0x000000D057C85244ULL:
            result.type = CicType::X102; result.seed = 0x3FU; break;
        case 0x000000D6497E414BULL:
            result.type = CicType::X103; result.seed = 0x78U; result.osCicId = 6103U; break;
        case 0x0000011A49F60E96ULL:
            result.type = CicType::X105; result.seed = 0x91U; result.osCicId = 6105U; break;
        case 0x000000D6D5BE5580ULL:
            result.type = CicType::X106; result.seed = 0x85U; result.osCicId = 6104U; break;
        case 0x000001053BC19870ULL:
            result.type = CicType::Cic5167; result.seed = 0xDDU; break;
        case 0x000000D2E53EF008ULL:
            result.type = CicType::Cic8303; result.seed = 0xDDU; break;
        case 0x000000D2E53EF39FULL:
            result.type = CicType::Cic8401; result.seed = 0xDDU; break;
        case 0x000000D2E53E5DDAULL:
            result.type = CicType::Cic8501; result.seed = 0xDEU; break;
        default:
            return result;
    }
    result.recognized = true;
    return result;
}

TvType detectTvType(std::uint8_t countryCode) noexcept {
    switch (countryCode) {
        case 'P': case 'D': case 'F': case 'I': case 'S':
        case 'U': case 'X': case 'Y': case 'A': case 'L':
            return TvType::Pal;
        case 'B':
            return TvType::Mpal;
        default:
            return TvType::Ntsc;
    }
}

} // namespace m64plus::n64
