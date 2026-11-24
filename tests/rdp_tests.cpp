#include "n64/rdp.h"
#include "n64/bus.h"
#include "n64/framebuffer.h"

#include <vector>

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

void testMaximumTriangleCommandWaitsForAllWords() {
    using namespace m64plus::n64;
    Bus bus;
    RdpCommandPacket packet{};
    constexpr std::uint32_t start = 0xA00U;
    constexpr std::uint32_t wordCount = 22U;
    for (std::uint32_t i = 0; i < wordCount; ++i) {
        const std::uint64_t value = i == 0U
            ? (0x0FULL << 56U)
            : static_cast<std::uint64_t>(i);
        require(bus.write64(start + i * 8U, value),
                "seed maximum-length triangle command");
    }
    require(bus.write32(Bus::kDpStart, start),
            "set DPC start for maximum triangle command");
    require(bus.write32(Bus::kDpEnd, start + (wordCount - 1U) * 8U),
            "publish all but final triangle command word");
    require(!RdpCommandStream::fetchNext(bus, packet),
            "maximum triangle command waits for its final word");
    std::uint32_t current = 0U;
    require(bus.read32(Bus::kDpCurrent, current) && current == start,
            "incomplete maximum triangle leaves DPC cursor unchanged");

    require(bus.write32(Bus::kDpEnd, start + wordCount * 8U),
            "publish complete maximum triangle command");
    require(RdpCommandStream::fetchNext(bus, packet),
            "maximum triangle command is fetched when all words are available");
    require(packet.info.opcode == 0x0FU && packet.info.lengthWords == wordCount,
            "maximum triangle packet reports its 22-word framing");
    require(packet.words[0] == (0x0FULL << 56U) &&
            packet.words[wordCount - 1U] == wordCount - 1U,
            "maximum triangle packet retains first and final command words");
    require(bus.read32(Bus::kDpCurrent, current) &&
            current == start + wordCount * 8U,
            "complete maximum triangle advances DPC by all 22 words");
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


void testRdpTextureMemory() {
    using namespace m64plus::n64;
    RdpTextureMemory tmem{};
    std::uint8_t byte = 0xFFU;
    require(tmem.readByte(0U, byte) && byte == 0U, "TMEM starts cleared");
    require(tmem.writeByte(0x0123U, 0xA5U), "write TMEM byte");
    require(tmem.readByte(0x0123U, byte) && byte == 0xA5U, "read back TMEM byte");
    constexpr std::uint64_t word = 0x0123456789ABCDEFULL;
    require(tmem.write64(0x0200U, word), "write TMEM 64-bit word");
    std::uint64_t actual = 0U;
    require(tmem.read64(0x0200U, actual) && actual == word, "TMEM 64-bit big-endian round trip");
    constexpr std::uint64_t wrappingWord = 0xFEDCBA9876543210ULL;
    require(tmem.write64(0x0FFCU, wrappingWord), "write TMEM word crossing 4 KiB boundary");
    require(tmem.read64(0x0FFCU, actual) && actual == wrappingWord, "TMEM wraps at 4 KiB boundary");
    require(tmem.writeByte(0x1000U, 0x5AU), "write address beyond TMEM wraps");
    require(tmem.readByte(0U, byte) && byte == 0x5AU, "TMEM 12-bit address mask");
    tmem.reset();
    require(tmem.readByte(0U, byte) && byte == 0U &&
            tmem.readByte(0x0FFCU, byte) && byte == 0U, "TMEM reset clears memory");
}


void testRdpTextureLoadCommandFields() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    const std::uint64_t setImage =
        (0x3DULL << 56U) | (1ULL << 51U) | (63ULL << 32U);
    const std::uint64_t setTile =
        (0x35ULL << 56U) | (8ULL << 41U) | (7ULL << 24U);
    const std::uint64_t commands[] = {
        (0x30ULL << 56U) | (0x012ULL << 44U) | (0x003ULL << 32U) |
            (5ULL << 24U) | (0x045ULL << 12U) | 0x006ULL,
        (0x33ULL << 56U) | (0x021ULL << 44U) | (0x004ULL << 32U) |
            (6ULL << 24U) | (0x087ULL << 12U) | 0x123ULL,
        (0x34ULL << 56U) | (0x031ULL << 44U) | (0x007ULL << 32U) |
            (7ULL << 24U) | (0x098ULL << 12U) | 0x00AULL
    };
    require(bus.write64(0x880U, setImage) && bus.write64(0x888U, setTile),
            "seed image and tile state for LOAD_TILE");
    for (std::size_t i = 0; i < 3U; ++i)
        require(bus.write64(0x890U + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "seed RDP texture load command");
    require(bus.write32(Bus::kDpStart, 0x880U), "set DPC start for texture loads");
    require(bus.write32(Bus::kDpEnd, 0x8A8U), "publish texture load commands");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "SET_TEXTURE_IMAGE is processed");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "SET_TILE is processed");
    const RdpTextureLoadKind expectedKinds[] = {
        RdpTextureLoadKind::Tlut, RdpTextureLoadKind::Block, RdpTextureLoadKind::Tile
    };
    const RdpProcessResult expectedResults[] = {
        RdpProcessResult::Unsupported, RdpProcessResult::Unsupported,
        RdpProcessResult::Processed
    };
    for (std::size_t i = 0; i < 3U; ++i) {
        require(RdpCommandProcessor::processNext(bus, state) == expectedResults[i],
                "texture load command is decoded and execution status is explicit");
        require(state.hasTextureLoad && state.lastTextureLoad.kind == expectedKinds[i],
                "texture load kind is identified");
        require(state.lastTextureLoad.rawCommand == commands[i],
                "texture load command is retained");
    }
    require(state.lastTextureLoad.tile == 7U && state.lastTextureLoad.sl == 0x031U &&
            state.lastTextureLoad.tl == 7U && state.lastTextureLoad.sh == 0x098U &&
            state.lastTextureLoad.th == 0x00AU,
            "LOAD_TILE coordinate and tile fields are decoded");
}

