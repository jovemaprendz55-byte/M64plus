#include "n64/rdp.h"
#include "n64/bus.h"

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::uint64_t header(std::uint8_t opcode) {
    return static_cast<std::uint64_t>(opcode) << 56U;
}

void testSingleWordCommands() {
    for (std::uint8_t opcode = 0; opcode < 64U; ++opcode) {
        if ((opcode >= 0x08U && opcode <= 0x0FU) ||
            opcode == 0x24U || opcode == 0x25U) continue;
        const auto decoded = m64plus::n64::RdpCommandDecoder::decode(header(opcode));
        require(decoded.opcode == opcode, "opcode extraction");
        require(decoded.lengthWords == 1U, "single-word command length");
        const bool documented = (opcode >= 0x08U && opcode <= 0x0FU) || opcode >= 0x24U;
        require(decoded.knownOpcode == documented, "documented opcode classification");
    }
}

void testTriangleCommandLengths() {
    const std::uint8_t expected[] = {4U, 6U, 12U, 14U, 12U, 14U, 20U, 22U};
    for (std::uint8_t flags = 0; flags < 8U; ++flags) {
        const auto decoded = m64plus::n64::RdpCommandDecoder::decode(
            header(static_cast<std::uint8_t>(0x08U + flags)));
        require(decoded.lengthWords == expected[flags],
                "triangle coefficient blocks determine full length");
    }
}

void testDpcCommandStreamIntegration() {
    m64plus::n64::Bus bus;
    m64plus::n64::RdpCommandPacket packet{};
    packet.info.opcode = 0x3FU;
    packet.words[0] = 0xDEADBEEFDEADBEEFULL;

    const std::uint64_t header = 0x2400000012345678ULL;
    const std::uint64_t payload = 0x9ABCDEF001020304ULL;
    require(bus.write64(0x400U, header), "seed DPC texture rectangle header");
    require(bus.write64(0x408U, payload), "seed DPC texture rectangle payload");
    require(bus.write32(m64plus::n64::Bus::kDpStart, 0x400U),
            "set DPC start for RDP stream");
    require(bus.write32(m64plus::n64::Bus::kDpEnd, 0x408U),
            "publish incomplete RDP command");

    require(!m64plus::n64::RdpCommandStream::fetchNext(bus, packet),
            "RDP stream waits for complete multiword command");
    std::uint32_t current = 0;
    require(bus.read32(m64plus::n64::Bus::kDpCurrent, current) && current == 0x400U,
            "incomplete RDP command leaves DPC cursor unchanged");
    require(packet.info.opcode == 0x3FU &&
            packet.words[0] == 0xDEADBEEFDEADBEEFULL,
            "failed fetch leaves caller packet unchanged");

    require(bus.write32(m64plus::n64::Bus::kDpEnd, 0x410U),
            "publish complete RDP command");
    require(m64plus::n64::RdpCommandStream::fetchNext(bus, packet),
            "RDP stream fetches complete texture rectangle");
    require(packet.info.opcode == 0x24U && packet.info.lengthWords == 2U &&
            packet.info.knownOpcode,
            "RDP stream returns decoded command metadata");
    require(packet.words[0] == header && packet.words[1] == payload,
            "RDP stream returns all command words");
    require(bus.read32(m64plus::n64::Bus::kDpCurrent, current) && current == 0x410U,
            "RDP stream advances DPC cursor by complete command length");
}

void testRdpSynchronizationCommands() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    const std::uint64_t commands[] = {
        static_cast<std::uint64_t>(0x26U) << 56U,
        static_cast<std::uint64_t>(0x27U) << 56U,
        static_cast<std::uint64_t>(0x28U) << 56U,
        static_cast<std::uint64_t>(0x29U) << 56U
    };
    for (std::size_t i = 0; i < 4U; ++i)
        require(bus.write64(0x500U + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "seed RDP synchronization command");
    require(bus.write32(Bus::kDpStart, 0x500U), "set DPC start for sync commands");
    require(bus.write32(Bus::kDpEnd, 0x520U), "publish four sync commands");

    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "SYNC_LOAD is processed");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "SYNC_PIPE is processed");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "SYNC_TILE is processed");
    std::uint32_t mi = 0;
    require(bus.read32(Bus::kMiInterrupt, mi) && (mi & (1U << 5U)) == 0U,
            "DP interrupt is not asserted before SYNC_FULL");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "SYNC_FULL is processed");
    require(state.syncLoadCount == 1U && state.syncPipeCount == 1U &&
            state.syncTileCount == 1U && state.syncFullCount == 1U,
            "all synchronization commands update their counters");
    require(bus.read32(Bus::kMiInterrupt, mi) && (mi & (1U << 5U)) != 0U,
            "SYNC_FULL asserts MI DP interrupt");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::NoCommand,
            "empty DPC stream reports no command");
}

