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

void testUnsupportedRdpCommandIsReported() {
    using namespace m64plus::n64;
    Bus bus;
    RdpSyncState state{};
    require(bus.write64(0x600U, static_cast<std::uint64_t>(0x2AU) << 56U),
            "seed unsupported key command");
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
    testUnsupportedRdpCommandIsReported();
    std::cout << "All RDP decoder host tests passed.\n";
    return 0;
}
