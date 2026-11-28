#include "n64/rdp.h"

#include <algorithm>
#include <array>

#include "n64/bus.h"

namespace m64plus::n64 {
namespace {
std::int16_t signExtend9(std::uint64_t value) noexcept {
    const std::uint16_t bits = static_cast<std::uint16_t>(value & 0x01FFU);
    const std::int16_t signedValue = static_cast<std::int16_t>(bits);
    return (bits & 0x0100U) != 0U
        ? static_cast<std::int16_t>(signedValue - 0x0200)
        : signedValue;
}

bool fillRectangle(Bus& bus, const RdpSyncState& state,
                    std::uint64_t command) noexcept {
    if (!state.hasColorImage || !state.hasFillColor ||
        !state.hasOtherModes || state.otherModes.cycleType != 3U) return false;
    const RdpImageState& image = state.colorImage;
    // FILL-cycle support currently covers RGBA16 and RGBA32 color images.
    if (image.format != 0U || (image.size != 2U && image.size != 3U) ||
        image.width == 0U) return false;
    const std::uint32_t bytesPerPixel = image.size == 2U ? 2U : 4U;

    std::uint32_t left = static_cast<std::uint32_t>((command >> 12U) & 0x0FFFU) >> 2U;
    std::uint32_t top = static_cast<std::uint32_t>(command & 0x0FFFU) >> 2U;
    std::uint32_t right = static_cast<std::uint32_t>((command >> 44U) & 0x0FFFU) >> 2U;
    std::uint32_t bottom = static_cast<std::uint32_t>((command >> 32U) & 0x0FFFU) >> 2U;
    if (right < left || bottom < top) return false;

    if (state.hasScissor) {
        const std::uint32_t scissorLeft = state.scissor.xh >> 2U;
        const std::uint32_t scissorTop = state.scissor.yh >> 2U;
        const std::uint32_t scissorRightExclusive = state.scissor.xl >> 2U;
        const std::uint32_t scissorBottomExclusive = state.scissor.yl >> 2U;
        if (scissorRightExclusive == 0U || scissorBottomExclusive == 0U) return false;
        if (left < scissorLeft) left = scissorLeft;
        if (top < scissorTop) top = scissorTop;
        if (right >= scissorRightExclusive) right = scissorRightExclusive - 1U;
        if (bottom >= scissorBottomExclusive) bottom = scissorBottomExclusive - 1U;
    }

    if (right >= image.width || left > right || top > bottom) return false;
    const std::uint64_t memorySize = bus.rdram().size();
    // Validate every destination before the first write, so a rectangle that
    // crosses the end of RDRAM cannot leave a partially modified framebuffer.
    for (std::uint32_t y = top; y <= bottom; ++y) {
        // Interlaced scissor mode selects one field: oddLine=0 keeps even
        // scanlines, oddLine=1 keeps odd scanlines.
        if (state.hasScissor && state.scissor.fieldEnable &&
            (y & 1U) != static_cast<std::uint32_t>(state.scissor.oddLine)) continue;
        for (std::uint32_t x = left; x <= right; ++x) {
            const std::uint64_t pixel =
                static_cast<std::uint64_t>(y) * image.width + x;
            const std::uint64_t address =
                static_cast<std::uint64_t>(image.address) + pixel * bytesPerPixel;
            if (address + bytesPerPixel > memorySize) return false;
        }
    }
    for (std::uint32_t y = top; y <= bottom; ++y) {
        if (state.hasScissor && state.scissor.fieldEnable &&
            (y & 1U) != static_cast<std::uint32_t>(state.scissor.oddLine)) continue;
        for (std::uint32_t x = left; x <= right; ++x) {
            const std::uint64_t pixel =
                static_cast<std::uint64_t>(y) * image.width + x;
            const std::uint32_t address = static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(image.address) + pixel * bytesPerPixel);
            if (bytesPerPixel == 2U) {
                // In 16-bit fill mode the 32-bit fill color repeats its
                // upper and lower halfwords across successive pixels.
                const std::uint16_t value = (pixel & 1U) == 0U
                    ? static_cast<std::uint16_t>(state.fillColor >> 16U)
                    : static_cast<std::uint16_t>(state.fillColor);
                if (!bus.write16(address, value)) return false;
            } else if (!bus.write8(address, static_cast<std::uint8_t>(state.fillColor >> 24U)) ||
                       !bus.write8(address + 1U, static_cast<std::uint8_t>(state.fillColor >> 16U)) ||
                       !bus.write8(address + 2U, static_cast<std::uint8_t>(state.fillColor >> 8U)) ||
                       !bus.write8(address + 3U, static_cast<std::uint8_t>(state.fillColor))) {
                return false;
            }
        }
    }
    return true;
}


bool rasterizeTextureRectangleRgba16(Bus& bus, const RdpSyncState& state,
                                     const RdpTextureRectangleCommand& rectangle) noexcept {
    // Initial deterministic slice: aligned, non-flipped RGBA16 rectangles,
    // nearest sampling, unit derivatives, and an unmasked/unmirrored tile.
    if (!state.hasColorImage || !state.hasTextureImage || !state.hasTile[rectangle.tile] ||
        !state.hasOtherModes || !state.hasScissor ||
        state.otherModes.cycleType > 1U) return false;
    const RdpImageState& color = state.colorImage;
    const RdpTileState& tile = state.tiles[rectangle.tile];
    if (color.format != 0U || color.size != 2U || color.width == 0U ||
        state.textureImage.format != 0U || state.textureImage.size != 2U ||
        tile.format != 0U || tile.size != 2U || tile.line == 0U ||
        tile.maskS != 0U || tile.maskT != 0U || tile.mirrorS || tile.mirrorT ||
        !tile.clampS || !tile.clampT ||
        (rectangle.xh & 3U) != 0U || (rectangle.yh & 3U) != 0U ||
        (rectangle.xl & 3U) != 0U || (rectangle.yl & 3U) != 0U ||
        (rectangle.s & 31) != 0 || (rectangle.t & 31) != 0 ||
        rectangle.dsdx != 0x0400 || rectangle.dtdy != 0x0400) return false;

    const std::int32_t left = rectangle.xh / 4;
    const std::int32_t top = rectangle.yh / 4;
    const std::int32_t right = rectangle.xl / 4;
    const std::int32_t bottom = rectangle.yl / 4;
    if (right <= left || bottom <= top) return false;
    const std::int32_t clipLeft = std::max(left, static_cast<std::int32_t>(state.scissor.xh / 4U));
    const std::int32_t clipTop = std::max(top, static_cast<std::int32_t>(state.scissor.yh / 4U));
    const std::int32_t clipRight = std::min(
        std::min(right, static_cast<std::int32_t>(state.scissor.xl / 4U)),
        static_cast<std::int32_t>(color.width));
    const std::int32_t clipBottom = std::min(bottom, static_cast<std::int32_t>(state.scissor.yl / 4U));
    if (clipRight <= clipLeft || clipBottom <= clipTop) return false;

    if (!state.hasTileSize[rectangle.tile]) return false;
    const RdpTileSizeState& tileSize = state.tileSizes[rectangle.tile];
    if (tileSize.sh < tileSize.sl || tileSize.th < tileSize.tl) return false;
    const std::int32_t sourceS = rectangle.s / 32;
    const std::int32_t sourceT = rectangle.t / 32;
    const std::int32_t maxS = static_cast<std::int32_t>(
        (tileSize.sh - tileSize.sl) / 4U);
    const std::int32_t maxT = static_cast<std::int32_t>(
        (tileSize.th - tileSize.tl) / 4U);
    // Clamp coordinates to the SET_TILE_SIZE rectangle instead of
    // rejecting a rectangle whose sampling coordinates cross its edges.

    const std::uint64_t memorySize = bus.rdram().size();
    for (std::int32_t y = clipTop; y < clipBottom; ++y) {
        if (state.scissor.fieldEnable &&
            (y & 1) != static_cast<std::int32_t>(state.scissor.oddLine)) continue;
        for (std::int32_t x = clipLeft; x < clipRight; ++x) {
            const std::uint64_t pixel = static_cast<std::uint64_t>(y) * color.width +
                static_cast<std::uint32_t>(x);
            if (static_cast<std::uint64_t>(color.address) + pixel * 2U + 2U > memorySize)
                return false;
        }
    }

    for (std::int32_t y = clipTop; y < clipBottom; ++y) {
        if (state.scissor.fieldEnable &&
            (y & 1) != static_cast<std::int32_t>(state.scissor.oddLine)) continue;
        for (std::int32_t x = clipLeft; x < clipRight; ++x) {
            const std::int32_t sampleS = rectangle.flip ? sourceS + y - top : sourceS + x - left;
            const std::int32_t sampleT = rectangle.flip ? sourceT + x - left : sourceT + y - top;
            const std::uint32_t texS = static_cast<std::uint32_t>(
                std::clamp(sampleS, 0, maxS));
            const std::int32_t texT = std::clamp(sampleT, 0, maxT);
            const std::uint32_t row = static_cast<std::uint32_t>(tile.tmem) * 8U +
                static_cast<std::uint32_t>(texT) * static_cast<std::uint32_t>(tile.line) * 8U;
            const std::uint32_t logical = row + texS * 2U;
            const std::uint16_t tmemAddress = static_cast<std::uint16_t>(
                (logical & ~7U) | ((logical & 7U) ^ ((texT & 1U) != 0U ? 4U : 0U)));
            std::uint8_t high = 0U;
            std::uint8_t low = 0U;
            state.tmem.readByte(tmemAddress, high);
            state.tmem.readByte(static_cast<std::uint16_t>(tmemAddress + 1U), low);
            const std::uint16_t texel = static_cast<std::uint16_t>(
                (static_cast<std::uint16_t>(high) << 8U) | low);
            const std::uint32_t address = color.address +
                (static_cast<std::uint32_t>(y) * color.width +
                 static_cast<std::uint32_t>(x)) * 2U;
            if (!bus.write16(address, texel)) return false;
        }
    }
    return true;
}


std::int64_t floorDivide(std::int64_t value, std::int64_t divisor) noexcept {
    const std::int64_t quotient = value / divisor;
    const std::int64_t remainder = value % divisor;
    return remainder < 0 ? quotient - 1 : quotient;
}

std::int32_t signExtend14(std::uint16_t value) noexcept {
    const std::int32_t bits = static_cast<std::int32_t>(value & 0x3FFFU);
    return (bits & 0x2000) != 0 ? bits - 0x4000 : bits;
}

std::array<std::uint8_t, 4> interpolateShadeImpl(
    const RdpTriangleShadeCoefficients& shade, std::int64_t majorX,
    std::int32_t y, std::int32_t yBase, std::int32_t x) noexcept {
    // Native-resolution form of ParaLLEl-RDP's full-coverage interpolation:
    // dRGBA/dE advances one pixel row along the major edge, and dRGBA/dX
    // advances one pixel column across the span. Subscanline Y coordinates
    // must be reduced to pixel-row deltas before calling this helper. The
    // low five bits of dRGBA/dX are discarded before stepping across the span.
    const std::int32_t baseX = static_cast<std::int32_t>(floorDivide(majorX, 65536));
    const auto wrap32 = [](std::uint32_t bits) noexcept {
        if (bits <= 0x7FFFFFFFU) return static_cast<std::int32_t>(bits);
        return -1 - static_cast<std::int32_t>(0xFFFFFFFFU - bits);
    };
    const auto sext9 = [](std::int32_t value) noexcept {
        const std::int32_t bits = value & 0x1FF;
        return bits >= 0x100 ? bits - 0x200 : bits;
    };
    std::array<std::uint8_t, 4> result{};
    for (std::size_t channel = 0; channel < result.size(); ++channel) {
        const std::int32_t scan = wrap32(
            static_cast<std::uint32_t>(shade.rgbaFixed[channel]) +
            static_cast<std::uint32_t>(shade.drgbaDeFixed[channel]) *
                static_cast<std::uint32_t>(y - yBase));
        const std::int32_t dx = shade.drgbaDxFixed[channel] & ~0x1F;
        const std::int32_t value = wrap32(
            static_cast<std::uint32_t>(scan) +
            static_cast<std::uint32_t>(dx) *
                static_cast<std::uint32_t>(x - baseX));
        // RDP snaps through a signed 16-bit intermediate before the 8-bit
        // color clamp. Explicit sign extension avoids host narrowing ambiguity.
        // C++17 leaves right-shifting a negative signed value implementation-defined.
        // Use mathematical floor to preserve the RDP's arithmetic-shift behavior portably.
        const std::int32_t shifted = static_cast<std::int32_t>(
            floorDivide(value, 1 << 14U));
        const std::int32_t signed16 = ((shifted & 0xFFFF) >= 0x8000)
            ? (shifted & 0xFFFF) - 0x10000 : (shifted & 0xFFFF);
        const std::uint16_t shifted2Bits = static_cast<std::uint16_t>(
            static_cast<std::uint32_t>(signed16) << 2U);
        const std::int32_t signedShifted2 =
            (shifted2Bits & 0x8000U) != 0U
                ? static_cast<std::int32_t>(shifted2Bits) - 0x10000
                : static_cast<std::int32_t>(shifted2Bits);
        const std::int32_t snapped = static_cast<std::int32_t>(
            floorDivide(signedShifted2, 16));
        result[channel] = static_cast<std::uint8_t>(
            std::clamp(sext9(snapped - 0x80) + 0x80, 0, 0xFF));
    }
    return result;
}

bool fillTriangle(Bus& bus, const RdpSyncState& state,
                  const RdpTriangleCommand& triangle) noexcept {
    // First rasterizer slice: unshaded, untextured, un-Z triangles in FILL mode.
    if (triangle.layout.hasShade || triangle.layout.hasTexture ||
        triangle.layout.hasZ || !state.hasColorImage ||
        !state.hasFillColor || !state.hasOtherModes ||
        (state.otherModes.cycleType != 2U &&
         state.otherModes.cycleType != 3U) || !state.hasScissor) return false;
    const RdpImageState& image = state.colorImage;
    if (image.format != 0U || (image.size != 2U && image.size != 3U) ||
        image.width == 0U) return false;

    const RdpTriangleEdgeState& edge = triangle.edge;
    const std::int32_t yh = signExtend14(edge.yHigh);
    const std::int32_t ym = signExtend14(edge.yMiddle);
    const std::int32_t yl = signExtend14(edge.yLow);
    if (yl <= yh) return false;
    const std::int32_t middle = ym < yh ? yh : (ym > yl ? yl : ym);
    const std::int32_t scissorLeft = static_cast<std::int32_t>(state.scissor.xh) >> 2U;
    const std::int32_t scissorRight = static_cast<std::int32_t>(state.scissor.xl) >> 2U;
    const std::int32_t scissorTop = static_cast<std::int32_t>(state.scissor.yh) >> 2U;
    const std::int32_t scissorBottom = static_cast<std::int32_t>(state.scissor.yl) >> 2U;
    if (scissorRight <= scissorLeft || scissorBottom <= scissorTop) return false;

    const std::int32_t startLine = std::max(static_cast<std::int32_t>(floorDivide(yh, 4)), scissorTop);
    const std::int32_t endLine = std::min(static_cast<std::int32_t>(floorDivide(static_cast<std::int64_t>(yl) - 1, 4)), scissorBottom - 1);
    if (endLine < startLine) return false;
    const std::int32_t yhBase = static_cast<std::int32_t>(floorDivide(yh, 4) * 4);
    const std::int32_t width = static_cast<std::int32_t>(image.width);
    const std::int32_t maxX = std::min(scissorRight, width - 1);
    const std::uint32_t bytesPerPixel = image.size == 2U ? 2U : 4U;
    const std::uint64_t memorySize = bus.rdram().size();
    const bool flip = !edge.leftMajor;

    const auto spanForLine = [&](std::int32_t line, std::int32_t& left,
                                 std::int32_t& right) noexcept {
        if (state.scissor.fieldEnable &&
            (line & 1) != static_cast<std::int32_t>(state.scissor.oddLine))
            return false;
        left = INT32_MAX;
        right = INT32_MIN;
        for (std::int32_t sub = 0; sub < 4; ++sub) {
            const std::int32_t y = line * 4 + sub;
            if (y < yh || y >= yl) continue;
            const std::int64_t major = static_cast<std::int64_t>(edge.xHigh) +
                static_cast<std::int64_t>(y - yhBase) * edge.dxHighDy;
            const std::int64_t minor = y < middle
                ? static_cast<std::int64_t>(edge.xMiddle) +
                    static_cast<std::int64_t>(y - yhBase) * edge.dxMiddleDy
                : static_cast<std::int64_t>(edge.xLeft) +
                    static_cast<std::int64_t>(y - middle) * edge.dxLeftDy;
            const std::int32_t majorPixel = static_cast<std::int32_t>(floorDivide(major, 65536));
            const std::int32_t minorPixel = static_cast<std::int32_t>(floorDivide(minor, 65536));
            const std::int32_t candidateLeft = flip ? majorPixel : minorPixel;
            const std::int32_t candidateRight = flip ? minorPixel : majorPixel;
            if (candidateLeft > candidateRight) continue;
            left = std::min(left, candidateLeft);
            right = std::max(right, candidateRight);
        }
        left = std::max(std::max(left, scissorLeft), 0);
        right = std::min(right, maxX);
        return left <= right;
    };

    // Validate every destination before the first write. This mirrors the
    // rectangle path's all-or-nothing behavior for framebuffer bounds errors.
    for (std::int32_t y = startLine; y <= endLine; ++y) {
        std::int32_t left = 0;
        std::int32_t right = -1;
        if (!spanForLine(y, left, right)) continue;
        for (std::int32_t x = left; x <= right; ++x) {
            const std::uint64_t pixel =
                static_cast<std::uint64_t>(y) * image.width +
                static_cast<std::uint32_t>(x);
            const std::uint64_t address =
                static_cast<std::uint64_t>(image.address) + pixel * bytesPerPixel;
            if (address + bytesPerPixel > memorySize) return false;
        }
    }
    for (std::int32_t y = startLine; y <= endLine; ++y) {
        std::int32_t left = 0;
        std::int32_t right = -1;
        if (!spanForLine(y, left, right)) continue;
        for (std::int32_t x = left; x <= right; ++x) {
            const std::uint64_t pixel =
                static_cast<std::uint64_t>(y) * image.width +
                static_cast<std::uint32_t>(x);
            const std::uint32_t address = static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(image.address) + pixel * bytesPerPixel);
            if (bytesPerPixel == 2U) {
                const std::uint16_t value = (pixel & 1U) == 0U
                    ? static_cast<std::uint16_t>(state.fillColor >> 16U)
                    : static_cast<std::uint16_t>(state.fillColor);
                if (!bus.write16(address, value)) return false;
            } else if (!bus.write8(address, static_cast<std::uint8_t>(state.fillColor >> 24U)) ||
                       !bus.write8(address + 1U, static_cast<std::uint8_t>(state.fillColor >> 16U)) ||
                       !bus.write8(address + 2U, static_cast<std::uint8_t>(state.fillColor >> 8U)) ||
                       !bus.write8(address + 3U, static_cast<std::uint8_t>(state.fillColor))) {
                return false;
            }
        }
    }
    return true;
}