void testRdpBasicStateCommands() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    const std::uint64_t commands[] = {
        (static_cast<std::uint64_t>(0x2DU) << 56U) | 0x0000000012345678ULL,
        (static_cast<std::uint64_t>(0x2FU) << 56U) | 0x0000000087654321ULL,
        (static_cast<std::uint64_t>(0x32U) << 56U) | (3ULL << 24U) | 0x1234ULL,
        (static_cast<std::uint64_t>(0x35U) << 56U) | (3ULL << 24U) | 0x5678ULL,
        (static_cast<std::uint64_t>(0x37U) << 56U) | 0xAABBCCDDULL,
        (static_cast<std::uint64_t>(0x3DU) << 56U) | 0x01020304ULL,
        (static_cast<std::uint64_t>(0x3FU) << 56U) | 0x05060708ULL
    };
    for (std::size_t i = 0; i < 7U; ++i)
        require(bus.write64(0x700U + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "seed basic RDP state command");
    require(bus.write32(Bus::kDpStart, 0x700U), "set DPC start for state commands");
    require(bus.write32(Bus::kDpEnd, 0x738U), "publish basic state commands");
    for (std::size_t i = 0; i < 7U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "basic RDP state command is dispatched");

    require(state.hasScissor && state.scissorCommand == commands[0],
            "SET_SCISSOR raw command retained");
    require(state.hasOtherModes && state.otherModesCommand == commands[1],
            "SET_OTHER_MODES raw command retained");
    require(state.hasTileSize[3] && state.tileSizeCommands[3] == commands[2],
            "SET_TILE_SIZE retained for selected tile");
    require(state.hasTile[3] && state.tileCommands[3] == commands[3],
            "SET_TILE retained for selected tile");
    require(state.hasFillColor && state.fillColorCommand == commands[4],
            "SET_FILL_COLOR raw command retained");
    require(state.hasTextureImage && state.textureImageCommand == commands[5],
            "SET_TEXTURE_IMAGE raw command retained");
    require(state.hasColorImage && state.colorImageCommand == commands[6],
            "SET_COLOR_IMAGE raw command retained");
}


