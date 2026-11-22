#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "n64/rdp.h"
#include "n64/rdram.h"

namespace m64plus::n64 {

// CPU-side snapshot of an RDP color image, converted to Android-friendly
// 0xAARRGGBB pixels. This is a memory bridge, not an RDP rasterizer.
class RdpFramebuffer final {
public:
    static constexpr std::uint16_t kMaxWidth = 1024U;
    static constexpr std::uint16_t kMaxHeight = 1024U;
    static constexpr std::size_t kMaxPixels =
        static_cast<std::size_t>(kMaxWidth) * kMaxHeight;

    // Supports RGBA16 (5551) and RGBA32 (8888). The output is replaced only
    // when the complete source rectangle is valid and conversion succeeds.
    static bool copyToArgb8888(const Rdram& rdram,
                               const RdpImageState& image,
                               std::uint16_t height,
                               std::vector<std::uint32_t>& output);
};

} // namespace m64plus::n64