void testLoadTileCopiesTextureImageIntoTmem() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    for (std::uint32_t i = 0; i < 12U; ++i)
        require(bus.write8(0x1000U + i, static_cast<std::uint8_t>(i + 1U)),
                "seed 8-bit source texture");
    const std::uint64_t setImage =
        (0x3DULL << 56U) | (1ULL << 51U) | (3ULL << 32U) | 0x1000ULL;
    const std::uint64_t setTile =
        (0x35ULL << 56U) | (1ULL << 51U) | (1ULL << 41U);
    const std::uint64_t loadTile =
        (0x34ULL << 56U) | (4ULL << 44U) | (4ULL << 32U) |
        (8ULL << 12U) | 8ULL;
    require(bus.write64(0x900U, setImage) &&
            bus.write64(0x908U, setTile) &&
            bus.write64(0x910U, loadTile), "seed LOAD_TILE sequence");
    require(bus.write32(Bus::kDpStart, 0x900U) &&
            bus.write32(Bus::kDpEnd, 0x918U), "publish LOAD_TILE sequence");
    for (unsigned i = 0; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "SET_TEXTURE_IMAGE, SET_TILE and LOAD_TILE execute");
    std::uint8_t value = 0U;
    require(state.tmem.readByte(0U, value) && value == 6U,
            "LOAD_TILE copies selected first-row texel from RDRAM");
    require(state.tmem.readByte(1U, value) && value == 7U,
            "LOAD_TILE copies second texel from first row");
    require(state.tmem.readByte(12U, value) && value == 10U,
            "LOAD_TILE swaps the two 32-bit halves for the odd row");
    require(state.tmem.readByte(13U, value) && value == 11U,
            "LOAD_TILE preserves the odd-row texels after halfword swap");
}


void testLoadTilePacksFourBitNibblesAndSwizzlesOddRows() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    require(bus.write8(0x1700U, 0x12U) &&
            bus.write8(0x1701U, 0x34U) &&
            bus.write8(0x1702U, 0x56U) &&
            bus.write8(0x1703U, 0x78U),
            "seed two-row 4-bit texture image");
    const std::uint64_t setImage =
        (0x3DULL << 56U) | (3ULL << 32U) | 0x1700ULL;
    const std::uint64_t setTile =
        (0x35ULL << 56U) | (1ULL << 41U);
    const std::uint64_t loadTile =
        (0x34ULL << 56U) | (12ULL << 12U) | 4ULL;
    require(bus.write64(0xA40U, setImage) &&
            bus.write64(0xA48U, setTile) &&
            bus.write64(0xA50U, loadTile),
            "seed 4-bit LOAD_TILE command sequence");
    require(bus.write32(Bus::kDpStart, 0xA40U) &&
            bus.write32(Bus::kDpEnd, 0xA58U),
            "publish 4-bit LOAD_TILE command sequence");
    for (unsigned i = 0; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) ==
                    RdpProcessResult::Processed,
                "4-bit LOAD_TILE commands execute");
    std::uint8_t value = 0U;
    require(state.tmem.readByte(0U, value) && value == 0x12U &&
            state.tmem.readByte(1U, value) && value == 0x34U,
            "4-bit LOAD_TILE packs first-row high and low nibbles");
    require(state.tmem.readByte(12U, value) && value == 0x56U &&
            state.tmem.readByte(13U, value) && value == 0x78U,
            "4-bit LOAD_TILE packs and half-swizzles the odd row");
}

void testLoadTileSwizzlesOddRowsFor16BitTexels() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    require(bus.write16(0x1800U, 0x1122U) &&
            bus.write16(0x1802U, 0x3344U) &&
            bus.write16(0x1808U, 0x5566U) &&
            bus.write16(0x180AU, 0x7788U),
            "seed two-row 16-bit LOAD_TILE texture");
    const std::uint64_t setImage =
        (0x3DULL << 56U) | (2ULL << 51U) | (3ULL << 32U) | 0x1800ULL;
    const std::uint64_t setTile =
        (0x35ULL << 56U) | (2ULL << 51U) | (1ULL << 41U);
    const std::uint64_t loadTile =
        (0x34ULL << 56U) | (4ULL << 12U) | 4ULL;
    require(bus.write64(0xA80U, setImage) &&
            bus.write64(0xA88U, setTile) &&
            bus.write64(0xA90U, loadTile),
            "seed 16-bit LOAD_TILE command sequence");
    require(bus.write32(Bus::kDpStart, 0xA80U) &&
            bus.write32(Bus::kDpEnd, 0xA98U),
            "publish 16-bit LOAD_TILE command sequence");
    for (unsigned i = 0; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) ==
                    RdpProcessResult::Processed,
                "16-bit LOAD_TILE command executes");
    std::uint8_t value = 0U;
    require(state.tmem.readByte(0U, value) && value == 0x11U &&
            state.tmem.readByte(1U, value) && value == 0x22U &&
            state.tmem.readByte(2U, value) && value == 0x33U &&
            state.tmem.readByte(3U, value) && value == 0x44U,
            "16-bit LOAD_TILE preserves first-row big-endian texels");
    require(state.tmem.readByte(12U, value) && value == 0x55U &&
            state.tmem.readByte(13U, value) && value == 0x66U &&
            state.tmem.readByte(14U, value) && value == 0x77U &&
            state.tmem.readByte(15U, value) && value == 0x88U,
            "16-bit LOAD_TILE swaps odd-row 32-bit halves");
}