void testRdpStateFieldDecoding() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};

    const std::uint64_t scissor =
        (0x2DULL << 56U) | (0x123ULL << 44U) | (0x234ULL << 32U) |
        (1ULL << 25U) | (1ULL << 24U) | (0x345ULL << 12U) | 0x456ULL;
    const std::uint64_t tile =
        (0x35ULL << 56U) | (5ULL << 53U) | (2ULL << 51U) |
        (0x101ULL << 41U) | (0x155ULL << 32U) | (6ULL << 24U) |
        (0xAULL << 20U) | (1ULL << 19U) | (1ULL << 18U) |
        (0xBULL << 14U) | (0xCULL << 10U) | (1ULL << 9U) |
        (1ULL << 8U) | (0xDULL << 4U) | 0xEULL;
    const std::uint64_t tileSize =
        (0x32ULL << 56U) | (0x111ULL << 44U) | (0x222ULL << 32U) |
        (6ULL << 24U) | (0x333ULL << 12U) | 0x444ULL;
    const std::uint64_t textureImage =
        (0x3DULL << 56U) | (3ULL << 53U) | (2ULL << 51U) |
        (0x155ULL << 32U) | 0x01234567ULL;
    const std::uint64_t colorImage =
        (0x3FULL << 56U) | (4ULL << 53U) | (3ULL << 51U) |
        (0x2AAULL << 32U) | 0x02345678ULL;
    const std::uint64_t commands[] = {scissor, tile, tileSize, textureImage, colorImage};

    for (std::size_t i = 0; i < 5U; ++i)
        require(bus.write64(0x780U + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "seed RDP field-decoding commands");
    require(bus.write32(Bus::kDpStart, 0x780U), "set DPC start for field decoding");
    require(bus.write32(Bus::kDpEnd, 0x7A8U), "publish field-decoding commands");
    for (std::size_t i = 0; i < 5U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "RDP field-decoding command is processed");

    require(state.scissor.xh == 0x123U && state.scissor.yh == 0x234U &&
            state.scissor.xl == 0x345U && state.scissor.yl == 0x456U &&
            state.scissor.fieldEnable && state.scissor.oddLine,
            "SET_SCISSOR extracts 10.2 coordinates and field flags");
    require(state.tiles[6].format == 5U && state.tiles[6].size == 2U &&
            state.tiles[6].line == 0x101U && state.tiles[6].tmem == 0x155U &&
            state.tiles[6].palette == 0xAU && state.tiles[6].clampT &&
            state.tiles[6].mirrorT && state.tiles[6].maskT == 0xBU &&
            state.tiles[6].shiftT == 0xCU && state.tiles[6].clampS &&
            state.tiles[6].mirrorS && state.tiles[6].maskS == 0xDU &&
            state.tiles[6].shiftS == 0xEU,
            "SET_TILE extracts descriptor, TMEM and S/T sampling fields");
    require(state.tileSizes[6].sl == 0x111U && state.tileSizes[6].tl == 0x222U &&
            state.tileSizes[6].sh == 0x333U && state.tileSizes[6].th == 0x444U,
            "SET_TILE_SIZE extracts four 10.2 coordinates");
    require(state.textureImage.format == 3U && state.textureImage.size == 2U &&
            state.textureImage.width == 0x156U &&
            state.textureImage.address == 0x01234567U,
            "SET_TEXTURE_IMAGE extracts format, size, width plus one and address");
    require(state.colorImage.format == 4U && state.colorImage.size == 3U &&
            state.colorImage.width == 0x2ABU &&
            state.colorImage.address == 0x02345678U,
            "SET_COLOR_IMAGE extracts format, size, width plus one and address");
}


void testRdpOtherModesAndCombineDecoding() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    const std::uint64_t other =
        (0x2FULL << 56U) | (2ULL << 52U) | (1ULL << 51U) |
        (1ULL << 49U) | (1ULL << 47U) | (1ULL << 45U) |
        (1ULL << 43U) | (1ULL << 40U) | (2ULL << 38U) |
        (1ULL << 36U) | (2ULL << 30U) | (1ULL << 28U) |
        (3ULL << 26U) | (2ULL << 24U) | (1ULL << 22U) |
        (3ULL << 20U) | (2ULL << 18U) | (1ULL << 16U) |
        (1ULL << 14U) | (1ULL << 13U) | (1ULL << 12U) |
        (2ULL << 10U) | (3ULL << 8U) | (1ULL << 7U) |
        (1ULL << 6U) | (1ULL << 5U) | (1ULL << 4U) |
        (1ULL << 3U) | (1ULL << 2U) | (1ULL << 1U) | 1ULL;
    const std::uint64_t combine =
        (0x3CULL << 56U) | (0xAULL << 52U) | (0x15ULL << 47U) |
        (0x5ULL << 44U) | (0x3ULL << 41U) | (0x6ULL << 37U) |
        (0x1BULL << 32U) | (0xCULL << 28U) | (0x9ULL << 24U) |
        (0x5ULL << 21U) | (0x2ULL << 18U) | (0x4ULL << 15U) |
        (0x3ULL << 12U) | (0x6ULL << 9U) | (0x5ULL << 6U) |
        (0x2ULL << 3U) | 0x7ULL;
    require(bus.write64(0x7C0U, other), "seed SET_OTHER_MODES command");
    require(bus.write64(0x7C8U, combine), "seed SET_COMBINE command");
    require(bus.write32(Bus::kDpStart, 0x7C0U), "set DPC start for modes/combine");
    require(bus.write32(Bus::kDpEnd, 0x7D0U), "publish modes/combine commands");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "SET_OTHER_MODES is decoded");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "SET_COMBINE is decoded");

    const auto& m = state.otherModes;
    require(m.cycleType == 2U && m.perspectiveTexture && m.sharpenTexture &&
            m.textureLut && m.sampleType && m.biLerp0 && m.keyEnable,
            "SET_OTHER_MODES decodes upper texture and cycle controls");
    require(m.rgbDither == 2U && m.alphaDither == 1U &&
            m.blendM1a[0] == 2U && m.blendM1a[1] == 1U &&
            m.blendM1b[0] == 3U && m.blendM1b[1] == 2U &&
            m.blendM2a[0] == 1U && m.blendM2a[1] == 3U &&
            m.blendM2b[0] == 2U && m.blendM2b[1] == 1U,
            "SET_OTHER_MODES decodes dither and both blender-cycle muxes");
    require(m.forceBlend && m.alphaCoverageSelect && m.coverageTimesAlpha &&
            m.zMode == 2U && m.coverageDestination == 3U &&
            m.colorOnCoverage && m.imageRead && m.zUpdate && m.zCompare &&
            m.antialias && m.zSourceSelect && m.ditherAlphaEnable && m.alphaCompare,
            "SET_OTHER_MODES decodes blender, coverage, depth and alpha controls");

    const auto& c0 = state.combine[0];
    const auto& c1 = state.combine[1];
    require(c0.colorA == 0xAU && c0.colorB == 0xCU && c0.colorC == 0x15U &&
            c0.colorD == 4U && c0.alphaA == 5U && c0.alphaB == 3U &&
            c0.alphaC == 3U && c0.alphaD == 6U,
            "SET_COMBINE decodes all eight first-cycle selectors");
    require(c1.colorA == 6U && c1.colorB == 9U && c1.colorC == 0x1BU &&
            c1.colorD == 5U && c1.alphaA == 5U && c1.alphaB == 2U &&
            c1.alphaC == 2U && c1.alphaD == 7U,
            "SET_COMBINE decodes all eight second-cycle selectors");
}