bool loadTile32BitTexels(Bus& bus, RdpSyncState& state,
                           const RdpTextureLoadState& load) noexcept {
    if (!state.hasTextureImage || !state.hasTile[load.tile]) return false;
    const RdpImageState& image = state.textureImage;
    const RdpTileState& tile = state.tiles[load.tile];
    // The documented 32-bit path is RGBA only and splits RG from BA into
    // the lower and upper 2 KiB TMEM banks, respectively.
    if (image.size != 3U || image.format != 0U ||
        tile.size != 3U || tile.format != 0U) return false;

    const std::uint32_t x0 = load.sl >> 2U;
    const std::uint32_t y0 = load.tl >> 2U;
    const std::uint32_t x1 = load.sh >> 2U;
    const std::uint32_t y1 = load.th >> 2U;
    if (x1 < x0 || y1 < y0 || x1 >= image.width) return false;

    const std::uint32_t rowTexels = x1 - x0 + 1U;
    const std::uint32_t rowCount = y1 - y0 + 1U;
    const std::uint64_t sourceBase = image.address;
    // Validate the whole rectangle before writing either TMEM bank.
    for (std::uint32_t row = 0; row < rowCount; ++row) {
        for (std::uint32_t column = 0; column < rowTexels; ++column) {
            const std::uint64_t texel =
                (static_cast<std::uint64_t>(y0 + row) * image.width) +
                x0 + column;
            const std::uint64_t address = sourceBase + texel * 4U;
            for (unsigned byte = 0; byte < 4U; ++byte) {
                std::uint8_t ignored = 0U;
                if (address + byte > UINT32_MAX ||
                    !bus.read8(static_cast<std::uint32_t>(address + byte), ignored))
                    return false;
            }
        }
    }

    const std::uint64_t base = static_cast<std::uint64_t>(tile.tmem) * 8U;
    const std::uint64_t stride = static_cast<std::uint64_t>(tile.line) * 8U;
    for (std::uint32_t row = 0; row < rowCount; ++row) {
        for (std::uint32_t column = 0; column < rowTexels; ++column) {
            const std::uint64_t texel =
                (static_cast<std::uint64_t>(y0 + row) * image.width) +
                x0 + column;
            const std::uint64_t source = sourceBase + texel * 4U;
            const std::uint64_t logical =
                base + static_cast<std::uint64_t>(row) * stride +
                static_cast<std::uint64_t>(column) * 2U;
            const std::uint64_t low = (row & 1U) != 0U
                ? (logical ^ 4U) : logical;
            // Normalize within the lower 2 KiB bank before deriving the
            // matching upper-bank address; otherwise crossing 0x800 can
            // make the BA write wrap into the lower bank.
            const std::uint64_t lowerBankAddress = low & 0x07FFU;
            const std::uint64_t upperBankAddress = lowerBankAddress + 0x0800U;
            std::uint8_t red = 0U;
            std::uint8_t green = 0U;
            std::uint8_t blue = 0U;
            std::uint8_t alpha = 0U;
            if (!bus.read8(static_cast<std::uint32_t>(source), red) ||
                !bus.read8(static_cast<std::uint32_t>(source + 1U), green) ||
                !bus.read8(static_cast<std::uint32_t>(source + 2U), blue) ||
                !bus.read8(static_cast<std::uint32_t>(source + 3U), alpha))
                return false;
            state.tmem.writeByte(static_cast<std::uint16_t>(lowerBankAddress), red);
            state.tmem.writeByte(static_cast<std::uint16_t>((lowerBankAddress + 1U) & 0x07FFU), green);
            state.tmem.writeByte(static_cast<std::uint16_t>(upperBankAddress & 0x0FFFU), blue);
            state.tmem.writeByte(static_cast<std::uint16_t>((upperBankAddress + 1U) & 0x0FFFU), alpha);
        }
    }
    return true;
}

