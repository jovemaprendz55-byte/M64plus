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
        const bool documented = (opcode >= 0x08U && opcode <= 0x0FU) ||
            opcode == 0x24U || opcode == 0x25U ||
            (opcode >= 0x26U && opcode <= 0x30U) || opcode == 0x32U ||
            (opcode >= 0x33U && opcode <= 0x3FU);
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

void testTriangleEdgeHeaderDecoding() {
    using namespace m64plus::n64;
    const std::array<std::uint64_t, 4> words = {
        (5ULL << 51U) | (3ULL << 48U) |
            (0x0123ULL << 32U) | (0x0234ULL << 16U) | 0x0345ULL,
        (0xFFF00000ULL << 32U) | 0x80000001ULL,
        (0x00018000ULL << 32U) | 0x7FFFFFFFULL,
        (0xFFFF8000ULL << 32U) | 0x00020000ULL
    };
    RdpTriangleEdgeState edge{};
    require(RdpCommandDecoder::decodeTriangleEdge(words, edge),
            "four-word triangle edge header decodes");
    require(edge.leftMajor && edge.level == 5U && edge.tile == 3U &&
            edge.yLow == 0x0345U && edge.yMiddle == 0x0234U &&
            edge.yHigh == 0x0123U,
            "triangle LOD level, tile, Y coordinates and major-edge direction decode");
    require(edge.xLeft == -1048576 && edge.dxLeftDy == 0 &&
            edge.xHigh == 98304 && edge.dxHighDy == -1 &&
            edge.xMiddle == -32768 && edge.dxMiddleDy == 32768,
            "triangle edge X fields sign-extend from 28 bits and slopes from 30 bits then scale to quarter-pixel steps");

    const std::array<std::uint64_t, 4> negativeSlopes = {
        0ULL, 0x000000003FFFFFFDULL, 0x000000003FFFFFFFULL,
        0x000000003FFFFFFCULL
    };
    RdpTriangleEdgeState negative{};
    require(RdpCommandDecoder::decodeTriangleEdge(negativeSlopes, negative) &&
            negative.dxLeftDy == -1 && negative.dxHighDy == -1 &&
            negative.dxMiddleDy == -1,
            "negative edge slopes use floor division when scaled to quarter-pixel steps");

    RdpTriangleEdgeState zero{};
    const std::array<std::uint64_t, 4> empty{};
    require(RdpCommandDecoder::decodeTriangleEdge(empty, zero) &&
            zero.leftMajor && zero.level == 0U && zero.tile == 0U &&
            zero.yLow == 0U && zero.xLeft == 0,
            "zeroed triangle edge header decodes deterministically");
}

void testTriangleDecoderRejectsUnknownOpcodeMetadataAtomically() {
    using namespace m64plus::n64;
    RdpCommandPacket packet{};
    packet.info = {0x08U, 4U, false};
    packet.words[0] = header(0x08U);
    RdpTriangleCommand command{};
    command.edge.xLeft = 12345;
    command.layout.wordCount = 77U;
    require(!RdpCommandDecoder::decodeTriangleCommand(packet, command),
            "triangle decoder rejects packets marked with unknown opcode metadata");
    require(command.edge.xLeft == 12345 && command.layout.wordCount == 77U,
            "unknown triangle metadata rejection leaves output unchanged");
}

void testTriangleCommandPacketDecoding() {
    using namespace m64plus::n64;
    const auto packFour = [](std::uint16_t a, std::uint16_t b,
                             std::uint16_t c, std::uint16_t d) noexcept {
        return (static_cast<std::uint64_t>(a) << 48U) |
               (static_cast<std::uint64_t>(b) << 32U) |
               (static_cast<std::uint64_t>(c) << 16U) |
               static_cast<std::uint64_t>(d);
    };
    const auto packTexture = [](std::uint16_t a, std::uint16_t b,
                                std::uint16_t c) noexcept {
        return (static_cast<std::uint64_t>(a) << 48U) |
               (static_cast<std::uint64_t>(b) << 32U) |
               (static_cast<std::uint64_t>(c) << 16U) | 0xBEEFULL;
    };
    constexpr std::array<std::int16_t, 4> expectedFour = {
        4097, -32766, 32765, -1
    };
    constexpr std::array<std::uint16_t, 4> encodedFour = {
        0x1001U, 0x8002U, 0x7FFDU, 0xFFFFU
    };
    for (std::uint8_t flags = 0U; flags < 8U; ++flags) {
        RdpCommandPacket packet{};
        const std::uint8_t opcode = static_cast<std::uint8_t>(0x08U + flags);
        packet.info = RdpCommandDecoder::decode(
            static_cast<std::uint64_t>(opcode) << 56U);
        packet.words[0] = (static_cast<std::uint64_t>(opcode) << 56U) |
                          (6ULL << 51U) | (2ULL << 48U) |
                          (0x0123ULL << 32U) | (0x0234ULL << 16U) | 0x0345ULL;
        packet.words[1] = 0xFFF0000080000001ULL;
        packet.words[2] = 0x000180007FFFFFFFULL;
        packet.words[3] = 0xFFFF800000020000ULL;

        RdpTriangleLayout layout{};
        require(RdpCommandDecoder::decodeTriangleLayout(opcode, layout),
                "triangle layout is available for coefficient fixture");
        if (layout.hasShade) {
            for (std::size_t word = 0; word < 8U; ++word) {
                const std::uint16_t base = static_cast<std::uint16_t>(word * 4U);
                packet.words[layout.shadeWordOffset + word] = packFour(
                    static_cast<std::uint16_t>(encodedFour[0] + base),
                    static_cast<std::uint16_t>(encodedFour[1] + base),
                    static_cast<std::uint16_t>(encodedFour[2] + base),
                    static_cast<std::uint16_t>(encodedFour[3] + base));
            }
        }
        if (layout.hasTexture) {
            for (std::size_t word = 0; word < 8U; ++word) {
                const std::uint16_t base = static_cast<std::uint16_t>(word * 3U);
                packet.words[layout.textureWordOffset + word] = packTexture(
                    static_cast<std::uint16_t>(encodedFour[0] + base),
                    static_cast<std::uint16_t>(encodedFour[1] + base),
                    static_cast<std::uint16_t>(encodedFour[2] + base));
            }
        }
        if (layout.hasZ) {
            for (std::size_t word = 0; word < 2U; ++word) {
                const std::uint16_t base = static_cast<std::uint16_t>(word * 4U);
                packet.words[layout.zWordOffset + word] = packFour(
                    static_cast<std::uint16_t>(encodedFour[0] + base),
                    static_cast<std::uint16_t>(encodedFour[1] + base),
                    static_cast<std::uint16_t>(encodedFour[2] + base),
                    static_cast<std::uint16_t>(encodedFour[3] + base));
            }
        }

        RdpTriangleCommand command{};
        command.shade.rgba.fill(111);
        command.texture.stw.fill(222);
        command.depth.z = 333;
        command.shadeCoefficients.fill(111);
        command.textureCoefficients.fill(222);
        command.zCoefficients.fill(333);
        require(RdpCommandDecoder::decodeTriangleCommand(packet, command),
                "complete triangle packet decodes");
        require(command.layout.wordCount == packet.info.lengthWords &&
                command.edge.level == 6U && command.edge.tile == 2U &&
                command.edge.yLow == 0x0345U,
                "triangle packet carries LOD level, tile, edge header and optional layout");
        if (!layout.hasShade) {
            require(command.shade.rgba[0] == 0 && command.shadeCoefficients[0] == 0,
                    "absent shade block clears prior decoded values");
        }
        if (!layout.hasTexture) {
            require(command.texture.stw[0] == 0 && command.textureCoefficients[0] == 0,
                    "absent texture block clears prior decoded values");
        }
        if (!layout.hasZ) {
            require(command.depth.z == 0 && command.zCoefficients[0] == 0,
                    "absent depth block clears prior decoded values");
        }
        if (layout.hasShade) {
            for (std::size_t i = 0; i < command.shadeCoefficients.size(); ++i) {
                const std::int16_t expected = expectedFour[i % 4U] +
                    static_cast<std::int16_t>((i / 4U) * 4U);
                require(command.shadeCoefficients[i] == expected,
                        "all shade coefficient fields retain their signed order");
            }
            require(command.shade.rgba[0] == command.shadeCoefficients[0] &&
                    command.shade.drgbaDx[0] == command.shadeCoefficients[4] &&
                    command.shade.rgbaFraction[0] == command.shadeCoefficients[8] &&
                    command.shade.drgbaDxFraction[0] == command.shadeCoefficients[12] &&
                    command.shade.drgbaDe[0] == command.shadeCoefficients[16] &&
                    command.shade.drgbaDy[0] == command.shadeCoefficients[20] &&
                    command.shade.drgbaDeFraction[0] == command.shadeCoefficients[24] &&
                    command.shade.drgbaDyFraction[0] == command.shadeCoefficients[28],
                    "named shade groups map to the documented eight-word order");
            const std::array<const std::array<std::int16_t, 4>*, 8> shadeGroups = {
                &command.shade.rgba, &command.shade.drgbaDx,
                &command.shade.rgbaFraction, &command.shade.drgbaDxFraction,
                &command.shade.drgbaDe, &command.shade.drgbaDy,
                &command.shade.drgbaDeFraction, &command.shade.drgbaDyFraction
            };
            for (std::size_t group = 0; group < shadeGroups.size(); ++group) {
                for (std::size_t component = 0; component < 4U; ++component) {
                    require((*shadeGroups[group])[component] ==
                            command.shadeCoefficients[group * 4U + component],
                            "every named shade component matches its raw coefficient");
                }
            }
        }
        if (layout.hasTexture) {
            for (std::size_t i = 0; i < command.textureCoefficients.size(); ++i) {
                const std::int16_t expected = expectedFour[i % 3U] +
                    static_cast<std::int16_t>((i / 3U) * 3U);
                require(command.textureCoefficients[i] == expected,
                        "all texture coefficient fields retain order and skip low 16 bits");
            }
            for (const std::uint64_t word : command.textureWords) {
                require((word & 0xFFFFU) == 0xBEEFULL,
                        "texture coefficient low 16 bits remain preserved as unused payload");
            }
            require(command.texture.stw[0] == command.textureCoefficients[0] &&
                    command.texture.dstwDx[0] == command.textureCoefficients[3] &&
                    command.texture.stwFraction[0] == command.textureCoefficients[6] &&
                    command.texture.dstwDxFraction[0] == command.textureCoefficients[9] &&
                    command.texture.dstwDe[0] == command.textureCoefficients[12] &&
                    command.texture.dstwDy[0] == command.textureCoefficients[15] &&
                    command.texture.dstwDeFraction[0] == command.textureCoefficients[18] &&
                    command.texture.dstwDyFraction[0] == command.textureCoefficients[21],
                    "named texture groups map to the documented eight-word order");
            const std::array<const std::array<std::int16_t, 3>*, 8> textureGroups = {
                &command.texture.stw, &command.texture.dstwDx,
                &command.texture.stwFraction, &command.texture.dstwDxFraction,
                &command.texture.dstwDe, &command.texture.dstwDy,
                &command.texture.dstwDeFraction, &command.texture.dstwDyFraction
            };
            for (std::size_t group = 0; group < textureGroups.size(); ++group) {
                for (std::size_t component = 0; component < 3U; ++component) {
                    require((*textureGroups[group])[component] ==
                            command.textureCoefficients[group * 3U + component],
                            "every named texture component matches its raw coefficient");
                }
            }
        }
        if (layout.hasZ) {
            for (std::size_t i = 0; i < command.zCoefficients.size(); ++i) {
                const std::int16_t expected = expectedFour[i % 4U] +
                    static_cast<std::int16_t>((i / 4U) * 4U);
                require(command.zCoefficients[i] == expected,
                        "all Z coefficient fields retain their signed order");
            }
            require(command.depth.z == command.zCoefficients[0] &&
                    command.depth.zFraction == command.zCoefficients[1] &&
                    command.depth.dzDx == command.zCoefficients[2] &&
                    command.depth.dzDxFraction == command.zCoefficients[3] &&
                    command.depth.dzDe == command.zCoefficients[4] &&
                    command.depth.dzDeFraction == command.zCoefficients[5] &&
                    command.depth.dzDy == command.zCoefficients[6] &&
                    command.depth.dzDyFraction == command.zCoefficients[7],
                    "named depth fields map to the documented two-word order");
        }
    }

    RdpCommandPacket invalid{};
    invalid.info.opcode = 0x08U;
    invalid.info.lengthWords = 4U;
    invalid.words[0] = 0x0900000000000000ULL;
    RdpTriangleCommand unchanged{};
    unchanged.edge.yLow = 123U;
    require(!RdpCommandDecoder::decodeTriangleCommand(invalid, unchanged) &&
            unchanged.edge.yLow == 123U,
            "mismatched triangle packet is rejected without changing output");
}

void testTriangleOptionalBlockLayout() {
    using namespace m64plus::n64;
    for (std::uint8_t flags = 0U; flags < 8U; ++flags) {
        const std::uint8_t opcode = static_cast<std::uint8_t>(0x08U + flags);
        RdpTriangleLayout layout{};
        require(RdpCommandDecoder::decodeTriangleLayout(opcode, layout),
                "triangle opcode has a decodable block layout");
        const bool shade = (flags & 0x04U) != 0U;
        const bool texture = (flags & 0x02U) != 0U;
        const bool z = (flags & 0x01U) != 0U;
        std::uint8_t expectedOffset = 4U;
        require(layout.hasShade == shade && layout.hasTexture == texture &&
                layout.hasZ == z, "triangle optional block flags");
        if (shade) {
            require(layout.shadeWordOffset == expectedOffset,
                    "shade block starts after four-word edge base");
            expectedOffset = static_cast<std::uint8_t>(expectedOffset + 8U);
        }
        if (texture) {
            require(layout.textureWordOffset == expectedOffset,
                    "texture block follows optional shade block");
            expectedOffset = static_cast<std::uint8_t>(expectedOffset + 8U);
        }
        if (z) {
            require(layout.zWordOffset == expectedOffset,
                    "Z block follows shade and texture blocks");
            expectedOffset = static_cast<std::uint8_t>(expectedOffset + 2U);
        }
        require(layout.wordCount == expectedOffset &&
                layout.wordCount == RdpCommandDecoder::decode(header(opcode)).lengthWords,
                "triangle block layout agrees with packet framing");
    }
    RdpTriangleLayout unchanged{};
    unchanged.wordCount = 77U;
    require(!RdpCommandDecoder::decodeTriangleLayout(0x24U, unchanged),
            "non-triangle opcode has no triangle layout");
    require(unchanged.wordCount == 77U,
            "failed triangle layout decode leaves output unchanged");
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

void testLoadTileUpdatesTileSizeAndSetTileSizeOverrides() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    for (std::uint32_t i = 0U; i < 16U; ++i)
        require(bus.write8(0x1800U + i, static_cast<std::uint8_t>(i)),
                "seed LOAD_TILE tile-size state source");
    const std::uint64_t setImage =
        (0x3DULL << 56U) | (1ULL << 51U) | (3ULL << 32U) | 0x1800ULL;
    const std::uint64_t setTile =
        (0x35ULL << 56U) | (1ULL << 51U) | (1ULL << 41U);
    const std::uint64_t loadTile =
        (0x34ULL << 56U) | (4ULL << 44U) | (4ULL << 32U) |
        (8ULL << 12U) | 8ULL;
    const std::uint64_t setTileSize =
        (0x32ULL << 56U) | (12ULL << 44U) | (16ULL << 32U) |
        (20ULL << 12U) | 24ULL;
    const std::uint32_t start = 0xB80U;
    const std::uint64_t commands[] = {setImage, setTile, loadTile, setTileSize};
    for (std::size_t i = 0U; i < 4U; ++i)
        require(bus.write64(start + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "seed LOAD_TILE and SET_TILE_SIZE sequence");
    require(bus.write32(Bus::kDpStart, start) &&
            bus.write32(Bus::kDpEnd, start + 32U),
            "publish LOAD_TILE tile-size sequence");

    for (unsigned i = 0U; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "LOAD_TILE tile-size setup and transfer are processed");
    require(state.hasTileSize[0] &&
            state.tileSizes[0].sl == 4U && state.tileSizes[0].tl == 4U &&
            state.tileSizes[0].sh == 8U && state.tileSizes[0].th == 8U,
            "LOAD_TILE records its coordinates in the tile-size registers");

    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "SET_TILE_SIZE after LOAD_TILE is processed");
    require(state.tileSizes[0].sl == 12U && state.tileSizes[0].tl == 16U &&
            state.tileSizes[0].sh == 20U && state.tileSizes[0].th == 24U,
            "SET_TILE_SIZE replaces the coordinates written by LOAD_TILE");
}

void testLoadTileUsesLoadTileDescriptorSizeForSourceAddressing() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    require(bus.write8(0x16C0U, 0x11U) &&
            bus.write8(0x16C1U, 0x22U) &&
            bus.write8(0x16C2U, 0x33U) &&
            bus.write8(0x16C3U, 0x44U),
            "seed source bytes for LOAD_TILE descriptor-size test");

    // The source image is declared 8-bit, but the load tile is 16-bit.
    // The load engine's texel addressing must follow the selected load tile.
    const std::uint64_t setImage =
        (0x3DULL << 56U) | (1ULL << 51U) | (3ULL << 32U) | 0x16C0ULL;
    const std::uint64_t setTile =
        (0x35ULL << 56U) | (2ULL << 51U) | (1ULL << 41U);
    const std::uint64_t loadTile =
        (0x34ULL << 56U) | (4ULL << 12U) | 4ULL;
    constexpr std::uint32_t start = 0xAC0U;
    require(bus.write64(start, setImage) &&
            bus.write64(start + 8U, setTile) &&
            bus.write64(start + 16U, loadTile),
            "seed mismatched image/load-tile size command sequence");
    require(bus.write32(Bus::kDpStart, start) &&
            bus.write32(Bus::kDpEnd, start + 24U),
            "publish mismatched image/load-tile size sequence");
    for (unsigned i = 0U; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) ==
                    RdpProcessResult::Processed,
                "mismatched image/load-tile size commands execute");

    std::uint8_t value = 0U;
    require(state.tmem.readByte(0U, value) && value == 0x11U &&
            state.tmem.readByte(1U, value) && value == 0x22U &&
            state.tmem.readByte(2U, value) && value == 0x33U &&
            state.tmem.readByte(3U, value) && value == 0x44U,
            "LOAD_TILE addresses source texels using the load tile's 16-bit size");
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


void testLoadTileHonorsTileLineStrideAcrossOddRows() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    require(bus.write8(0x17C0U, 0x11U) &&
            bus.write8(0x17C1U, 0x22U) &&
            bus.write8(0x17C2U, 0x33U) &&
            bus.write8(0x17C3U, 0x44U),
            "seed two-row texture for tile line-stride test");
    for (std::uint32_t i = 0; i < 24U; ++i)
        state.tmem.writeByte(static_cast<std::uint16_t>(i), 0xA5U);

    const std::uint64_t setImage =
        (0x3DULL << 56U) | (1ULL << 51U) | (1ULL << 32U) | 0x17C0ULL;
    // A line value of two reserves 16 bytes between successive tile rows.
    const std::uint64_t setTile =
        (0x35ULL << 56U) | (1ULL << 51U) | (2ULL << 41U);
    const std::uint64_t loadTile =
        (0x34ULL << 56U) | (4ULL << 12U) | 4ULL;
    require(bus.write64(0xAE0U, setImage) &&
            bus.write64(0xAE8U, setTile) &&
            bus.write64(0xAF0U, loadTile),
            "seed LOAD_TILE line-stride command sequence");
    require(bus.write32(Bus::kDpStart, 0xAE0U) &&
            bus.write32(Bus::kDpEnd, 0xAF8U),
            "publish LOAD_TILE line-stride sequence");
    for (unsigned i = 0; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "LOAD_TILE line-stride setup and transfer are processed");

    std::uint8_t value = 0U;
    require(state.tmem.readByte(0U, value) && value == 0x11U &&
            state.tmem.readByte(1U, value) && value == 0x22U,
            "LOAD_TILE writes the first row at the tile base");
    require(state.tmem.readByte(20U, value) && value == 0x33U &&
            state.tmem.readByte(21U, value) && value == 0x44U,
            "LOAD_TILE applies line stride before odd-row half swap");
    require(state.tmem.readByte(16U, value) && value == 0xA5U &&
            state.tmem.readByte(17U, value) && value == 0xA5U,
            "LOAD_TILE leaves padding between tile rows untouched");
}

void testLoadTileWrapsDestinationAcrossTmemEnd() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    require(bus.write8(0x17E0U, 0x11U) && bus.write8(0x17E1U, 0x22U) &&
            bus.write8(0x17E2U, 0x33U) && bus.write8(0x17E3U, 0x44U),
            "seed two-row LOAD_TILE source near TMEM wrap");
    for (std::uint16_t i = 0U; i < 8U; ++i)
        require(state.tmem.writeByte(i, 0xEEU),
                "seed TMEM bytes at wrapped LOAD_TILE destination");

    const std::uint64_t image =
        (0x3DULL << 56U) | (1ULL << 51U) | (1ULL << 32U) | 0x17E0ULL;
    const std::uint64_t tile =
        (0x35ULL << 56U) | (1ULL << 51U) | (1ULL << 41U) |
        (511ULL << 32U);
    const std::uint64_t load = (0x34ULL << 56U) | (4ULL << 12U) | 4ULL;
    require(bus.write64(0xB80U, image) && bus.write64(0xB88U, tile) &&
            bus.write64(0xB90U, load),
            "seed LOAD_TILE command crossing TMEM end");
    require(bus.write32(Bus::kDpStart, 0xB80U) &&
            bus.write32(Bus::kDpEnd, 0xB98U),
            "publish wrapping LOAD_TILE sequence");
    for (unsigned i = 0; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "wrapping LOAD_TILE sequence executes");

    std::uint8_t value = 0U;
    require(state.tmem.readByte(4088U, value) && value == 0x11U &&
            state.tmem.readByte(4089U, value) && value == 0x22U,
            "LOAD_TILE preserves first row at final TMEM bytes");
    require(state.tmem.readByte(4U, value) && value == 0x33U &&
            state.tmem.readByte(5U, value) && value == 0x44U,
            "LOAD_TILE wraps odd-row-swizzled destination to TMEM start");
    require(state.tmem.readByte(0U, value) && value == 0xEEU &&
            state.tmem.readByte(1U, value) && value == 0xEEU,
            "LOAD_TILE wrap leaves bytes outside row span unchanged");
}

void testLoadTileFourBitOddSourceNibbleWrapsAcrossTmemEnd() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    // Three 4-bit texels per row: texel 1 is the low nibble of byte 0.
    require(bus.write8(0x1C00U, 0x12U) && bus.write8(0x1C01U, 0x30U) &&
            bus.write8(0x1C02U, 0x45U) && bus.write8(0x1C03U, 0x60U),
            "seed two rows with odd-start 4-bit source texels");
    for (std::uint16_t i = 0U; i < 8U; ++i)
        require(state.tmem.writeByte(i, 0xEEU),
                "seed TMEM bytes at wrapped 4-bit destination");

    const std::uint64_t image =
        (0x3DULL << 56U) | (2ULL << 32U) | 0x1C00ULL;
    const std::uint64_t tile =
        (0x35ULL << 56U) | (1ULL << 41U) | (511ULL << 32U);
    const std::uint64_t load =
        (0x34ULL << 56U) | (4ULL << 44U) | (8ULL << 12U) | 4ULL;
    require(bus.write64(0xB60U, image) && bus.write64(0xB68U, tile) &&
            bus.write64(0xB70U, load),
            "seed odd-source-nibble LOAD_TILE crossing TMEM end");
    require(bus.write32(Bus::kDpStart, 0xB60U) &&
            bus.write32(Bus::kDpEnd, 0xB78U),
            "publish odd-source-nibble wrapping LOAD_TILE sequence");
    for (unsigned i = 0U; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "odd-source-nibble wrapping LOAD_TILE sequence executes");

    std::uint8_t value = 0U;
    require(state.tmem.readByte(4088U, value) && value == 0x23U,
            "first row packs odd-start source nibbles at final TMEM byte");
    require(state.tmem.readByte(4U, value) && value == 0x45U,
            "odd row swizzles and wraps packed nibbles to TMEM start");
    require(state.tmem.readByte(0U, value) && value == 0xEEU &&
            state.tmem.readByte(1U, value) && value == 0xEEU,
            "combined nibble and wrap edge case preserves untouched TMEM bytes");
}