void testRdpColorAndDepthStateCommands() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    const std::uint64_t commands[] = {
        (0x37ULL << 56U) | 0x11223344ULL,
        (0x38ULL << 56U) | 0x22334455ULL,
        (0x39ULL << 56U) | 0x33445566ULL,
        (0x3AULL << 56U) | (0x12ULL << 40U) | (0xABULL << 32U) | 0x44556677ULL,
        (0x3BULL << 56U) | 0x55667788ULL,
        (0x3EULL << 56U) | 0x02345678ULL
    };
    for (std::size_t i = 0; i < 6U; ++i)
        require(bus.write64(0x800U + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "seed RDP color/depth state command");
    require(bus.write32(Bus::kDpStart, 0x800U), "set DPC start for color/depth state");
    require(bus.write32(Bus::kDpEnd, 0x830U), "publish color/depth state commands");
    for (std::size_t i = 0; i < 6U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "RDP color/depth state command is processed");

    require(state.hasFillColor && state.fillColor == 0x11223344U,
            "SET_FILL_COLOR extracts packed color");
    require(state.hasFogColor && state.fogColor == 0x22334455U,
            "SET_FOG_COLOR extracts RGBA color");
    require(state.hasBlendColor && state.blendColor == 0x33445566U,
            "SET_BLEND_COLOR extracts RGBA color");
    require(state.hasPrimColor && state.primColor == 0x44556677U &&
            state.primLodMinimum == 0x12U && state.primLodFraction == 0xABU,
            "SET_PRIM_COLOR extracts LOD fields and RGBA color");
    require(state.hasEnvColor && state.envColor == 0x55667788U,
            "SET_ENV_COLOR extracts RGBA color");
    require(state.hasZImage && state.zImageAddress == 0x02345678U,
            "SET_Z_IMAGE extracts 26-bit DRAM address");
}