void testLoadTileSplits32BitRgbaAcrossTmemBanks() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    const std::uint8_t source[] = {
        0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U, 0x88U,
        0x99U, 0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU, 0xF0U, 0x0FU
    };
    for (std::uint32_t i = 0; i < sizeof(source); ++i)
        require(bus.write8(0x1A00U + i, source[i]),
                "seed two-row RGBA32 LOAD_TILE source");

    const std::uint64_t setImage =
        (0x3DULL << 56U) | (3ULL << 51U) | (1ULL << 32U) | 0x1A00ULL;
    const std::uint64_t setTile =
        (0x35ULL << 56U) | (3ULL << 51U) | (1ULL << 41U) | (2ULL << 32U);
    const std::uint64_t loadTile =
        (0x34ULL << 56U) | (4ULL << 12U) | 4ULL;
    require(bus.write64(0xAA0U, setImage) &&
            bus.write64(0xAA8U, setTile) &&
            bus.write64(0xAB0U, loadTile),
            "seed RGBA32 LOAD_TILE command sequence");
    require(bus.write32(Bus::kDpStart, 0xAA0U) &&
            bus.write32(Bus::kDpEnd, 0xAB8U),
            "publish RGBA32 LOAD_TILE command sequence");
    for (unsigned i = 0; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "RGBA32 LOAD_TILE command executes");

    std::uint8_t value = 0U;
    require(state.tmem.readByte(16U, value) && value == 0x11U &&
            state.tmem.readByte(17U, value) && value == 0x22U &&
            state.tmem.readByte(18U, value) && value == 0x55U &&
            state.tmem.readByte(19U, value) && value == 0x66U,
            "RGBA32 LOAD_TILE stores RG pairs at a nonzero base in the lower TMEM bank");
    require(state.tmem.readByte(0x810U, value) && value == 0x33U &&
            state.tmem.readByte(0x811U, value) && value == 0x44U &&
            state.tmem.readByte(0x812U, value) && value == 0x77U &&
            state.tmem.readByte(0x813U, value) && value == 0x88U,
            "RGBA32 LOAD_TILE stores BA pairs at the matching nonzero base in the upper TMEM bank");
    require(state.tmem.readByte(28U, value) && value == 0x99U &&
            state.tmem.readByte(29U, value) && value == 0xAAU &&
            state.tmem.readByte(30U, value) && value == 0xDDU &&
            state.tmem.readByte(31U, value) && value == 0xEEU,
            "RGBA32 LOAD_TILE applies odd-row halfword swap in lower bank");
    require(state.tmem.readByte(0x81CU, value) && value == 0xBBU &&
            state.tmem.readByte(0x81DU, value) && value == 0xCCU &&
            state.tmem.readByte(0x81EU, value) && value == 0xF0U &&
            state.tmem.readByte(0x81FU, value) && value == 0x0FU,
            "RGBA32 LOAD_TILE applies odd-row halfword swap in upper bank");
}

void testLoadTileRgba32UpperBankStaysAlignedAcrossBankWrap() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    const std::uint8_t source[] = {
        0x10U, 0x20U, 0x30U, 0x40U,
        0x50U, 0x60U, 0x70U, 0x80U
    };
    for (std::uint32_t i = 0; i < sizeof(source); ++i)
        require(bus.write8(0x1B00U + i, source[i]),
                "seed RGBA32 source crossing lower TMEM bank boundary");

    const std::uint64_t setImage =
        (0x3DULL << 56U) | (3ULL << 51U) | 0x1B00ULL;
    const std::uint64_t setTile =
        (0x35ULL << 56U) | (3ULL << 51U) |
        (1ULL << 41U) | (255ULL << 32U);
    const std::uint64_t loadTile =
        (0x34ULL << 56U) | 4ULL;
    require(bus.write64(0xAE0U, setImage) &&
            bus.write64(0xAE8U, setTile) &&
            bus.write64(0xAF0U, loadTile),
            "seed boundary-crossing RGBA32 LOAD_TILE commands");
    require(bus.write32(Bus::kDpStart, 0xAE0U) &&
            bus.write32(Bus::kDpEnd, 0xAF8U),
            "publish boundary-crossing RGBA32 LOAD_TILE commands");
    for (unsigned i = 0; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "boundary-crossing RGBA32 LOAD_TILE executes");

    std::uint8_t value = 0U;
    require(state.tmem.readByte(0x7F8U, value) && value == 0x10U &&
            state.tmem.readByte(0x7F9U, value) && value == 0x20U &&
            state.tmem.readByte(0xFF8U, value) && value == 0x30U &&
            state.tmem.readByte(0xFF9U, value) && value == 0x40U,
            "last lower/upper bank texel pair remains correctly separated");
    require(state.tmem.readByte(4U, value) && value == 0x50U &&
            state.tmem.readByte(5U, value) && value == 0x60U &&
            state.tmem.readByte(0x804U, value) && value == 0x70U &&
            state.tmem.readByte(0x805U, value) && value == 0x80U,
            "odd row wraps both RGBA32 banks independently without overlap");
}

