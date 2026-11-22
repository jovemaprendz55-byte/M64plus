#include "n64/framebuffer.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void testRgba16Conversion() {
    using namespace m64plus::n64;
    Rdram rdram;
    // RGBA5551: opaque red, opaque green, opaque blue, transparent white.
    require(rdram.write16(0x100U, 0xF801U), "write red RGBA16 pixel");
    require(rdram.write16(0x102U, 0x07C1U), "write green RGBA16 pixel");
    require(rdram.write16(0x104U, 0x003FU), "write blue RGBA16 pixel");
    require(rdram.write16(0x106U, 0xFFFEU), "write transparent RGBA16 pixel");
    RdpImageState image{0U, 2U, 4U, 0x100U};
    std::vector<std::uint32_t> pixels{0xDEADBEEFU};
    require(RdpFramebuffer::copyToArgb8888(rdram, image, 1U, pixels),
            "convert RGBA16 framebuffer");
    require(pixels.size() == 4U, "RGBA16 output pixel count");
    require(pixels[0] == 0xFFFF0000U, "RGBA16 red conversion");
    require(pixels[1] == 0xFF00FF00U, "RGBA16 green conversion");
    require(pixels[2] == 0xFF0000FFU, "RGBA16 blue conversion");
    require(pixels[3] == 0x00FFFFFFU, "RGBA16 alpha bit conversion");
}

void testRgba32ConversionAndStride() {
    using namespace m64plus::n64;
    Rdram rdram;
    require(rdram.write32(0x200U, 0x12345678U), "write first RGBA32 pixel");
    require(rdram.write32(0x204U, 0x90ABCDEFU), "write second RGBA32 pixel");
    RdpImageState image{0U, 3U, 2U, 0x200U};
    std::vector<std::uint32_t> pixels;
    require(RdpFramebuffer::copyToArgb8888(rdram, image, 1U, pixels),
            "convert RGBA32 framebuffer");
    require(pixels.size() == 2U, "RGBA32 output pixel count");
    require(pixels[0] == 0x78123456U && pixels[1] == 0xEF90ABCDU,
            "RGBA32 big-endian RGBA to ARGB conversion");
}

void testInvalidFrameDoesNotModifyOutput() {
    using namespace m64plus::n64;
    Rdram rdram;
    std::vector<std::uint32_t> pixels{0x11223344U, 0x55667788U};
    const std::vector<std::uint32_t> original = pixels;
    RdpImageState unsupported{1U, 2U, 1U, 0U};
    require(!RdpFramebuffer::copyToArgb8888(rdram, unsupported, 1U, pixels),
            "reject non-RGBA image format");
    require(pixels == original, "unsupported format preserves previous frame");

    RdpImageState outOfRange{0U, 3U, 2U, 0x7FFFFCU};
    require(!RdpFramebuffer::copyToArgb8888(rdram, outOfRange, 2U, pixels),
            "reject framebuffer extending past RDRAM");
    require(pixels == original, "out-of-range frame preserves previous frame");

    RdpImageState tooWide{0U, 3U, 1025U, 0U};
    require(!RdpFramebuffer::copyToArgb8888(rdram, tooWide, 1U, pixels),
            "reject dimensions above framebuffer limits");
    require(pixels == original, "invalid dimensions preserve previous frame");
}
} // namespace

int main() {
    testRgba16Conversion();
    testRgba32ConversionAndStride();
    testInvalidFrameDoesNotModifyOutput();
    std::cout << "All framebuffer tests passed\n";
    return EXIT_SUCCESS;
}