void testRdpConvertAndKeyStateCommands() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    const std::uint64_t convert = (0x2CULL << 56U) | (0x1FFULL << 45U) | (0x001ULL << 36U) | (0x100ULL << 27U) | (0x0FFULL << 18U) | (0x180ULL << 9U) | 0x07FULL;
    const std::uint64_t keyGb = (0x2AULL << 56U) | (0xABCULL << 44U) | (0x123ULL << 32U) | (0x45ULL << 24U) | (0x67ULL << 16U) | (0x89ULL << 8U) | 0xABULL;
    const std::uint64_t keyR = (0x2BULL << 56U) | (0xDEFULL << 16U) | (0xCDULL << 8U) | 0xEFULL;
    const std::uint64_t commands[] = {convert, keyGb, keyR};
    for (std::size_t i = 0; i < 3U; ++i)
        require(bus.write64(0x840U + static_cast<std::uint32_t>(i * 8U), commands[i]), "seed RDP convert/key command");
    require(bus.write32(Bus::kDpStart, 0x840U), "set DPC start for convert/key");
    require(bus.write32(Bus::kDpEnd, 0x858U), "publish convert/key commands");
    for (std::size_t i = 0; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed, "RDP convert/key command is processed");
    require(state.hasConvert && state.convert.coefficients[0] == -1 && state.convert.coefficients[1] == 1 && state.convert.coefficients[2] == -256 && state.convert.coefficients[3] == 255 && state.convert.coefficients[4] == -128 && state.convert.coefficients[5] == 127, "SET_CONVERT extracts and sign-extends six 9-bit coefficients");
    require(state.convertCommand == convert, "SET_CONVERT retains raw command");
    require(state.hasKeyGb && state.keyGb.widthG == 0xABCU && state.keyGb.widthB == 0x123U && state.keyGb.centerG == 0x45U && state.keyGb.scaleG == 0x67U && state.keyGb.centerB == 0x89U && state.keyGb.scaleB == 0xABU, "SET_KEY_GB extracts green and blue key controls");
    require(state.keyGbCommand == keyGb, "SET_KEY_GB retains raw command");
    require(state.hasKeyR && state.keyR.width == 0xDEFU && state.keyR.center == 0xCDU && state.keyR.scale == 0xEFU, "SET_KEY_R extracts red key controls");
    require(state.keyRCommand == keyR, "SET_KEY_R retains raw command");
}


void testRdpPrimitiveDepthCommand() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    const std::uint64_t command = (0x2EULL << 56U) | (0xBEEFULL << 16U) | 0x8123ULL;
    require(bus.write64(0x860U, command), "seed SET_PRIM_DEPTH command");
    require(bus.write32(Bus::kDpStart, 0x860U), "set DPC start for primitive depth");
    require(bus.write32(Bus::kDpEnd, 0x868U), "publish primitive depth command");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed, "SET_PRIM_DEPTH is processed");
    require(state.hasPrimDepth && state.primDepth == 0xBEEFU && state.primDeltaZ == 0x8123U, "SET_PRIM_DEPTH extracts 16-bit Z and delta-Z registers");
    require(state.primDepthCommand == command, "SET_PRIM_DEPTH retains raw command");
}

void testUnsupportedRdpCommandIsReported() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    require(bus.write64(0x600U, static_cast<std::uint64_t>(0x30U) << 56U),
            "seed unsupported LOAD_TLUT command");
    require(bus.write32(Bus::kDpStart, 0x600U), "set DPC start for unsupported command");
    require(bus.write32(Bus::kDpEnd, 0x608U), "publish unsupported command");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Unsupported,
            "unimplemented command is reported as unsupported");
    std::uint32_t current = 0;
    require(bus.read32(Bus::kDpCurrent, current) && current == 0x608U,
            "unsupported command framing still advances DPC cursor");
}

void testTextureRectangleLengths() {
    require(m64plus::n64::RdpCommandDecoder::decode(header(0x24U)).lengthWords == 2U,
            "texture rectangle has two words");
    require(m64plus::n64::RdpCommandDecoder::decode(header(0x25U)).lengthWords == 2U,
            "texture rectangle flip has two words");
}
} // namespace

int main() {
    testSingleWordCommands();
    testTriangleCommandLengths();
    testTextureRectangleLengths();
    testDpcCommandStreamIntegration();
    testRdpSynchronizationCommands();
    testRdpBasicStateCommands();
    testRdpStateFieldDecoding();
    testRdpOtherModesAndCombineDecoding();
    testRdpColorAndDepthStateCommands();
    testRdpConvertAndKeyStateCommands();
    testRdpPrimitiveDepthCommand();
    testUnsupportedRdpCommandIsReported();
    std::cout << "All RDP decoder host tests passed.\n";
    return 0;
}