void testLoadTile16BitWrapsDestinationAcrossTmemEnd() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    const std::uint8_t source[] = {
        0x11U, 0x22U, 0x33U, 0x44U,
        0x55U, 0x66U, 0x77U, 0x88U
    };
    for (std::uint32_t i = 0; i < sizeof(source); ++i)
        require(bus.write8(0x1810U + i, source[i]),
                "seed two-row 16-bit LOAD_TILE source");
    for (std::uint16_t i = 0U; i < 8U; ++i)
        require(state.tmem.writeByte(i, 0xEEU),
                "seed wrapped 16-bit TMEM destination sentinels");

    const std::uint64_t image =
        (0x3DULL << 56U) | (2ULL << 51U) | (1ULL << 32U) | 0x1810ULL;
    const std::uint64_t tile =
        (0x35ULL << 56U) | (2ULL << 51U) | (1ULL << 41U) |
        (511ULL << 32U);
    const std::uint64_t load =
        (0x34ULL << 56U) | (4ULL << 12U) | 4ULL;
    require(bus.write64(0xBA0U, image) && bus.write64(0xBA8U, tile) &&
            bus.write64(0xBB0U, load),
            "seed 16-bit LOAD_TILE command crossing TMEM end");
    require(bus.write32(Bus::kDpStart, 0xBA0U) &&
            bus.write32(Bus::kDpEnd, 0xBB8U),
            "publish wrapping 16-bit LOAD_TILE sequence");
    for (unsigned i = 0; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "wrapping 16-bit LOAD_TILE sequence executes");

    std::uint8_t value = 0U;
    require(state.tmem.readByte(4088U, value) && value == 0x11U &&
            state.tmem.readByte(4089U, value) && value == 0x22U &&
            state.tmem.readByte(4090U, value) && value == 0x33U &&
            state.tmem.readByte(4091U, value) && value == 0x44U,
            "16-bit LOAD_TILE preserves first row at final TMEM bytes");
    require(state.tmem.readByte(4U, value) && value == 0x55U &&
            state.tmem.readByte(5U, value) && value == 0x66U &&
            state.tmem.readByte(6U, value) && value == 0x77U &&
            state.tmem.readByte(7U, value) && value == 0x88U,
            "16-bit LOAD_TILE wraps odd-row-swizzled second row to TMEM start");
    require(state.tmem.readByte(0U, value) && value == 0xEEU &&
            state.tmem.readByte(1U, value) && value == 0xEEU &&
            state.tmem.readByte(2U, value) && value == 0xEEU &&
            state.tmem.readByte(3U, value) && value == 0xEEU,
            "16-bit LOAD_TILE wrap leaves bytes outside row span unchanged");
}

void testLoadTile4BitWrapsDestinationAcrossTmemEnd() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    require(bus.write8(0x1820U, 0x12U) &&
            bus.write8(0x1821U, 0x34U),
            "seed two-row 4-bit LOAD_TILE source");
    for (std::uint16_t i = 0U; i < 8U; ++i)
        require(state.tmem.writeByte(i, 0xEEU),
                "seed wrapped 4-bit TMEM destination sentinels");

    const std::uint64_t image =
        (0x3DULL << 56U) | (1ULL << 32U) | 0x1820ULL;
    const std::uint64_t tile =
        (0x35ULL << 56U) | (1ULL << 41U) | (511ULL << 32U);
    const std::uint64_t load =
        (0x34ULL << 56U) | (4ULL << 12U) | 4ULL;
    require(bus.write64(0xBC0U, image) && bus.write64(0xBC8U, tile) &&
            bus.write64(0xBD0U, load),
            "seed 4-bit LOAD_TILE command crossing TMEM end");
    require(bus.write32(Bus::kDpStart, 0xBC0U) &&
            bus.write32(Bus::kDpEnd, 0xBD8U),
            "publish wrapping 4-bit LOAD_TILE sequence");
    for (unsigned i = 0; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "wrapping 4-bit LOAD_TILE sequence executes");

    std::uint8_t value = 0U;
    require(state.tmem.readByte(4088U, value) && value == 0x12U,
            "4-bit LOAD_TILE packs first row at final TMEM bytes");
    require(state.tmem.readByte(4U, value) && value == 0x34U,
            "4-bit LOAD_TILE packs both nibbles after odd-row TMEM wrap");
    require(state.tmem.readByte(0U, value) && value == 0xEEU &&
            state.tmem.readByte(1U, value) && value == 0xEEU &&
            state.tmem.readByte(2U, value) && value == 0xEEU &&
            state.tmem.readByte(3U, value) && value == 0xEEU,
            "4-bit LOAD_TILE wrap leaves bytes outside row span unchanged");
}

void testLoadTileRejectsInvertedCoordinatesWithoutWrites() {
    using namespace m64plus::n64;
    const std::uint64_t invalidLoads[] = {
        (0x34ULL << 56U) | (4ULL << 44U), // SL > SH
        (0x34ULL << 56U) | (4ULL << 32U)  // TL > TH
    };

    for (std::size_t scenario = 0; scenario < 2U; ++scenario) {
        Bus bus;
        RdpSyncState state{};
        require(bus.write8(0x17D0U, 0x12U) &&
                bus.write8(0x17D1U, 0x34U) &&
                bus.write8(0x17D2U, 0x56U),
                "seed source bytes for inverted LOAD_TILE coordinates");
        for (std::uint16_t i = 0U; i < 8U; ++i)
            state.tmem.writeByte(i, static_cast<std::uint8_t>(0xA0U + i));

        const std::uint64_t setImage =
            (0x3DULL << 56U) | (1ULL << 51U) | (1ULL << 32U) | 0x17D0ULL;
        const std::uint64_t setTile = (0x35ULL << 56U);
        require(bus.write64(0xB60U, setImage) &&
                bus.write64(0xB68U, setTile) &&
                bus.write64(0xB70U, invalidLoads[scenario]),
                "seed inverted-coordinate LOAD_TILE sequence");
        require(bus.write32(Bus::kDpStart, 0xB60U) &&
                bus.write32(Bus::kDpEnd, 0xB78U),
                "publish inverted-coordinate LOAD_TILE sequence");
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed &&
                RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "configure image and tile before inverted LOAD_TILE");
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Unsupported,
                scenario == 0U
                    ? "LOAD_TILE rejects SL greater than SH"
                    : "LOAD_TILE rejects TL greater than TH");

        for (std::uint16_t i = 0U; i < 8U; ++i) {
            std::uint8_t value = 0U;
            require(state.tmem.readByte(i, value) &&
                    value == static_cast<std::uint8_t>(0xA0U + i),
                    "inverted-coordinate LOAD_TILE leaves TMEM unchanged");
        }
    }
}

void testLoadTileValidatesEverySourceRowBeforeWritingTmem() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    const std::uint32_t lastByte = static_cast<std::uint32_t>(Rdram::kSize - 1U);
    require(bus.write8(lastByte, 0x7BU),
            "seed final valid byte for multi-row LOAD_TILE");
    for (std::uint16_t i = 0U; i < 8U; ++i)
        state.tmem.writeByte(i, static_cast<std::uint8_t>(0xC0U + i));

    const std::uint64_t setImage =
        (0x3DULL << 56U) | (1ULL << 51U) | lastByte;
    const std::uint64_t setTile =
        (0x35ULL << 56U) | (1ULL << 41U);
    // One texel per row: row zero is valid, but row one exceeds RDRAM.
    const std::uint64_t loadTile =
        (0x34ULL << 56U) | (4ULL << 12U);
    require(bus.write64(0xB40U, setImage) &&
            bus.write64(0xB48U, setTile) &&
            bus.write64(0xB50U, loadTile),
            "seed multi-row out-of-range LOAD_TILE sequence");
    require(bus.write32(Bus::kDpStart, 0xB40U) &&
            bus.write32(Bus::kDpEnd, 0xB58U),
            "publish multi-row out-of-range LOAD_TILE sequence");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed &&
            RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "configure image and tile before multi-row invalid LOAD_TILE");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Unsupported,
            "LOAD_TILE rejects a later source row outside RDRAM");

    for (std::uint16_t i = 0U; i < 8U; ++i) {
        std::uint8_t value = 0U;
        require(state.tmem.readByte(i, value) &&
                value == static_cast<std::uint8_t>(0xC0U + i),
                "invalid later source row leaves all TMEM bytes unchanged");
    }
}

void testLoadTileRejectsRangePastTextureImageWidthWithoutWrites() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    require(bus.write8(0x17E0U, 0x11U) &&
            bus.write8(0x17E1U, 0x22U) &&
            bus.write8(0x17E2U, 0x33U),
            "seed bytes around the declared texture row boundary");
    state.tmem.writeByte(0U, 0xA5U);
    state.tmem.writeByte(1U, 0x5AU);
    const std::uint64_t setImage =
        (0x3DULL << 56U) | (1ULL << 51U) | (1ULL << 32U) | 0x17E0ULL;
    const std::uint64_t setTile = (0x35ULL << 56U);
    // The image is two texels wide, but this rectangle requests three.
    const std::uint64_t loadTile =
        (0x34ULL << 56U) | (8ULL << 12U);
    require(bus.write64(0xB80U, setImage) &&
            bus.write64(0xB88U, setTile) &&
            bus.write64(0xB90U, loadTile),
            "seed LOAD_TILE image-width rejection sequence");
    require(bus.write32(Bus::kDpStart, 0xB80U) &&
            bus.write32(Bus::kDpEnd, 0xB98U),
            "publish LOAD_TILE image-width rejection sequence");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed &&
            RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "configure image and tile before invalid LOAD_TILE");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Unsupported,
            "LOAD_TILE rejects a rectangle extending past image width");
    std::uint8_t value = 0U;
    require(state.tmem.readByte(0U, value) && value == 0xA5U &&
            state.tmem.readByte(1U, value) && value == 0x5AU,
            "out-of-width LOAD_TILE leaves TMEM unchanged");
}

void testLoadTileRejectsOutOfRangeSourceWithoutPartialTmemWrites() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    require(bus.write8(static_cast<std::uint32_t>(Rdram::kSize - 1U), 0xABU),
            "seed final valid RDRAM byte");
    state.tmem.writeByte(0U, 0x5AU);
    state.tmem.writeByte(1U, 0xC3U);

    const std::uint64_t setImage =
        (0x3DULL << 56U) | (1ULL << 51U) | (1ULL << 32U) |
        static_cast<std::uint64_t>(Rdram::kSize - 1U);
    const std::uint64_t setTile =
        (0x35ULL << 56U) | (1ULL << 51U) | (1ULL << 41U);
    const std::uint64_t loadTile =
        (0x34ULL << 56U) | (4ULL << 12U);
    require(bus.write64(0xB00U, setImage) &&
            bus.write64(0xB08U, setTile) &&
            bus.write64(0xB10U, loadTile),
            "seed out-of-range LOAD_TILE sequence");
    require(bus.write32(Bus::kDpStart, 0xB00U) &&
            bus.write32(Bus::kDpEnd, 0xB18U),
            "publish out-of-range LOAD_TILE sequence");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed &&
            RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "configure image and tile before invalid LOAD_TILE");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Unsupported,
            "LOAD_TILE rejects a source rectangle crossing RDRAM end");

    std::uint8_t value = 0U;
    require(state.tmem.readByte(0U, value) && value == 0x5AU &&
            state.tmem.readByte(1U, value) && value == 0xC3U,
            "invalid LOAD_TILE leaves all destination TMEM bytes unchanged");
}

void testLoadTileFourBitRangeStartingAtOddSourceNibble() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    require(bus.write8(0x1780U, 0x12U) &&
            bus.write8(0x1781U, 0x34U),
            "seed 4-bit image for odd-nibble source range");
    const std::uint64_t setImage =
        (0x3DULL << 56U) | (3ULL << 32U) | 0x1780ULL;
    const std::uint64_t setTile =
        (0x35ULL << 56U) | (1ULL << 41U);
    const std::uint64_t loadTile =
        (0x34ULL << 56U) | (4ULL << 44U) | (8ULL << 12U);
    require(bus.write64(0xA20U, setImage) &&
            bus.write64(0xA28U, setTile) &&
            bus.write64(0xA30U, loadTile),
            "seed odd-nibble 4-bit LOAD_TILE sequence");
    require(bus.write32(Bus::kDpStart, 0xA20U) &&
            bus.write32(Bus::kDpEnd, 0xA38U),
            "publish odd-nibble 4-bit LOAD_TILE sequence");
    for (unsigned i = 0; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) ==
                    RdpProcessResult::Processed,
                "odd-nibble 4-bit LOAD_TILE command is processed");

    std::uint8_t value = 0U;
    require(state.tmem.readByte(0U, value) && value == 0x23U,
            "4-bit LOAD_TILE selects source low nibble then next high nibble");
}

void testLoadTileFourBitOddSourceNibbleAcrossSwizzledRows() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    require(bus.write8(0x1790U, 0x12U) &&
            bus.write8(0x1791U, 0x34U) &&
            bus.write8(0x1792U, 0x56U) &&
            bus.write8(0x1793U, 0x78U),
            "seed two-row 4-bit image for odd-nibble range");
    const std::uint64_t setImage =
        (0x3DULL << 56U) | (3ULL << 32U) | 0x1790ULL;
    const std::uint64_t setTile =
        (0x35ULL << 56U) | (1ULL << 41U);
    const std::uint64_t loadTile =
        (0x34ULL << 56U) | (4ULL << 44U) | (8ULL << 12U) | 4ULL;
    require(bus.write64(0xA60U, setImage) &&
            bus.write64(0xA68U, setTile) &&
            bus.write64(0xA70U, loadTile),
            "seed two-row odd-nibble LOAD_TILE sequence");
    require(bus.write32(Bus::kDpStart, 0xA60U) &&
            bus.write32(Bus::kDpEnd, 0xA78U),
            "publish two-row odd-nibble LOAD_TILE sequence");
    for (unsigned i = 0; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) ==
                    RdpProcessResult::Processed,
                "two-row odd-nibble LOAD_TILE commands execute");

    std::uint8_t value = 0U;
    require(state.tmem.readByte(0U, value) && value == 0x23U,
            "first row packs low source nibble before next high nibble");
    require(state.tmem.readByte(12U, value) && value == 0x67U,
            "odd row packs source nibbles after TMEM word swizzle");
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

    // Fill both TMEM banks with sentinels so the line-stride gap can
    // be checked independently of the loaded RGBA32 texels.
    for (std::uint16_t i = 20U; i < 28U; ++i) {
        require(state.tmem.writeByte(i, 0xA5U),
                "seed lower-bank RGBA32 row padding sentinel");
        require(state.tmem.writeByte(static_cast<std::uint16_t>(0x800U + i),
                                     0x5AU),
                "seed upper-bank RGBA32 row padding sentinel");
    }

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
    for (std::uint16_t i = 20U; i < 28U; ++i) {
        require(state.tmem.readByte(i, value) && value == 0xA5U,
                "RGBA32 LOAD_TILE preserves lower-bank line padding");
        require(state.tmem.readByte(static_cast<std::uint16_t>(0x800U + i), value) &&
                value == 0x5AU,
                "RGBA32 LOAD_TILE preserves upper-bank line padding");
    }
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

void testLoadTile32BitUsesNonzeroSourceXAcrossRows() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    const std::uint8_t source[] = {
        0x00U, 0x01U, 0x02U, 0x03U, 0x10U, 0x11U, 0x12U, 0x13U,
        0x20U, 0x21U, 0x22U, 0x23U, 0x30U, 0x31U, 0x32U, 0x33U,
        0x40U, 0x41U, 0x42U, 0x43U, 0x50U, 0x51U, 0x52U, 0x53U,
        0x60U, 0x61U, 0x62U, 0x63U, 0x70U, 0x71U, 0x72U, 0x73U
    };
    for (std::uint32_t i = 0U; i < sizeof(source); ++i)
        require(bus.write8(0x1E00U + i, source[i]),
                "seed three-wide two-row RGBA32 source");
    const std::uint64_t setImage =
        (0x3DULL << 56U) | (3ULL << 51U) | (3ULL << 32U) | 0x1E00ULL;
    const std::uint64_t setTile =
        (0x35ULL << 56U) | (3ULL << 51U) | (1ULL << 41U);
    const std::uint64_t loadTile =
        (0x34ULL << 56U) | (4ULL << 44U) |
        (8ULL << 12U) | 4ULL;
    require(bus.write64(0xC10U, setImage) &&
            bus.write64(0xC18U, setTile) &&
            bus.write64(0xC20U, loadTile),
            "seed offset-source RGBA32 LOAD_TILE sequence");
    require(bus.write32(Bus::kDpStart, 0xC10U) &&
            bus.write32(Bus::kDpEnd, 0xC28U),
            "publish offset-source RGBA32 LOAD_TILE sequence");
    for (unsigned i = 0U; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "offset-source RGBA32 LOAD_TILE executes");

    const auto expect = [&state](std::uint16_t address, std::uint8_t expected,
                                 const char* message) {
        std::uint8_t value = 0U;
        require(state.tmem.readByte(address, value) && value == expected, message);
    };
    expect(0U, 0x10U, "first selected texel red comes from nonzero source X");
    expect(1U, 0x11U, "first selected texel green comes from nonzero source X");
    expect(2U, 0x20U, "second selected texel red follows source X");
    expect(3U, 0x21U, "second selected texel green follows source X");
    expect(0x800U, 0x12U, "first selected texel blue uses upper bank");
    expect(0x801U, 0x13U, "first selected texel alpha uses upper bank");
    expect(0x802U, 0x22U, "second selected texel blue uses upper bank");
    expect(0x803U, 0x23U, "second selected texel alpha uses upper bank");
    expect(12U, 0x50U, "second source row applies odd-row swizzle at tile stride");
    expect(13U, 0x51U, "second source row preserves green channel");
    expect(14U, 0x60U, "second source row selects next texel at nonzero X");
    expect(15U, 0x61U, "second source row selects next texel green");
    expect(0x80CU, 0x52U, "second source row blue is swizzled in upper bank");
    expect(0x80DU, 0x53U, "second source row alpha is swizzled in upper bank");
    expect(0x80EU, 0x62U, "second source row second blue texel");
    expect(0x80FU, 0x63U, "second source row second alpha texel");
}

void testLoadTile32BitWrapsBothBanksAcrossTmemEnd() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    const std::uint8_t source[] = {
        0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U, 0x88U,
        0x99U, 0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU, 0xFFU, 0x10U
    };
    for (std::uint32_t i = 0U; i < sizeof(source); ++i)
        require(bus.write8(0x1D00U + i, source[i]),
                "seed two-row RGBA32 source for TMEM bank wrap");
    for (std::uint16_t i = 0U; i < 8U; ++i) {
        require(state.tmem.writeByte(i, 0xE0U) &&
                state.tmem.writeByte(static_cast<std::uint16_t>(0x800U + i), 0xE1U),
                "seed lower and upper RGBA32 TMEM wrap sentinels");
    }

    const std::uint64_t setImage =
        (0x3DULL << 56U) | (3ULL << 51U) | (1ULL << 32U) | 0x1D00ULL;
    const std::uint64_t setTile =
        (0x35ULL << 56U) | (3ULL << 51U) | (1ULL << 41U) |
        (511ULL << 32U);
    const std::uint64_t loadTile =
        (0x34ULL << 56U) | (4ULL << 12U) | 4ULL;
    require(bus.write64(0xBE0U, setImage) &&
            bus.write64(0xBE8U, setTile) &&
            bus.write64(0xBF0U, loadTile),
            "seed two-row RGBA32 LOAD_TILE crossing both TMEM bank ends");
    require(bus.write32(Bus::kDpStart, 0xBE0U) &&
            bus.write32(Bus::kDpEnd, 0xBF8U),
            "publish RGBA32 dual-bank wrapping LOAD_TILE sequence");
    for (unsigned i = 0U; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "RGBA32 dual-bank wrapping LOAD_TILE sequence executes");

    const auto expect = [&state](std::uint16_t address, std::uint8_t expected,
                                 const char* message) {
        std::uint8_t value = 0U;
        require(state.tmem.readByte(address, value) && value == expected, message);
    };
    expect(0x7F8U, 0x11U, "RGBA32 first texel red wraps to lower bank end");
    expect(0x7F9U, 0x22U, "RGBA32 first texel green wraps to lower bank end");
    expect(0x7FAU, 0x55U, "RGBA32 second texel red follows at lower bank end");
    expect(0x7FBU, 0x66U, "RGBA32 second texel green follows at lower bank end");
    expect(0xFF8U, 0x33U, "RGBA32 first texel blue wraps to upper bank end");
    expect(0xFF9U, 0x44U, "RGBA32 first texel alpha wraps to upper bank end");
    expect(0xFFAU, 0x77U, "RGBA32 second texel blue follows at upper bank end");
    expect(0xFFBU, 0x88U, "RGBA32 second texel alpha follows at upper bank end");
    expect(4U, 0x99U, "RGBA32 odd row swizzles red to lower bank start");
    expect(5U, 0xAAU, "RGBA32 odd row swizzles green to lower bank start");
    expect(6U, 0xDDU, "RGBA32 odd row swizzles second red texel");
    expect(7U, 0xEEU, "RGBA32 odd row swizzles second green texel");
    expect(0x804U, 0xBBU, "RGBA32 odd row swizzles blue to upper bank start");
    expect(0x805U, 0xCCU, "RGBA32 odd row swizzles alpha to upper bank start");
    expect(0x806U, 0xFFU, "RGBA32 odd row swizzles second blue texel");
    expect(0x807U, 0x10U, "RGBA32 odd row swizzles second alpha texel");
    expect(0U, 0xE0U, "RGBA32 lower bank wrap preserves bytes outside loaded span");
    expect(1U, 0xE0U, "RGBA32 lower bank wrap preserves adjacent sentinel");
    expect(0x800U, 0xE1U, "RGBA32 upper bank wrap preserves bytes outside loaded span");
    expect(0x801U, 0xE1U, "RGBA32 upper bank wrap preserves adjacent sentinel");
}

void testLoadTile32BitValidatesWholeSourceBeforeWritingEitherBank() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    const std::uint32_t sourceBase = static_cast<std::uint32_t>(Rdram::kSize - 4U);
    require(bus.write32(sourceBase, 0x11223344U),
            "seed final valid RGBA32 source texel");
    for (std::uint16_t i = 0U; i < 8U; ++i) {
        require(state.tmem.writeByte(i, static_cast<std::uint8_t>(0xA0U + i)) &&
                state.tmem.writeByte(static_cast<std::uint16_t>(0x800U + i),
                                     static_cast<std::uint8_t>(0xB0U + i)),
                "seed lower and upper TMEM bank sentinels");
    }

    const std::uint64_t setImage =
        (0x3DULL << 56U) | (3ULL << 51U) | (1ULL << 32U) | sourceBase;
    const std::uint64_t setTile =
        (0x35ULL << 56U) | (3ULL << 51U) | (1ULL << 41U);
    const std::uint64_t loadTile =
        (0x34ULL << 56U) | (4ULL << 12U);
    require(bus.write64(0xB00U, setImage) &&
            bus.write64(0xB08U, setTile) &&
            bus.write64(0xB10U, loadTile),
            "seed RGBA32 LOAD_TILE with second source texel beyond RDRAM");
    require(bus.write32(Bus::kDpStart, 0xB00U) &&
            bus.write32(Bus::kDpEnd, 0xB18U),
            "publish invalid RGBA32 source sequence");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed &&
            RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "configure RGBA32 image and tile before source validation");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Unsupported,
            "RGBA32 LOAD_TILE rejects a rectangle crossing the RDRAM boundary");

    for (std::uint16_t i = 0U; i < 8U; ++i) {
        std::uint8_t lower = 0U;
        std::uint8_t upper = 0U;
        require(state.tmem.readByte(i, lower) &&
                state.tmem.readByte(static_cast<std::uint16_t>(0x800U + i), upper) &&
                lower == static_cast<std::uint8_t>(0xA0U + i) &&
                upper == static_cast<std::uint8_t>(0xB0U + i),
                "rejected RGBA32 LOAD_TILE leaves both TMEM banks unchanged");
    }
}

void testLoadTile32BitValidatesEverySourceRowBeforeWritingBanks() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    const std::uint32_t sourceBase = static_cast<std::uint32_t>(Rdram::kSize - 4U);
    require(bus.write32(sourceBase, 0x11223344U),
            "seed final valid RGBA32 source row");
    for (std::uint16_t i = 0U; i < 8U; ++i) {
        require(state.tmem.writeByte(i, static_cast<std::uint8_t>(0xC0U + i)) &&
                state.tmem.writeByte(static_cast<std::uint16_t>(0x800U + i),
                                     static_cast<std::uint8_t>(0xD0U + i)),
                "seed both TMEM banks before invalid second row");
    }

    const std::uint64_t setImage =
        (0x3DULL << 56U) | (3ULL << 51U) | (1ULL << 32U) | sourceBase;
    const std::uint64_t setTile =
        (0x35ULL << 56U) | (3ULL << 51U) | (1ULL << 41U) | (1ULL << 32U);
    const std::uint64_t loadTile =
        (0x34ULL << 56U) | (4ULL << 32U);
    require(bus.write64(0xB40U, setImage) &&
            bus.write64(0xB48U, setTile) &&
            bus.write64(0xB50U, loadTile),
            "seed two-row RGBA32 LOAD_TILE with second row beyond RDRAM");
    require(bus.write32(Bus::kDpStart, 0xB40U) &&
            bus.write32(Bus::kDpEnd, 0xB58U),
            "publish two-row invalid RGBA32 source sequence");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed &&
            RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "configure RGBA32 image and tile before validating both rows");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Unsupported,
            "RGBA32 LOAD_TILE rejects a later row crossing the RDRAM boundary");

    for (std::uint16_t i = 0U; i < 8U; ++i) {
        std::uint8_t lower = 0U;
        std::uint8_t upper = 0U;
        require(state.tmem.readByte(i, lower) &&
                state.tmem.readByte(static_cast<std::uint16_t>(0x800U + i), upper) &&
                lower == static_cast<std::uint8_t>(0xC0U + i) &&
                upper == static_cast<std::uint8_t>(0xD0U + i),
                "rejected later RGBA32 row leaves both TMEM banks unchanged");
    }
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