bool loadTileTexels(Bus& bus, RdpSyncState& state,
                    const RdpTextureLoadState& load) noexcept {
    if (!state.hasTextureImage || !state.hasTile[load.tile]) return false;
    const RdpImageState& image = state.textureImage;
    const RdpTileState& tile = state.tiles[load.tile];
    // LOAD_TILE uses the selected tile descriptor's size to calculate
    // texel addressing and TMEM layout. SET_TEXTURE_IMAGE supplies the
    // source image pointer and width; the load tile controls transfer size.
    if (image.size == 3U) return loadTile32BitTexels(bus, state, load);
    if (tile.size == 3U) return false;
    const unsigned bitsPerTexel = tile.size == 0U ? 4U
        : (tile.size == 1U ? 8U : 16U);
    const std::uint32_t x0 = load.sl >> 2U;
    const std::uint32_t y0 = load.tl >> 2U;
    const std::uint32_t x1 = load.sh >> 2U;
    const std::uint32_t y1 = load.th >> 2U;
    if (x1 < x0 || y1 < y0 || x1 >= image.width) return false;
    const std::uint32_t rowTexels = x1 - x0 + 1U;
    const std::uint32_t rowCount = y1 - y0 + 1U;
    const std::uint64_t rowBits = static_cast<std::uint64_t>(rowTexels) * bitsPerTexel;
    const std::uint64_t sourceBase = image.address;

    // Validate the complete source rectangle before changing TMEM.
    for (std::uint32_t row = 0; row < rowCount; ++row) {
        const std::uint64_t firstTexel =
            static_cast<std::uint64_t>(y0 + row) * image.width + x0;
        const std::uint64_t firstBit = (firstTexel * bitsPerTexel) & 7U;
        const std::uint64_t firstByte = (firstTexel * bitsPerTexel) / 8U;
        const std::uint64_t bytesToCheck = (firstBit + rowBits + 7U) / 8U;
        for (std::uint64_t byte = 0; byte < bytesToCheck; ++byte) {
            const std::uint64_t address = sourceBase + firstByte + byte;
            std::uint8_t ignored = 0;
            if (address > UINT32_MAX ||
                !bus.read8(static_cast<std::uint32_t>(address), ignored)) return false;
        }
    }

    const std::uint64_t destinationBase = static_cast<std::uint64_t>(tile.tmem) * 8U;
    const std::uint64_t destinationStride = static_cast<std::uint64_t>(tile.line) * 8U;
    for (std::uint32_t row = 0; row < rowCount; ++row) {
        const std::uint64_t destinationRow =
            destinationBase + static_cast<std::uint64_t>(row) * destinationStride;
        for (std::uint32_t column = 0; column < rowTexels; ++column) {
            const std::uint64_t sourceTexel =
                static_cast<std::uint64_t>(y0 + row) * image.width + x0 + column;
            const std::uint64_t destinationTexel =
                destinationRow + (static_cast<std::uint64_t>(column) * bitsPerTexel) / 8U;
            if (bitsPerTexel == 4U) {
                const std::uint64_t sourceAddress = sourceBase +
                    (sourceTexel / 2U);
                std::uint8_t sourceByte = 0;
                if (sourceAddress > UINT32_MAX ||
                    !bus.read8(static_cast<std::uint32_t>(sourceAddress), sourceByte)) return false;
                const bool sourceLow = (sourceTexel & 1U) != 0U;
                const std::uint8_t nibble = sourceLow
                    ? static_cast<std::uint8_t>(sourceByte & 0x0FU)
                    : static_cast<std::uint8_t>(sourceByte >> 4U);
                const std::uint64_t swizzledDestination =
                    (row & 1U) != 0U ? (destinationTexel ^ 4U) : destinationTexel;
                const std::uint16_t destinationAddress =
                    static_cast<std::uint16_t>(swizzledDestination & 0x0FFFU);
                std::uint8_t destinationByte = 0;
                state.tmem.readByte(destinationAddress, destinationByte);
                const bool destinationLow = (column & 1U) != 0U;
                destinationByte = destinationLow
                    ? static_cast<std::uint8_t>((destinationByte & 0xF0U) | nibble)
                    : static_cast<std::uint8_t>((destinationByte & 0x0FU) | (nibble << 4U));
                state.tmem.writeByte(destinationAddress, destinationByte);
            } else {
                const unsigned bytesPerTexel = bitsPerTexel / 8U;
                for (unsigned byte = 0; byte < bytesPerTexel; ++byte) {
                    const std::uint64_t sourceAddress =
                        sourceBase + (sourceTexel * bitsPerTexel) / 8U + byte;
                    std::uint8_t value = 0;
                    if (sourceAddress > UINT32_MAX ||
                        !bus.read8(static_cast<std::uint32_t>(sourceAddress), value)) return false;
                    const std::uint64_t destinationAddress =
                        destinationTexel + byte;
                    const std::uint64_t swizzledDestination =
                        (row & 1U) != 0U ? (destinationAddress ^ 4U)
                                         : destinationAddress;
                    state.tmem.writeByte(
                        static_cast<std::uint16_t>(swizzledDestination & 0x0FFFU),
                        value);
                }
            }
        }
    }
    return true;
}