void testLoadTileRejectsNonRgba32FormatsWithoutWritingTmem() {
    using namespace m64plus::n64;
    for (const bool invalidImageFormat : {true, false}) {
        Bus bus;
        RdpSyncState state{};
        require(state.tmem.writeByte(0U, 0x5AU) &&
                state.tmem.writeByte(1U, 0xA5U) &&
                state.tmem.writeByte(0x800U, 0x3CU) &&
                state.tmem.writeByte(0x801U, 0xC3U),
                "seed both TMEM banks with sentinels before invalid-format load");
        const std::uint64_t setImage =
            (0x3DULL << 56U) | (3ULL << 51U) |
            (invalidImageFormat ? (1ULL << 53U) : 0ULL) |
            (1ULL << 32U) | 0x1A00ULL;
        const std::uint64_t setTile =
            (0x35ULL << 56U) | (3ULL << 51U) |
            (invalidImageFormat ? 0ULL : (1ULL << 53U));
        const std::uint64_t loadTile =
            (0x34ULL << 56U) | (4ULL << 12U) | 4ULL;
        require(bus.write64(0xAC0U, setImage) &&
                bus.write64(0xAC8U, setTile) &&
                bus.write64(0xAD0U, loadTile),
                "seed invalid-format RGBA32 LOAD_TILE sequence");
        require(bus.write32(Bus::kDpStart, 0xAC0U) &&
                bus.write32(Bus::kDpEnd, 0xAD8U),
                "publish invalid-format RGBA32 LOAD_TILE sequence");
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "SET_TEXTURE_IMAGE records format before LOAD_TILE validation");
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "SET_TILE records format before LOAD_TILE validation");
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Unsupported,
                "RGBA32 LOAD_TILE rejects non-RGBA source or tile format");
        std::uint8_t value = 0xFFU;
        require(state.tmem.readByte(0U, value) && value == 0x5AU &&
                state.tmem.readByte(1U, value) && value == 0xA5U &&
                state.tmem.readByte(0x800U, value) && value == 0x3CU &&
                state.tmem.readByte(0x801U, value) && value == 0xC3U,
                "invalid RGBA32 format preserves pre-existing bytes in both TMEM banks");
    }
}

void testLoadBlockCopiesContiguousTexelsIntoTmem() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    for (std::uint32_t i = 0; i < 8U; ++i)
        require(bus.write8(0x1600U + i, static_cast<std::uint8_t>(0x20U + i)),
                "seed 8-bit LOAD_BLOCK source");
    const std::uint64_t setImage =
        (0x3DULL << 56U) | (1ULL << 51U) | (7ULL << 32U) | 0x1600ULL;
    const std::uint64_t setTile = (0x35ULL << 56U);
    const std::uint64_t loadBlock =
        (0x33ULL << 56U) | (28ULL << 12U);
    require(bus.write64(0x9C0U, setImage) &&
            bus.write64(0x9C8U, setTile) &&
            bus.write64(0x9D0U, loadBlock), "seed 8-bit LOAD_BLOCK sequence");
    require(bus.write32(Bus::kDpStart, 0x9C0U) &&
            bus.write32(Bus::kDpEnd, 0x9D8U), "publish 8-bit LOAD_BLOCK sequence");
    for (unsigned i = 0; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "8-bit LOAD_BLOCK sequence executes");
    std::uint8_t value = 0U;
    require(state.tmem.readByte(0U, value) && value == 0x20U &&
            state.tmem.readByte(7U, value) && value == 0x27U,
            "8-bit LOAD_BLOCK copies all requested texels");

    Bus halfwordBus;
    RdpSyncState halfwordState{};
    require(halfwordBus.write16(0x1700U, 0x1234U) &&
            halfwordBus.write16(0x1702U, 0x5678U) &&
            halfwordBus.write16(0x1704U, 0x9ABCU) &&
            halfwordBus.write16(0x1706U, 0xDEF0U),
            "seed 16-bit LOAD_BLOCK source");
    const std::uint64_t setImage16 =
        (0x3DULL << 56U) | (2ULL << 51U) | (7ULL << 32U) | 0x1700ULL;
    const std::uint64_t setTile16 = (0x35ULL << 56U);
    const std::uint64_t loadBlock16 = (0x33ULL << 56U) | (12ULL << 12U);
    require(halfwordBus.write64(0xA00U, setImage16) &&
            halfwordBus.write64(0xA08U, setTile16) &&
            halfwordBus.write64(0xA10U, loadBlock16),
            "seed 16-bit LOAD_BLOCK sequence");
    require(halfwordBus.write32(Bus::kDpStart, 0xA00U) &&
            halfwordBus.write32(Bus::kDpEnd, 0xA18U),
            "publish 16-bit LOAD_BLOCK sequence");
    for (unsigned i = 0; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(halfwordBus, halfwordState) ==
                    RdpProcessResult::Processed,
                "16-bit LOAD_BLOCK sequence executes");
    require(halfwordState.tmem.readByte(0U, value) && value == 0x12U &&
            halfwordState.tmem.readByte(1U, value) && value == 0x34U &&
            halfwordState.tmem.readByte(6U, value) && value == 0xDEU &&
            halfwordState.tmem.readByte(7U, value) && value == 0xF0U,
            "16-bit LOAD_BLOCK preserves big-endian halfwords");
}