void testLoadBlockUpdatesTileSizeAndSetTileSizeOverrides() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    for (std::uint32_t i = 0U; i < 8U; ++i)
        require(bus.write8(0x1F80U + i, static_cast<std::uint8_t>(0x60U + i)),
                "seed LOAD_BLOCK tile-size state source");

    const std::uint64_t setImage =
        (0x3DULL << 56U) | (1ULL << 51U) | (7ULL << 32U) | 0x1F80U;
    const std::uint64_t setTile = (0x35ULL << 56U);
    const std::uint64_t loadBlock =
        (0x33ULL << 56U) | (28ULL << 12U);
    const std::uint64_t setTileSize =
        (0x32ULL << 56U) | (12ULL << 44U) | (16ULL << 32U) |
        (20ULL << 12U) | 24ULL;
    constexpr std::uint32_t start = 0xBB0U;
    const std::uint64_t commands[] = {setImage, setTile, loadBlock, setTileSize};
    for (std::size_t i = 0U; i < 4U; ++i)
        require(bus.write64(start + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "seed LOAD_BLOCK and SET_TILE_SIZE sequence");
    require(bus.write32(Bus::kDpStart, start) &&
            bus.write32(Bus::kDpEnd, start + 32U),
            "publish LOAD_BLOCK tile-size sequence");

    for (unsigned i = 0U; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "LOAD_BLOCK setup and transfer are processed");
    require(state.hasTileSize[0] &&
            state.tileSizes[0].sl == 0U && state.tileSizes[0].tl == 0U &&
            state.tileSizes[0].sh == 28U && state.tileSizes[0].th == 0U,
            "LOAD_BLOCK records its S/T bounds in tile-size state");

    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "SET_TILE_SIZE after LOAD_BLOCK is processed");
    require(state.tileSizes[0].sl == 12U && state.tileSizes[0].tl == 16U &&
            state.tileSizes[0].sh == 20U && state.tileSizes[0].th == 24U,
            "SET_TILE_SIZE replaces the bounds written by LOAD_BLOCK");
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

void testLoadBlockWrapsDestinationAcrossTmemEnd() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    for (std::uint32_t i = 0; i < 16U; ++i)
        require(bus.write8(0x1E00U + i, static_cast<std::uint8_t>(0x40U + i)),
                "seed LOAD_BLOCK data for TMEM wrap");
    for (std::uint32_t i = 0; i < 9U; ++i) {
        require(state.tmem.writeByte(static_cast<std::uint16_t>(i), 0xEEU),
                "seed TMEM bytes after wrap destination");
    }

    const std::uint64_t image =
        (0x3DULL << 56U) | (1ULL << 51U) | (15ULL << 32U) | 0x1E00ULL;
    const std::uint64_t tile =
        (0x35ULL << 56U) | (511ULL << 32U);
    const std::uint64_t load =
        (0x33ULL << 56U) | (60ULL << 12U);
    require(bus.write64(0xB00U, image) &&
            bus.write64(0xB08U, tile) &&
            bus.write64(0xB10U, load), "seed wrapping LOAD_BLOCK sequence");
    require(bus.write32(Bus::kDpStart, 0xB00U) &&
            bus.write32(Bus::kDpEnd, 0xB18U), "publish wrapping LOAD_BLOCK sequence");
    for (unsigned i = 0; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "wrapping LOAD_BLOCK sequence executes");

    std::uint8_t value = 0U;
    for (std::uint32_t i = 0; i < 16U; ++i) {
        const std::uint16_t address = static_cast<std::uint16_t>((4088U + i) & 0x0FFFU);
        require(state.tmem.readByte(address, value) &&
                value == static_cast<std::uint8_t>(0x40U + i),
                "LOAD_BLOCK wraps destination bytes at TMEM boundary");
    }
    require(state.tmem.readByte(8U, value) && value == 0xEEU,
            "LOAD_BLOCK wrap does not overwrite bytes beyond its destination span");
}

void testLoadBlockRejectsSourcePastRdramEndWithoutPartialWrites() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    constexpr std::uint32_t source = static_cast<std::uint32_t>(Rdram::kSize - 4U);
    for (std::uint32_t i = 0; i < 4U; ++i)
        require(bus.write8(source + i, static_cast<std::uint8_t>(0x30U + i)),
                "seed valid prefix at end of RDRAM");
    for (std::uint32_t i = 0; i < 8U; ++i)
        require(state.tmem.writeByte(static_cast<std::uint16_t>(i), 0xEEU),
                "seed TMEM before invalid LOAD_BLOCK source");

    const std::uint64_t image =
        (0x3DULL << 56U) | (1ULL << 51U) | (7ULL << 32U) | source;
    const std::uint64_t tile = (0x35ULL << 56U);
    const std::uint64_t load = (0x33ULL << 56U) | (28ULL << 12U);
    require(bus.write64(0xB20U, image) && bus.write64(0xB28U, tile) &&
            bus.write64(0xB30U, load),
            "seed LOAD_BLOCK crossing RDRAM boundary");
    require(bus.write32(Bus::kDpStart, 0xB20U) &&
            bus.write32(Bus::kDpEnd, 0xB38U),
            "publish RDRAM-boundary LOAD_BLOCK sequence");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "SET_TEXTURE_IMAGE configures end-of-RDRAM source");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "SET_TILE configures RDRAM-boundary test");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Unsupported,
            "LOAD_BLOCK rejects a source range extending beyond RDRAM");

    std::uint8_t value = 0U;
    for (std::uint16_t i = 0; i < 8U; ++i)
        require(state.tmem.readByte(i, value) && value == 0xEEU,
                "rejected LOAD_BLOCK leaves all destination TMEM bytes unchanged");
}

void testLoadBlockEightBitRejectsSourceOverflowAtomically() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    constexpr std::uint32_t source =
        static_cast<std::uint32_t>(Rdram::kSize - 4U);
    for (std::uint32_t i = 0U; i < 4U; ++i)
        require(bus.write8(source + i, static_cast<std::uint8_t>(0xA0U + i)),
                "seed valid eight-bit source prefix at RDRAM end");
    for (std::uint16_t i = 0U; i < 8U; ++i)
        require(state.tmem.writeByte(i, 0x5AU),
                "seed TMEM before overflowing eight-bit LOAD_BLOCK");

    const std::uint64_t image =
        (0x3DULL << 56U) | (1ULL << 51U) | source;
    const std::uint64_t tile =
        (0x35ULL << 56U) | (1ULL << 51U);
    const std::uint64_t load = (0x33ULL << 56U) | (28ULL << 12U);
    require(bus.write64(0xB80U, image) &&
            bus.write64(0xB88U, tile) &&
            bus.write64(0xB90U, load),
            "seed overflowing eight-bit LOAD_BLOCK");
    require(bus.write32(Bus::kDpStart, 0xB80U) &&
            bus.write32(Bus::kDpEnd, 0xB98U),
            "publish overflowing eight-bit LOAD_BLOCK");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "SET_TEXTURE_IMAGE configures eight-bit source");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "SET_TILE configures eight-bit destination");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Unsupported,
            "eight-bit LOAD_BLOCK rejects source range beyond RDRAM");

    std::uint8_t value = 0U;
    for (std::uint16_t i = 0U; i < 8U; ++i)
        require(state.tmem.readByte(i, value) && value == 0x5AU,
                "rejected eight-bit LOAD_BLOCK leaves TMEM unchanged");
}

void testLoadBlockEightBitDxtRejectsSourceOverflowAtomically() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    const std::uint32_t source = static_cast<std::uint32_t>(Rdram::kSize - 8U);
    for (std::uint32_t i = 0U; i < 8U; ++i)
        require(bus.write8(source + i, static_cast<std::uint8_t>(0xA0U + i)),
                "seed valid eight-bit DXT source prefix at RDRAM end");
    for (std::uint16_t i = 0U; i < 32U; ++i)
        require(state.tmem.writeByte(i, 0x5AU),
                "seed TMEM before overflowing eight-bit DXT LOAD_BLOCK");

    const std::uint64_t image =
        (0x3DULL << 56U) | (1ULL << 51U) | (31ULL << 32U) | source;
    const std::uint64_t tile = (0x35ULL << 56U) | (1ULL << 51U);
    const std::uint64_t load =
        (0x33ULL << 56U) | (60ULL << 12U) | 0x800ULL;
    require(bus.write64(0xB40U, image) && bus.write64(0xB48U, tile) &&
            bus.write64(0xB50U, load), "seed overflowing eight-bit DXT LOAD_BLOCK");
    require(bus.write32(Bus::kDpStart, 0xB40U) &&
            bus.write32(Bus::kDpEnd, 0xB58U),
            "publish overflowing eight-bit DXT LOAD_BLOCK");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "SET_TEXTURE_IMAGE configures eight-bit DXT source");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "SET_TILE configures eight-bit DXT destination");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Unsupported,
            "eight-bit DXT LOAD_BLOCK rejects source range beyond RDRAM");

    std::uint8_t value = 0U;
    for (std::uint16_t i = 0U; i < 32U; ++i)
        require(state.tmem.readByte(i, value) && value == 0x5AU,
                "rejected eight-bit DXT LOAD_BLOCK leaves TMEM unchanged");
}

void testLoadBlockSixteenBitRejectsSourceOverflowAtomically() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    const std::uint32_t source = static_cast<std::uint32_t>(Rdram::kSize - 4U);
    for (std::uint32_t i = 0U; i < 4U; ++i)
        require(bus.write8(source + i, static_cast<std::uint8_t>(0xA0U + i)),
                "seed valid 16-bit source prefix at RDRAM end");
    for (std::uint16_t i = 0U; i < 16U; ++i)
        require(state.tmem.writeByte(i, 0x5AU),
                "seed TMEM before overflowing 16-bit LOAD_BLOCK");
    const std::uint64_t image =
        (0x3DULL << 56U) | (2ULL << 51U) | (3ULL << 32U) | source;
    const std::uint64_t tile = (0x35ULL << 56U) | (2ULL << 51U);
    const std::uint64_t load = (0x33ULL << 56U) | (12ULL << 12U);
    require(bus.write64(0xB60U, image) && bus.write64(0xB68U, tile) &&
            bus.write64(0xB70U, load), "seed overflowing 16-bit LOAD_BLOCK");
    require(bus.write32(Bus::kDpStart, 0xB60U) &&
            bus.write32(Bus::kDpEnd, 0xB78U),
            "publish overflowing 16-bit LOAD_BLOCK");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "SET_TEXTURE_IMAGE configures 16-bit source");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "SET_TILE configures 16-bit destination");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Unsupported,
            "16-bit LOAD_BLOCK rejects source range beyond RDRAM");
    std::uint8_t value = 0U;
    for (std::uint16_t i = 0U; i < 16U; ++i)
        require(state.tmem.readByte(i, value) && value == 0x5AU,
                "rejected 16-bit LOAD_BLOCK leaves TMEM unchanged");
}

void testLoadBlockSixteenBitDxtRejectsSourceOverflowAtomically() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    const std::uint32_t source = static_cast<std::uint32_t>(Rdram::kSize - 8U);
    for (std::uint32_t i = 0U; i < 8U; ++i)
        require(bus.write8(source + i, static_cast<std::uint8_t>(0xB0U + i)),
                "seed valid 16-bit DXT source prefix at RDRAM end");
    for (std::uint16_t i = 0U; i < 32U; ++i)
        require(state.tmem.writeByte(i, 0x6BU),
                "seed TMEM before overflowing 16-bit DXT LOAD_BLOCK");

    const std::uint64_t image =
        (0x3DULL << 56U) | (2ULL << 51U) | (31ULL << 32U) | source;
    const std::uint64_t tile = (0x35ULL << 56U) | (2ULL << 51U);
    const std::uint64_t load =
        (0x33ULL << 56U) | (20ULL << 12U) | 0x800ULL;
    require(bus.write64(0xB20U, image) && bus.write64(0xB28U, tile) &&
            bus.write64(0xB30U, load), "seed overflowing 16-bit DXT LOAD_BLOCK");
    require(bus.write32(Bus::kDpStart, 0xB20U) &&
            bus.write32(Bus::kDpEnd, 0xB38U),
            "publish overflowing 16-bit DXT LOAD_BLOCK");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "SET_TEXTURE_IMAGE configures 16-bit DXT source");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "SET_TILE configures 16-bit DXT destination");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Unsupported,
            "16-bit DXT LOAD_BLOCK rejects source range beyond RDRAM");

    std::uint8_t value = 0U;
    for (std::uint16_t i = 0U; i < 32U; ++i)
        require(state.tmem.readByte(i, value) && value == 0x6BU,
                "rejected 16-bit DXT LOAD_BLOCK leaves TMEM unchanged");
}

void testLoadBlockDxtCarriesAtExactHalfLine() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    for (std::uint32_t i = 0; i < 24U; ++i)
        require(bus.write8(0x1F00U + i, static_cast<std::uint8_t>(0x40U + i)),
                "seed exact-half-line DXT data");

    const std::uint64_t image =
        (0x3DULL << 56U) | (1ULL << 51U) | (31ULL << 32U) | 0x1F00U;
    const std::uint64_t tile = (0x35ULL << 56U);
    // Exactly 0x800 advances one complete line after each 64-bit word.
    const std::uint64_t load =
        (0x33ULL << 56U) | (92ULL << 12U) | 0x800ULL;
    require(bus.write64(0xB90U, image) &&
            bus.write64(0xB98U, tile) &&
            bus.write64(0xBA0U, load),
            "seed exact-half-line DXT command sequence");
    require(bus.write32(Bus::kDpStart, 0xB90U) &&
            bus.write32(Bus::kDpEnd, 0xBA8U),
            "publish exact-half-line DXT sequence");
    for (unsigned i = 0; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) ==
                    RdpProcessResult::Processed,
                "exact-half-line DXT sequence executes");

    std::uint8_t value = 0U;
    require(state.tmem.readByte(0U, value) && value == 0x40U &&
            state.tmem.readByte(7U, value) && value == 0x47U,
            "first LOAD_BLOCK word remains unswapped");
    require(state.tmem.readByte(8U, value) && value == 0x4CU &&
            state.tmem.readByte(12U, value) && value == 0x48U,
            "second LOAD_BLOCK word swaps halves at the exact DXT carry");
    require(state.tmem.readByte(16U, value) && value == 0x50U &&
            state.tmem.readByte(20U, value) && value == 0x54U,
            "third LOAD_BLOCK word returns to normal order after the next carry");
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

void testLoadBlockContinuesAcrossTextureImageRows() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    for (std::uint32_t i = 0; i < 8U; ++i)
        require(bus.write8(0x1B00U + i, static_cast<std::uint8_t>(0x20U + i)),
                "seed two rows for linear LOAD_BLOCK transfer");

    const std::uint64_t setImage =
        (0x3DULL << 56U) | (1ULL << 51U) | (3ULL << 32U) | 0x1B00U;
    const std::uint64_t setTile = (0x35ULL << 56U);
    // Begin at X=2 of a four-texel row and load four contiguous texels.
    const std::uint64_t loadBlock =
        (0x33ULL << 56U) | (8ULL << 44U) | (20ULL << 12U);
    require(bus.write64(0xB60U, setImage) &&
            bus.write64(0xB68U, setTile) &&
            bus.write64(0xB70U, loadBlock),
            "seed LOAD_BLOCK row-crossing sequence");
    require(bus.write32(Bus::kDpStart, 0xB60U) &&
            bus.write32(Bus::kDpEnd, 0xB78U),
            "publish LOAD_BLOCK row-crossing sequence");
    for (unsigned i = 0; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "row-crossing LOAD_BLOCK sequence executes");

    std::uint8_t value = 0U;
    for (std::uint16_t i = 0; i < 4U; ++i) {
        require(state.tmem.readByte(i, value) &&
                value == static_cast<std::uint8_t>(0x22U + i),
                "LOAD_BLOCK reads contiguous texels across the image row boundary");
    }
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

void testLoadBlockRgba32RejectsSourceOverflowAtomically() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    constexpr std::uint32_t source =
        static_cast<std::uint32_t>(Rdram::kSize - 4U);
    for (std::uint32_t i = 0U; i < 4U; ++i)
        require(bus.write8(source + i, static_cast<std::uint8_t>(0xA0U + i)),
                "seed valid RGBA32 source prefix at RDRAM end");
    for (std::uint16_t i = 0U; i < 8U; ++i) {
        require(state.tmem.writeByte(i, 0x5AU),
                "seed lower RGBA32 TMEM bank before invalid load");
        require(state.tmem.writeByte(static_cast<std::uint16_t>(0x800U + i), 0xC3U),
                "seed upper RGBA32 TMEM bank before invalid load");
    }

    const std::uint64_t image =
        (0x3DULL << 56U) | (3ULL << 51U) | source;
    const std::uint64_t tile =
        (0x35ULL << 56U) | (3ULL << 51U);
    const std::uint64_t load =
        (0x33ULL << 56U) | (4ULL << 12U);
    require(bus.write64(0xBC0U, image) &&
            bus.write64(0xBC8U, tile) &&
            bus.write64(0xBD0U, load),
            "seed RGBA32 LOAD_BLOCK crossing RDRAM boundary");
    require(bus.write32(Bus::kDpStart, 0xBC0U) &&
            bus.write32(Bus::kDpEnd, 0xBD8U),
            "publish RGBA32 RDRAM-boundary LOAD_BLOCK sequence");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "RGBA32 SET_TEXTURE_IMAGE configures boundary source");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "RGBA32 SET_TILE configures boundary test");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Unsupported,
            "RGBA32 LOAD_BLOCK rejects source extending beyond RDRAM");

    std::uint8_t value = 0U;
    for (std::uint16_t i = 0U; i < 8U; ++i) {
        require(state.tmem.readByte(i, value) && value == 0x5AU,
                "rejected RGBA32 LOAD_BLOCK preserves lower TMEM bank");
        require(state.tmem.readByte(static_cast<std::uint16_t>(0x800U + i), value) &&
                value == 0xC3U,
                "rejected RGBA32 LOAD_BLOCK preserves upper TMEM bank");
    }
}

void testLoadBlockRgba32DxtMultipleCarriesApplyOddLineSwizzle() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    for (std::uint32_t i = 0U; i < 16U; ++i)
        require(bus.write8(0x1E80U + i, static_cast<std::uint8_t>(0x40U + i)),
                "seed RGBA32 maximum-DXT transfer words");

    const std::uint64_t image =
        (0x3DULL << 56U) | (3ULL << 51U) | (3ULL << 32U) | 0x1E80U;
    const std::uint64_t tile =
        (0x35ULL << 56U) | (3ULL << 51U);
    const std::uint64_t load =
        (0x33ULL << 56U) | (12ULL << 12U) | 0xFFFULL;
    require(bus.write64(0xC80U, image) && bus.write64(0xC88U, tile) &&
            bus.write64(0xC90U, load),
            "seed RGBA32 multiple-carry DXT sequence");
    require(bus.write32(Bus::kDpStart, 0xC80U) &&
            bus.write32(Bus::kDpEnd, 0xC98U),
            "publish RGBA32 multiple-carry DXT sequence");
    for (unsigned i = 0U; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "RGBA32 multiple-carry DXT sequence executes");

    const std::uint8_t order[] = {0U, 1U, 3U, 2U};
    for (std::uint16_t i = 0U; i < 4U; ++i) {
        const std::uint16_t lower = static_cast<std::uint16_t>(i * 2U);
        const std::uint16_t upper = static_cast<std::uint16_t>(0x800U + lower);
        const std::uint8_t red = static_cast<std::uint8_t>(0x40U + order[i] * 4U);
        std::uint8_t value = 0U;
        require(state.tmem.readByte(lower, value) && value == red &&
                state.tmem.readByte(static_cast<std::uint16_t>(lower + 1U), value) &&
                value == static_cast<std::uint8_t>(red + 1U),
                "multiple DXT carries preserve odd-line lower-bank texel swizzle");
        require(state.tmem.readByte(upper, value) &&
                value == static_cast<std::uint8_t>(red + 2U) &&
                state.tmem.readByte(static_cast<std::uint16_t>(upper + 1U), value) &&
                value == static_cast<std::uint8_t>(red + 3U),
                "multiple DXT carries preserve odd-line upper-bank texel swizzle");
    }
}

void testLoadBlockRgba32DxtRejectsSourceOverflowAtomically() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    const std::uint32_t source = static_cast<std::uint32_t>(Rdram::kSize - 4U);
    for (std::uint32_t i = 0U; i < 4U; ++i)
        require(bus.write8(source + i, static_cast<std::uint8_t>(0xD0U + i)),
                "seed valid RGBA32 DXT source prefix at RDRAM end");
    for (std::uint16_t i = 0U; i < 16U; ++i) {
        require(state.tmem.writeByte(i, 0x69U),
                "seed lower TMEM bank before overflowing RGBA32 DXT load");
        require(state.tmem.writeByte(static_cast<std::uint16_t>(0x800U + i), 0x96U),
                "seed upper TMEM bank before overflowing RGBA32 DXT load");
    }

    const std::uint64_t image =
        (0x3DULL << 56U) | (3ULL << 51U) | (7ULL << 32U) | source;
    const std::uint64_t tile =
        (0x35ULL << 56U) | (3ULL << 51U);
    const std::uint64_t load =
        (0x33ULL << 56U) | (4ULL << 12U) | 0x800ULL;
    require(bus.write64(0xC60U, image) && bus.write64(0xC68U, tile) &&
            bus.write64(0xC70U, load),
            "seed overflowing RGBA32 DXT LOAD_BLOCK");
    require(bus.write32(Bus::kDpStart, 0xC60U) &&
            bus.write32(Bus::kDpEnd, 0xC78U),
            "publish overflowing RGBA32 DXT LOAD_BLOCK");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "SET_TEXTURE_IMAGE configures RGBA32 DXT source");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "SET_TILE configures RGBA32 DXT destination");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Unsupported,
            "RGBA32 DXT LOAD_BLOCK rejects source range beyond RDRAM");

    std::uint8_t value = 0U;
    for (std::uint16_t i = 0U; i < 16U; ++i) {
        require(state.tmem.readByte(i, value) && value == 0x69U,
                "rejected RGBA32 DXT LOAD_BLOCK preserves lower TMEM bank");
        require(state.tmem.readByte(static_cast<std::uint16_t>(0x800U + i), value) &&
                    value == 0x96U,
                "rejected RGBA32 DXT LOAD_BLOCK preserves upper TMEM bank");
    }
}

void testLoadBlockRgba32DxtWrapsBothTmemBanks() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    for (std::uint32_t i = 0U; i < 32U; ++i)
        require(bus.write8(0x1F80U + i, static_cast<std::uint8_t>(0x20U + i)),
                "seed RGBA32 DXT data for dual-bank TMEM wrap");

    const std::uint64_t image =
        (0x3DULL << 56U) | (3ULL << 51U) | (7ULL << 32U) | 0x1F80U;
    const std::uint64_t tile =
        (0x35ULL << 56U) | (3ULL << 51U) | (511ULL << 32U);
    const std::uint64_t load =
        (0x33ULL << 56U) | (28ULL << 12U) | 0x800ULL;
    require(bus.write64(0xC40U, image) && bus.write64(0xC48U, tile) &&
            bus.write64(0xC50U, load),
            "seed RGBA32 DXT dual-bank wrap sequence");
    require(bus.write32(Bus::kDpStart, 0xC40U) &&
            bus.write32(Bus::kDpEnd, 0xC58U),
            "publish RGBA32 DXT dual-bank wrap sequence");
    for (unsigned i = 0U; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "RGBA32 DXT dual-bank wrap sequence executes");

    const std::uint8_t order[] = {0U, 1U, 3U, 2U, 4U, 5U, 7U, 6U};
    for (std::uint16_t i = 0U; i < 8U; ++i) {
        const std::uint16_t lower = static_cast<std::uint16_t>(
            (0x7F8U + i * 2U) & 0x07FFU);
        const std::uint16_t upper = static_cast<std::uint16_t>(lower + 0x0800U);
        const std::uint8_t texel = order[i];
        const std::uint8_t red = static_cast<std::uint8_t>(0x20U + texel * 4U);
        std::uint8_t value = 0U;
        require(state.tmem.readByte(lower, value) && value == red,
                "RGBA32 DXT lower-bank wrap preserves red texel order");
        require(state.tmem.readByte(static_cast<std::uint16_t>(lower + 1U), value) &&
                    value == static_cast<std::uint8_t>(red + 1U),
                "RGBA32 DXT lower-bank wrap preserves green texel order");
        require(state.tmem.readByte(upper, value) &&
                    value == static_cast<std::uint8_t>(red + 2U),
                "RGBA32 DXT upper-bank wrap preserves blue texel order");
        require(state.tmem.readByte(static_cast<std::uint16_t>(upper + 1U), value) &&
                    value == static_cast<std::uint8_t>(red + 3U),
                "RGBA32 DXT upper-bank wrap preserves alpha texel order");
    }
}