bool loadBlockTexels(Bus& bus, RdpSyncState& state,
                      const RdpTextureLoadState& load) noexcept {
    if (!state.hasTextureImage || !state.hasTile[load.tile] ||
        load.sh < load.sl || (load.sl & 0x03U) != 0U ||
        (load.sl >> 2U) >= state.textureImage.width ||
        (load.tl & 0x03U) != 0U || (load.sh & 0x03U) != 0U) return false;
    const RdpImageState& image = state.textureImage;
    const RdpTileState& tile = state.tiles[load.tile];
    // LOAD_BLOCK supports packed 4-bit, 8-bit and 16-bit texels in the
    // linear path. RGBA32 uses the RDP's split TMEM banks and a dedicated store.
    if (image.size > 3U) return false;
    if (image.size == 0U && tile.size != 0U) return false;
    if (image.size == 3U &&
        (image.format != 0U || tile.size != 3U || tile.format != 0U)) return false;
    const unsigned bitsPerTexel = image.size == 0U ? 4U
        : (image.size == 1U ? 8U : (image.size == 2U ? 16U : 32U));
    const std::uint32_t count =
        (static_cast<std::uint32_t>(load.sh - load.sl) >> 2U) + 1U;
    if (count == 0U || count > 2048U) return false;
    const std::uint64_t firstTexel =
        static_cast<std::uint64_t>(load.tl >> 2U) * image.width + (load.sl >> 2U);
    const std::uint64_t totalBits = static_cast<std::uint64_t>(count) * bitsPerTexel;
    const std::uint64_t totalBytes = (totalBits + 7U) / 8U;
    if (totalBytes > RdpTextureMemory::kSize) return false;
    const std::uint64_t sourceFirstBit = firstTexel * bitsPerTexel;
    const std::uint64_t sourceFirstByte = sourceFirstBit / 8U;
    const std::uint64_t sourceBytes =
        ((sourceFirstBit & 7U) + totalBits + 7U) / 8U;
    for (std::uint64_t byte = 0; byte < sourceBytes; ++byte) {
        const std::uint64_t address =
            static_cast<std::uint64_t>(image.address) + sourceFirstByte + byte;
        std::uint8_t ignored = 0;
        if (address > UINT32_MAX ||
            !bus.read8(static_cast<std::uint32_t>(address), ignored)) return false;
    }

    std::array<std::uint8_t, RdpTextureMemory::kSize> transfer{};
    if (bitsPerTexel == 4U) {
        // The first texel may begin in either half of its source byte
        // (for example, an odd image width can start the next row low-nibble).
        for (std::uint64_t byte = 0U; byte < sourceBytes; ++byte) {
            const std::uint64_t sourceAddress =
                static_cast<std::uint64_t>(image.address) + sourceFirstByte + byte;
            if (sourceAddress > UINT32_MAX ||
                !bus.read8(static_cast<std::uint32_t>(sourceAddress),
                           transfer[static_cast<std::size_t>(byte)])) return false;
        }
        if ((sourceFirstBit & 4U) != 0U) {
            for (std::uint64_t byte = 0U; byte < totalBytes; ++byte) {
                const std::uint8_t current = transfer[static_cast<std::size_t>(byte)];
                const std::uint8_t next = byte + 1U < sourceBytes
                    ? transfer[static_cast<std::size_t>(byte + 1U)] : 0U;
                transfer[static_cast<std::size_t>(byte)] =
                    static_cast<std::uint8_t>((current << 4U) | (next >> 4U));
            }
        }
    } else {
        const unsigned bytesPerTexel = bitsPerTexel / 8U;
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint64_t sourceTexel = firstTexel + i;
            for (unsigned byte = 0; byte < bytesPerTexel; ++byte) {
                const std::uint64_t sourceAddress =
                    static_cast<std::uint64_t>(image.address) +
                    sourceTexel * bytesPerTexel + byte;
                if (sourceAddress > UINT32_MAX ||
                    !bus.read8(static_cast<std::uint32_t>(sourceAddress),
                               transfer[static_cast<std::size_t>(i) * bytesPerTexel + byte]))
                    return false;
            }
        }
    }

    // DXT is an 11-bit fractional accumulator advanced once per 64-bit word.
    // Every carry advances the texture line; odd lines swap the two 32-bit halves.
    std::uint32_t dxtAccumulator = 0U;
    std::uint32_t line = 0U;
    for (std::uint64_t offset = 0; offset + 8U <= totalBytes; offset += 8U) {
        if ((line & 1U) != 0U) {
            for (std::uint32_t byte = 0; byte < 4U; ++byte) {
                const std::size_t first = static_cast<std::size_t>(offset + byte);
                const std::size_t second = first + 4U;
                const std::uint8_t temporary = transfer[first];
                transfer[first] = transfer[second];
                transfer[second] = temporary;
            }
        }
        dxtAccumulator += load.dxt;
        while (dxtAccumulator >= 0x800U) {
            dxtAccumulator -= 0x800U;
            ++line;
        }
    }

    const std::uint64_t destinationBase =
        static_cast<std::uint64_t>(tile.tmem) * 8U;
    if (image.size == 3U) {
        // RGBA32 texels are split across TMEM's lower RG and upper BA banks.
        // DXT has already reordered odd 64-bit words above, so store the
        // resulting texel stream into corresponding two-byte bank pairs.
        for (std::uint32_t i = 0U; i < count; ++i) {
            const std::size_t source = static_cast<std::size_t>(i) * 4U;
            const std::uint64_t logical = destinationBase +
                static_cast<std::uint64_t>(i) * 2U;
            const std::uint16_t lower = static_cast<std::uint16_t>(logical & 0x07FFU);
            const std::uint16_t upper = static_cast<std::uint16_t>(lower + 0x0800U);
            state.tmem.writeByte(lower, transfer[source]);
            state.tmem.writeByte(static_cast<std::uint16_t>((lower + 1U) & 0x07FFU),
                                 transfer[source + 1U]);
            state.tmem.writeByte(upper, transfer[source + 2U]);
            state.tmem.writeByte(static_cast<std::uint16_t>(upper + 1U),
                                 transfer[source + 3U]);
        }
    } else if (image.size == 0U) {
        // 4-bit texels are packed high-nibble first in both RDRAM and TMEM.
        for (std::uint32_t i = 0U; i < count; ++i) {
            const std::uint8_t packed = transfer[i / 2U];
            const std::uint8_t nibble = (i & 1U) == 0U
                ? static_cast<std::uint8_t>(packed >> 4U)
                : static_cast<std::uint8_t>(packed & 0x0FU);
            const std::uint64_t logical = destinationBase + i / 2U;
            const std::uint16_t address =
                static_cast<std::uint16_t>(logical & 0x0FFFU);
            std::uint8_t destination = 0U;
            state.tmem.readByte(address, destination);
            destination = (i & 1U) == 0U
                ? static_cast<std::uint8_t>((destination & 0x0FU) | (nibble << 4U))
                : static_cast<std::uint8_t>((destination & 0xF0U) | nibble);
            state.tmem.writeByte(address, destination);
        }
    } else {
        for (std::uint64_t byte = 0; byte < totalBytes; ++byte) {
            state.tmem.writeByte(
                static_cast<std::uint16_t>((destinationBase + byte) & 0x0FFFU),
                transfer[static_cast<std::size_t>(byte)]);
        }
    }
    state.tileSizes[load.tile] = {load.sl, load.tl, load.sh, load.th};
    state.hasTileSize[load.tile] = true;
    return true;
}