void testLoadBlockDxtHandlesMultipleCarriesAndFractionalLines() {
    using namespace m64plus::n64;
    // DXT=0xFFF advances at least one line per 64-bit word. The line
    // parity alternates, so words 1 and 3 (zero-based) must be half-swapped.
    Bus fastBus;
    RdpSyncState fastState{};
    for (std::uint32_t i = 0; i < 32U; ++i)
        require(fastBus.write8(0x1C00U + i, static_cast<std::uint8_t>(i)),
                "seed high-DXT LOAD_BLOCK data");
    const std::uint64_t image =
        (0x3DULL << 56U) | (1ULL << 51U) | (31ULL << 32U) | 0x1C00ULL;
    const std::uint64_t tile = (0x35ULL << 56U);
    const std::uint64_t fastLoad =
        (0x33ULL << 56U) | (124ULL << 12U) | 0xFFFULL;
    require(fastBus.write64(0xAC0U, image) &&
            fastBus.write64(0xAC8U, tile) &&
            fastBus.write64(0xAD0U, fastLoad), "seed high-DXT sequence");
    require(fastBus.write32(Bus::kDpStart, 0xAC0U) &&
            fastBus.write32(Bus::kDpEnd, 0xAD8U), "publish high-DXT sequence");
    for (unsigned i = 0; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(fastBus, fastState) ==
                    RdpProcessResult::Processed,
                "high-DXT LOAD_BLOCK sequence executes");
    std::uint8_t value = 0U;
    require(fastState.tmem.readByte(8U, value) && value == 12U &&
            fastState.tmem.readByte(12U, value) && value == 8U,
            "high DXT swaps the second 64-bit word");
    require(fastState.tmem.readByte(24U, value) && value == 28U &&
            fastState.tmem.readByte(28U, value) && value == 24U,
            "high DXT swaps the fourth 64-bit word");

    // A non-power-of-two DXT accumulates fractional line progress. With
    // 0x600, only the third word starts on an odd line in this four-word span.
    Bus fractionalBus;
    RdpSyncState fractionalState{};
    for (std::uint32_t i = 0; i < 32U; ++i)
        require(fractionalBus.write8(0x1D00U + i, static_cast<std::uint8_t>(i)),
                "seed fractional-DXT LOAD_BLOCK data");
    const std::uint64_t fractionalImage =
        (0x3DULL << 56U) | (1ULL << 51U) | (31ULL << 32U) | 0x1D00ULL;
    const std::uint64_t fractionalLoad =
        (0x33ULL << 56U) | (124ULL << 12U) | 0x600ULL;
    require(fractionalBus.write64(0xAE0U, fractionalImage) &&
            fractionalBus.write64(0xAE8U, tile) &&
            fractionalBus.write64(0xAF0U, fractionalLoad),
            "seed fractional-DXT sequence");
    require(fractionalBus.write32(Bus::kDpStart, 0xAE0U) &&
            fractionalBus.write32(Bus::kDpEnd, 0xAF8U),
            "publish fractional-DXT sequence");
    for (unsigned i = 0; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(fractionalBus, fractionalState) ==
                    RdpProcessResult::Processed,
                "fractional-DXT LOAD_BLOCK sequence executes");
    require(fractionalState.tmem.readByte(16U, value) && value == 20U &&
            fractionalState.tmem.readByte(20U, value) && value == 16U,
            "fractional DXT swaps only the third word");
    require(fractionalState.tmem.readByte(24U, value) && value == 24U &&
            fractionalState.tmem.readByte(28U, value) && value == 28U,
            "fractional DXT leaves the fourth word unswapped");
}

void testLoadBlockRejectsStartOutsideTextureImageWidth() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    require(bus.write8(0x1A00U, 0x12U) && bus.write8(0x1A01U, 0x34U),
            "seed bytes beyond the declared texture row");
    state.tmem.writeByte(0U, 0xA5U);

    const std::uint64_t setImage =
        (0x3DULL << 56U) | (1ULL << 51U) | (3ULL << 32U) | 0x1A00ULL;
    const std::uint64_t setTile = (0x35ULL << 56U);
    // The image is four texels wide, so S=4 is outside its valid row.
    const std::uint64_t loadBlock =
        (0x33ULL << 56U) | (16ULL << 44U) | (16ULL << 12U);
    require(bus.write64(0xB40U, setImage) &&
            bus.write64(0xB48U, setTile) &&
            bus.write64(0xB50U, loadBlock),
            "seed out-of-range LOAD_BLOCK start sequence");
    require(bus.write32(Bus::kDpStart, 0xB40U) &&
            bus.write32(Bus::kDpEnd, 0xB58U),
            "publish out-of-range LOAD_BLOCK sequence");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "SET_TEXTURE_IMAGE configures four-texel row");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "SET_TILE configures out-of-range LOAD_BLOCK test");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Unsupported,
            "LOAD_BLOCK rejects a starting S coordinate beyond image width");
    std::uint8_t value = 0U;
    require(state.tmem.readByte(0U, value) && value == 0xA5U,
            "rejected out-of-range LOAD_BLOCK leaves TMEM unchanged");
}