void testLoadBlockRgba32WrapsBothTmemBanks() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    for (std::uint32_t i = 0U; i < 24U; ++i)
        require(bus.write8(0x1F00U + i, static_cast<std::uint8_t>(0x10U + i)),
                "seed RGBA32 LOAD_BLOCK data for dual-bank TMEM wrap");
    for (std::uint16_t i = 0U; i < 8U; ++i) {
        require(state.tmem.writeByte(i, 0xE0U) &&
                state.tmem.writeByte(static_cast<std::uint16_t>(0x800U + i), 0xE1U),
                "seed RGBA32 LOAD_BLOCK sentinels after bank wrap");
    }

    const std::uint64_t image =
        (0x3DULL << 56U) | (3ULL << 51U) | (5ULL << 32U) | 0x1F00U;
    const std::uint64_t tile =
        (0x35ULL << 56U) | (3ULL << 51U) | (511ULL << 32U);
    const std::uint64_t load =
        (0x33ULL << 56U) | (20ULL << 12U);
    require(bus.write64(0xC00U, image) && bus.write64(0xC08U, tile) &&
            bus.write64(0xC10U, load),
            "seed RGBA32 LOAD_BLOCK dual-bank wrap sequence");
    require(bus.write32(Bus::kDpStart, 0xC00U) &&
            bus.write32(Bus::kDpEnd, 0xC18U),
            "publish RGBA32 LOAD_BLOCK dual-bank wrap sequence");
    for (unsigned i = 0U; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "RGBA32 LOAD_BLOCK dual-bank wrap sequence executes");

    const auto expect = [&state](std::uint16_t address, std::uint8_t expected,
                                 const char* message) {
        std::uint8_t value = 0U;
        require(state.tmem.readByte(address, value) && value == expected, message);
    };
    for (std::uint16_t texel = 0U; texel < 6U; ++texel) {
        const std::uint16_t lower = static_cast<std::uint16_t>(
            (0x7F8U + texel * 2U) & 0x07FFU);
        const std::uint16_t upper = static_cast<std::uint16_t>(lower + 0x0800U);
        const std::uint8_t source = static_cast<std::uint8_t>(0x10U + texel * 4U);
        expect(lower, source, "RGBA32 LOAD_BLOCK red wraps within lower TMEM bank");
        expect(static_cast<std::uint16_t>(lower + 1U),
               static_cast<std::uint8_t>(source + 1U),
               "RGBA32 LOAD_BLOCK green wraps within lower TMEM bank");
        expect(upper, static_cast<std::uint8_t>(source + 2U),
               "RGBA32 LOAD_BLOCK blue wraps within upper TMEM bank");
        expect(static_cast<std::uint16_t>(upper + 1U),
               static_cast<std::uint8_t>(source + 3U),
               "RGBA32 LOAD_BLOCK alpha wraps within upper TMEM bank");
    }
    for (std::uint16_t i = 4U; i < 8U; ++i) {
        expect(i, 0xE0U, "RGBA32 LOAD_BLOCK lower-bank wrap preserves following bytes");
        expect(static_cast<std::uint16_t>(0x800U + i), 0xE1U,
               "RGBA32 LOAD_BLOCK upper-bank wrap preserves following bytes");
    }
}

void testLoadBlockFourBitRejectsSourceOverflowAtomically() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    constexpr std::uint32_t source =
        static_cast<std::uint32_t>(Rdram::kSize - 1U);
    require(bus.write8(source, 0xABU),
            "seed valid four-bit source prefix at RDRAM end");
    for (std::uint16_t i = 0U; i < 4U; ++i)
        require(state.tmem.writeByte(i, 0x5AU),
                "seed TMEM before overflowing four-bit LOAD_BLOCK");

    const std::uint64_t image =
        (0x3DULL << 56U) | source;
    const std::uint64_t tile = (0x35ULL << 56U);
    const std::uint64_t load = (0x33ULL << 56U) | (12ULL << 12U);
    require(bus.write64(0xB40U, image) &&
            bus.write64(0xB48U, tile) &&
            bus.write64(0xB50U, load),
            "seed overflowing four-bit LOAD_BLOCK");
    require(bus.write32(Bus::kDpStart, 0xB40U) &&
            bus.write32(Bus::kDpEnd, 0xB58U),
            "publish overflowing four-bit LOAD_BLOCK");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "SET_TEXTURE_IMAGE configures four-bit source");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "SET_TILE configures four-bit destination");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Unsupported,
            "four-bit LOAD_BLOCK rejects source range beyond RDRAM");

    std::uint8_t value = 0U;
    for (std::uint16_t i = 0U; i < 4U; ++i)
        require(state.tmem.readByte(i, value) && value == 0x5AU,
                "rejected four-bit LOAD_BLOCK leaves TMEM unchanged");
}

void testLoadBlockCopiesPackedFourBitTexels() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    constexpr std::uint32_t source = 0x1A80U;
    require(bus.write8(source, 0x12U) && bus.write8(source + 1U, 0x34U),
            "seed packed four-bit LOAD_BLOCK source");

    const std::uint64_t setImage =
        (0x3DULL << 56U) | (1ULL << 32U) | source;
    const std::uint64_t setTile = (0x35ULL << 56U);
    const std::uint64_t loadBlock =
        (0x33ULL << 56U) | (12ULL << 12U);
    require(bus.write64(0xB20U, setImage) &&
            bus.write64(0xB28U, setTile) &&
            bus.write64(0xB30U, loadBlock),
            "seed packed four-bit LOAD_BLOCK command sequence");
    require(bus.write32(Bus::kDpStart, 0xB20U) &&
            bus.write32(Bus::kDpEnd, 0xB38U),
            "publish packed four-bit LOAD_BLOCK sequence");
    for (unsigned i = 0U; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) ==
                    RdpProcessResult::Processed,
                "packed four-bit LOAD_BLOCK sequence executes");

    std::uint8_t value = 0U;
    require(state.tmem.readByte(0U, value) && value == 0x12U,
            "four-bit LOAD_BLOCK preserves first packed byte");
    require(state.tmem.readByte(1U, value) && value == 0x34U,
            "four-bit LOAD_BLOCK preserves second packed byte");
}

void testLoadBlockFourBitStartsAtOddSourceNibble() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    constexpr std::uint32_t source = 0x1AE0U;
    require(bus.write8(source, 0x12U) &&
            bus.write8(source + 1U, 0x34U) &&
            bus.write8(source + 2U, 0x56U) &&
            bus.write8(source + 3U, 0x78U),
            "seed odd-nibble four-bit LOAD_BLOCK source");

    // Width three makes T=1, S=0 begin at texel three (the low nibble).
    const std::uint64_t setImage =
        (0x3DULL << 56U) | (2ULL << 32U) | source;
    const std::uint64_t setTile = (0x35ULL << 56U);
    const std::uint64_t loadBlock =
        (0x33ULL << 56U) | (4ULL << 32U) | (12ULL << 12U);
    require(bus.write64(0xB80U, setImage) &&
            bus.write64(0xB88U, setTile) &&
            bus.write64(0xB90U, loadBlock),
            "seed odd-nibble four-bit LOAD_BLOCK sequence");
    require(bus.write32(Bus::kDpStart, 0xB80U) &&
            bus.write32(Bus::kDpEnd, 0xB98U),
            "publish odd-nibble four-bit LOAD_BLOCK sequence");
    for (unsigned i = 0U; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "odd-nibble four-bit LOAD_BLOCK sequence executes");

    std::uint8_t value = 0U;
    require(state.tmem.readByte(0U, value) && value == 0x45U,
            "four-bit LOAD_BLOCK begins with the source low nibble");
    require(state.tmem.readByte(1U, value) && value == 0x67U,
            "four-bit LOAD_BLOCK continues from the following source nibbles");
}

void testLoadBlockFourBitOddNibbleDxtSwizzlesOddLine() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    constexpr std::uint32_t source = 0x1B20U;
    const std::uint8_t sourceBytes[] = {
        0x12U, 0x34U, 0x56U, 0x78U, 0x9AU, 0xBCU, 0xDEU, 0xF0U,
        0x12U, 0x34U, 0x56U, 0x78U, 0x9AU, 0xBCU, 0xDEU, 0xF0U,
        0x12U, 0x34U
    };
    for (std::uint32_t i = 0U; i < sizeof(sourceBytes); ++i)
        require(bus.write8(source + i, sourceBytes[i]),
                "seed odd-nibble DXT four-bit source");

    // Width three and T=1 start at the low nibble of source byte one.
    const std::uint64_t setImage =
        (0x3DULL << 56U) | (2ULL << 32U) | source;
    const std::uint64_t setTile = (0x35ULL << 56U);
    const std::uint64_t loadBlock =
        (0x33ULL << 56U) | (4ULL << 32U) | (124ULL << 12U) | 0x800ULL;
    require(bus.write64(0xBB0U, setImage) &&
            bus.write64(0xBB8U, setTile) &&
            bus.write64(0xBC0U, loadBlock),
            "seed odd-nibble DXT LOAD_BLOCK sequence");
    require(bus.write32(Bus::kDpStart, 0xBB0U) &&
            bus.write32(Bus::kDpEnd, 0xBC8U),
            "publish odd-nibble DXT LOAD_BLOCK sequence");
    for (unsigned i = 0U; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "odd-nibble DXT LOAD_BLOCK sequence executes");

    const std::uint8_t expected[] = {
        0x45U, 0x67U, 0x89U, 0xABU, 0xCDU, 0xEFU, 0x01U, 0x23U,
        0xCDU, 0xEFU, 0x01U, 0x23U, 0x45U, 0x67U, 0x89U, 0xABU
    };
    for (std::uint16_t i = 0U; i < sizeof(expected); ++i) {
        std::uint8_t value = 0U;
        require(state.tmem.readByte(i, value) && value == expected[i],
                "odd-nibble source alignment combines with DXT odd-line swizzle");
    }
}

void testLoadBlockFourBitDxtSwizzlesOddWords() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    constexpr std::uint32_t source = 0x1AC0U;
    for (std::uint32_t i = 0U; i < 16U; ++i)
        require(bus.write8(source + i, static_cast<std::uint8_t>(i + 1U)),
                "seed packed four-bit DXT source bytes");

    const std::uint64_t setImage =
        (0x3DULL << 56U) | (1ULL << 32U) | source;
    const std::uint64_t setTile = (0x35ULL << 56U);
    const std::uint64_t loadBlock =
        (0x33ULL << 56U) | (124ULL << 12U) | 0x800ULL;
    require(bus.write64(0xB60U, setImage) &&
            bus.write64(0xB68U, setTile) &&
            bus.write64(0xB70U, loadBlock),
            "seed packed four-bit DXT command sequence");
    require(bus.write32(Bus::kDpStart, 0xB60U) &&
            bus.write32(Bus::kDpEnd, 0xB78U),
            "publish packed four-bit DXT sequence");
    for (unsigned i = 0U; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) ==
                    RdpProcessResult::Processed,
                "packed four-bit DXT LOAD_BLOCK sequence executes");

    const std::uint8_t expected[] = {
        1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U,
        13U, 14U, 15U, 16U, 9U, 10U, 11U, 12U
    };
    std::uint8_t value = 0U;
    for (std::uint16_t i = 0U; i < 16U; ++i)
        require(state.tmem.readByte(i, value) && value == expected[i],
                "four-bit DXT odd word swaps its 32-bit halves");
}

void testLoadBlockCopiesRgba32IntoSplitTmemBanks() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    constexpr std::uint32_t source = 0x1B00U;
    const std::uint8_t rgba[] = {
        0x11U, 0x22U, 0x33U, 0x44U,
        0x55U, 0x66U, 0x77U, 0x88U
    };
    for (std::size_t i = 0U; i < sizeof(rgba); ++i)
        require(bus.write8(source + static_cast<std::uint32_t>(i), rgba[i]),
                "seed RGBA32 LOAD_BLOCK source texels");

    const std::uint64_t setImage =
        (0x3DULL << 56U) | (3ULL << 51U) | (1ULL << 32U) | source;
    const std::uint64_t setTile =
        (0x35ULL << 56U) | (3ULL << 51U);
    const std::uint64_t loadBlock =
        (0x33ULL << 56U) | (4ULL << 12U);
    require(bus.write64(0xB40U, setImage) &&
            bus.write64(0xB48U, setTile) &&
            bus.write64(0xB50U, loadBlock),
            "seed RGBA32 LOAD_BLOCK command sequence");
    require(bus.write32(Bus::kDpStart, 0xB40U) &&
            bus.write32(Bus::kDpEnd, 0xB58U),
            "publish RGBA32 LOAD_BLOCK sequence");
    for (unsigned i = 0U; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) ==
                    RdpProcessResult::Processed,
                "RGBA32 LOAD_BLOCK sequence executes");

    std::uint8_t value = 0U;
    const std::uint8_t expected[] = {
        0x11U, 0x22U, 0x55U, 0x66U
    };
    for (std::size_t i = 0U; i < 4U; ++i) {
        const std::uint16_t address = static_cast<std::uint16_t>(
            (i < 2U ? 0U : 2U) + (i % 2U));
        require(state.tmem.readByte(address, value) &&
                    value == expected[i],
                "RGBA32 LOAD_BLOCK stores RG channels in lower TMEM bank");
    }
    const std::uint8_t expectedUpper[] = {
        0x33U, 0x44U, 0x77U, 0x88U
    };
    for (std::size_t i = 0U; i < 4U; ++i) {
        const std::uint16_t address = static_cast<std::uint16_t>(
            0x800U + (i < 2U ? 0U : 2U) + (i % 2U));
        require(state.tmem.readByte(address, value) &&
                    value == expectedUpper[i],
                "RGBA32 LOAD_BLOCK stores BA channels in upper TMEM bank");
    }
}

void testLoadBlockRgba32DxtFractionalLineCarry() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    constexpr std::uint32_t source = 0x1D00U;
    std::uint8_t rgba[32]{};
    for (std::uint32_t texel = 0U; texel < 8U; ++texel) {
        rgba[texel * 4U] = static_cast<std::uint8_t>(0x10U + texel);
        rgba[texel * 4U + 1U] = static_cast<std::uint8_t>(0x20U + texel);
        rgba[texel * 4U + 2U] = static_cast<std::uint8_t>(0x30U + texel);
        rgba[texel * 4U + 3U] = static_cast<std::uint8_t>(0x40U + texel);
    }
    for (std::size_t i = 0U; i < sizeof(rgba); ++i)
        require(bus.write8(source + static_cast<std::uint32_t>(i), rgba[i]),
                "seed RGBA32 fractional-DXT texels");

    const std::uint64_t setImage =
        (0x3DULL << 56U) | (3ULL << 51U) | (7ULL << 32U) | source;
    const std::uint64_t setTile =
        (0x35ULL << 56U) | (3ULL << 51U);
    const std::uint64_t loadBlock =
        (0x33ULL << 56U) | (28ULL << 12U) | 0x400ULL;
    require(bus.write64(0xBE0U, setImage) &&
            bus.write64(0xBE8U, setTile) &&
            bus.write64(0xBF0U, loadBlock),
            "seed RGBA32 fractional-DXT command sequence");
    require(bus.write32(Bus::kDpStart, 0xBE0U) &&
            bus.write32(Bus::kDpEnd, 0xBF8U),
            "publish RGBA32 fractional-DXT sequence");
    for (unsigned i = 0U; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) ==
                    RdpProcessResult::Processed,
                "RGBA32 fractional-DXT LOAD_BLOCK sequence executes");

    // DXT=0x400 carries after every two 64-bit words. The first four
    // texels remain ordered; the next two words are both on odd line 1.
    const std::uint8_t expectedTexelOrder[] = {0U, 1U, 2U, 3U, 5U, 4U, 7U, 6U};
    std::uint8_t value = 0U;
    for (std::size_t i = 0U; i < 8U; ++i) {
        const std::uint8_t texel = expectedTexelOrder[i];
        require(state.tmem.readByte(static_cast<std::uint16_t>(i * 2U), value) &&
                    value == static_cast<std::uint8_t>(0x10U + texel),
                "fractional DXT RGBA32 lower bank preserves expected texel order");
        require(state.tmem.readByte(static_cast<std::uint16_t>(i * 2U + 1U), value) &&
                    value == static_cast<std::uint8_t>(0x20U + texel),
                "fractional DXT RGBA32 lower bank preserves green channel");
        require(state.tmem.readByte(static_cast<std::uint16_t>(0x800U + i * 2U), value) &&
                    value == static_cast<std::uint8_t>(0x30U + texel),
                "fractional DXT RGBA32 upper bank preserves blue channel");
        require(state.tmem.readByte(static_cast<std::uint16_t>(0x801U + i * 2U), value) &&
                    value == static_cast<std::uint8_t>(0x40U + texel),
                "fractional DXT RGBA32 upper bank preserves alpha channel");
    }
}

void testLoadBlockRgba32DxtSwapsOddLineWords() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    constexpr std::uint32_t source = 0x1C00U;
    const std::uint8_t rgba[] = {
        0x10U, 0x11U, 0x12U, 0x13U,
        0x20U, 0x21U, 0x22U, 0x23U,
        0x30U, 0x31U, 0x32U, 0x33U,
        0x40U, 0x41U, 0x42U, 0x43U
    };
    for (std::size_t i = 0U; i < sizeof(rgba); ++i)
        require(bus.write8(source + static_cast<std::uint32_t>(i), rgba[i]),
                "seed RGBA32 DXT LOAD_BLOCK texels");

    const std::uint64_t setImage =
        (0x3DULL << 56U) | (3ULL << 51U) | (3ULL << 32U) | source;
    const std::uint64_t setTile =
        (0x35ULL << 56U) | (3ULL << 51U);
    const std::uint64_t loadBlock =
        (0x33ULL << 56U) | (12ULL << 12U) | 0x800ULL;
    require(bus.write64(0xB80U, setImage) &&
            bus.write64(0xB88U, setTile) &&
            bus.write64(0xB90U, loadBlock),
            "seed RGBA32 DXT command sequence");
    require(bus.write32(Bus::kDpStart, 0xB80U) &&
            bus.write32(Bus::kDpEnd, 0xB98U),
            "publish RGBA32 DXT sequence");
    for (unsigned i = 0U; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) ==
                    RdpProcessResult::Processed,
                "RGBA32 DXT LOAD_BLOCK sequence executes");

    // DXT=0x800 advances one line per 64-bit word. The second word is
    // therefore half-swapped before RG/BA are split into their TMEM banks.
    const std::uint8_t expectedLower[] = {
        0x10U, 0x11U, 0x20U, 0x21U,
        0x40U, 0x41U, 0x30U, 0x31U
    };
    const std::uint8_t expectedUpper[] = {
        0x12U, 0x13U, 0x22U, 0x23U,
        0x42U, 0x43U, 0x32U, 0x33U
    };
    std::uint8_t value = 0U;
    for (std::size_t i = 0U; i < 8U; ++i) {
        require(state.tmem.readByte(static_cast<std::uint16_t>(i), value) &&
                    value == expectedLower[i],
                "RGBA32 DXT odd word is swapped in lower TMEM bank");
        require(state.tmem.readByte(static_cast<std::uint16_t>(0x800U + i), value) &&
                    value == expectedUpper[i],
                "RGBA32 DXT odd word is swapped in upper TMEM bank");
    }
}

void testLoadBlockRejectsMismatchedRgba32TileSize() {
    using namespace m64plus::n64;
    for (const std::uint32_t size : {3U}) {
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
                "RGBA32 LOAD_BLOCK rejects a tile with a mismatched texel size");
    }
}

void testLoadBlockRgba32DxtRejectsMismatchedTileSizeAtomically() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    for (std::uint32_t i = 0U; i < 16U; ++i)
        require(bus.write8(0x1A60U + i, static_cast<std::uint8_t>(0x20U + i)),
                "seed source bytes for mismatched RGBA32 tile size test");
    for (std::uint16_t i = 0U; i < 16U; ++i) {
        require(state.tmem.writeByte(i, 0x96U),
                "seed lower TMEM bank before RGBA32 tile size rejection");
        require(state.tmem.writeByte(static_cast<std::uint16_t>(0x800U + i), 0x69U),
                "seed upper TMEM bank before RGBA32 tile size rejection");
    }

    const std::uint64_t image =
        (0x3DULL << 56U) | (3ULL << 51U) | (7ULL << 32U) | 0x1A60U;
    const std::uint64_t tile =
        (0x35ULL << 56U) | (2ULL << 51U);
    const std::uint64_t load =
        (0x33ULL << 56U) | (4ULL << 12U) | 0x800ULL;
    require(bus.write64(0xCE0U, image) && bus.write64(0xCE8U, tile) &&
            bus.write64(0xCF0U, load),
            "seed RGBA32 DXT LOAD_BLOCK with mismatched tile size");
    require(bus.write32(Bus::kDpStart, 0xCE0U) &&
            bus.write32(Bus::kDpEnd, 0xCF8U),
            "publish mismatched tile size RGBA32 DXT LOAD_BLOCK");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "SET_TEXTURE_IMAGE configures RGBA32 source");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "SET_TILE records mismatched 16-bit tile descriptor");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Unsupported,
            "RGBA32 DXT LOAD_BLOCK rejects mismatched tile size");

    std::uint8_t value = 0U;
    for (std::uint16_t i = 0U; i < 16U; ++i) {
        require(state.tmem.readByte(i, value) && value == 0x96U,
                "rejected mismatched tile size preserves lower TMEM bank");
        require(state.tmem.readByte(static_cast<std::uint16_t>(0x800U + i), value) &&
                    value == 0x69U,
                "rejected mismatched tile size preserves upper TMEM bank");
    }
}

void testLoadBlockRgba32DxtRejectsNonRgbaImageFormatAtomically() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    for (std::uint32_t i = 0U; i < 16U; ++i)
        require(bus.write8(0x1A80U + i, static_cast<std::uint8_t>(0x40U + i)),
                "seed source bytes for non-RGBA image format test");
    for (std::uint16_t i = 0U; i < 16U; ++i) {
        require(state.tmem.writeByte(i, 0xA5U),
                "seed lower TMEM bank before non-RGBA image rejection");
        require(state.tmem.writeByte(static_cast<std::uint16_t>(0x800U + i), 0x5AU),
                "seed upper TMEM bank before non-RGBA image rejection");
    }

    const std::uint64_t image =
        (0x3DULL << 56U) | (1ULL << 53U) | (3ULL << 51U) |
        (7ULL << 32U) | 0x1A80U;
    const std::uint64_t tile =
        (0x35ULL << 56U) | (3ULL << 51U);
    const std::uint64_t load =
        (0x33ULL << 56U) | (4ULL << 12U) | 0x800ULL;
    require(bus.write64(0xCC0U, image) && bus.write64(0xCC8U, tile) &&
            bus.write64(0xCD0U, load),
            "seed RGBA32 DXT LOAD_BLOCK with non-RGBA image format");
    require(bus.write32(Bus::kDpStart, 0xCC0U) &&
            bus.write32(Bus::kDpEnd, 0xCD8U),
            "publish non-RGBA image format LOAD_BLOCK");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "SET_TEXTURE_IMAGE records non-RGBA 32-bit source format");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "SET_TILE configures RGBA32 destination");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Unsupported,
            "RGBA32 DXT LOAD_BLOCK rejects non-RGBA image format");

    std::uint8_t value = 0U;
    for (std::uint16_t i = 0U; i < 16U; ++i) {
        require(state.tmem.readByte(i, value) && value == 0xA5U,
                "rejected non-RGBA image preserves lower TMEM bank");
        require(state.tmem.readByte(static_cast<std::uint16_t>(0x800U + i), value) &&
                    value == 0x5AU,
                "rejected non-RGBA image preserves upper TMEM bank");
    }
}

void testLoadBlockRgba32DxtRejectsNonRgbaTileFormatAtomically() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    for (std::uint32_t i = 0U; i < 16U; ++i)
        require(bus.write8(0x1A00U + i, static_cast<std::uint8_t>(0x30U + i)),
                "seed RGBA32 DXT source for mismatched tile format");
    for (std::uint16_t i = 0U; i < 16U; ++i) {
        require(state.tmem.writeByte(i, 0x5AU),
                "seed lower TMEM bank before mismatched RGBA32 DXT load");
        require(state.tmem.writeByte(static_cast<std::uint16_t>(0x800U + i), 0xC3U),
                "seed upper TMEM bank before mismatched RGBA32 DXT load");
    }

    const std::uint64_t image =
        (0x3DULL << 56U) | (3ULL << 51U) | (7ULL << 32U) | 0x1A00U;
    const std::uint64_t tile =
        (0x35ULL << 56U) | (1ULL << 53U) | (3ULL << 51U);
    const std::uint64_t load =
        (0x33ULL << 56U) | (4ULL << 12U) | 0x800ULL;
    require(bus.write64(0xCA0U, image) && bus.write64(0xCA8U, tile) &&
            bus.write64(0xCB0U, load),
            "seed RGBA32 DXT LOAD_BLOCK with non-RGBA tile format");
    require(bus.write32(Bus::kDpStart, 0xCA0U) &&
            bus.write32(Bus::kDpEnd, 0xCB8U),
            "publish mismatched-format RGBA32 DXT LOAD_BLOCK");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "SET_TEXTURE_IMAGE configures RGBA32 source");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "SET_TILE accepts non-RGBA format descriptor");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Unsupported,
            "RGBA32 DXT LOAD_BLOCK rejects non-RGBA tile format");

    std::uint8_t value = 0U;
    for (std::uint16_t i = 0U; i < 16U; ++i) {
        require(state.tmem.readByte(i, value) && value == 0x5AU,
                "rejected mismatched-format load preserves lower TMEM bank");
        require(state.tmem.readByte(static_cast<std::uint16_t>(0x800U + i), value) &&
                    value == 0xC3U,
                "rejected mismatched-format load preserves upper TMEM bank");
    }
}