bool loadTlutEntries(Bus& bus, RdpSyncState& state,
                     const RdpTextureLoadState& load) noexcept {
    if (!state.hasTextureImage || !state.hasTile[load.tile] ||
        state.textureImage.size != 2U) return false;
    const RdpImageState& image = state.textureImage;
    const RdpTileState& tile = state.tiles[load.tile];
    const std::uint32_t firstX = load.sl >> 2U;
    const std::uint32_t firstY = load.tl >> 2U;
    const std::uint32_t lastX = load.sh >> 2U;
    if (lastX < firstX) return false;
    const std::uint32_t count = lastX - firstX + 1U;
    if (count == 0U || count > 256U || tile.tmem < 256U ||
        static_cast<std::uint32_t>(tile.tmem) + count > 512U) return false;

    const std::uint64_t firstTexel =
        static_cast<std::uint64_t>(firstY) * image.width + firstX;
    const std::uint64_t sourceBase =
        static_cast<std::uint64_t>(image.address) + firstTexel * 2U;
    const std::uint64_t sourceEnd =
        sourceBase + static_cast<std::uint64_t>(count) * 2U;
    if (sourceEnd > static_cast<std::uint64_t>(UINT32_MAX) + 1U) return false;

    // Stage the complete source range before touching TMEM. Besides avoiding
    // duplicate bus reads, this keeps the transfer atomic if a later read fails.
    std::array<std::uint16_t, 256> entries{};
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint64_t address = sourceBase + i * 2U;
        if (!bus.read16(static_cast<std::uint32_t>(address), entries[i])) return false;
    }

    const std::uint64_t destinationBase =
        static_cast<std::uint64_t>(tile.tmem) * 8U;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint16_t entry = entries[i];
        const std::uint64_t replicated =
            (static_cast<std::uint64_t>(entry) << 48U) |
            (static_cast<std::uint64_t>(entry) << 32U) |
            (static_cast<std::uint64_t>(entry) << 16U) |
            static_cast<std::uint64_t>(entry);
        state.tmem.write64(
            static_cast<std::uint16_t>((destinationBase + i * 8U) & 0x0FFFU),
            replicated);
    }
    state.tileSizes[load.tile] = {load.sl, load.tl, load.sh, load.th};
    state.hasTileSize[load.tile] = true;
    return true;
}
} // namespace

std::array<std::uint8_t, 4> RdpShadeInterpolator::interpolate(
    const RdpTriangleShadeCoefficients& shade, std::int64_t majorX,
    std::int32_t y, std::int32_t yBase, std::int32_t x) noexcept {
    return interpolateShadeImpl(shade, majorX, y, yBase, x);
}


void RdpTextureMemory::reset() noexcept {
    bytes_.fill(0U);
}
bool RdpTextureMemory::readByte(std::uint16_t address, std::uint8_t& value) const noexcept {
    value = bytes_[static_cast<std::size_t>(address) & (kSize - 1U)];
    return true;
}
bool RdpTextureMemory::writeByte(std::uint16_t address, std::uint8_t value) noexcept {
    bytes_[static_cast<std::size_t>(address) & (kSize - 1U)] = value;
    return true;
}
bool RdpTextureMemory::read64(std::uint16_t address, std::uint64_t& value) const noexcept {
    value = 0U;
    for (std::size_t i = 0; i < 8U; ++i) {
        const std::size_t index = (static_cast<std::size_t>(address) + i) & (kSize - 1U);
        value = (value << 8U) | bytes_[index];
    }
    return true;
}
bool RdpTextureMemory::write64(std::uint16_t address, std::uint64_t value) noexcept {
    for (std::size_t i = 0; i < 8U; ++i) {
        const unsigned shift = static_cast<unsigned>((7U - i) * 8U);
        const std::size_t index = (static_cast<std::size_t>(address) + i) & (kSize - 1U);
        bytes_[index] = static_cast<std::uint8_t>((value >> shift) & 0xFFU);
    }
    return true;
}

bool RdpCommandDecoder::decodeTriangleEdge(
    const std::array<std::uint64_t, 4>& words,
    RdpTriangleEdgeState& edge) noexcept {
    const auto signedField = [](std::uint32_t bits, unsigned width) noexcept -> std::int32_t {
        const std::uint32_t mask = (1U << width) - 1U;
        const std::uint32_t value = bits & mask;
        const std::uint32_t sign = 1U << (width - 1U);
        return (value & sign) != 0U
            ? static_cast<std::int32_t>(static_cast<std::int64_t>(value) -
                                        (static_cast<std::int64_t>(1U) << width))
            : static_cast<std::int32_t>(value);
    };
    RdpTriangleEdgeState candidate{};
    candidate.leftMajor = ((words[0] >> 55U) & 1U) == 0U;
    const bool flip = !candidate.leftMajor;
    const bool signDxHighDy = (words[2] & 0x80000000ULL) != 0U;
    candidate.doOffset = flip == signDxHighDy;
    candidate.level = static_cast<std::uint8_t>((words[0] >> 51U) & 0x07U);
    candidate.tile = static_cast<std::uint8_t>((words[0] >> 48U) & 0x07U);
    candidate.yLow = static_cast<std::uint16_t>(words[0] & 0x3FFFU);
    candidate.yMiddle = static_cast<std::uint16_t>((words[0] >> 16U) & 0x3FFFU);
    candidate.yHigh = static_cast<std::uint16_t>((words[0] >> 32U) & 0x3FFFU);
    candidate.xLeft = signedField(static_cast<std::uint32_t>(words[1] >> 32U), 28U);
    candidate.dxLeftDy = static_cast<std::int32_t>(floorDivide(
        signedField(static_cast<std::uint32_t>(words[1]), 30U), 4));
    candidate.xHigh = signedField(static_cast<std::uint32_t>(words[2] >> 32U), 28U);
    candidate.dxHighDy = static_cast<std::int32_t>(floorDivide(
        signedField(static_cast<std::uint32_t>(words[2]), 30U), 4));
    candidate.xMiddle = signedField(static_cast<std::uint32_t>(words[3] >> 32U), 28U);
    candidate.dxMiddleDy = static_cast<std::int32_t>(floorDivide(
        signedField(static_cast<std::uint32_t>(words[3]), 30U), 4));
    edge = candidate;
    return true;
}