void testLoadBlockRejectsUnsupportedTexelSizes() {
    using namespace m64plus::n64;
    for (const std::uint32_t size : {0U, 3U}) {
        Bus bus;
        RdpSyncState state{};
        const std::uint64_t setImage =
            (0x3DULL << 56U) | (static_cast<std::uint64_t>(size) << 51U) |
            (7ULL << 32U) | 0x1A00ULL;
        const std::uint64_t setTile = (0x35ULL << 56U);
        const std::uint64_t loadBlock =
            (0x33ULL << 56U) | (0ULL << 44U) | (0ULL << 32U) |
            (0ULL << 24U) | (4ULL << 12U);
        require(bus.write64(0xB00U, setImage) &&
                bus.write64(0xB08U, setTile) &&
                bus.write64(0xB10U, loadBlock), "seed unsupported LOAD_BLOCK size");
        require(bus.write32(Bus::kDpStart, 0xB00U) &&
                bus.write32(Bus::kDpEnd, 0xB18U), "publish unsupported LOAD_BLOCK size");
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "SET_TEXTURE_IMAGE accepts descriptor before size validation");
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "SET_TILE configures unsupported LOAD_BLOCK test");
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Unsupported,
                "LOAD_BLOCK rejects 4-bit and 32-bit paths until specialized support exists");
    }
}

void testLoadBlockDxtSwapsOddLineWords() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    for (std::uint32_t i = 0; i < 24U; ++i)
        require(bus.write8(0x1800U + i, static_cast<std::uint8_t>(i)),
                "seed three 64-bit LOAD_BLOCK words");
    const std::uint64_t setImage =
        (0x3DULL << 56U) | (1ULL << 51U) | (31ULL << 32U) | 0x1800ULL;
    const std::uint64_t setTile = (0x35ULL << 56U);
    const std::uint64_t loadBlock =
        (0x33ULL << 56U) | (0ULL << 44U) | (0ULL << 32U) |
        (0ULL << 24U) | (92ULL << 12U) | 0x400ULL;
    require(bus.write64(0xA40U, setImage) &&
            bus.write64(0xA48U, setTile) &&
            bus.write64(0xA50U, loadBlock), "seed DXT interleave sequence");
    require(bus.write32(Bus::kDpStart, 0xA40U) &&
            bus.write32(Bus::kDpEnd, 0xA58U), "publish DXT interleave sequence");
    for (unsigned i = 0; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "DXT LOAD_BLOCK sequence executes");
    std::uint8_t value = 0U;
    require(state.tmem.readByte(16U, value) && value == 20U,
            "DXT odd line swaps first 32-bit half with second half");
    require(state.tmem.readByte(19U, value) && value == 23U,
            "DXT odd line moves fourth source byte into first half");
    require(state.tmem.readByte(20U, value) && value == 16U,
            "DXT odd line moves first source byte into second half");
    require(state.tmem.readByte(23U, value) && value == 19U,
            "DXT odd line completes 32-bit half swap");
}

void testLoadTlutReplicatesEntriesIntoHighTmem() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    require(bus.write16(0x1400U, 0xF801U) &&
            bus.write16(0x1402U, 0x07C1U) &&
            bus.write16(0x1404U, 0x003FU), "seed three 16-bit TLUT entries");

    const std::uint64_t setImage =
        (0x3DULL << 56U) | (2ULL << 51U) | 0x1400ULL;
    const std::uint64_t setTile =
        (0x35ULL << 56U) | (2ULL << 51U) | (256ULL << 32U);
    const std::uint64_t loadTlut =
        (0x30ULL << 56U) | (2ULL << 14U);
    require(bus.write64(0x980U, setImage) &&
            bus.write64(0x988U, setTile) &&
            bus.write64(0x990U, loadTlut), "seed TLUT load sequence");
    require(bus.write32(Bus::kDpStart, 0x980U) &&
            bus.write32(Bus::kDpEnd, 0x998U), "publish TLUT load sequence");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "SET_TEXTURE_IMAGE configures 16-bit palette source");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "SET_TILE configures high-half TMEM destination");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "LOAD_TLUT transfers three palette entries");

    std::uint64_t word = 0U;
    require(state.tmem.read64(0x800U, word) && word == 0xF801F801F801F801ULL,
            "LOAD_TLUT quadricates first 16-bit palette entry");
    require(state.tmem.read64(0x808U, word) && word == 0x07C107C107C107C1ULL,
            "LOAD_TLUT quadricates second palette entry");
    require(state.tmem.read64(0x810U, word) && word == 0x003F003F003F003FULL,
            "LOAD_TLUT quadricates third palette entry");
}