void testLoadBlockFourBitDxtRejectsSourceOverflowAtomically() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    const std::uint32_t source = static_cast<std::uint32_t>(Rdram::kSize - 8U);
    for (std::uint32_t i = 0U; i < 8U; ++i)
        require(bus.write8(source + i, static_cast<std::uint8_t>(0xC0U + i)),
                "seed valid four-bit DXT source prefix at RDRAM end");
    for (std::uint16_t i = 0U; i < 32U; ++i)
        require(state.tmem.writeByte(i, 0x7CU),
                "seed TMEM before overflowing four-bit DXT LOAD_BLOCK");

    const std::uint64_t image =
        (0x3DULL << 56U) | (31ULL << 32U) | source;
    const std::uint64_t tile = (0x35ULL << 56U);
    const std::uint64_t load =
        (0x33ULL << 56U) | (92ULL << 12U) | 0x800ULL;
    require(bus.write64(0xB40U, image) && bus.write64(0xB48U, tile) &&
            bus.write64(0xB50U, load), "seed overflowing four-bit DXT LOAD_BLOCK");
    require(bus.write32(Bus::kDpStart, 0xB40U) &&
            bus.write32(Bus::kDpEnd, 0xB58U),
            "publish overflowing four-bit DXT LOAD_BLOCK");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "SET_TEXTURE_IMAGE configures four-bit DXT source");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "SET_TILE configures four-bit DXT destination");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Unsupported,
            "four-bit DXT LOAD_BLOCK rejects source range beyond RDRAM");

    std::uint8_t value = 0U;
    for (std::uint16_t i = 0U; i < 32U; ++i)
        require(state.tmem.readByte(i, value) && value == 0x7CU,
                "rejected four-bit DXT LOAD_BLOCK leaves TMEM unchanged");
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

void testLoadBlockDxtSwizzlesOddLinesForEightAndSixteenBitTexels() {
    using namespace m64plus::n64;
    for (const std::uint8_t size : {1U, 2U}) {
        Bus bus;
        RdpSyncState state{};
        for (std::uint32_t i = 0; i < 16U; ++i)
            require(bus.write8(0x1800U + i, static_cast<std::uint8_t>(i)),
                    "seed two 64-bit LOAD_BLOCK words");
        const std::uint64_t setImage =
            (0x3DULL << 56U) | (static_cast<std::uint64_t>(size) << 51U) |
            (31ULL << 32U) | 0x1800ULL;
        const std::uint64_t setTile =
            (0x35ULL << 56U) | (static_cast<std::uint64_t>(size) << 51U);
        const std::uint64_t texelCount = size == 1U ? 16ULL : 8ULL;
        const std::uint64_t loadBlock =
            (0x33ULL << 56U) | ((texelCount - 1U) * 4ULL << 12U) | 0x800ULL;
        require(bus.write64(0xA40U, setImage) &&
                bus.write64(0xA48U, setTile) &&
                bus.write64(0xA50U, loadBlock), "seed 8/16-bit DXT sequence");
        require(bus.write32(Bus::kDpStart, 0xA40U) &&
                bus.write32(Bus::kDpEnd, 0xA58U), "publish 8/16-bit DXT sequence");
        for (unsigned i = 0; i < 3U; ++i)
            require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                    "8/16-bit DXT LOAD_BLOCK sequence executes");
        std::uint8_t value = 0U;
        require(state.tmem.readByte(8U, value) && value == 12U,
                "odd DXT line begins with source byte twelve");
        require(state.tmem.readByte(11U, value) && value == 15U,
                "odd DXT line preserves second half ordering");
        require(state.tmem.readByte(12U, value) && value == 8U,
                "odd DXT line swaps first 32-bit half");
        require(state.tmem.readByte(15U, value) && value == 11U,
                "odd DXT line completes 32-bit half swap");
    }
}

void testLoadTlutReplicatesEntriesIntoHighTmem() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    require(bus.write16(0x1400U, 0xF801U) &&
            bus.write16(0x1402U, 0x07C1U) &&
            bus.write16(0x1404U, 0x003FU), "seed three 16-bit TLUT entries");

    const std::uint64_t setImage =
        (0x3DULL << 56U) | (2ULL << 51U) | (2ULL << 32U) | 0x1400ULL;
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

void testLoadTlutRejectsOutOfRangeSourceWithoutPartialTmemWrites() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    const std::uint32_t source = static_cast<std::uint32_t>(Rdram::kSize - 2U);
    require(bus.write16(source, 0xF801U),
            "seed final valid TLUT source entry");
    state.tmem.write64(0x800U, 0x1122334455667788ULL);
    state.tmem.write64(0x808U, 0x99AABBCCDDEEFF00ULL);

    const std::uint64_t setImage =
        (0x3DULL << 56U) | (2ULL << 51U) | (1ULL << 32U) | source;
    const std::uint64_t setTile =
        (0x35ULL << 56U) | (256ULL << 32U);
    const std::uint64_t loadTlut =
        (0x30ULL << 56U) | (4ULL << 12U);
    require(bus.write64(0x960U, setImage) &&
            bus.write64(0x968U, setTile) &&
            bus.write64(0x970U, loadTlut),
            "seed out-of-range TLUT load sequence");
    require(bus.write32(Bus::kDpStart, 0x960U) &&
            bus.write32(Bus::kDpEnd, 0x978U),
            "publish out-of-range TLUT sequence");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed &&
            RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "configure image and tile before invalid LOAD_TLUT");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Unsupported,
            "LOAD_TLUT rejects source entries crossing RDRAM end");

    std::uint64_t first = 0U;
    std::uint64_t second = 0U;
    require(state.tmem.read64(0x800U, first) &&
            state.tmem.read64(0x808U, second) &&
            first == 0x1122334455667788ULL &&
            second == 0x99AABBCCDDEEFF00ULL,
            "invalid LOAD_TLUT leaves all destination TMEM entries unchanged");
}

void testLoadTlutAcceptsCanonicalWidthOnePaletteLoad() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    constexpr std::uint32_t image = 0x1500U;
    for (std::uint32_t i = 0; i < 16U; ++i) {
        require(bus.write16(image + i * 2U,
                            static_cast<std::uint16_t>(0x8000U + i)),
                "seed canonical 16-entry TLUT source");
    }

    const std::uint64_t setImage =
        (0x3DULL << 56U) | (2ULL << 51U) | (1ULL << 32U) | image;
    const std::uint64_t setTile =
        (0x35ULL << 56U) | (256ULL << 32U);
    const std::uint64_t loadTlut =
        (0x30ULL << 56U) | (60ULL << 12U);
    const std::uint64_t setTileSize =
        (0x32ULL << 56U) | (4ULL << 44U) | (8ULL << 32U) |
        (20ULL << 12U) | 24ULL;
    require(bus.write64(0x920U, setImage) &&
            bus.write64(0x928U, setTile) &&
            bus.write64(0x930U, loadTlut) &&
            bus.write64(0x938U, setTileSize),
            "seed canonical width-one TLUT and tile-size sequence");
    require(bus.write32(Bus::kDpStart, 0x920U) &&
            bus.write32(Bus::kDpEnd, 0x940U),
            "publish canonical width-one TLUT sequence");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed &&
            RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "configure width-one source image and palette destination");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "LOAD_TLUT accepts 16 entries although texture image width is one");
    require(state.hasTileSize[0] && state.tileSizes[0].sl == 0U &&
            state.tileSizes[0].tl == 0U && state.tileSizes[0].sh == 60U &&
            state.tileSizes[0].th == 0U,
            "LOAD_TLUT latches its coordinates into the tile descriptor");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "SET_TILE_SIZE restores render coordinates after LOAD_TLUT");
    require(state.tileSizes[0].sl == 4U && state.tileSizes[0].tl == 8U &&
            state.tileSizes[0].sh == 20U && state.tileSizes[0].th == 24U,
            "SET_TILE_SIZE overwrites the coordinates latched by LOAD_TLUT");

    std::uint64_t word = 0U;
    for (std::uint32_t i = 0; i < 16U; ++i) {
        require(state.tmem.read64(static_cast<std::uint16_t>(0x800U + i * 8U), word) &&
                word == (static_cast<std::uint64_t>(0x8000U + i) * 0x0001000100010001ULL),
                "canonical width-one TLUT entry is replicated in TMEM");
    }
}

void testLoadTlutRejectsDestinationBeyondTmemWithoutPartialWrites() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    constexpr std::uint32_t image = 0x1600U;
    require(bus.write16(image, 0xF801U) &&
            bus.write16(image + 2U, 0x07C1U),
            "seed two TLUT source entries");
    state.tmem.write64(0xFF8U, 0x1122334455667788ULL);

    const std::uint64_t setImage =
        (0x3DULL << 56U) | (2ULL << 51U) | (1ULL << 32U) | image;
    const std::uint64_t setTile =
        (0x35ULL << 56U) | (511ULL << 32U);
    const std::uint64_t loadTlut =
        (0x30ULL << 56U) | (4ULL << 12U);
    require(bus.write64(0x900U, setImage) &&
            bus.write64(0x908U, setTile) &&
            bus.write64(0x910U, loadTlut),
            "seed TLUT destination overflow sequence");
    require(bus.write32(Bus::kDpStart, 0x900U) &&
            bus.write32(Bus::kDpEnd, 0x918U),
            "publish TLUT destination overflow sequence");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed &&
            RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "configure source and last TMEM palette slot");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Unsupported,
            "LOAD_TLUT rejects a range extending beyond TMEM");

    std::uint64_t last = 0U;
    require(state.tmem.read64(0xFF8U, last) &&
            last == 0x1122334455667788ULL,
            "rejected TLUT destination overflow leaves final TMEM slot unchanged");
}

void testLoadTlutRejectsDestinationBelowPaletteRegionWithoutWrites() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    constexpr std::uint32_t image = 0x1700U;
    require(bus.write16(image, 0xF801U), "seed TLUT source entry below palette region");
    state.tmem.write64(0x7F8U, 0x1122334455667788ULL);

    const std::uint64_t setImage =
        (0x3DULL << 56U) | (2ULL << 51U) | (1ULL << 32U) | image;
    const std::uint64_t setTile =
        (0x35ULL << 56U) | (255ULL << 32U);
    const std::uint64_t loadTlut = (0x30ULL << 56U);
    require(bus.write64(0x8C0U, setImage) &&
            bus.write64(0x8C8U, setTile) &&
            bus.write64(0x8D0U, loadTlut),
            "seed TLUT destination below palette region sequence");
    require(bus.write32(Bus::kDpStart, 0x8C0U) &&
            bus.write32(Bus::kDpEnd, 0x8D8U),
            "publish TLUT low destination sequence");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed &&
            RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "configure source and low TMEM destination");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Unsupported,
            "LOAD_TLUT rejects destination below TMEM palette region");

    std::uint64_t actual = 0U;
    require(state.tmem.read64(0x7F8U, actual) &&
            actual == 0x1122334455667788ULL,
            "rejected low TLUT destination leaves TMEM unchanged");
}

void testLoadTlutHonorsTextureImageCoordinatesAndRange() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    constexpr std::uint32_t image = 0x1400U;
    require(bus.write16(image + 10U, 0xF801U) &&
            bus.write16(image + 12U, 0x07C1U),
            "seed TLUT entries at nonzero image coordinates");

    const std::uint64_t setImage =
        (0x3DULL << 56U) | (2ULL << 51U) | (3ULL << 32U) | image;
    const std::uint64_t setTile =
        (0x35ULL << 56U) | (256ULL << 32U);
    const std::uint64_t loadTlut =
        (0x30ULL << 56U) | (4ULL << 44U) | (4ULL << 32U) |
        (8ULL << 12U) | 4ULL;
    require(bus.write64(0x940U, setImage) &&
            bus.write64(0x948U, setTile) &&
            bus.write64(0x950U, loadTlut), "seed offset TLUT load sequence");
    require(bus.write32(Bus::kDpStart, 0x940U) &&
            bus.write32(Bus::kDpEnd, 0x958U), "publish offset TLUT sequence");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "SET_TEXTURE_IMAGE configures four-wide palette source");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "SET_TILE configures TLUT destination");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "LOAD_TLUT loads the selected source range");

    std::uint64_t word = 0U;
    require(state.tmem.read64(0x800U, word) && word == 0xF801F801F801F801ULL,
            "LOAD_TLUT starts at SL/TL texture image coordinate");
    require(state.tmem.read64(0x808U, word) && word == 0x07C107C107C107C1ULL,
            "LOAD_TLUT count is based on SH minus SL");
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



void testFillRectangleRejectsUnsupportedCycleTypeWithoutWrites() {
    using namespace m64plus::n64;
    constexpr std::uint32_t base = 0x3D00U;
    constexpr std::uint32_t sentinel = 0xAABBCCDDU;
    Bus bus;
    RdpSyncState state{};
    for (std::uint32_t i = 0; i < 4U; ++i)
        require(bus.write32(base + i * 4U, sentinel),
                "seed unsupported-cycle framebuffer sentinels");

    const std::uint64_t commands[] = {
        (0x3FULL << 56U) | (3ULL << 51U) | (3ULL << 32U) | base,
        (0x37ULL << 56U) | 0x55667788U,
        (0x2FULL << 56U) | (2ULL << 52U),
        (0x36ULL << 56U) | (4ULL << 44U) | (4ULL << 32U) |
            (4ULL << 12U) | 4ULL
    };
    for (std::size_t i = 0; i < 4U; ++i)
        require(bus.write64(0xE40U + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "seed unsupported-cycle FILL_RECTANGLE command stream");
    require(bus.write32(Bus::kDpStart, 0xE40U) &&
            bus.write32(Bus::kDpEnd, 0xE60U),
            "publish unsupported-cycle fill stream");
    for (unsigned i = 0; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "unsupported-cycle fill setup commands are processed");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Unsupported,
            "FILL_RECTANGLE rejects non-FILL cycle types");

    for (std::uint32_t i = 0; i < 4U; ++i) {
        std::uint32_t pixel = 0U;
        require(bus.read32(base + i * 4U, pixel),
                "read unsupported-cycle framebuffer pixel");
        require(pixel == sentinel,
                "unsupported-cycle FILL_RECTANGLE leaves framebuffer unchanged");
    }
}

void testFillRectangleRejectsMissingOtherModesWithoutWrites() {
    using namespace m64plus::n64;
    constexpr std::uint32_t base = 0x3E00U;
    constexpr std::uint32_t sentinel = 0xAABBCCDDU;
    Bus bus;
    RdpSyncState state{};
    for (std::uint32_t i = 0; i < 4U; ++i)
        require(bus.write32(base + i * 4U, sentinel),
                "seed missing-other-modes framebuffer sentinels");

    const std::uint64_t commands[] = {
        (0x3FULL << 56U) | (3ULL << 51U) | (3ULL << 32U) | base,
        (0x37ULL << 56U) | 0x55667788U,
        (0x36ULL << 56U) | (4ULL << 44U) | (4ULL << 32U) |
            (4ULL << 12U) | 4ULL
    };
    for (std::size_t i = 0; i < 3U; ++i)
        require(bus.write64(0xEB0U + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "seed missing-other-modes FILL_RECTANGLE stream");
    require(bus.write32(Bus::kDpStart, 0xEB0U) &&
            bus.write32(Bus::kDpEnd, 0xEC8U),
            "publish missing-other-modes fill stream");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed &&
            RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "missing-other-modes setup commands are processed");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Unsupported,
            "FILL_RECTANGLE rejects missing other-modes state");

    for (std::uint32_t i = 0; i < 4U; ++i) {
        std::uint32_t pixel = 0U;
        require(bus.read32(base + i * 4U, pixel) && pixel == sentinel,
                "missing other-modes leaves framebuffer unchanged");
    }
}

void testFillRectangleRejectsMissingFillColorWithoutWrites() {
    using namespace m64plus::n64;
    constexpr std::uint32_t base = 0x3DC0U;
    constexpr std::uint32_t sentinel = 0xAABBCCDDU;
    Bus bus;
    RdpSyncState state{};
    for (std::uint32_t i = 0; i < 4U; ++i)
        require(bus.write32(base + i * 4U, sentinel),
                "seed missing-fill-color framebuffer sentinels");

    const std::uint64_t commands[] = {
        (0x3FULL << 56U) | (3ULL << 51U) | (3ULL << 32U) | base,
        (0x2FULL << 56U) | (3ULL << 52U),
        (0x36ULL << 56U) | (4ULL << 44U) | (4ULL << 32U) |
            (4ULL << 12U) | 4ULL
    };
    for (std::size_t i = 0; i < 3U; ++i)
        require(bus.write64(0xE98U + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "seed missing-fill-color FILL_RECTANGLE stream");
    require(bus.write32(Bus::kDpStart, 0xE98U) &&
            bus.write32(Bus::kDpEnd, 0xEB0U),
            "publish missing-fill-color fill stream");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed &&
            RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "missing-fill-color setup commands are processed");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Unsupported,
            "FILL_RECTANGLE rejects missing fill color state");

    for (std::uint32_t i = 0; i < 4U; ++i) {
        std::uint32_t pixel = 0U;
        require(bus.read32(base + i * 4U, pixel) && pixel == sentinel,
                "missing fill color leaves framebuffer unchanged");
    }
}

void testFillRectangleRejectsMissingColorImageWithoutWrites() {
    using namespace m64plus::n64;
    constexpr std::uint32_t base = 0x3D80U;
    constexpr std::uint32_t sentinel = 0xAABBCCDDU;
    Bus bus;
    RdpSyncState state{};
    require(bus.write32(base, sentinel) && bus.write32(base + 4U, sentinel),
            "seed missing-color-image framebuffer sentinels");

    const std::uint64_t commands[] = {
        (0x37ULL << 56U) | 0x55667788U,
        (0x2FULL << 56U) | (3ULL << 52U),
        (0x36ULL << 56U) | (4ULL << 44U) | (4ULL << 32U) |
            (4ULL << 12U) | 4ULL
    };
    for (std::size_t i = 0; i < 3U; ++i)
        require(bus.write64(0xE80U + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "seed missing-color-image FILL_RECTANGLE stream");
    require(bus.write32(Bus::kDpStart, 0xE80U) &&
            bus.write32(Bus::kDpEnd, 0xE98U),
            "publish missing-color-image fill stream");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed &&
            RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "missing-color-image setup commands are processed");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Unsupported,
            "FILL_RECTANGLE rejects missing color image state");

    std::uint32_t pixel = 0U;
    require(bus.read32(base, pixel) && pixel == sentinel,
            "missing color image leaves framebuffer unchanged");
    require(bus.read32(base + 4U, pixel) && pixel == sentinel,
            "missing color image preserves second sentinel");
}

void testFillRectangleRejectsUnsupportedColorImageFormatsWithoutWrites() {
    using namespace m64plus::n64;
    constexpr std::uint32_t base = 0x3D40U;
    constexpr std::uint32_t sentinel = 0xAABBCCDDU;
    const std::array<std::uint64_t, 6> unsupportedImages = {
        (0x3FULL << 56U) | (3ULL << 51U) | (1ULL << 53U) |
            (3ULL << 32U) | base,
        (0x3FULL << 56U) | (1ULL << 51U) |
            (3ULL << 32U) | base,
        (0x3FULL << 56U) | (0ULL << 51U) |
            (3ULL << 32U) | base,
        (0x3FULL << 56U) | (2ULL << 53U) | (2ULL << 51U) |
            (3ULL << 32U) | base,
        (0x3FULL << 56U) | (3ULL << 53U) | (2ULL << 51U) |
            (3ULL << 32U) | base,
        (0x3FULL << 56U) | (4ULL << 53U) | (3ULL << 51U) |
            (3ULL << 32U) | base
    };

    for (std::size_t mode = 0; mode < unsupportedImages.size(); ++mode) {
        Bus bus;
        RdpSyncState state{};
        for (std::uint32_t i = 0; i < 4U; ++i)
            require(bus.write32(base + i * 4U, sentinel),
                    "seed unsupported-image framebuffer sentinels");

        const std::uint64_t commands[] = {
            unsupportedImages[mode],
            (0x37ULL << 56U) | 0x55667788U,
            (0x2FULL << 56U) | (3ULL << 52U),
            (0x36ULL << 56U) | (4ULL << 44U) | (4ULL << 32U) |
                (4ULL << 12U) | 4ULL
        };
        const std::uint32_t stream = 0xE80U + static_cast<std::uint32_t>(mode * 0x40U);
        for (std::size_t i = 0; i < 4U; ++i)
            require(bus.write64(stream + static_cast<std::uint32_t>(i * 8U), commands[i]),
                    "seed unsupported-image FILL_RECTANGLE command stream");
        require(bus.write32(Bus::kDpStart, stream) &&
                bus.write32(Bus::kDpEnd, stream + 0x20U),
                "publish unsupported-image fill stream");
        for (unsigned i = 0; i < 3U; ++i)
            require(RdpCommandProcessor::processNext(bus, state) ==
                        RdpProcessResult::Processed,
                    "unsupported-image fill setup commands are processed");
        require(RdpCommandProcessor::processNext(bus, state) ==
                    RdpProcessResult::Unsupported,
                "FILL_RECTANGLE rejects unsupported color image format or size");

        for (std::uint32_t i = 0; i < 4U; ++i) {
            std::uint32_t pixel = 0U;
            require(bus.read32(base + i * 4U, pixel),
                    "read unsupported-image framebuffer pixel");
            require(pixel == sentinel,
                    "unsupported color image leaves framebuffer unchanged");
        }
    }
}

