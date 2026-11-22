#include "n64/rdp.h"

#include "n64/bus.h"

namespace m64plus::n64 {

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
        (opcode >= 0x24U && opcode <= 0x3FU);
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
    case 0x2FU: // SET_OTHER_MODES
        state.otherModesCommand = packet.words[0];
        state.hasOtherModes = true;
        return RdpProcessResult::Processed;
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
    case 0x37U: // SET_FILL_COLOR
        state.fillColorCommand = packet.words[0];
        state.hasFillColor = true;
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