void testFillRectangleWritesRgba32PixelsAndHonorsScissor() {
    using namespace m64plus::n64;
    const std::uint32_t base = 0x3000U;
    const std::uint32_t sentinel = 0xAABBCCDDU;
    const std::uint32_t color = 0x11223344U;
    Bus bus;
    RdpSyncState state{};
    for (std::uint32_t i = 0; i < 16U; ++i)
        require(bus.write32(base + i * 4U, sentinel), "seed RGBA32 framebuffer sentinels");

    const std::uint64_t commands[] = {
        (0x3FULL << 56U) | (3ULL << 51U) | (3ULL << 32U) | base,
        (0x37ULL << 56U) | color,
        (0x2FULL << 56U) | (3ULL << 52U),
        (0x36ULL << 56U) | (8ULL << 44U) | (8ULL << 32U) |
            (4ULL << 12U) | 4ULL
    };
    for (std::size_t i = 0; i < 4U; ++i)
        require(bus.write64(0xC00U + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "seed RGBA32 FILL_RECTANGLE command stream");
    require(bus.write32(Bus::kDpStart, 0xC00U) &&
            bus.write32(Bus::kDpEnd, 0xC20U), "publish RGBA32 fill stream");
    for (unsigned i = 0; i < 4U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "RGBA32 FILL_RECTANGLE setup and draw are processed");

    for (std::uint32_t y = 0; y < 4U; ++y) {
        for (std::uint32_t x = 0; x < 4U; ++x) {
            std::uint32_t pixel = 0U;
            require(bus.read32(base + (y * 4U + x) * 4U, pixel),
                    "read rendered RGBA32 framebuffer pixel");
            const bool inside = x >= 1U && x <= 2U && y >= 1U && y <= 2U;
            require(pixel == (inside ? color : sentinel),
                    "FILL_RECTANGLE writes only inclusive rectangle pixels");
        }
    }

    std::vector<std::uint32_t> converted;
    require(RdpFramebuffer::copyToArgb8888(bus.rdram(), state.colorImage, 4U, converted),
            "RDP-produced RGBA32 framebuffer converts to Android ARGB8888");
    require(converted.size() == 16U, "converted framebuffer has expected pixel count");
    require(converted[5] == 0x44112233U && converted[0] == 0xDDAABBCCU,
            "framebuffer converter preserves RDP pixel channels and alpha");

    Bus clippedBus;
    RdpSyncState clippedState{};
    for (std::uint32_t i = 0; i < 16U; ++i)
        require(clippedBus.write32(base + i * 4U, sentinel),
                "seed clipped framebuffer sentinels");
    const std::uint64_t clippedCommands[] = {
        commands[0], commands[1], commands[2],
        (0x2DULL << 56U) | (4ULL << 44U) | (4ULL << 32U) |
            (8ULL << 12U) | 8ULL,
        commands[3]
    };
    for (std::size_t i = 0; i < 5U; ++i)
        require(clippedBus.write64(0xC40U + static_cast<std::uint32_t>(i * 8U),
                                  clippedCommands[i]),
                "seed scissored RGBA32 fill stream");
    require(clippedBus.write32(Bus::kDpStart, 0xC40U) &&
            clippedBus.write32(Bus::kDpEnd, 0xC68U),
            "publish scissored fill stream");
    for (unsigned i = 0; i < 5U; ++i)
        require(RdpCommandProcessor::processNext(clippedBus, clippedState) ==
                    RdpProcessResult::Processed,
                "scissored RGBA32 FILL_RECTANGLE is processed");
    for (std::uint32_t y = 0; y < 4U; ++y) {
        for (std::uint32_t x = 0; x < 4U; ++x) {
            std::uint32_t pixel = 0U;
            require(clippedBus.read32(base + (y * 4U + x) * 4U, pixel),
                    "read scissored framebuffer pixel");
            const bool inside = x == 1U && y == 1U;
            require(pixel == (inside ? color : sentinel),
                    "scissor treats its lower-right X and Y bounds as exclusive");
        }
    }
}

void testFillRectangleWritesRgba16AlternatingHalfwords() {
    using namespace m64plus::n64;
    const std::uint32_t base = 0x3400U;
    Bus bus;
    RdpSyncState state{};
    const std::uint64_t commands[] = {
        (0x3FULL << 56U) | (2ULL << 51U) | (3ULL << 32U) | base,
        (0x37ULL << 56U) | 0xF80107C1ULL,
        (0x2FULL << 56U) | (3ULL << 52U),
        (0x36ULL << 56U) | (8ULL << 44U) | (8ULL << 32U)
    };
    for (std::size_t i = 0; i < 4U; ++i)
        require(bus.write64(0xD80U + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "seed RGBA16 FILL_RECTANGLE command stream");
    require(bus.write32(Bus::kDpStart, 0xD80U) &&
            bus.write32(Bus::kDpEnd, 0xDA0U),
            "publish RGBA16 fill stream");
    for (unsigned i = 0; i < 4U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "RGBA16 FILL_RECTANGLE setup and draw are processed");

    for (std::uint32_t y = 0; y < 3U; ++y) {
        for (std::uint32_t x = 0; x < 3U; ++x) {
            std::uint16_t pixel = 0U;
            const std::uint32_t index = y * 4U + x;
            require(bus.read16(base + index * 2U, pixel),
                    "read rendered RGBA16 framebuffer pixel");
            const std::uint16_t expected = (index & 1U) == 0U ? 0xF801U : 0x07C1U;
            require(pixel == expected,
                    "RGBA16 fill repeats upper/lower fill-color halfwords");
        }
    }
}

void testFillRectangleHonorsScissorFieldParity() {
    using namespace m64plus::n64;
    constexpr std::uint32_t base = 0x3800U;
    constexpr std::uint32_t sentinel = 0xAABBCCDDU;
    constexpr std::uint32_t color = 0x10203040U;

    for (std::uint32_t selectedParity = 0U; selectedParity < 2U; ++selectedParity) {
        Bus bus;
        RdpSyncState state{};
        for (std::uint32_t i = 0; i < 16U; ++i)
            require(bus.write32(base + i * 4U, sentinel),
                    "seed interlaced framebuffer sentinels");

        const std::uint64_t commands[] = {
            (0x3FULL << 56U) | (3ULL << 51U) | (3ULL << 32U) | base,
            (0x37ULL << 56U) | color,
            (0x2FULL << 56U) | (3ULL << 52U),
            (0x2DULL << 56U) | (1ULL << 25U) |
                (static_cast<std::uint64_t>(selectedParity) << 24U) |
                (12ULL << 12U) | 16ULL,
            (0x36ULL << 56U) | (12ULL << 44U) | (12ULL << 32U)
        };
        for (std::size_t i = 0; i < 5U; ++i)
            require(bus.write64(0xDC0U + static_cast<std::uint32_t>(i * 8U), commands[i]),
                    "seed field-selected fill command stream");
        require(bus.write32(Bus::kDpStart, 0xDC0U) &&
                bus.write32(Bus::kDpEnd, 0xDE8U),
                "publish field-selected fill stream");
        for (unsigned i = 0; i < 5U; ++i)
            require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                    "field-selected FILL_RECTANGLE is processed");

        for (std::uint32_t y = 0; y < 4U; ++y) {
            for (std::uint32_t x = 0; x < 4U; ++x) {
                std::uint32_t pixel = 0U;
                require(bus.read32(base + (y * 4U + x) * 4U, pixel),
                        "read field-selected framebuffer pixel");
                const bool selectedField = (y & 1U) == selectedParity;
                const bool insideScissor = x < 3U;
                require(pixel == (selectedField && insideScissor ? color : sentinel),
                        "scissor field selection and exclusive right edge are both enforced");
            }
        }
    }
}

void testFillRectangleRejectsOutOfRangeWithoutPartialWrites() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    const std::uint32_t base = static_cast<std::uint32_t>(Rdram::kSize - 8U);
    const std::uint32_t sentinel = 0xA1B2C3D4U;
    require(bus.write32(base, sentinel) && bus.write32(base + 4U, sentinel),
            "seed final two RDRAM words before invalid fill");

    const std::uint64_t commands[] = {
        (0x3FULL << 56U) | (3ULL << 51U) | (2ULL << 32U) | base,
        (0x37ULL << 56U) | 0x11223344ULL,
        (0x2FULL << 56U) | (3ULL << 52U),
        (0x36ULL << 56U) | (8ULL << 44U) | (8ULL << 12U)
    };
    for (std::size_t i = 0; i < 4U; ++i)
        require(bus.write64(0xD00U + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "seed out-of-range fill command stream");
    require(bus.write32(Bus::kDpStart, 0xD00U) &&
            bus.write32(Bus::kDpEnd, 0xD20U),
            "publish out-of-range fill stream");
    for (unsigned i = 0; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "out-of-range fill setup command is processed");

    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Unsupported,
            "fill crossing the RDRAM boundary is rejected");
    std::uint32_t first = 0U;
    std::uint32_t second = 0U;
    require(bus.read32(base, first) && bus.read32(base + 4U, second),
            "read framebuffer words after rejected fill");
    require(first == sentinel && second == sentinel,
            "rejected fill leaves every framebuffer word unchanged");
}