void testFillRectangleWritesSinglePixelForDegenerateBounds() {
    using namespace m64plus::n64;
    constexpr std::uint32_t base = 0x3C00U;
    constexpr std::uint32_t sentinel = 0xAABBCCDDU;
    constexpr std::uint32_t color = 0x55667788U;
    Bus bus;
    RdpSyncState state{};
    for (std::uint32_t i = 0; i < 16U; ++i)
        require(bus.write32(base + i * 4U, sentinel),
                "seed degenerate-bounds framebuffer sentinels");

    const std::uint64_t commands[] = {
        (0x3FULL << 56U) | (3ULL << 51U) | (3ULL << 32U) | base,
        (0x37ULL << 56U) | color,
        (0x2FULL << 56U) | (3ULL << 52U),
        (0x36ULL << 56U) | (4ULL << 44U) | (4ULL << 32U) |
            (4ULL << 12U) | 4ULL
    };
    for (std::size_t i = 0; i < 4U; ++i)
        require(bus.write64(0xE00U + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "seed single-pixel FILL_RECTANGLE command stream");
    require(bus.write32(Bus::kDpStart, 0xE00U) &&
            bus.write32(Bus::kDpEnd, 0xE20U),
            "publish single-pixel fill stream");
    for (unsigned i = 0; i < 4U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "single-pixel FILL_RECTANGLE setup and draw are processed");

    for (std::uint32_t i = 0; i < 16U; ++i) {
        std::uint32_t pixel = 0U;
        require(bus.read32(base + i * 4U, pixel),
                "read degenerate-bounds framebuffer pixel");
        require(pixel == (i == 5U ? color : sentinel),
                "equal inclusive bounds fill exactly one pixel");
    }
}

void testFillRectangleClipsAllEdgesAgainstScissor() {
    using namespace m64plus::n64;
    constexpr std::uint32_t base = 0x3600U;
    constexpr std::uint32_t sentinel = 0xAABBCCDDU;
    constexpr std::uint32_t color = 0x12345678U;
    Bus bus;
    RdpSyncState state{};
    for (std::uint32_t i = 0U; i < 16U; ++i)
        require(bus.write32(base + i * 4U, sentinel),
                "seed all-edge scissor framebuffer sentinels");

    const std::uint64_t commands[] = {
        (0x3FULL << 56U) | (3ULL << 51U) | (3ULL << 32U) | base,
        (0x37ULL << 56U) | color,
        (0x2FULL << 56U) | (3ULL << 52U),
        (0x2DULL << 56U) | (4ULL << 44U) | (4ULL << 32U) |
            (12ULL << 12U) | 12ULL,
        (0x36ULL << 56U) | (16ULL << 44U) | (16ULL << 32U)
    };
    constexpr std::uint32_t start = 0xD00U;
    for (std::size_t i = 0U; i < 5U; ++i)
        require(bus.write64(start + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "seed all-edge scissor fill command stream");
    require(bus.write32(Bus::kDpStart, start) &&
            bus.write32(Bus::kDpEnd, start + 40U),
            "publish all-edge scissor fill stream");
    for (unsigned i = 0U; i < 5U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "all-edge scissored FILL_RECTANGLE setup and draw are processed");

    for (std::uint32_t y = 0U; y < 4U; ++y) {
        for (std::uint32_t x = 0U; x < 4U; ++x) {
            std::uint32_t pixel = 0U;
            require(bus.read32(base + (y * 4U + x) * 4U, pixel),
                    "read all-edge scissored framebuffer pixel");
            const bool inside = x >= 1U && x < 3U && y >= 1U && y < 3U;
            require(pixel == (inside ? color : sentinel),
                    "scissor clips all four edges without writing outside bounds");
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

void testFillRectangleRgba16ScissorPreservesPixelParity() {
    using namespace m64plus::n64;
    constexpr std::uint32_t base = 0x3A00U;
    constexpr std::uint16_t sentinel = 0x55AAU;
    Bus bus;
    RdpSyncState state{};
    for (std::uint32_t i = 0U; i < 8U; ++i)
        require(bus.write16(base + i * 2U, sentinel),
                "seed RGBA16 scissor parity sentinels");

    const std::uint64_t commands[] = {
        (0x3FULL << 56U) | (2ULL << 51U) | (3ULL << 32U) | base,
        (0x37ULL << 56U) | 0xF80107C1ULL,
        (0x2FULL << 56U) | (3ULL << 52U),
        (0x2DULL << 56U) | (4ULL << 44U) | (12ULL << 12U) | 4ULL,
        (0x36ULL << 56U) | (12ULL << 44U)
    };
    constexpr std::uint32_t start = 0xDA0U;
    for (std::size_t i = 0U; i < 5U; ++i)
        require(bus.write64(start + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "seed RGBA16 scissor parity command stream");
    require(bus.write32(Bus::kDpStart, start) &&
            bus.write32(Bus::kDpEnd, start + 40U),
            "publish RGBA16 scissor parity stream");
    for (unsigned i = 0U; i < 5U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "RGBA16 scissored fill setup and draw are processed");

    for (std::uint32_t x = 0U; x < 4U; ++x) {
        std::uint16_t pixel = 0U;
        require(bus.read16(base + x * 2U, pixel),
                "read RGBA16 scissor parity pixel");
        const std::uint16_t expected = x == 1U ? 0x07C1U
            : (x == 2U ? 0xF801U : sentinel);
        require(pixel == expected,
                "RGBA16 scissor keeps absolute framebuffer halfword parity");
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

void testFillRectangleWritesFinalRdramPixelAtExactBoundary() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    const std::uint32_t base = static_cast<std::uint32_t>(Rdram::kSize - 4U);
    constexpr std::uint32_t color = 0x89ABCDEFU;

    const std::uint64_t commands[] = {
        (0x3FULL << 56U) | (3ULL << 51U) | (1ULL << 32U) | base,
        (0x37ULL << 56U) | color,
        (0x2FULL << 56U) | (3ULL << 52U),
        (0x36ULL << 56U)
    };
    constexpr std::uint32_t start = 0xE80U;
    for (std::size_t i = 0U; i < 4U; ++i)
        require(bus.write64(start + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "seed exact-boundary fill command stream");
    require(bus.write32(Bus::kDpStart, start) &&
            bus.write32(Bus::kDpEnd, start + 32U),
            "publish exact-boundary fill command stream");

    for (unsigned i = 0U; i < 4U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "single-pixel fill at final RDRAM word is processed");

    std::uint32_t pixel = 0U;
    require(bus.read32(base, pixel) && pixel == color,
            "final valid RDRAM word receives the fill color");
}

void testFillRectangleWritesFinalRdramHalfwordAtExactBoundary() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    const std::uint32_t base = static_cast<std::uint32_t>(Rdram::kSize - 2U);
    constexpr std::uint32_t color = 0x89ABCDEFU;

    const std::uint64_t commands[] = {
        (0x3FULL << 56U) | (2ULL << 51U) | (1ULL << 32U) | base,
        (0x37ULL << 56U) | color,
        (0x2FULL << 56U) | (3ULL << 52U),
        (0x36ULL << 56U)
    };
    constexpr std::uint32_t start = 0xEC0U;
    for (std::size_t i = 0U; i < 4U; ++i)
        require(bus.write64(start + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "seed exact-boundary RGBA16 fill command stream");
    require(bus.write32(Bus::kDpStart, start) &&
            bus.write32(Bus::kDpEnd, start + 32U),
            "publish exact-boundary RGBA16 fill command stream");

    for (unsigned i = 0U; i < 4U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "single RGBA16 fill at final RDRAM halfword is processed");

    std::uint16_t pixel = 0U;
    require(bus.read16(base, pixel) && pixel == 0x89ABU,
            "final valid RDRAM halfword receives the upper fill-color halfword");
}

void testFillRectangleRejectsRgba16OverflowWithoutPartialWrites() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    const std::uint32_t base = static_cast<std::uint32_t>(Rdram::kSize - 2U);
    constexpr std::uint16_t sentinel = 0x5A6BU;
    require(bus.write16(base, sentinel),
            "seed final RDRAM halfword before overflowing RGBA16 fill");

    const std::uint64_t commands[] = {
        (0x3FULL << 56U) | (2ULL << 51U) | (2ULL << 32U) | base,
        (0x37ULL << 56U) | 0x11223344ULL,
        (0x2FULL << 56U) | (3ULL << 52U),
        (0x36ULL << 56U) | (4ULL << 44U)
    };
    constexpr std::uint32_t start = 0xEE0U;
    for (std::size_t i = 0U; i < 4U; ++i)
        require(bus.write64(start + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "seed overflowing RGBA16 fill command stream");
    require(bus.write32(Bus::kDpStart, start) &&
            bus.write32(Bus::kDpEnd, start + 32U),
            "publish overflowing RGBA16 fill command stream");

    for (unsigned i = 0U; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "overflowing RGBA16 fill setup command is processed");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Unsupported,
            "RGBA16 fill crossing RDRAM end is rejected");

    std::uint16_t pixel = 0U;
    require(bus.read16(base, pixel) && pixel == sentinel,
            "rejected RGBA16 fill leaves final valid halfword unchanged");
}

void testFillRectangleRejectsRgba32OverflowWithoutPartialWrites() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    const std::uint32_t base = static_cast<std::uint32_t>(Rdram::kSize - 4U);
    constexpr std::uint32_t sentinel = 0x5A6B7C8DU;
    require(bus.write32(base, sentinel),
            "seed final RDRAM word before overflowing RGBA32 fill");

    const std::uint64_t commands[] = {
        (0x3FULL << 56U) | (3ULL << 51U) | (2ULL << 32U) | base,
        (0x37ULL << 56U) | 0x11223344ULL,
        (0x2FULL << 56U) | (3ULL << 52U),
        (0x36ULL << 56U) | (4ULL << 44U)
    };
    constexpr std::uint32_t start = 0xF00U;
    for (std::size_t i = 0U; i < 4U; ++i)
        require(bus.write64(start + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "seed overflowing RGBA32 fill command stream");
    require(bus.write32(Bus::kDpStart, start) &&
            bus.write32(Bus::kDpEnd, start + 32U),
            "publish overflowing RGBA32 fill command stream");

    for (unsigned i = 0U; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "overflowing RGBA32 fill setup command is processed");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Unsupported,
            "RGBA32 fill crossing RDRAM end is rejected");

    std::uint32_t pixel = 0U;
    require(bus.read32(base, pixel) && pixel == sentinel,
            "rejected RGBA32 fill leaves final valid word unchanged");
}

void testFillRectangleIgnoresUnselectedFieldOverflow() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    const std::uint32_t base = static_cast<std::uint32_t>(Rdram::kSize - 4U);
    constexpr std::uint32_t sentinel = 0xA1B2C3D4U;
    constexpr std::uint32_t color = 0x10293847U;
    require(bus.write32(base, sentinel),
            "seed final RDRAM word before field-selected fill");

    const std::uint64_t commands[] = {
        (0x3FULL << 56U) | (3ULL << 51U) | base,
        (0x37ULL << 56U) | color,
        (0x2FULL << 56U) | (3ULL << 52U),
        (0x2DULL << 56U) | (1ULL << 25U) |
            (4ULL << 12U) | 8ULL,
        (0x36ULL << 56U) | (4ULL << 32U)
    };
    constexpr std::uint32_t start = 0xF60U;
    for (std::size_t i = 0U; i < 5U; ++i)
        require(bus.write64(start + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "seed field-selected boundary fill command stream");
    require(bus.write32(Bus::kDpStart, start) &&
            bus.write32(Bus::kDpEnd, start + 40U),
            "publish field-selected boundary fill command stream");

    for (unsigned i = 0U; i < 5U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "field-selected boundary fill command is processed");

    std::uint32_t pixel = 0U;
    require(bus.read32(base, pixel) && pixel == color,
            "valid selected scanline is written while overflowing unselected scanline is ignored");
}

void testFillRectangleRejectsLaterRowOverflowWithoutPartialWrites() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    const std::uint32_t base = static_cast<std::uint32_t>(Rdram::kSize - 4U);
    constexpr std::uint32_t sentinel = 0xC1D2E3F4U;
    require(bus.write32(base, sentinel),
            "seed final RDRAM word before later-row overflow");

    const std::uint64_t commands[] = {
        (0x3FULL << 56U) | (3ULL << 51U) | base,
        (0x37ULL << 56U) | 0x11223344ULL,
        (0x2FULL << 56U) | (3ULL << 52U),
        (0x36ULL << 56U) | 4ULL
    };
    constexpr std::uint32_t start = 0xF20U;
    for (std::size_t i = 0U; i < 4U; ++i)
        require(bus.write64(start + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "seed later-row overflow fill command stream");
    require(bus.write32(Bus::kDpStart, start) &&
            bus.write32(Bus::kDpEnd, start + 32U),
            "publish later-row overflow fill command stream");

    for (unsigned i = 0U; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "later-row overflow fill setup command is processed");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Unsupported,
            "RGBA32 fill with a later row beyond RDRAM is rejected");

    std::uint32_t pixel = 0U;
    require(bus.read32(base, pixel) && pixel == sentinel,
            "later-row overflow rejection preserves the earlier valid row");
}

void testFillRectangleRejectsRgba16LaterRowOverflowWithoutPartialWrites() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    const std::uint32_t base = static_cast<std::uint32_t>(Rdram::kSize - 2U);
    constexpr std::uint16_t sentinel = 0x6B7CU;
    require(bus.write16(base, sentinel),
            "seed final RDRAM halfword before RGBA16 later-row overflow");

    const std::uint64_t commands[] = {
        (0x3FULL << 56U) | (2ULL << 51U) | base,
        (0x37ULL << 56U) | 0x11223344ULL,
        (0x2FULL << 56U) | (3ULL << 52U),
        (0x36ULL << 56U) | 4ULL
    };
    constexpr std::uint32_t start = 0xF40U;
    for (std::size_t i = 0U; i < 4U; ++i)
        require(bus.write64(start + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "seed RGBA16 later-row overflow fill command stream");
    require(bus.write32(Bus::kDpStart, start) &&
            bus.write32(Bus::kDpEnd, start + 32U),
            "publish RGBA16 later-row overflow fill command stream");

    for (unsigned i = 0U; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "RGBA16 later-row overflow setup command is processed");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Unsupported,
            "RGBA16 fill with a later row beyond RDRAM is rejected");

    std::uint16_t pixel = 0U;
    require(bus.read16(base, pixel) && pixel == sentinel,
            "RGBA16 later-row overflow rejection preserves the earlier valid row");
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

void testFillRectangleRejectsEmptyScissorIntersectionWithoutWrites() {
    using namespace m64plus::n64;
    constexpr std::uint32_t base = 0x3C00U;
    constexpr std::uint32_t sentinel = 0xAABBCCDDU;
    Bus bus;
    RdpSyncState state{};
    for (std::uint32_t i = 0; i < 16U; ++i)
        require(bus.write32(base + i * 4U, sentinel),
                "seed empty-scissor framebuffer sentinels");

    const std::uint64_t commands[] = {
        (0x3FULL << 56U) | (3ULL << 51U) | (3ULL << 32U) | base,
        (0x37ULL << 56U) | 0x11223344ULL,
        (0x2FULL << 56U) | (3ULL << 52U),
        (0x2DULL << 56U) | (8ULL << 44U) | (8ULL << 32U) |
            (8ULL << 12U) | 8ULL,
        (0x36ULL << 56U) | (12ULL << 44U) | (12ULL << 32U)
    };
    constexpr std::uint32_t start = 0xE00U;
    for (std::size_t i = 0; i < 5U; ++i)
        require(bus.write64(start + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "seed empty-scissor fill command stream");
    require(bus.write32(Bus::kDpStart, start) &&
            bus.write32(Bus::kDpEnd, start + 40U),
            "publish empty-scissor fill command stream");

    for (unsigned i = 0; i < 4U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "empty-scissor setup commands are processed");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Unsupported,
            "empty scissor intersection does not render a fill rectangle");

    for (std::uint32_t i = 0; i < 16U; ++i) {
        std::uint32_t pixel = 0U;
        require(bus.read32(base + i * 4U, pixel) && pixel == sentinel,
                "empty scissor intersection leaves framebuffer unchanged");
    }
}

void testFillRectangleRejectsInvertedCoordinatesWithoutWrites() {
    using namespace m64plus::n64;
    constexpr std::uint32_t base = 0x3E00U;
    constexpr std::uint32_t sentinel = 0x5A6B7C8DU;
    Bus bus;
    RdpSyncState state{};
    for (std::uint32_t i = 0U; i < 16U; ++i)
        require(bus.write32(base + i * 4U, sentinel),
                "seed inverted-coordinate framebuffer sentinels");

    const std::uint64_t commands[] = {
        (0x3FULL << 56U) | (3ULL << 51U) | (3ULL << 32U) | base,
        (0x37ULL << 56U) | 0x11223344ULL,
        (0x2FULL << 56U) | (3ULL << 52U),
        (0x36ULL << 56U) | (4ULL << 44U) | (8ULL << 12U) | 8ULL
    };
    constexpr std::uint32_t start = 0xE40U;
    for (std::size_t i = 0U; i < 4U; ++i)
        require(bus.write64(start + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "seed inverted-coordinate fill command stream");
    require(bus.write32(Bus::kDpStart, start) &&
            bus.write32(Bus::kDpEnd, start + 32U),
            "publish inverted-coordinate fill stream");

    for (unsigned i = 0U; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "inverted-coordinate fill setup commands are processed");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Unsupported,
            "fill with right edge before left edge is rejected");

    for (std::uint32_t i = 0U; i < 16U; ++i) {
        std::uint32_t pixel = 0U;
        require(bus.read32(base + i * 4U, pixel) && pixel == sentinel,
                "inverted-coordinate fill leaves framebuffer unchanged");
    }
}

void testFillRectangleRejectsInvertedVerticalCoordinatesWithoutWrites() {
    using namespace m64plus::n64;
    constexpr std::uint32_t base = 0x3F00U;
    constexpr std::uint32_t sentinel = 0x6A7B8C9DU;
    Bus bus;
    RdpSyncState state{};
    for (std::uint32_t i = 0U; i < 16U; ++i)
        require(bus.write32(base + i * 4U, sentinel),
                "seed inverted-vertical framebuffer sentinels");

    const std::uint64_t commands[] = {
        (0x3FULL << 56U) | (3ULL << 51U) | (3ULL << 32U) | base,
        (0x37ULL << 56U) | 0x11223344ULL,
        (0x2FULL << 56U) | (3ULL << 52U),
        (0x36ULL << 56U) | (8ULL << 44U) | (4ULL << 32U) | 8ULL
    };
    constexpr std::uint32_t start = 0xE60U;
    for (std::size_t i = 0U; i < 4U; ++i)
        require(bus.write64(start + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "seed inverted-vertical fill command stream");
    require(bus.write32(Bus::kDpStart, start) &&
            bus.write32(Bus::kDpEnd, start + 32U),
            "publish inverted-vertical fill command stream");

    for (unsigned i = 0U; i < 3U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "inverted-vertical fill setup commands are processed");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Unsupported,
            "fill with bottom edge before top edge is rejected");

    for (std::uint32_t i = 0U; i < 16U; ++i) {
        std::uint32_t pixel = 0U;
        require(bus.read32(base + i * 4U, pixel) && pixel == sentinel,
                "inverted-vertical fill leaves framebuffer unchanged");
    }
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

void testTriangleCommandsAreConsumedAsUnsupportedUntilRasterizationExists() {
    using namespace m64plus::n64;
    for (std::uint8_t flags = 0U; flags < 8U; ++flags) {
        Bus bus;
        RdpSyncState state{};
        const std::uint32_t start = 0xA80U;
        const std::uint8_t opcode = static_cast<std::uint8_t>(0x08U + flags);
        const std::uint8_t words = RdpCommandDecoder::decode(header(opcode)).lengthWords;
        for (std::uint32_t i = 0U; i < words; ++i) {
            const std::uint64_t value = i == 0U
                ? (static_cast<std::uint64_t>(opcode) << 56U)
                : (0xA500000000000000ULL | i);
            require(bus.write64(start + i * 8U, value),
                    "seed complete triangle command packet");
        }
        require(bus.write32(Bus::kDpStart, start),
                "set DPC start for triangle command");
        require(bus.write32(Bus::kDpEnd, start + static_cast<std::uint32_t>(words) * 8U),
                "publish complete triangle command");

        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Unsupported,
                "triangle command remains explicitly unsupported before rasterization");
        require(state.hasTriangle &&
                state.lastTriangle.layout.wordCount == words &&
                state.lastTriangle.edge.yLow == 0U,
                "processor retains the fully decoded triangle for a future rasterizer");
        std::uint32_t current = 0U;
        require(bus.read32(Bus::kDpCurrent, current) &&
                current == start + static_cast<std::uint32_t>(words) * 8U,
                "unsupported triangle consumes its entire framed packet");
    }
}


void testFlatFillTriangleRasterizesRightTriangle() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    constexpr std::uint32_t framebuffer = 0x200U;
    constexpr std::uint32_t stream = 0xB00U;
    constexpr std::uint32_t sentinel = 0xDEADBEEFU;
    constexpr std::uint32_t fill = 0xAABBCCDDU;
    for (std::uint32_t i = 0U; i < 8U * 8U; ++i)
        require(bus.write32(framebuffer + i * 4U, sentinel),
                "seed flat-triangle framebuffer");

    const std::uint64_t commands[] = {
        (0x3FULL << 56U) | (3ULL << 51U) | (7ULL << 32U) | framebuffer,
        (0x37ULL << 56U) | fill,
        (0x2FULL << 56U) | (3ULL << 52U),
        (0x2DULL << 56U) | (32ULL << 12U) | 32ULL,
        // Right-major triangle: y=[0,4), xH=2, xM=2 and dxM/dy=1 px/row.
        (0x08ULL << 56U) | (1ULL << 55U) | (16ULL << 16U) | 16ULL,
        0ULL,
        (2ULL * 65536ULL) << 32U,
        ((2ULL * 65536ULL) << 32U) | 0x00010000ULL
    };
    for (std::size_t i = 0U; i < sizeof(commands) / sizeof(commands[0]); ++i)
        require(bus.write64(stream + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "write flat-triangle command stream");
    require(bus.write32(Bus::kDpStart, stream) &&
            bus.write32(Bus::kDpEnd, stream + 64U),
            "publish flat-triangle command stream");

    for (unsigned i = 0U; i < 4U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "flat-triangle setup command is processed");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "flat-fill triangle is rasterized");

    for (std::uint32_t y = 0U; y < 8U; ++y) {
        for (std::uint32_t x = 0U; x < 8U; ++x) {
            std::uint32_t pixel = 0U;
            require(bus.read32(framebuffer + (y * 8U + x) * 4U, pixel),
                    "read flat-triangle output pixel");
            const bool expected = y < 4U && x >= 2U && x <= 2U + y;
            require(pixel == (expected ? fill : sentinel),
                    "flat triangle fills only the expected staircase pixels");
        }
    }
}


void testFlatFillTriangleUsesLowerEdgeAfterMiddleY() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    constexpr std::uint32_t framebuffer = 0x1900U;
    constexpr std::uint32_t stream = 0x1A00U;
    constexpr std::uint32_t sentinel = 0xDEADBEEFU;
    constexpr std::uint32_t fill = 0x89ABCDEFU;
    for (std::uint32_t i = 0U; i < 8U * 8U; ++i)
        require(bus.write32(framebuffer + i * 4U, sentinel),
                "seed middle-Y transition triangle framebuffer");

    const std::uint64_t commands[] = {
        (0x3FULL << 56U) | (3ULL << 51U) | (7ULL << 32U) | framebuffer,
        (0x37ULL << 56U) | fill,
        (0x2FULL << 56U) | (3ULL << 52U),
        (0x2DULL << 56U) | (32ULL << 12U) | 32ULL,
        // The minor edge switches at Y=2: it is vertical above YM,
        // then advances one pixel per scanline below YM.
        (0x08ULL << 56U) | (8ULL << 16U) | 16ULL,
        (2ULL * 65536ULL << 32U) | 0x00010000ULL,
        (6ULL * 65536ULL) << 32U,
        (2ULL * 65536ULL) << 32U
    };
    for (std::size_t i = 0U; i < sizeof(commands) / sizeof(commands[0]); ++i)
        require(bus.write64(stream + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "write middle-Y transition triangle command stream");
    require(bus.write32(Bus::kDpStart, stream) &&
            bus.write32(Bus::kDpEnd, stream + 64U),
            "publish middle-Y transition triangle command stream");

    for (unsigned i = 0U; i < 4U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "middle-Y transition triangle setup command is processed");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "middle-Y transition flat-fill triangle is rasterized");

    for (std::uint32_t y = 0U; y < 8U; ++y) {
        for (std::uint32_t x = 0U; x < 8U; ++x) {
            std::uint32_t pixel = 0U;
            require(bus.read32(framebuffer + (y * 8U + x) * 4U, pixel),
                    "read middle-Y transition triangle output pixel");
            const std::uint32_t left = y < 2U ? 2U : y;
            const bool covered = y < 4U && x >= left && x <= 6U;
            require(pixel == (covered ? fill : sentinel),
                    "triangle switches from upper to lower minor edge at middle Y");
        }
    }
}


void testFlatFillTriangleUsesNegativeLowerEdgeSlope() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    constexpr std::uint32_t framebuffer = 0x1B00U;
    constexpr std::uint32_t stream = 0x1C00U;
    constexpr std::uint32_t sentinel = 0xDEADBEEFU;
    constexpr std::uint32_t fill = 0x76543210U;
    for (std::uint32_t i = 0U; i < 8U * 8U; ++i)
        require(bus.write32(framebuffer + i * 4U, sentinel),
                "seed negative lower-edge slope triangle framebuffer");

    const std::uint64_t commands[] = {
        (0x3FULL << 56U) | (3ULL << 51U) | (7ULL << 32U) | framebuffer,
        (0x37ULL << 56U) | fill,
        (0x2FULL << 56U) | (3ULL << 52U),
        (0x2DULL << 56U) | (32ULL << 12U) | 32ULL,
        // After YM=2, the minor edge moves left by one pixel per scanline.
        (0x08ULL << 56U) | (8ULL << 16U) | 16ULL,
        (4ULL * 65536ULL << 32U) | 0x3FFF0000ULL,
        (6ULL * 65536ULL) << 32U,
        (2ULL * 65536ULL) << 32U
    };
    for (std::size_t i = 0U; i < sizeof(commands) / sizeof(commands[0]); ++i)
        require(bus.write64(stream + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "write negative lower-edge slope triangle command stream");
    require(bus.write32(Bus::kDpStart, stream) &&
            bus.write32(Bus::kDpEnd, stream + 64U),
            "publish negative lower-edge slope triangle command stream");

    for (unsigned i = 0U; i < 4U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "negative lower-edge slope triangle setup command is processed");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "negative lower-edge slope flat-fill triangle is rasterized");

    for (std::uint32_t y = 0U; y < 8U; ++y) {
        for (std::uint32_t x = 0U; x < 8U; ++x) {
            std::uint32_t pixel = 0U;
            require(bus.read32(framebuffer + (y * 8U + x) * 4U, pixel),
                    "read negative lower-edge slope triangle output pixel");
            const std::uint32_t left = y < 2U ? 2U : (y == 2U ? 3U : 2U);
            const bool covered = y < 4U && x >= left && x <= 6U;
            require(pixel == (covered ? fill : sentinel),
                    "negative minor-edge slope moves left after middle Y");
        }
    }
}


void testFlatFillTriangleFloorsPositiveFractionalX() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    constexpr std::uint32_t framebuffer = 0x1900U;
    constexpr std::uint32_t stream = 0x1A00U;
    constexpr std::uint32_t sentinel = 0xDEADBEEFU;
    constexpr std::uint32_t fill = 0x468ACE02U;
    for (std::uint32_t i = 0U; i < 8U * 8U; ++i)
        require(bus.write32(framebuffer + i * 4U, sentinel),
                "seed positive-fractional-X triangle framebuffer");

    const std::uint64_t commands[] = {
        (0x3FULL << 56U) | (3ULL << 51U) | (7ULL << 32U) | framebuffer,
        (0x37ULL << 56U) | fill,
        (0x2FULL << 56U) | (3ULL << 52U),
        (0x2DULL << 56U) | (32ULL << 12U) | 32ULL,
        (0x08ULL << 56U) | (16ULL << 16U) | 16ULL,
        (0x00018000ULL << 32U),
        (0x00058000ULL << 32U),
        (0x00018000ULL << 32U)
    };
    for (std::size_t i = 0U; i < sizeof(commands) / sizeof(commands[0]); ++i)
        require(bus.write64(stream + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "write positive-fractional-X triangle command stream");
    require(bus.write32(Bus::kDpStart, stream) &&
            bus.write32(Bus::kDpEnd, stream + 64U),
            "publish positive-fractional-X triangle command stream");

    for (unsigned i = 0U; i < 4U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "positive-fractional-X triangle setup command is processed");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "positive-fractional-X flat-fill triangle is rasterized");

    for (std::uint32_t y = 0U; y < 8U; ++y) {
        for (std::uint32_t x = 0U; x < 8U; ++x) {
            std::uint32_t pixel = 0U;
            require(bus.read32(framebuffer + (y * 8U + x) * 4U, pixel),
                    "read positive-fractional-X triangle output pixel");
            const bool covered = y < 4U && x >= 1U && x <= 5U;
            require(pixel == (covered ? fill : sentinel),
                    "positive fractional edge coordinates floor to covered pixel boundaries");
        }
    }
}



void testFlatFillTriangleFloorsPositiveFractionalXWithSlope() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    constexpr std::uint32_t framebuffer = 0x1D00U;
    constexpr std::uint32_t stream = 0x1E00U;
    constexpr std::uint32_t sentinel = 0xDEADBEEFU;
    constexpr std::uint32_t fill = 0x68ACE024U;
    for (std::uint32_t i = 0U; i < 8U * 8U; ++i)
        require(bus.write32(framebuffer + i * 4U, sentinel),
                "seed sloped positive-fractional-X triangle framebuffer");

    const std::uint64_t commands[] = {
        (0x3FULL << 56U) | (3ULL << 51U) | (7ULL << 32U) | framebuffer,
        (0x37ULL << 56U) | fill,
        (0x2FULL << 56U) | (3ULL << 52U),
        (0x2DULL << 56U) | (32ULL << 12U) | 32ULL,
        (0x08ULL << 56U) | (16ULL << 16U) | 16ULL,
        (0x00018000ULL << 32U),
        (0x00058000ULL << 32U) | 0x00010000ULL,
        (0x00058000ULL << 32U)
    };
    for (std::size_t i = 0U; i < sizeof(commands) / sizeof(commands[0]); ++i)
        require(bus.write64(stream + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "write sloped positive-fractional-X triangle command stream");
    require(bus.write32(Bus::kDpStart, stream) &&
            bus.write32(Bus::kDpEnd, stream + 64U),
            "publish sloped positive-fractional-X triangle command stream");

    for (unsigned i = 0U; i < 4U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "sloped positive-fractional-X triangle setup command is processed");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "sloped positive-fractional-X flat-fill triangle is rasterized");

    for (std::uint32_t y = 0U; y < 8U; ++y) {
        for (std::uint32_t x = 0U; x < 8U; ++x) {
            std::uint32_t pixel = 0U;
            require(bus.read32(framebuffer + (y * 8U + x) * 4U, pixel),
                    "read sloped positive-fractional-X triangle output pixel");
            const bool covered = y < 4U && x >= 5U && x <= 6U + y;
            require(pixel == (covered ? fill : sentinel),
                    "negative major edge retreats one pixel per scanline");
        }
    }
}