bool RdpCommandDecoder::decodeTriangleCommand(
    const RdpCommandPacket& packet, RdpTriangleCommand& command) noexcept {
    const std::uint8_t opcode =
        static_cast<std::uint8_t>((packet.words[0] >> 56U) & 0x3FU);
    if (!packet.info.knownOpcode || packet.info.opcode != opcode) return false;

    RdpTriangleCommand candidate{};
    if (!decodeTriangleLayout(opcode, candidate.layout) ||
        packet.info.lengthWords != candidate.layout.wordCount) {
        return false;
    }

    std::array<std::uint64_t, 4> edgeWords{};
    for (std::size_t i = 0; i < edgeWords.size(); ++i)
        edgeWords[i] = packet.words[i];
    if (!decodeTriangleEdge(edgeWords, candidate.edge)) return false;

    const auto signedCoefficient = [](std::uint16_t bits) noexcept {
        const std::int32_t value = (bits & 0x8000U) != 0U
            ? static_cast<std::int32_t>(bits) - 0x10000
            : static_cast<std::int32_t>(bits);
        return static_cast<std::int16_t>(value);
    };
    const auto decodeCoefficients = [&signedCoefficient](
        const std::uint64_t* source, std::int16_t* destination,
        std::size_t wordCount, std::size_t fieldsPerWord) noexcept {
        for (std::size_t i = 0; i < wordCount; ++i) {
            for (std::size_t field = 0; field < fieldsPerWord; ++field) {
                const unsigned shift = static_cast<unsigned>(48U - field * 16U);
                const auto bits = static_cast<std::uint16_t>(
                    (source[i] >> shift) & 0xFFFFU);
                destination[i * fieldsPerWord + field] =
                    signedCoefficient(bits);
            }
        }
    };
    if (candidate.layout.hasShade) {
        for (std::size_t i = 0; i < candidate.shadeWords.size(); ++i)
            candidate.shadeWords[i] =
                packet.words[candidate.layout.shadeWordOffset + i];
        decodeCoefficients(candidate.shadeWords.data(),
                           candidate.shadeCoefficients.data(),
                           candidate.shadeWords.size(), 4U);
    }
    if (candidate.layout.hasTexture) {
        for (std::size_t i = 0; i < candidate.textureWords.size(); ++i)
            candidate.textureWords[i] =
                packet.words[candidate.layout.textureWordOffset + i];
        decodeCoefficients(candidate.textureWords.data(),
                           candidate.textureCoefficients.data(),
                           candidate.textureWords.size(), 3U);
    }
    if (candidate.layout.hasZ) {
        for (std::size_t i = 0; i < candidate.zWords.size(); ++i)
            candidate.zWords[i] = packet.words[candidate.layout.zWordOffset + i];
        decodeCoefficients(candidate.zWords.data(),
                           candidate.zCoefficients.data(),
                           candidate.zWords.size(), 4U);
    }
    const auto copyGroup = [](auto& destination, const auto& source,
                              std::size_t offset) noexcept {
        for (std::size_t i = 0; i < destination.size(); ++i)
            destination[i] = source[offset + i];
    };
    if (candidate.layout.hasShade) {
        copyGroup(candidate.shade.rgba, candidate.shadeCoefficients, 0U);
        copyGroup(candidate.shade.drgbaDx, candidate.shadeCoefficients, 4U);
        copyGroup(candidate.shade.rgbaFraction, candidate.shadeCoefficients, 8U);
        copyGroup(candidate.shade.drgbaDxFraction, candidate.shadeCoefficients, 12U);
        copyGroup(candidate.shade.drgbaDe, candidate.shadeCoefficients, 16U);
        copyGroup(candidate.shade.drgbaDy, candidate.shadeCoefficients, 20U);
        copyGroup(candidate.shade.drgbaDeFraction, candidate.shadeCoefficients, 24U);
        copyGroup(candidate.shade.drgbaDyFraction, candidate.shadeCoefficients, 28U);
        const auto fixedPoint = [](std::int32_t integer, std::uint16_t fraction) noexcept {
            return integer * 65536 + static_cast<std::int32_t>(fraction);
        };
        for (std::size_t i = 0; i < 4U; ++i) {
            const std::int32_t baseInteger = signExtend9(
                static_cast<std::uint16_t>(candidate.shade.rgba[i]));
            candidate.shade.rgbaFixed[i] = fixedPoint(
                baseInteger, static_cast<std::uint16_t>(candidate.shade.rgbaFraction[i]));
            candidate.shade.drgbaDxFixed[i] = fixedPoint(
                candidate.shade.drgbaDx[i],
                static_cast<std::uint16_t>(candidate.shade.drgbaDxFraction[i]));
            candidate.shade.drgbaDeFixed[i] = fixedPoint(
                candidate.shade.drgbaDe[i],
                static_cast<std::uint16_t>(candidate.shade.drgbaDeFraction[i]));
            candidate.shade.drgbaDyFixed[i] = fixedPoint(
                candidate.shade.drgbaDy[i],
                static_cast<std::uint16_t>(candidate.shade.drgbaDyFraction[i]));
        }
    }
    if (candidate.layout.hasTexture) {
        copyGroup(candidate.texture.stw, candidate.textureCoefficients, 0U);
        copyGroup(candidate.texture.dstwDx, candidate.textureCoefficients, 3U);
        copyGroup(candidate.texture.stwFraction, candidate.textureCoefficients, 6U);
        copyGroup(candidate.texture.dstwDxFraction, candidate.textureCoefficients, 9U);
        copyGroup(candidate.texture.dstwDe, candidate.textureCoefficients, 12U);
        copyGroup(candidate.texture.dstwDy, candidate.textureCoefficients, 15U);
        copyGroup(candidate.texture.dstwDeFraction, candidate.textureCoefficients, 18U);
        copyGroup(candidate.texture.dstwDyFraction, candidate.textureCoefficients, 21U);
    }
    if (candidate.layout.hasZ) {
        candidate.depth.z = candidate.zCoefficients[0];
        candidate.depth.zFraction = candidate.zCoefficients[1];
        candidate.depth.dzDx = candidate.zCoefficients[2];
        candidate.depth.dzDxFraction = candidate.zCoefficients[3];
        candidate.depth.dzDe = candidate.zCoefficients[4];
        candidate.depth.dzDeFraction = candidate.zCoefficients[5];
        candidate.depth.dzDy = candidate.zCoefficients[6];
        candidate.depth.dzDyFraction = candidate.zCoefficients[7];
    }
    command = candidate;
    return true;
}

bool RdpCommandDecoder::decodeTextureRectangle(
    const RdpCommandPacket& packet, RdpTextureRectangleCommand& command) noexcept {
    if ((packet.info.opcode != 0x24U && packet.info.opcode != 0x25U) ||
        !packet.info.knownOpcode || packet.info.lengthWords != 2U ||
        static_cast<std::uint8_t>((packet.words[0] >> 56U) & 0x3FU) != packet.info.opcode) {
        return false;
    }

    const std::uint64_t first = packet.words[0];
    const std::uint64_t second = packet.words[1];
    const auto signed16 = [](std::uint64_t value) noexcept {
        const std::uint16_t bits = static_cast<std::uint16_t>(value & 0xFFFFU);
        const std::int32_t signedValue = (bits & 0x8000U) != 0U
            ? static_cast<std::int32_t>(bits) - 0x10000
            : static_cast<std::int32_t>(bits);
        return static_cast<std::int16_t>(signedValue);
    };

    RdpTextureRectangleCommand candidate{};
    candidate.xh = static_cast<std::uint16_t>((first >> 44U) & 0x0FFFU);
    candidate.yh = static_cast<std::uint16_t>((first >> 32U) & 0x0FFFU);
    candidate.tile = static_cast<std::uint8_t>((first >> 24U) & 0x07U);
    candidate.xl = static_cast<std::uint16_t>((first >> 12U) & 0x0FFFU);
    candidate.yl = static_cast<std::uint16_t>(first & 0x0FFFU);
    candidate.s = signed16(second >> 48U);
    candidate.t = signed16(second >> 32U);
    candidate.dsdx = signed16(second >> 16U);
    candidate.dtdy = signed16(second);
    candidate.flip = packet.info.opcode == 0x25U;
    candidate.rawWords = {{first, second}};
    command = candidate;
    return true;
}

bool RdpCommandDecoder::decodeTriangleLayout(
    std::uint8_t opcode, RdpTriangleLayout& layout) noexcept {
    if (opcode < 0x08U || opcode > 0x0FU) return false;

    const bool shade = (opcode & 0x04U) != 0U;
    const bool texture = (opcode & 0x02U) != 0U;
    const bool z = (opcode & 0x01U) != 0U;
    std::uint8_t offset = 4U;

    RdpTriangleLayout candidate{};
    candidate.hasShade = shade;
    candidate.hasTexture = texture;
    candidate.hasZ = z;
    if (shade) {
        candidate.shadeWordOffset = offset;
        offset = static_cast<std::uint8_t>(offset + 8U);
    }
    if (texture) {
        candidate.textureWordOffset = offset;
        offset = static_cast<std::uint8_t>(offset + 8U);
    }
    if (z) {
        candidate.zWordOffset = offset;
        offset = static_cast<std::uint8_t>(offset + 2U);
    }
    candidate.wordCount = offset;
    layout = candidate;
    return true;
}