void testUnsupportedRdpCommandIsReported() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    require(bus.write64(0x600U, static_cast<std::uint64_t>(0x31U) << 56U),
            "seed reserved RDP command");
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
    testMaximumTriangleCommandWaitsForAllWords();
    testTextureRectangleLengths();
    testDpcCommandStreamIntegration();
    testRdpSynchronizationCommands();
    testRdpBasicStateCommands();
    testRdpStateFieldDecoding();
    testRdpOtherModesAndCombineDecoding();
    testRdpColorAndDepthStateCommands();
    testRdpConvertAndKeyStateCommands();
    testRdpPrimitiveDepthCommand();
    testRdpTextureMemory();
    testRdpTextureLoadCommandFields();
    testLoadTileCopiesTextureImageIntoTmem();
    testLoadTilePacksFourBitNibblesAndSwizzlesOddRows();
    testLoadTileSwizzlesOddRowsFor16BitTexels();
    testLoadTileSplits32BitRgbaAcrossTmemBanks();
    testLoadTileRgba32UpperBankStaysAlignedAcrossBankWrap();
    testLoadTileRejectsNonRgba32FormatsWithoutWritingTmem();
    testLoadBlockCopiesContiguousTexelsIntoTmem();
    testLoadBlockDxtSwapsOddLineWords();
    testLoadBlockDxtHandlesMultipleCarriesAndFractionalLines();
    testLoadBlockRejectsStartOutsideTextureImageWidth();
    testLoadBlockRejectsUnsupportedTexelSizes();
    testLoadTlutReplicatesEntriesIntoHighTmem();
    testFillRectangleWritesRgba32PixelsAndHonorsScissor();
    testFillRectangleWritesRgba16AlternatingHalfwords();
    testFillRectangleHonorsScissorFieldParity();
    testFillRectangleRejectsOutOfRangeWithoutPartialWrites();
    testUnsupportedRdpCommandIsReported();
    std::cout << "All RDP decoder host tests passed.\n";
    return 0;
}