void testFlatFillTriangleFloorsNegativeMajorSlope() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    constexpr std::uint32_t framebuffer = 0x1D00U;
    constexpr std::uint32_t stream = 0x1E00U;
    constexpr std::uint32_t sentinel = 0xDEADBEEFU;
    constexpr std::uint32_t fill = 0x68ACE024U;
    for (std::uint32_t i = 0U; i < 8U * 8U; ++i)
        require(bus.write32(framebuffer + i * 4U, sentinel),
                "seed sloped negative-major-slope triangle framebuffer");

    const std::uint64_t commands[] = {
        (0x3FULL << 56U) | (3ULL << 51U) | (7ULL << 32U) | framebuffer,
        (0x37ULL << 56U) | fill,
        (0x2FULL << 56U) | (3ULL << 52U),
        (0x2DULL << 56U) | (32ULL << 12U) | 32ULL,
        (0x08ULL << 56U) | (16ULL << 16U) | 16ULL,
        (0x00018000ULL << 32U),
        (0x00058000ULL << 32U) | 0x3FFF0000ULL,
        (0x00018000ULL << 32U)
    };
    for (std::size_t i = 0U; i < sizeof(commands) / sizeof(commands[0]); ++i)
        require(bus.write64(stream + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "write sloped negative-major-slope triangle command stream");
    require(bus.write32(Bus::kDpStart, stream) &&
            bus.write32(Bus::kDpEnd, stream + 64U),
            "publish sloped negative-major-slope triangle command stream");

    for (unsigned i = 0U; i < 4U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "sloped negative-major-slope triangle setup command is processed");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "sloped negative-major-slope flat-fill triangle is rasterized");

    for (std::uint32_t y = 0U; y < 8U; ++y) {
        for (std::uint32_t x = 0U; x < 8U; ++x) {
            std::uint32_t pixel = 0U;
            require(bus.read32(framebuffer + (y * 8U + x) * 4U, pixel),
                    "read sloped negative-major-slope triangle output pixel");
            const bool covered = y < 4U && x >= 1U && x <= 5U - y;
            require(pixel == (covered ? fill : sentinel),
                    "positive fractional major edge advances one pixel per scanline");
        }
    }
}



void testFlatFillTriangleEdgeFlipFloorsPositiveFractionalX() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    constexpr std::uint32_t framebuffer = 0x1B00U;
    constexpr std::uint32_t stream = 0x1C00U;
    constexpr std::uint32_t sentinel = 0xDEADBEEFU;
    constexpr std::uint32_t fill = 0x579BDF13U;
    for (std::uint32_t i = 0U; i < 8U * 8U; ++i)
        require(bus.write32(framebuffer + i * 4U, sentinel),
                "seed edge-flip positive-fractional-X triangle framebuffer");

    const std::uint64_t commands[] = {
        (0x3FULL << 56U) | (3ULL << 51U) | (7ULL << 32U) | framebuffer,
        (0x37ULL << 56U) | fill,
        (0x2FULL << 56U) | (3ULL << 52U),
        (0x2DULL << 56U) | (32ULL << 12U) | 32ULL,
        (0x08ULL << 56U) | (1ULL << 55U) | (16ULL << 16U) | 16ULL,
        (0x00058000ULL << 32U),
        (0x00018000ULL << 32U) | 0x00010000ULL,
        (0x00058000ULL << 32U)
    };
    for (std::size_t i = 0U; i < sizeof(commands) / sizeof(commands[0]); ++i)
        require(bus.write64(stream + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "write edge-flip positive-fractional-X sloped triangle command stream");
    require(bus.write32(Bus::kDpStart, stream) &&
            bus.write32(Bus::kDpEnd, stream + 64U),
            "publish edge-flip positive-fractional-X triangle command stream");

    for (unsigned i = 0U; i < 4U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "edge-flip positive-fractional-X triangle setup command is processed");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "edge-flip positive-fractional-X flat-fill triangle is rasterized");

    for (std::uint32_t y = 0U; y < 8U; ++y) {
        for (std::uint32_t x = 0U; x < 8U; ++x) {
            std::uint32_t pixel = 0U;
            require(bus.read32(framebuffer + (y * 8U + x) * 4U, pixel),
                    "read edge-flip positive-fractional-X triangle output pixel");
            const bool covered = y < 4U && x >= y + 1U && x <= 5U;
            require(pixel == (covered ? fill : sentinel),
                    "edge-flip positive fractional sloped edge advances one pixel per scanline");
        }
    }
}


void testFlatFillTriangleEdgeFlipFloorsNegativeMajorSlope() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    constexpr std::uint32_t framebuffer = 0x1F00U;
    constexpr std::uint32_t stream = 0x2000U;
    constexpr std::uint32_t sentinel = 0xDEADBEEFU;
    constexpr std::uint32_t fill = 0x68ACE024U;
    for (std::uint32_t i = 0U; i < 8U * 8U; ++i)
        require(bus.write32(framebuffer + i * 4U, sentinel),
                "seed edge-flip negative-major-slope triangle framebuffer");

    const std::uint64_t commands[] = {
        (0x3FULL << 56U) | (3ULL << 51U) | (7ULL << 32U) | framebuffer,
        (0x37ULL << 56U) | fill,
        (0x2FULL << 56U) | (3ULL << 52U),
        (0x2DULL << 56U) | (32ULL << 12U) | 32ULL,
        (0x08ULL << 56U) | (1ULL << 55U) | (16ULL << 16U) | 16ULL,
        (0x00018000ULL << 32U),
        (0x00048000ULL << 32U) | 0x3FFF0000ULL,
        (0x00058000ULL << 32U)
    };
    for (std::size_t i = 0U; i < sizeof(commands) / sizeof(commands[0]); ++i)
        require(bus.write64(stream + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "write edge-flip negative-major-slope sloped triangle command stream");
    require(bus.write32(Bus::kDpStart, stream) &&
            bus.write32(Bus::kDpEnd, stream + 64U),
            "publish edge-flip negative-major-slope triangle command stream");

    for (unsigned i = 0U; i < 4U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "edge-flip negative-major-slope triangle setup command is processed");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "edge-flip negative-major-slope flat-fill triangle is rasterized");

    for (std::uint32_t y = 0U; y < 8U; ++y) {
        for (std::uint32_t x = 0U; x < 8U; ++x) {
            std::uint32_t pixel = 0U;
            require(bus.read32(framebuffer + (y * 8U + x) * 4U, pixel),
                    "read edge-flip negative-major-slope triangle output pixel");
            const bool covered = y < 4U && x >= 3U - y && x <= 5U;
            require(pixel == (covered ? fill : sentinel),
                    "edge-flip negative major edge retreats one pixel per scanline");
        }
    }
}



void testFlatFillTriangleClipsNegativeFractionalX() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    constexpr std::uint32_t framebuffer = 0x1500U;
    constexpr std::uint32_t stream = 0x1600U;
    constexpr std::uint32_t sentinel = 0xDEADBEEFU;
    constexpr std::uint32_t fill = 0x2468ACE0U;
    for (std::uint32_t i = 0U; i < 8U * 8U; ++i)
        require(bus.write32(framebuffer + i * 4U, sentinel),
                "seed negative-X triangle framebuffer");

    const std::uint64_t commands[] = {
        (0x3FULL << 56U) | (3ULL << 51U) | (7ULL << 32U) | framebuffer,
        (0x37ULL << 56U) | fill,
        (0x2FULL << 56U) | (3ULL << 52U),
        (0x2DULL << 56U) | (32ULL << 12U) | 32ULL,
        // The minor edge starts at x=-1 and advances one pixel per row.
        (0x08ULL << 56U) | (16ULL << 16U) | 16ULL,
        0ULL,
        (3ULL * 65536ULL) << 32U,
        (0x0FFF0000ULL << 32U) | 0x00010000ULL
    };
    for (std::size_t i = 0U; i < sizeof(commands) / sizeof(commands[0]); ++i)
        require(bus.write64(stream + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "write negative-X triangle command stream");
    require(bus.write32(Bus::kDpStart, stream) &&
            bus.write32(Bus::kDpEnd, stream + 64U),
            "publish negative-X triangle command stream");

    for (unsigned i = 0U; i < 4U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "negative-X triangle setup command is processed");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "negative-X flat-fill triangle is clipped and rasterized");

    for (std::uint32_t y = 0U; y < 8U; ++y) {
        for (std::uint32_t x = 0U; x < 8U; ++x) {
            std::uint32_t pixel = 0U;
            require(bus.read32(framebuffer + (y * 8U + x) * 4U, pixel),
                    "read negative-X triangle output pixel");
            const bool covered = y < 4U && x <= 3U &&
                x >= (y > 1U ? y - 1U : 0U);
            require(pixel == (covered ? fill : sentinel),
                    "negative and off-screen fractional edge clips to visible framebuffer pixels");
        }
    }
}


void testFlatFillTriangleEdgeFlipClipsNegativeFractionalX() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    constexpr std::uint32_t framebuffer = 0x1700U;
    constexpr std::uint32_t stream = 0x1800U;
    constexpr std::uint32_t sentinel = 0xDEADBEEFU;
    constexpr std::uint32_t fill = 0x3579BDF1U;
    for (std::uint32_t i = 0U; i < 8U * 8U; ++i)
        require(bus.write32(framebuffer + i * 4U, sentinel),
                "seed edge-flip negative-X triangle framebuffer");

    const std::uint64_t commands[] = {
        (0x3FULL << 56U) | (3ULL << 51U) | (7ULL << 32U) | framebuffer,
        (0x37ULL << 56U) | fill,
        (0x2FULL << 56U) | (3ULL << 52U),
        (0x2DULL << 56U) | (32ULL << 12U) | 32ULL,
        // Edge flip makes the negative, rising major edge the left boundary.
        (0x08ULL << 56U) | (1ULL << 55U) | (16ULL << 16U) | 16ULL,
        (3ULL * 65536ULL) << 32U,
        (0x0FFF0000ULL << 32U) | 0x00010000ULL,
        (3ULL * 65536ULL) << 32U
    };
    for (std::size_t i = 0U; i < sizeof(commands) / sizeof(commands[0]); ++i)
        require(bus.write64(stream + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "write edge-flip negative-X triangle command stream");
    require(bus.write32(Bus::kDpStart, stream) &&
            bus.write32(Bus::kDpEnd, stream + 64U),
            "publish edge-flip negative-X triangle command stream");

    for (unsigned i = 0U; i < 4U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "edge-flip negative-X triangle setup command is processed");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "edge-flip negative-X flat-fill triangle is clipped and rasterized");

    for (std::uint32_t y = 0U; y < 8U; ++y) {
        for (std::uint32_t x = 0U; x < 8U; ++x) {
            std::uint32_t pixel = 0U;
            require(bus.read32(framebuffer + (y * 8U + x) * 4U, pixel),
                    "read edge-flip negative-X triangle output pixel");
            const bool covered = y < 4U && x <= 3U &&
                x >= (y > 1U ? y - 1U : 0U);
            require(pixel == (covered ? fill : sentinel),
                    "edge-flip keeps the negative fractional major edge on the left and clips at framebuffer X zero");
        }
    }
}


void testFlatFillTriangleRasterizesOppositeEdgeOrientation() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    constexpr std::uint32_t framebuffer = 0x1300U;
    constexpr std::uint32_t stream = 0x1400U;
    constexpr std::uint32_t sentinel = 0xDEADBEEFU;
    constexpr std::uint32_t fill = 0x13579BDFU;
    for (std::uint32_t i = 0U; i < 8U * 8U; ++i)
        require(bus.write32(framebuffer + i * 4U, sentinel),
                "seed opposite-orientation triangle framebuffer");

    const std::uint64_t commands[] = {
        (0x3FULL << 56U) | (3ULL << 51U) | (7ULL << 32U) | framebuffer,
        (0x37ULL << 56U) | fill,
        (0x2FULL << 56U) | (3ULL << 52U),
        (0x2DULL << 56U) | (32ULL << 12U) | 32ULL,
        // Opposite edge orientation: left edge advances from x=2,
        // while the right major edge remains at x=6.
        (0x08ULL << 56U) | (16ULL << 16U) | 16ULL,
        (2ULL * 65536ULL << 32U) | 0x00010000ULL,
        (6ULL * 65536ULL) << 32U,
        ((2ULL * 65536ULL) << 32U) | 0x00010000ULL
    };
    for (std::size_t i = 0U; i < sizeof(commands) / sizeof(commands[0]); ++i)
        require(bus.write64(stream + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "write opposite-orientation triangle command stream");
    require(bus.write32(Bus::kDpStart, stream) &&
            bus.write32(Bus::kDpEnd, stream + 64U),
            "publish opposite-orientation triangle command stream");

    for (unsigned i = 0U; i < 4U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "opposite-orientation triangle setup command is processed");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "opposite-orientation flat-fill triangle is rasterized");

    for (std::uint32_t y = 0U; y < 8U; ++y) {
        for (std::uint32_t x = 0U; x < 8U; ++x) {
            std::uint32_t pixel = 0U;
            require(bus.read32(framebuffer + (y * 8U + x) * 4U, pixel),
                    "read opposite-orientation triangle output pixel");
            const bool covered = y < 4U && x >= 2U + y && x <= 6U;
            require(pixel == (covered ? fill : sentinel),
                    "opposite edge orientation fills only expected staircase pixels");
        }
    }
}


void testFlatFillTriangleRgba16UsesFramebufferHalfwordParity() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    constexpr std::uint32_t framebuffer = 0x500U;
    constexpr std::uint32_t stream = 0xC00U;
    constexpr std::uint16_t evenColor = 0xAABBU;
    constexpr std::uint16_t oddColor = 0xCCDDU;
    constexpr std::uint16_t sentinel = 0xDEADU;
    for (std::uint32_t i = 0U; i < 8U * 8U; ++i)
        require(bus.write16(framebuffer + i * 2U, sentinel),
                "seed RGBA16 triangle framebuffer");

    const std::uint64_t commands[] = {
        (0x3FULL << 56U) | (2ULL << 51U) | (7ULL << 32U) | framebuffer,
        (0x37ULL << 56U) | 0xAABBCCDDULL,
        (0x2FULL << 56U) | (3ULL << 52U),
        (0x2DULL << 56U) | (32ULL << 12U) | 32ULL,
        (0x08ULL << 56U) | (1ULL << 55U) | (16ULL << 16U) | 16ULL,
        0ULL,
        (2ULL * 65536ULL) << 32U,
        ((2ULL * 65536ULL) << 32U) | 0x00010000ULL
    };
    for (std::size_t i = 0U; i < sizeof(commands) / sizeof(commands[0]); ++i)
        require(bus.write64(stream + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "write RGBA16 triangle command stream");
    require(bus.write32(Bus::kDpStart, stream) &&
            bus.write32(Bus::kDpEnd, stream + 64U),
            "publish RGBA16 triangle command stream");
    for (unsigned i = 0U; i < 4U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "RGBA16 triangle setup command is processed");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "RGBA16 flat-fill triangle is rasterized");

    for (std::uint32_t y = 0U; y < 8U; ++y) {
        for (std::uint32_t x = 0U; x < 8U; ++x) {
            std::uint16_t pixel = 0U;
            require(bus.read16(framebuffer + (y * 8U + x) * 2U, pixel),
                    "read RGBA16 triangle output pixel");
            const bool covered = y < 4U && x >= 2U && x <= 2U + y;
            const std::uint16_t expected = !covered ? sentinel
                : (((y * 8U + x) & 1U) == 0U ? evenColor : oddColor);
            require(pixel == expected,
                    "RGBA16 triangle selects fill halfword by framebuffer pixel parity");
        }
    }
}

void testFlatFillTriangleHonorsInterlacedFieldSelection() {
    using namespace m64plus::n64;
    for (std::uint8_t oddLine = 0U; oddLine < 2U; ++oddLine) {
        Bus bus;
        RdpSyncState state{};
        constexpr std::uint32_t framebuffer = 0x700U;
        constexpr std::uint32_t stream = 0xD00U;
        constexpr std::uint32_t sentinel = 0xDEADBEEFU;
        constexpr std::uint32_t fill = 0x12345678U;
        for (std::uint32_t i = 0U; i < 8U * 8U; ++i)
            require(bus.write32(framebuffer + i * 4U, sentinel),
                    "seed interlaced triangle framebuffer");
        const std::uint64_t commands[] = {
            (0x3FULL << 56U) | (3ULL << 51U) | (7ULL << 32U) | framebuffer,
            (0x37ULL << 56U) | fill,
            (0x2FULL << 56U) | (3ULL << 52U),
            (0x2DULL << 56U) | (1ULL << 25U) |
                (static_cast<std::uint64_t>(oddLine) << 24U) |
                (32ULL << 12U) | 32ULL,
            (0x08ULL << 56U) | (1ULL << 55U) | (16ULL << 16U) | 16ULL,
            0ULL,
            (2ULL * 65536ULL) << 32U,
            ((2ULL * 65536ULL) << 32U) | 0x00010000ULL
        };
        for (std::size_t i = 0U; i < sizeof(commands) / sizeof(commands[0]); ++i)
            require(bus.write64(stream + static_cast<std::uint32_t>(i * 8U), commands[i]),
                    "write interlaced triangle command stream");
        require(bus.write32(Bus::kDpStart, stream) &&
                bus.write32(Bus::kDpEnd, stream + 64U),
                "publish interlaced triangle command stream");
        for (unsigned i = 0U; i < 4U; ++i)
            require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                    "interlaced triangle setup command is processed");
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "interlaced flat-fill triangle is rasterized");

        for (std::uint32_t y = 0U; y < 8U; ++y) {
            for (std::uint32_t x = 0U; x < 8U; ++x) {
                std::uint32_t pixel = 0U;
                require(bus.read32(framebuffer + (y * 8U + x) * 4U, pixel),
                        "read interlaced triangle output pixel");
                const bool selectedField = (y & 1U) == oddLine;
                const bool covered = selectedField && y < 4U &&
                    x >= 2U && x <= 2U + y;
                require(pixel == (covered ? fill : sentinel),
                        "triangle writes only scanlines selected by interlaced field");
            }
        }
    }
}


void testFlatFillTriangleUsesFillColorInCopyCycle() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    constexpr std::uint32_t framebuffer = 0x900U;
    constexpr std::uint32_t stream = 0xE00U;
    constexpr std::uint32_t sentinel = 0xDEADBEEFU;
    constexpr std::uint32_t fill = 0x89ABCDEFU;
    for (std::uint32_t i = 0U; i < 8U * 8U; ++i)
        require(bus.write32(framebuffer + i * 4U, sentinel),
                "seed copy-cycle triangle framebuffer");
    const std::uint64_t commands[] = {
        (0x3FULL << 56U) | (3ULL << 51U) | (7ULL << 32U) | framebuffer,
        (0x37ULL << 56U) | fill,
        (0x2FULL << 56U) | (2ULL << 52U),
        (0x2DULL << 56U) | (32ULL << 12U) | 32ULL,
        (0x08ULL << 56U) | (1ULL << 55U) | (16ULL << 16U) | 16ULL,
        0ULL,
        (2ULL * 65536ULL) << 32U,
        ((2ULL * 65536ULL) << 32U) | 0x00010000ULL
    };
    for (std::size_t i = 0U; i < sizeof(commands) / sizeof(commands[0]); ++i)
        require(bus.write64(stream + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "write copy-cycle triangle command stream");
    require(bus.write32(Bus::kDpStart, stream) &&
            bus.write32(Bus::kDpEnd, stream + 64U),
            "publish copy-cycle triangle command stream");
    for (unsigned i = 0U; i < 4U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "copy-cycle triangle setup command is processed");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "flat-fill triangle is rasterized in COPY cycle");
    for (std::uint32_t y = 0U; y < 8U; ++y) {
        for (std::uint32_t x = 0U; x < 8U; ++x) {
            std::uint32_t pixel = 0U;
            require(bus.read32(framebuffer + (y * 8U + x) * 4U, pixel),
                    "read copy-cycle triangle output pixel");
            const bool covered = y < 4U && x >= 2U && x <= 2U + y;
            require(pixel == (covered ? fill : sentinel),
                    "COPY-cycle flat triangle repeats SET_FILL_COLOR");
        }
    }
}


void testFlatFillTriangleRejectsOutOfBoundsFramebufferAtomically() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    constexpr std::uint32_t stream = 0xF00U;
    constexpr std::uint32_t sentinel = 0xCAFEBABEU;
    const std::uint32_t framebuffer = static_cast<std::uint32_t>(
        bus.rdram().size() - 100U);
    for (std::uint32_t offset = 0U; offset < 100U; offset += 4U)
        require(bus.write32(framebuffer + offset, sentinel),
                "seed near-end triangle framebuffer");
    const std::uint64_t commands[] = {
        (0x3FULL << 56U) | (3ULL << 51U) | (7ULL << 32U) | framebuffer,
        (0x37ULL << 56U) | 0x11223344ULL,
        (0x2FULL << 56U) | (3ULL << 52U),
        (0x2DULL << 56U) | (32ULL << 12U) | 32ULL,
        (0x08ULL << 56U) | (1ULL << 55U) | (16ULL << 16U) | 16ULL,
        0ULL,
        (2ULL * 65536ULL) << 32U,
        ((2ULL * 65536ULL) << 32U) | 0x00010000ULL
    };
    for (std::size_t i = 0U; i < sizeof(commands) / sizeof(commands[0]); ++i)
        require(bus.write64(stream + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "write out-of-bounds triangle command stream");
    require(bus.write32(Bus::kDpStart, stream) &&
            bus.write32(Bus::kDpEnd, stream + 64U),
            "publish out-of-bounds triangle command stream");
    for (unsigned i = 0U; i < 4U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "out-of-bounds triangle setup command is processed");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Unsupported,
            "out-of-bounds triangle is rejected without rasterization");
    for (std::uint32_t offset = 0U; offset < 100U; offset += 4U) {
        std::uint32_t pixel = 0U;
        require(bus.read32(framebuffer + offset, pixel),
                "read near-end triangle framebuffer");
        require(pixel == sentinel,
                "out-of-bounds triangle leaves the entire framebuffer unchanged");
    }
}