RdpCommandInfo RdpCommandDecoder::decode(std::uint64_t header) noexcept {
    const auto opcode = static_cast<std::uint8_t>((header >> 56U) & 0x3FU);
    std::uint8_t words = 1U;

    // Triangle commands contain a four-word base followed by optional
    // shade (8), texture (8), and Z (2) coefficient blocks.
    if (opcode >= 0x08U && opcode <= 0x0FU) {
        words = static_cast<std::uint8_t>(
            4U + ((opcode & 0x04U) != 0U ? 8U : 0U) +
            ((opcode & 0x02U) != 0U ? 8U : 0U) +
            ((opcode & 0x01U) != 0U ? 2U : 0U));
    } else if (opcode == 0x24U || opcode == 0x25U) {
        // Texture Rectangle and Texture Rectangle Flip.
        words = 2U;
    }

    // All six-bit opcode values have a defined framing length. This flag
    // distinguishes currently documented command families from reserved
    // encodings; unknown commands still consume their one-word header.
    const bool known =
        (opcode >= 0x08U && opcode <= 0x0FU) ||
        opcode == 0x24U || opcode == 0x25U ||
        (opcode >= 0x26U && opcode <= 0x30U) || opcode == 0x32U ||
        (opcode >= 0x33U && opcode <= 0x3FU);
    return {opcode, words, known};
}

bool RdpCommandStream::fetchNext(Bus& bus, RdpCommandPacket& packet) noexcept {
    std::uint64_t header = 0;
    if (!bus.peekDpCommand(header)) return false;

    const RdpCommandInfo info = RdpCommandDecoder::decode(header);
    if (info.lengthWords == 0U || info.lengthWords > RdpCommandPacket::kMaxWords)
        return false;

    RdpCommandPacket candidate{};
    candidate.info = info;
    if (!bus.readDpCommandWords(candidate.words.data(), info.lengthWords))
        return false;

    packet = candidate;
    return true;
}

