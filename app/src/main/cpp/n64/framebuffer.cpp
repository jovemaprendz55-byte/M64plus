#include "n64/framebuffer.h"

#include <limits>

namespace m64plus::n64 {
namespace {

std::uint8_t expand5(std::uint16_t value) noexcept {
    const auto component = static_cast<std::uint8_t>(value & 0x1FU);
    return static_cast<std::uint8_t>((component << 3U) | (component >> 2U));
}

std::uint32_t packArgb(std::uint8_t red, std::uint8_t green,
                       std::uint8_t blue, std::uint8_t alpha) noexcept {
    return (static_cast<std::uint32_t>(alpha) << 24U) |
           (static_cast<std::uint32_t>(red) << 16U) |
           (static_cast<std::uint32_t>(green) << 8U) |
           static_cast<std::uint32_t>(blue);
}

} // namespace

bool RdpFramebuffer::copyToArgb8888(
    const Rdram& rdram, const RdpImageState& image, std::uint16_t height,
    std::vector<std::uint32_t>& output) {
    if (image.format != 0U || image.width == 0U || height == 0U ||
        image.width > kMaxWidth || height > kMaxHeight ||
        (image.size != 2U && image.size != 3U)) {
        return false;
    }

    const std::size_t bytesPerPixel = image.size == 2U ? 2U : 4U;
    const std::size_t pixelCount =
        static_cast<std::size_t>(image.width) * height;
    if (pixelCount == 0U || pixelCount > kMaxPixels ||
        pixelCount > std::numeric_limits<std::size_t>::max() / bytesPerPixel) {
        return false;
    }
    const std::size_t byteCount = pixelCount * bytesPerPixel;
    const std::uint64_t endAddress =
        static_cast<std::uint64_t>(image.address) + byteCount;
    if (endAddress > rdram.size()) {
        return false;
    }

    std::vector<std::uint32_t> converted;
    converted.reserve(pixelCount);
    for (std::size_t index = 0; index < pixelCount; ++index) {
        const std::uint64_t sourceAddress =
            static_cast<std::uint64_t>(image.address) + index * bytesPerPixel;
        if (image.size == 2U) {
            std::uint16_t pixel = 0;
            if (!rdram.read16(static_cast<std::uint32_t>(sourceAddress), pixel)) {
                return false;
            }
            const auto red = expand5(static_cast<std::uint16_t>(pixel >> 11U));
            const auto green = expand5(static_cast<std::uint16_t>(pixel >> 6U));
            const auto blue = expand5(static_cast<std::uint16_t>(pixel >> 1U));
            const auto alpha = (pixel & 1U) != 0U ? 0xFFU : 0x00U;
            converted.push_back(packArgb(red, green, blue,
                                         static_cast<std::uint8_t>(alpha)));
        } else {
            std::uint32_t pixel = 0;
            if (!rdram.read32(static_cast<std::uint32_t>(sourceAddress), pixel)) {
                return false;
            }
            converted.push_back(packArgb(
                static_cast<std::uint8_t>(pixel >> 24U),
                static_cast<std::uint8_t>(pixel >> 16U),
                static_cast<std::uint8_t>(pixel >> 8U),
                static_cast<std::uint8_t>(pixel)));
        }
    }

    output.swap(converted);
    return true;
}

} // namespace m64plus::n64