void testFlatFillTriangleHonorsFractionalYBounds() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    constexpr std::uint32_t framebuffer = 0x1700U;
    constexpr std::uint32_t stream = 0x1800U;
    constexpr std::uint32_t sentinel = 0xDEADBEEFU;
    constexpr std::uint32_t fill = 0x89ABCDEFU;
    for (std::uint32_t i = 0U; i < 8U * 8U; ++i)
        require(bus.write32(framebuffer + i * 4U, sentinel),
                "seed fractional-Y triangle framebuffer");
    const std::uint64_t commands[] = {
        (0x3FULL << 56U) | (3ULL << 51U) | (7ULL << 32U) | framebuffer,
        (0x37ULL << 56U) | fill,
        (0x2FULL << 56U) | (3ULL << 52U),
        (0x2DULL << 56U) | (32ULL << 12U) | 32ULL,
        (0x08ULL << 56U) | (1ULL << 55U) |
            (1ULL << 32U) | (9ULL << 16U) | 17ULL,
        (2ULL * 65536ULL) << 32U,
        (2ULL * 65536ULL) << 32U,
        (2ULL * 65536ULL) << 32U
    };
    for (std::size_t i = 0U; i < sizeof(commands) / sizeof(commands[0]); ++i)
        require(bus.write64(stream + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "write fractional-Y triangle command stream");
    require(bus.write32(Bus::kDpStart, stream) &&
            bus.write32(Bus::kDpEnd, stream + 64U),
            "publish fractional-Y triangle command stream");
    for (unsigned i = 0U; i < 4U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "fractional-Y triangle setup command is processed");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "fractional-Y flat triangle is rasterized");

    for (std::uint32_t y = 0U; y < 8U; ++y) {
        for (std::uint32_t x = 0U; x < 8U; ++x) {
            std::uint32_t pixel = 0U;
            require(bus.read32(framebuffer + (y * 8U + x) * 4U, pixel),
                    "read fractional-Y triangle output pixel");
            const bool covered = y < 5U && x == 2U;
            require(pixel == (covered ? fill : sentinel),
                    "fractional triangle Y bounds include intersected scanlines only");
        }
    }
}


void testFlatFillTriangleClipsNegativeYToScissorTop() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    constexpr std::uint32_t framebuffer = 0x1300U;
    constexpr std::uint32_t stream = 0x1400U;
    constexpr std::uint32_t sentinel = 0xDEADBEEFU;
    constexpr std::uint32_t fill = 0x76543210U;
    for (std::uint32_t i = 0U; i < 8U * 8U; ++i)
        require(bus.write32(framebuffer + i * 4U, sentinel),
                "seed negative-Y triangle framebuffer");
    const std::uint64_t commands[] = {
        (0x3FULL << 56U) | (3ULL << 51U) | (7ULL << 32U) | framebuffer,
        (0x37ULL << 56U) | fill,
        (0x2FULL << 56U) | (3ULL << 52U),
        (0x2DULL << 56U) | (32ULL << 12U) | 32ULL,
        (0x08ULL << 56U) | (1ULL << 55U) |
            (0x3FFCLL << 32U) | (8ULL << 16U) | 16ULL,
        (2ULL * 65536ULL) << 32U,
        (2ULL * 65536ULL) << 32U,
        (2ULL * 65536ULL) << 32U
    };
    for (std::size_t i = 0U; i < sizeof(commands) / sizeof(commands[0]); ++i)
        require(bus.write64(stream + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "write negative-Y triangle command stream");
    require(bus.write32(Bus::kDpStart, stream) &&
            bus.write32(Bus::kDpEnd, stream + 64U),
            "publish negative-Y triangle command stream");
    for (unsigned i = 0U; i < 4U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "negative-Y triangle setup command is processed");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "negative-Y flat triangle is clipped and rasterized");

    for (std::uint32_t y = 0U; y < 8U; ++y) {
        for (std::uint32_t x = 0U; x < 8U; ++x) {
            std::uint32_t pixel = 0U;
            require(bus.read32(framebuffer + (y * 8U + x) * 4U, pixel),
                    "read negative-Y triangle output pixel");
            const bool covered = y < 4U && x == 2U;
            require(pixel == (covered ? fill : sentinel),
                    "negative-Y triangle clips at scissor top without shifting visible scanlines");
        }
    }
}


void testFlatFillTriangleClipsNegativeFractionalYToScissorTop() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    constexpr std::uint32_t framebuffer = 0x1500U;
    constexpr std::uint32_t stream = 0x1600U;
    constexpr std::uint32_t sentinel = 0xDEADBEEFU;
    constexpr std::uint32_t fill = 0x76543210U;
    for (std::uint32_t i = 0U; i < 8U * 8U; ++i)
        require(bus.write32(framebuffer + i * 4U, sentinel),
                "seed negative-fractional-Y triangle framebuffer");
    const std::uint64_t commands[] = {
        (0x3FULL << 56U) | (3ULL << 51U) | (7ULL << 32U) | framebuffer,
        (0x37ULL << 56U) | fill,
        (0x2FULL << 56U) | (3ULL << 52U),
        (0x2DULL << 56U) | (32ULL << 12U) | 32ULL,
        (0x08ULL << 56U) | (1ULL << 55U) |
            (0x3FFFL << 32U) | (8ULL << 16U) | 16ULL,
        (2ULL * 65536ULL) << 32U,
        (2ULL * 65536ULL) << 32U,
        (2ULL * 65536ULL) << 32U
    };
    for (std::size_t i = 0U; i < sizeof(commands) / sizeof(commands[0]); ++i)
        require(bus.write64(stream + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "write negative-fractional-Y triangle command stream");
    require(bus.write32(Bus::kDpStart, stream) &&
            bus.write32(Bus::kDpEnd, stream + 64U),
            "publish negative-fractional-Y triangle command stream");
    for (unsigned i = 0U; i < 4U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "negative-fractional-Y triangle setup command is processed");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "negative-fractional-Y flat triangle is clipped and rasterized");

    for (std::uint32_t y = 0U; y < 8U; ++y) {
        for (std::uint32_t x = 0U; x < 8U; ++x) {
            std::uint32_t pixel = 0U;
            require(bus.read32(framebuffer + (y * 8U + x) * 4U, pixel),
                    "read negative-fractional-Y triangle output pixel");
            const bool covered = y < 4U && x == 2U;
            require(pixel == (covered ? fill : sentinel),
                    "negative-fractional-Y triangle clips at scissor top without shifting visible scanlines");
        }
    }
}


void testFlatFillTriangleClipsToScissorRectangle() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    constexpr std::uint32_t framebuffer = 0x1100U;
    constexpr std::uint32_t stream = 0x1200U;
    constexpr std::uint32_t sentinel = 0xDEADBEEFU;
    constexpr std::uint32_t fill = 0x55667788U;
    for (std::uint32_t i = 0U; i < 8U * 8U; ++i)
        require(bus.write32(framebuffer + i * 4U, sentinel),
                "seed scissor-clipped triangle framebuffer");
    const std::uint64_t commands[] = {
        (0x3FULL << 56U) | (3ULL << 51U) | (7ULL << 32U) | framebuffer,
        (0x37ULL << 56U) | fill,
        (0x2FULL << 56U) | (3ULL << 52U),
        (0x2DULL << 56U) | (8ULL << 44U) | (16ULL << 12U) | 8ULL,
        (0x08ULL << 56U) | (1ULL << 55U) | (16ULL << 16U) | 16ULL,
        0ULL,
        (2ULL * 65536ULL) << 32U,
        ((2ULL * 65536ULL) << 32U) | 0x00010000ULL
    };
    for (std::size_t i = 0U; i < sizeof(commands) / sizeof(commands[0]); ++i)
        require(bus.write64(stream + static_cast<std::uint32_t>(i * 8U), commands[i]),
                "write scissor-clipped triangle command stream");
    require(bus.write32(Bus::kDpStart, stream) &&
            bus.write32(Bus::kDpEnd, stream + 64U),
            "publish scissor-clipped triangle command stream");
    for (unsigned i = 0U; i < 4U; ++i)
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
                "scissor-clipped triangle setup command is processed");
    require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Processed,
            "scissor-clipped flat triangle is rasterized");
    for (std::uint32_t y = 0U; y < 8U; ++y) {
        for (std::uint32_t x = 0U; x < 8U; ++x) {
            std::uint32_t pixel = 0U;
            require(bus.read32(framebuffer + (y * 8U + x) * 4U, pixel),
                    "read scissor-clipped triangle pixel");
            const bool covered = y < 2U && x >= 2U && x <= 4U &&
                x <= 2U + y;
            require(pixel == (covered ? fill : sentinel),
                    "triangle writes only pixels inside scissor bounds");
        }
    }
}

void testTextureRectanglePacketDecoding() {
    using namespace m64plus::n64;
    for (std::uint8_t opcode : {0x24U, 0x25U}) {
        RdpCommandPacket packet{};
        packet.info = RdpCommandDecoder::decode(header(opcode));
        packet.words[0] = (static_cast<std::uint64_t>(opcode) << 56U) |
            (0x123ULL << 44U) | (0x456ULL << 32U) | (5ULL << 24U) |
            (0x789ULL << 12U) | 0xABCU;
        packet.words[1] = (0xFEDCULL << 48U) | (0x8123ULL << 32U) |
            (0x7FFFULL << 16U) | 0x8001ULL;

        RdpTextureRectangleCommand rectangle{};
        require(RdpCommandDecoder::decodeTextureRectangle(packet, rectangle),
                "two-word texture rectangle packet decodes");
        require(rectangle.xh == 0x123U && rectangle.yh == 0x456U &&
                rectangle.tile == 5U && rectangle.xl == 0x789U &&
                rectangle.yl == 0xABCU,
                "texture rectangle coordinates and tile decode from first word");
        require(rectangle.s == -292 && rectangle.t == -32477 &&
                rectangle.dsdx == 32767 && rectangle.dtdy == -32767,
                "texture rectangle signed S/T and derivative fields decode");
        require(rectangle.flip == (opcode == 0x25U) &&
                rectangle.rawWords[0] == packet.words[0] &&
                rectangle.rawWords[1] == packet.words[1],
                "texture rectangle preserves flip mode and both raw words");
    }

    RdpCommandPacket invalid{};
    invalid.info = RdpCommandDecoder::decode(header(0x24U));
    invalid.words[0] = header(0x25U);
    RdpTextureRectangleCommand unchanged{};
    unchanged.xh = 0x55U;
    require(!RdpCommandDecoder::decodeTextureRectangle(invalid, unchanged) &&
            unchanged.xh == 0x55U,
            "invalid texture rectangle packet leaves output unchanged");

    invalid.words[0] = header(0x24U);
    invalid.info.lengthWords = 1U;
    require(!RdpCommandDecoder::decodeTextureRectangle(invalid, unchanged) &&
            unchanged.xh == 0x55U,
            "truncated texture rectangle metadata is rejected without changing output");

    invalid.info = RdpCommandDecoder::decode(header(0x23U));
    invalid.words[0] = header(0x23U);
    require(!RdpCommandDecoder::decodeTextureRectangle(invalid, unchanged) &&
            unchanged.xh == 0x55U,
            "non-texture opcode is rejected without changing output");

    invalid.info = RdpCommandDecoder::decode(header(0x24U));
    invalid.info.knownOpcode = false;
    invalid.words[0] = header(0x24U);
    require(!RdpCommandDecoder::decodeTextureRectangle(invalid, unchanged) &&
            unchanged.xh == 0x55U,
            "texture rectangle with inconsistent unknown-opcode metadata is rejected");
}

void testTextureRectangleCommandsAreConsumedAsUnsupportedUntilRasterizationExists() {
    using namespace m64plus::n64;
    for (std::uint8_t opcode : {0x24U, 0x25U}) {
        Bus bus;
        RdpSyncState state{};
        constexpr std::uint32_t framebuffer = 0x2400U;
        constexpr std::uint32_t sentinel = 0xA1B2C3D4U;
        state.colorImage = {0U, 0U, 4U, framebuffer};
        state.hasColorImage = true;
        state.fillColor = 0x55667788U;
        state.hasFillColor = true;
        for (std::uint32_t i = 0U; i < 16U; ++i)
            require(bus.write32(framebuffer + i * 4U, sentinel),
                    "seed framebuffer before unsupported texture rectangle");
        const std::uint32_t start = 0xA40U;
        const std::uint64_t first = (static_cast<std::uint64_t>(opcode) << 56U) |
            (0x120ULL << 44U) | (0x230ULL << 32U) | (3ULL << 24U) |
            (0x340ULL << 12U) | 0x450ULL;
        const std::uint64_t second = (0xFFF0ULL << 48U) | (0x8001ULL << 32U) |
            (0x0100ULL << 16U) | 0x0200ULL;
        require(bus.write64(start, first) && bus.write64(start + 8U, second),
                "seed complete texture rectangle command packet");
        require(bus.write32(Bus::kDpStart, start) &&
                bus.write32(Bus::kDpEnd, start + 16U),
                "publish complete texture rectangle command");
        require(RdpCommandProcessor::processNext(bus, state) == RdpProcessResult::Unsupported,
                "texture rectangle remains unsupported until texture rasterization exists");
        require(state.hasTextureRectangle &&
                state.lastTextureRectangle.tile == 3U &&
                state.lastTextureRectangle.xh == 0x120U &&
                state.lastTextureRectangle.yl == 0x450U &&
                state.lastTextureRectangle.s == -16 &&
                state.lastTextureRectangle.t == -32767 &&
                state.lastTextureRectangle.flip == (opcode == 0x25U),
                "processor retains decoded texture rectangle state for future rasterizer");
        std::uint32_t current = 0U;
        require(bus.read32(Bus::kDpCurrent, current) && current == start + 16U,
                "texture rectangle consumes its complete two-word packet");
        for (std::uint32_t i = 0U; i < 16U; ++i) {
            std::uint32_t pixel = 0U;
            require(bus.read32(framebuffer + i * 4U, pixel) && pixel == sentinel,
                    "unsupported texture rectangle leaves framebuffer unchanged");
        }
    }
}

void testTextureRectangleWaitsForBothWordsWithoutConsumingPartialPacket() {
    using namespace m64plus::n64;
    Bus bus;
    RdpCommandPacket packet{};
    const std::uint32_t start = 0xB80U;
    const std::uint64_t first = (0x24ULL << 56U) |
        (0x100ULL << 44U) | (0x200ULL << 32U) |
        (2ULL << 24U) | (0x300ULL << 12U) | 0x400ULL;
    require(bus.write64(start, first), "write texture rectangle first word");
    require(bus.write32(Bus::kDpStart, start) &&
            bus.write32(Bus::kDpEnd, start + 8U),
            "publish incomplete texture rectangle packet");
    require(!RdpCommandStream::fetchNext(bus, packet),
            "incomplete texture rectangle packet is not fetched");
    std::uint32_t current = 0U;
    require(bus.read32(Bus::kDpCurrent, current) && current == start,
            "incomplete texture rectangle leaves DPC cursor unchanged");

    const std::uint64_t second = 0x0001000200030004ULL;
    require(bus.write64(start + 8U, second), "write texture rectangle second word");
    require(bus.write32(Bus::kDpEnd, start + 16U),
            "publish complete texture rectangle packet");
    require(RdpCommandStream::fetchNext(bus, packet),
            "complete texture rectangle packet becomes fetchable");
    require(packet.info.opcode == 0x24U &&
            packet.words[0] == first && packet.words[1] == second,
            "fetch returns both texture rectangle words intact");
    require(bus.read32(Bus::kDpCurrent, current) && current == start + 16U,
            "complete texture rectangle advances DPC cursor by two words");
}

void testTextureRectangleLengths() {
    require(m64plus::n64::RdpCommandDecoder::decode(header(0x24U)).lengthWords == 2U,
            "texture rectangle has two words");
    require(m64plus::n64::RdpCommandDecoder::decode(header(0x25U)).lengthWords == 2U,
            "texture rectangle flip has two words");
}
} // namespace

int main() {
    testTriangleDecoderRejectsUnknownOpcodeMetadataAtomically();

    testSingleWordCommands();
    testTriangleCommandLengths();
    testTriangleOptionalBlockLayout();
    testTriangleEdgeHeaderDecoding();
    testTriangleCommandPacketDecoding();
    testMaximumTriangleCommandWaitsForAllWords();
    testTriangleCommandsAreConsumedAsUnsupportedUntilRasterizationExists();
    testFlatFillTriangleRasterizesRightTriangle();
    testFlatFillTriangleUsesLowerEdgeAfterMiddleY();
    testFlatFillTriangleUsesNegativeLowerEdgeSlope();
    testFlatFillTriangleFloorsPositiveFractionalX();
    testFlatFillTriangleFloorsPositiveFractionalXWithSlope();
    testFlatFillTriangleFloorsNegativeMajorSlope();
    testFlatFillTriangleEdgeFlipFloorsPositiveFractionalX();
    testFlatFillTriangleEdgeFlipFloorsNegativeMajorSlope();
    testFlatFillTriangleClipsNegativeFractionalX();
    testFlatFillTriangleEdgeFlipClipsNegativeFractionalX();
    testFlatFillTriangleRasterizesOppositeEdgeOrientation();
    testFlatFillTriangleRgba16UsesFramebufferHalfwordParity();
    testFlatFillTriangleHonorsInterlacedFieldSelection();
    testFlatFillTriangleUsesFillColorInCopyCycle();
    testFlatFillTriangleRejectsOutOfBoundsFramebufferAtomically();
    testFlatFillTriangleHonorsFractionalYBounds();
    testFlatFillTriangleClipsNegativeYToScissorTop();
    testFlatFillTriangleClipsNegativeFractionalYToScissorTop();
    testFlatFillTriangleClipsToScissorRectangle();
    testTextureRectangleWaitsForBothWordsWithoutConsumingPartialPacket();
    testTextureRectangleLengths();
    testTextureRectanglePacketDecoding();
    testTextureRectangleCommandsAreConsumedAsUnsupportedUntilRasterizationExists();
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
    testLoadTileUpdatesTileSizeAndSetTileSizeOverrides();
    testLoadTileUsesLoadTileDescriptorSizeForSourceAddressing();
    testLoadTileCopiesTextureImageIntoTmem();
    testLoadTileRejectsInvertedCoordinatesWithoutWrites();
    testLoadTileValidatesEverySourceRowBeforeWritingTmem();
    testLoadTileRejectsRangePastTextureImageWidthWithoutWrites();
    testLoadTileHonorsTileLineStrideAcrossOddRows();
    testLoadTileWrapsDestinationAcrossTmemEnd();
    testLoadTile4BitWrapsDestinationAcrossTmemEnd();
    testLoadTileFourBitOddSourceNibbleWrapsAcrossTmemEnd();
    testLoadTile16BitWrapsDestinationAcrossTmemEnd();
    testLoadTileRejectsOutOfRangeSourceWithoutPartialTmemWrites();
    testLoadTileFourBitRangeStartingAtOddSourceNibble();
    testLoadTileFourBitOddSourceNibbleAcrossSwizzledRows();
    testLoadTilePacksFourBitNibblesAndSwizzlesOddRows();
    testLoadTileSwizzlesOddRowsFor16BitTexels();
    testLoadTileSplits32BitRgbaAcrossTmemBanks();
    testLoadTileRgba32UpperBankStaysAlignedAcrossBankWrap();
    testLoadTile32BitUsesNonzeroSourceXAcrossRows();
    testLoadTile32BitWrapsBothBanksAcrossTmemEnd();
    testLoadTile32BitValidatesWholeSourceBeforeWritingEitherBank();
    testLoadTile32BitValidatesEverySourceRowBeforeWritingBanks();
    testLoadTileRejectsNonRgba32FormatsWithoutWritingTmem();
    testLoadBlockUpdatesTileSizeAndSetTileSizeOverrides();
    testLoadBlockCopiesContiguousTexelsIntoTmem();
    testLoadBlockWrapsDestinationAcrossTmemEnd();
    testLoadBlockRejectsSourcePastRdramEndWithoutPartialWrites();
    testLoadBlockEightBitRejectsSourceOverflowAtomically();
    testLoadBlockEightBitDxtRejectsSourceOverflowAtomically();
    testLoadBlockSixteenBitRejectsSourceOverflowAtomically();
    testLoadBlockSixteenBitDxtRejectsSourceOverflowAtomically();
    testLoadBlockFourBitRejectsSourceOverflowAtomically();
    testLoadBlockFourBitDxtRejectsSourceOverflowAtomically();
    testLoadBlockDxtSwapsOddLineWords();
    testLoadBlockDxtSwizzlesOddLinesForEightAndSixteenBitTexels();
    testLoadBlockDxtCarriesAtExactHalfLine();
    testLoadBlockDxtHandlesMultipleCarriesAndFractionalLines();
    testLoadBlockContinuesAcrossTextureImageRows();
    testLoadBlockRejectsStartOutsideTextureImageWidth();
    testLoadBlockRgba32RejectsSourceOverflowAtomically();
    testLoadBlockRgba32DxtRejectsSourceOverflowAtomically();
    testLoadBlockRgba32DxtRejectsNonRgbaTileFormatAtomically();
    testLoadBlockRgba32DxtRejectsMismatchedTileSizeAtomically();
    testLoadBlockRgba32DxtRejectsNonRgbaImageFormatAtomically();
    testLoadBlockRgba32DxtMultipleCarriesApplyOddLineSwizzle();
    testLoadBlockRgba32DxtWrapsBothTmemBanks();
    testLoadBlockRgba32WrapsBothTmemBanks();
    testLoadBlockCopiesPackedFourBitTexels();
    testLoadBlockFourBitStartsAtOddSourceNibble();
    testLoadBlockFourBitOddNibbleDxtSwizzlesOddLine();
    testLoadBlockFourBitDxtSwizzlesOddWords();
    testLoadBlockCopiesRgba32IntoSplitTmemBanks();
    testLoadBlockRgba32DxtFractionalLineCarry();
    testLoadBlockRgba32DxtSwapsOddLineWords();
    testLoadBlockRejectsMismatchedRgba32TileSize();
    testLoadTlutReplicatesEntriesIntoHighTmem();
    testLoadTlutRejectsOutOfRangeSourceWithoutPartialTmemWrites();
    testLoadTlutAcceptsCanonicalWidthOnePaletteLoad();
    testLoadTlutRejectsDestinationBeyondTmemWithoutPartialWrites();
    testLoadTlutRejectsDestinationBelowPaletteRegionWithoutWrites();
    testLoadTlutHonorsTextureImageCoordinatesAndRange();
    testFillRectangleWritesRgba32PixelsAndHonorsScissor();
    testFillRectangleRejectsUnsupportedCycleTypeWithoutWrites();
    testFillRectangleRejectsMissingOtherModesWithoutWrites();
    testFillRectangleRejectsMissingFillColorWithoutWrites();
    testFillRectangleRejectsMissingColorImageWithoutWrites();
    testFillRectangleRejectsUnsupportedColorImageFormatsWithoutWrites();
    testFillRectangleWritesSinglePixelForDegenerateBounds();
    testFillRectangleClipsAllEdgesAgainstScissor();
    testFillRectangleWritesRgba16AlternatingHalfwords();
    testFillRectangleRgba16ScissorPreservesPixelParity();
    testFillRectangleHonorsScissorFieldParity();
    testFillRectangleWritesFinalRdramPixelAtExactBoundary();
    testFillRectangleWritesFinalRdramHalfwordAtExactBoundary();
    testFillRectangleRejectsRgba16OverflowWithoutPartialWrites();
    testFillRectangleRejectsRgba32OverflowWithoutPartialWrites();
    testFillRectangleIgnoresUnselectedFieldOverflow();
    testFillRectangleRejectsLaterRowOverflowWithoutPartialWrites();
    testFillRectangleRejectsRgba16LaterRowOverflowWithoutPartialWrites();
    testFillRectangleRejectsOutOfRangeWithoutPartialWrites();
    testFillRectangleRejectsEmptyScissorIntersectionWithoutWrites();
    testFillRectangleRejectsInvertedCoordinatesWithoutWrites();
    testFillRectangleRejectsInvertedVerticalCoordinatesWithoutWrites();
    testUnsupportedRdpCommandIsReported();
    std::cout << "All RDP decoder host tests passed.\n";
    return 0;
}