RdpProcessResult RdpCommandProcessor::processNext(
    Bus& bus, RdpSyncState& state) noexcept {
    RdpCommandPacket packet{};
    if (!RdpCommandStream::fetchNext(bus, packet))
        return RdpProcessResult::NoCommand;

    switch (packet.info.opcode) {
    case 0x24U: // TEXTURE_RECTANGLE
    case 0x25U: { // TEXTURE_RECTANGLE_FLIP
        RdpTextureRectangleCommand rectangle{};
        if (!RdpCommandDecoder::decodeTextureRectangle(packet, rectangle))
            return RdpProcessResult::Unsupported;
        state.lastTextureRectangle = rectangle;
        state.hasTextureRectangle = true;
        return rasterizeTextureRectangleRgba16(bus, state, rectangle)
            ? RdpProcessResult::Processed : RdpProcessResult::Unsupported;
    }
    case 0x08U: // TRIANGLE
    case 0x09U:
    case 0x0AU:
    case 0x0BU:
    case 0x0CU:
    case 0x0DU:
    case 0x0EU:
    case 0x0FU: {
        RdpTriangleCommand triangle{};
        if (!RdpCommandDecoder::decodeTriangleCommand(packet, triangle))
            return RdpProcessResult::Unsupported;
        state.lastTriangle = triangle;
        state.hasTriangle = true;
        // The initial flat-fill path is deliberately limited to opcode 0x08.
        // Shading, texturing and Z-buffer variants remain explicitly unsupported.
        if (packet.info.opcode != 0x08U) return RdpProcessResult::Unsupported;
        return fillTriangle(bus, state, triangle)
            ? RdpProcessResult::Processed : RdpProcessResult::Unsupported;
    }
    case 0x26U: // SYNC_LOAD
        ++state.syncLoadCount;
        return RdpProcessResult::Processed;
    case 0x27U: // SYNC_PIPE
        ++state.syncPipeCount;
        return RdpProcessResult::Processed;
    case 0x28U: // SYNC_TILE
        ++state.syncTileCount;
        return RdpProcessResult::Processed;
    case 0x29U: // SYNC_FULL
        ++state.syncFullCount;
        // MI interrupt source 5 is the Display Processor.
        bus.setMiInterrupt(5U, true);
        return RdpProcessResult::Processed;
    case 0x30U: // LOAD_TLUT
    case 0x33U: // LOAD_BLOCK
    case 0x34U: { // LOAD_TILE
        const std::uint64_t command = packet.words[0];
        const std::uint8_t tile = static_cast<std::uint8_t>((command >> 24U) & 0x07U);
        RdpTextureLoadState load{};
        load.kind = packet.info.opcode == 0x30U ? RdpTextureLoadKind::Tlut
            : (packet.info.opcode == 0x33U ? RdpTextureLoadKind::Block
                                          : RdpTextureLoadKind::Tile);
        load.tile = tile;
        load.sl = static_cast<std::uint16_t>((command >> 44U) & 0x0FFFU);
        load.tl = static_cast<std::uint16_t>((command >> 32U) & 0x0FFFU);
        load.sh = static_cast<std::uint16_t>((command >> 12U) & 0x0FFFU);
        if (packet.info.opcode == 0x33U) {
            load.dxt = static_cast<std::uint16_t>(command & 0x0FFFU);
        } else {
            load.th = static_cast<std::uint16_t>(command & 0x0FFFU);
        }
        load.rawCommand = command;
        state.lastTextureLoad = load;
        state.hasTextureLoad = true;
        if (load.kind == RdpTextureLoadKind::Tile) {
            if (!loadTileTexels(bus, state, load)) {
                return RdpProcessResult::Unsupported;
            }
            // LOAD_TILE updates the tile's SL/TL/SH/TH registers just like
            // the other texture-load commands. SET_TILE_SIZE may replace
            // these coordinates afterward for sampling.
            state.tileSizes[load.tile] = {load.sl, load.tl, load.sh, load.th};
            state.hasTileSize[load.tile] = true;
        }
        if (load.kind == RdpTextureLoadKind::Block &&
            !loadBlockTexels(bus, state, load)) {
            return RdpProcessResult::Unsupported;
        }
        if (load.kind == RdpTextureLoadKind::Tlut &&
            !loadTlutEntries(bus, state, load)) {
            return RdpProcessResult::Unsupported;
        }
        return RdpProcessResult::Processed;
    }
    case 0x2AU: { // SET_KEY_GB
        const std::uint64_t command = packet.words[0];
        state.keyGbCommand = command;
        state.keyGb = {
            static_cast<std::uint16_t>((command >> 44U) & 0x0FFFU),
            static_cast<std::uint16_t>((command >> 32U) & 0x0FFFU),
            static_cast<std::uint8_t>((command >> 24U) & 0xFFU),
            static_cast<std::uint8_t>((command >> 16U) & 0xFFU),
            static_cast<std::uint8_t>((command >> 8U) & 0xFFU),
            static_cast<std::uint8_t>(command & 0xFFU)
        };
        state.hasKeyGb = true;
        return RdpProcessResult::Processed;
    }
    case 0x2BU: { // SET_KEY_R
        const std::uint64_t command = packet.words[0];
        state.keyRCommand = command;
        state.keyR = {
            static_cast<std::uint16_t>((command >> 16U) & 0x0FFFU),
            static_cast<std::uint8_t>((command >> 8U) & 0xFFU),
            static_cast<std::uint8_t>(command & 0xFFU)
        };
        state.hasKeyR = true;
        return RdpProcessResult::Processed;
    }
    case 0x2CU: { // SET_CONVERT
        const std::uint64_t command = packet.words[0];
        state.convertCommand = command;
        state.convert.coefficients = {{
            signExtend9(command >> 45U), signExtend9(command >> 36U),
            signExtend9(command >> 27U), signExtend9(command >> 18U),
            signExtend9(command >> 9U), signExtend9(command)
        }};
        state.hasConvert = true;
        return RdpProcessResult::Processed;
    }
    case 0x2EU: { // SET_PRIM_DEPTH
        const std::uint64_t command = packet.words[0];
        state.primDepthCommand = command;
        state.primDepth = static_cast<std::uint16_t>((command >> 16U) & 0xFFFFU);
        state.primDeltaZ = static_cast<std::uint16_t>(command & 0xFFFFU);
        state.hasPrimDepth = true;
        return RdpProcessResult::Processed;
    }
    case 0x2DU: { // SET_SCISSOR
        const std::uint64_t command = packet.words[0];
        state.scissorCommand = command;
        state.scissor = {
            static_cast<std::uint16_t>((command >> 44U) & 0x0FFFU),
            static_cast<std::uint16_t>((command >> 32U) & 0x0FFFU),
            static_cast<std::uint16_t>((command >> 12U) & 0x0FFFU),
            static_cast<std::uint16_t>(command & 0x0FFFU),
            ((command >> 25U) & 1U) != 0U,
            ((command >> 24U) & 1U) != 0U
        };
        state.hasScissor = true;
        return RdpProcessResult::Processed;
    }
    case 0x2FU: { // SET_OTHER_MODES
        const std::uint64_t command = packet.words[0];
        state.otherModesCommand = command;
        state.otherModes = {
            static_cast<std::uint8_t>((command >> 52U) & 0x03U),
            ((command >> 51U) & 1U) != 0U,
            ((command >> 50U) & 1U) != 0U,
            ((command >> 49U) & 1U) != 0U,
            ((command >> 48U) & 1U) != 0U,
            ((command >> 47U) & 1U) != 0U,
            ((command >> 46U) & 1U) != 0U,
            ((command >> 45U) & 1U) != 0U,
            ((command >> 44U) & 1U) != 0U,
            ((command >> 43U) & 1U) != 0U,
            ((command >> 42U) & 1U) != 0U,
            ((command >> 41U) & 1U) != 0U,
            ((command >> 40U) & 1U) != 0U,
            static_cast<std::uint8_t>((command >> 38U) & 0x03U),
            static_cast<std::uint8_t>((command >> 36U) & 0x03U),
            {{static_cast<std::uint8_t>((command >> 30U) & 0x03U),
              static_cast<std::uint8_t>((command >> 28U) & 0x03U)}},
            {{static_cast<std::uint8_t>((command >> 26U) & 0x03U),
              static_cast<std::uint8_t>((command >> 24U) & 0x03U)}},
            {{static_cast<std::uint8_t>((command >> 22U) & 0x03U),
              static_cast<std::uint8_t>((command >> 20U) & 0x03U)}},
            {{static_cast<std::uint8_t>((command >> 18U) & 0x03U),
              static_cast<std::uint8_t>((command >> 16U) & 0x03U)}},
            ((command >> 14U) & 1U) != 0U,
            ((command >> 13U) & 1U) != 0U,
            ((command >> 12U) & 1U) != 0U,
            static_cast<std::uint8_t>((command >> 10U) & 0x03U),
            static_cast<std::uint8_t>((command >> 8U) & 0x03U),
            ((command >> 7U) & 1U) != 0U,
            ((command >> 6U) & 1U) != 0U,
            ((command >> 5U) & 1U) != 0U,
            ((command >> 4U) & 1U) != 0U,
            ((command >> 3U) & 1U) != 0U,
            ((command >> 2U) & 1U) != 0U,
            ((command >> 1U) & 1U) != 0U,
            (command & 1U) != 0U
        };
        state.hasOtherModes = true;
        return RdpProcessResult::Processed;
    }
    case 0x3CU: { // SET_COMBINE
        const std::uint64_t command = packet.words[0];
        state.combineCommand = command;
        state.combine[0] = {
            static_cast<std::uint8_t>((command >> 52U) & 0x0FU),
            static_cast<std::uint8_t>((command >> 28U) & 0x0FU),
            static_cast<std::uint8_t>((command >> 47U) & 0x1FU),
            static_cast<std::uint8_t>((command >> 15U) & 0x07U),
            static_cast<std::uint8_t>((command >> 44U) & 0x07U),
            static_cast<std::uint8_t>((command >> 12U) & 0x07U),
            static_cast<std::uint8_t>((command >> 41U) & 0x07U),
            static_cast<std::uint8_t>((command >> 9U) & 0x07U)
        };
        state.combine[1] = {
            static_cast<std::uint8_t>((command >> 37U) & 0x0FU),
            static_cast<std::uint8_t>((command >> 24U) & 0x0FU),
            static_cast<std::uint8_t>((command >> 32U) & 0x1FU),
            static_cast<std::uint8_t>((command >> 6U) & 0x07U),
            static_cast<std::uint8_t>((command >> 21U) & 0x07U),
            static_cast<std::uint8_t>((command >> 3U) & 0x07U),
            static_cast<std::uint8_t>((command >> 18U) & 0x07U),
            static_cast<std::uint8_t>(command & 0x07U)
        };
        state.hasCombine = true;
        return RdpProcessResult::Processed;
    }
    case 0x32U: { // SET_TILE_SIZE
        const std::uint64_t command = packet.words[0];
        const std::size_t tile = static_cast<std::size_t>((command >> 24U) & 0x07U);
        state.tileSizeCommands[tile] = command;
        state.tileSizes[tile] = {
            static_cast<std::uint16_t>((command >> 44U) & 0x0FFFU),
            static_cast<std::uint16_t>((command >> 32U) & 0x0FFFU),
            static_cast<std::uint16_t>((command >> 12U) & 0x0FFFU),
            static_cast<std::uint16_t>(command & 0x0FFFU)
        };
        state.hasTileSize[tile] = true;
        return RdpProcessResult::Processed;
    }
    case 0x35U: { // SET_TILE
        const std::uint64_t command = packet.words[0];
        const std::size_t tile = static_cast<std::size_t>((command >> 24U) & 0x07U);
        state.tileCommands[tile] = command;
        state.tiles[tile] = {
            static_cast<std::uint8_t>((command >> 53U) & 0x07U),
            static_cast<std::uint8_t>((command >> 51U) & 0x03U),
            static_cast<std::uint16_t>((command >> 41U) & 0x01FFU),
            static_cast<std::uint16_t>((command >> 32U) & 0x01FFU),
            static_cast<std::uint8_t>((command >> 20U) & 0x0FU),
            ((command >> 19U) & 1U) != 0U,
            ((command >> 18U) & 1U) != 0U,
            static_cast<std::uint8_t>((command >> 14U) & 0x0FU),
            static_cast<std::uint8_t>((command >> 10U) & 0x0FU),
            ((command >> 9U) & 1U) != 0U,
            ((command >> 8U) & 1U) != 0U,
            static_cast<std::uint8_t>((command >> 4U) & 0x0FU),
            static_cast<std::uint8_t>(command & 0x0FU)
        };
        state.hasTile[tile] = true;
        return RdpProcessResult::Processed;
    }
    case 0x36U: // FILL_RECTANGLE
        return fillRectangle(bus, state, packet.words[0])
            ? RdpProcessResult::Processed : RdpProcessResult::Unsupported;
    case 0x37U: // SET_FILL_COLOR
    case 0x38U: // SET_FOG_COLOR
    case 0x39U: // SET_BLEND_COLOR
    case 0x3AU: // SET_PRIM_COLOR
    case 0x3BU: { // SET_ENV_COLOR
        const std::uint64_t command = packet.words[0];
        const std::uint32_t color = static_cast<std::uint32_t>(command & 0xFFFFFFFFU);
        switch (packet.info.opcode) {
        case 0x37U:
            state.fillColorCommand = command;
            state.fillColor = color;
            state.hasFillColor = true;
            break;
        case 0x38U:
            state.fogColorCommand = command;
            state.fogColor = color;
            state.hasFogColor = true;
            break;
        case 0x39U:
            state.blendColorCommand = command;
            state.blendColor = color;
            state.hasBlendColor = true;
            break;
        case 0x3AU:
            state.primColorCommand = command;
            state.primColor = color;
            state.primLodMinimum = static_cast<std::uint8_t>((command >> 40U) & 0x1FU);
            state.primLodFraction = static_cast<std::uint8_t>((command >> 32U) & 0xFFU);
            state.hasPrimColor = true;
            break;
        default:
            state.envColorCommand = command;
            state.envColor = color;
            state.hasEnvColor = true;
            break;
        }
        return RdpProcessResult::Processed;
    }
    case 0x3EU: // SET_Z_IMAGE
        state.zImageCommand = packet.words[0];
        state.zImageAddress = static_cast<std::uint32_t>(packet.words[0] & 0x03FFFFFFU);
        state.hasZImage = true;
        return RdpProcessResult::Processed;
    case 0x3DU: // SET_TEXTURE_IMAGE
    case 0x3FU: { // SET_COLOR_IMAGE
        const std::uint64_t command = packet.words[0];
        const RdpImageState image{
            static_cast<std::uint8_t>((command >> 53U) & 0x07U),
            static_cast<std::uint8_t>((command >> 51U) & 0x03U),
            static_cast<std::uint16_t>(((command >> 32U) & 0x03FFU) + 1U),
            static_cast<std::uint32_t>(command & 0x03FFFFFFU)
        };
        if (packet.info.opcode == 0x3DU) {
            state.textureImageCommand = command;
            state.textureImage = image;
            state.hasTextureImage = true;
        } else {
            state.colorImageCommand = command;
            state.colorImage = image;
            state.hasColorImage = true;
        }
        return RdpProcessResult::Processed;
    }
    default:
        return RdpProcessResult::Unsupported;
    }
}

} // namespace m64plus::n64
